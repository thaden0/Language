/* LA-30 — stackful task substrate (designs/suspension/techdesign-01-task-substrate.md).
 *
 * One C substrate, two consumers: the `lvrt` archive (LLVM-emitted programs) and
 * the `langfront` library (the compiler binary — doc 3 hosts interpreter recursion
 * on these same fibers). Plain C17 + POSIX, no dependencies, no LLVM; testable
 * standalone through `runtime_selftest` (§9), the Track-B discipline.
 *
 * The substrate knows nothing about Promises, LvValue, or interpreters. Engine
 * knowledge enters through the two installed hooks (§6): a *poll* predicate ("is
 * this opaque parked-on object settled?") and a *loop_step* thunk. That keeps this
 * file identical for both consumers and keeps the loop dumb (RuntimeLoop.hpp:16-19).
 *
 * Tasks are pinned to their spawning thread forever — no migration, no work
 * stealing (overview §9, load-bearing). Every scheduler structure below is LV_TLS.
 *
 * SENSITIVE SECTION S1 (overview §12): the context-switch contract (the .S files +
 * the stack seeding that mirrors their frame layout), the fresh-task trampoline,
 * the DONE-task pool-return rule, and the sanitizer/valgrind fiber annotations are
 * S1-fenced work — authored under the model gate, marked "S1 (fenced)" below.
 * Review those regions against lv_ctx_x86_64.S's documented frame layout.
 */
#include "lv_task.h"
#include "lv_abi.h"      /* LV_TLS */

#include <stdlib.h>      /* getenv, strtoull, calloc; abort (win32 stubs too) */

#ifdef _WIN32
/* G5: the win32 leg is the Fiber API via lv_plat_win32.c (TIB StackBase/Limit,
 * SEH, __chkstk — hand-rolled asm is wrong there), scheduled at M4 iff the wine
 * lane executes async corpus (audit ruling: it does not — the lane's scope is
 * core + llvm_objects only, doc-2 §7's acceptance; no async corpus executes
 * there). Until the fiber leg exists, tasks stay OFF on Windows EVEN AFTER the
 * M5 default flip: this probe returning 0 is the platform's pump pin (the
 * engine keeps the pump) and every other entry is a loud misuse trap, so the
 * archive still links. The wine lane script (tests/run_wine_cross.sh) carries
 * the matching explicit LANG_PUMP pin + port-milestone comment per S5's
 * "never silently divergent" rule (doc 5 §5). */
int  lv_tasks_enabled(void) { return 0; }
void lv_sched_init(void) {}
void lv_sched_run(lv_task_fn main_fn, void* a, void* b) { (void)main_fn; (void)a; (void)b; abort(); }
void lv_sched_hooks(int (*poll)(const void*), int (*loop_step)(void)) { (void)poll; (void)loop_step; }
void lv_sched_throw_probe(int (*throwing)(void)) { (void)throwing; }
lv_task_t* lv_task_self(void) { return 0; }
void lv_task_spawn(lv_task_fn fn, void* a, void* b) { (void)fn; (void)a; (void)b; abort(); }
void lv_task_yield(void) { abort(); }
int  lv_task_park_on(const void* obj) { (void)obj; abort(); }
void lv_task_stats_report(void) {}
int  lv_sched_main_completed(void) { return 1; }   /* no scheduler ran: nothing was cut short */
void lv_sched_teardown(void) {}
/* B2 (doc 06): same pump-pin discipline — tasks are off on win32, so the
 * registry is empty (cancel/shield no-op) and the parks are misuse traps. */
uint64_t lv_task_spawn_registered(lv_task_fn fn, void* a, void* b) { (void)fn; (void)a; (void)b; abort(); }
int  lv_task_cancel(uint64_t id) { (void)id; return 0; }
void lv_task_shield(int delta) { (void)delta; }
int  lv_task_park_any2(const void* a, const void* b, int* which) { (void)a; (void)b; (void)which; abort(); }
int  lv_task_park_join(const uint64_t* ids, int n) { (void)ids; (void)n; abort(); }
#else /* the POSIX substrate proper */

/* POSIX-only includes live BELOW the _WIN32 split: the win32 stub block above
 * must compile under MinGW, which has no <sys/mman.h> (LA-30 doc 2 cross lane). */
#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>        /* clock_gettime — B2 §8's join report timing ONLY (never wake
                            correctness: a pure deadline with no loop timer would deadlock
                            the quiescence protocol; the timeout arm is a real loop timer) */
#include <unistd.h>      /* sysconf, write */

#ifdef LV_CTX_UCONTEXT
#include <ucontext.h>    /* G1 fallback: swapcontext costs a sigprocmask syscall
                            per switch on glibc — bring-up/porting path ONLY */
#endif

/* --- sanitizer detection (G6 — S1 (fenced): these annotations land at M0, not
 * later, or every later leg's sanitizer runs lie). LV_TASK_ASAN keys off the
 * compiler's ASan flag (the LANG_RT_SANITIZE build sets it); the two fiber hooks
 * are provided by libasan for both gcc and clang, declared here directly since
 * gcc ships no sanitizer interface headers. */
#if defined(__SANITIZE_ADDRESS__)
# define LV_TASK_ASAN 1
#elif defined(__has_feature)
# if __has_feature(address_sanitizer)
#  define LV_TASK_ASAN 1
# endif
#endif
#ifndef LV_TASK_ASAN
# define LV_TASK_ASAN 0
#endif
#if LV_TASK_ASAN
void __sanitizer_start_switch_fiber(void** fake_stack_save, const void* bottom, size_t size);
void __sanitizer_finish_switch_fiber(void* fake_stack_save, const void** bottom_old, size_t* size_old);
#endif

/* Valgrind stack registration (G6): guarded by the header's presence — the same
 * zero-cost-when-absent feature detection the repo uses for OpenSSL/LLVM (the
 * design's "vendored the usual way" adapted to detection; RUNNING_ON_VALGRIND
 * makes it zero-cost at runtime too). Without it valgrind reports wild "invalid
 * write below stack" on the first switch. */
#if defined(__has_include)
# if __has_include(<valgrind/valgrind.h>)
#  include <valgrind/valgrind.h>
#  define LV_TASK_VALGRIND 1
# endif
#endif
#ifndef LV_TASK_VALGRIND
# define LV_TASK_VALGRIND 0
#endif

/* ========================= §3 Task object and lists ========================= */

/* Task lifecycle states. A task is on at most one intrusive list at a time
 * (runq | parked | pool), so the states are exclusive and one `next` suffices. */
enum {
    LV_TASK_RUNNABLE = 0,   /* on the run queue, waiting for a switch-to      */
    LV_TASK_RUNNING,        /* == sched.current; executing on its own stack   */
    LV_TASK_PARKED,         /* on the parked list, awaiting poll(parked_on)   */
    LV_TASK_DONE,           /* fn returned; stack pending return to the pool  */
    LV_TASK_POOLED          /* on the free-list, stack mapped, no live frame  */
};

struct lv_task {
    void*       sp;            /* saved stack pointer; register file lives ON the stack */
    uint8_t*    stack_base;    /* mmap base (guard page is [base, base+PAGE))            */
    size_t      stack_size;    /* usable bytes above the guard                            */
    lv_task_t*  next;          /* intrusive: runq | parked | pool (exclusive states)      */
    const void* parked_on;     /* poll key while PARKED — BORROWED, never retained (§7)   */
    /* B2 (doc 06 §5) — the park is one of two SHAPES, chosen by which fields are
     * set: promise keys (parked_on, optionally parked_on2 — the N=2 inline
     * multi-park) or a join id-set (join_ids/join_n; parked_on stays NULL).
     * Every extra key is BORROWED under the same §7/G12 rule as parked_on: the
     * parked task's own frame holds the reference/buffer, so nothing here can
     * dangle while parked. Keep all access behind the park/poll helpers (§8's
     * reviewer note) so a future general-N park stays mechanical. */
    const void*     parked_on2;   /* second promise key, or NULL                */
    const uint64_t* join_ids;     /* join park: BORROWED id buffer, or NULL     */
    uint32_t        join_n;
    int64_t         park_ns;      /* park entry time — join report timing only  */
    lv_task_fn  fn; void* a; void* b;
    uint32_t    state;         /* LV_TASK_* above                                          */
    uint8_t     wake_reason;   /* LV_PARK_* set by the waker; SETTLED unless drain (G2)
                                  or cancel (B2) woke this park                            */
    uint8_t     woke_ix;       /* which promise key settled (0/1) — park_any2's out       */
    uint8_t     cancelled;     /* B2 §2: cancel MARK — consumed at delivery, held through
                                  shielded/settled parks until a deliverable park          */
    uint8_t     reported;      /* B2 §8: this join park already fired its report          */
    uint16_t    shield;        /* B2 §8: cancel-delivery mask counter (close() unwind)    */
    uint8_t     registered;    /* on the id registry; deregistered at completion          */
    uint64_t    id;            /* spawn ordinal — the "task #N" of §4's overflow diagnostic
                                  AND (B2) the public registry id (never reused)           */
#ifdef LV_CTX_UCONTEXT
    ucontext_t  uc;
#endif
#if LV_TASK_ASAN
    void* fake_stack; const void* asan_base; size_t asan_size;
#endif
#if LV_TASK_VALGRIND
    unsigned vg_id;
#endif
};

/* Per-thread scheduler state — one LV_TLS struct (§3). Intrusive FIFO run/parked
 * queues, a LIFO stack pool, the engine hooks, and the counters behind the
 * LANG_TASK_STATS `[tasks]` line (§8). No allocation on any scheduling path after
 * pool warm-up (the mincore scratch and altstack are one-time warm-up allocs). */
typedef struct lv_sched {
    lv_task_t*  current;                             /* RUNNING task; NULL on the scheduler */
    lv_task_t*  runq_head;   lv_task_t* runq_tail;   /* FIFO — completion/enqueue order (C1) */
    lv_task_t*  parked_head; lv_task_t* parked_tail; /* FIFO — park order = drain-wake order  */
    lv_task_t*  pool;                                /* LIFO free-list of returned stacks     */

    int       (*poll)(const void* obj);              /* G15: reads bool immediates only       */
    int       (*loop_step)(void);                    /* one loop batch; 0 == no work           */
    int       (*throwing)(void);                     /* G11 probe + the §8 dispatch gate      */
    lv_task_t*  main_task;                           /* task 0 while live (doc 2 §8/§5)       */
    int         main_done;                           /* task 0 ran to completion              */

#ifdef LV_CTX_UCONTEXT
    ucontext_t  sched_uc;
#else
    void*       sched_sp;                            /* scheduler context = the OS stack (§6) */
#endif
#if LV_TASK_ASAN
    void*       sched_fake;                          /* scheduler's fake-stack save slot      */
    const void* asan_sched_bottom; size_t asan_sched_size;  /* captured at first trampoline  */
#endif

    size_t      default_stack;                       /* per-consumer default, env-overridable */
    int         initialized;
    int         stats_on;                            /* LANG_TASK_STATS, cached at init: the
                                                        hwm mincore sample stays off the
                                                        completion path unless diagnosing */
    void*       altstack;                            /* sigaltstack storage (kept reachable)  */
    unsigned char* mincore_scratch; size_t mincore_pages;

    /* B2 (doc 06 §4): the id -> task registry. A growable open array of live
     * REGISTERED tasks (group children only — task 0 and plain loop-callback
     * tasks never enter, so main is unreachable by id). Registration happens
     * at spawn (not a switch/park path); removal at completion is a swap-pop.
     * Freed at teardown; on the main thread it lives for the process (same
     * rule as the pool). */
    struct { uint64_t id; lv_task_t* t; }* reg;
    uint32_t    reg_len, reg_cap;

    /* diagnostic counters (§8) */
    int64_t     n_spawned, n_completed, n_switches;
    int64_t     n_live, n_peak_live, n_parked, n_peak_parked;
    int64_t     n_mapped, n_pooled, drained_wakes;
    int64_t     n_cancelled, n_cancel_wakes, n_uncancellable_reports;   /* B2 */
    size_t      hwm_committed;
} lv_sched_t;

static LV_TLS lv_sched_t g_sched;
static long g_page;                 /* set once by the first lv_sched_init */

/* Per-consumer default usable stack (§4). Compiled programs use 1 MiB; the
 * interpreter legs need far bigger stacks (fat C++ frames — G9) and override via
 * -DLV_TASK_STACK_DEFAULT on their build target when doc 3 wires them. */
#ifndef LV_TASK_STACK_DEFAULT
#define LV_TASK_STACK_DEFAULT ((size_t)1u << 20)   /* 1 MiB usable */
#endif
#define LV_TASK_STACK_MIN     ((size_t)64u << 10)  /* 64 KiB floor (§4) */
#define LV_TASK_POOL_HOT      8                    /* §4 pool watermark */

#ifndef MAP_STACK
#define MAP_STACK 0
#endif

/* ====================== §5 Context-switch primitives ========================
 * S1 (fenced) with the .S files: the seeding below reproduces, byte for byte,
 * the saved-frame layout documented at the top of lv_ctx_x86_64.S /
 * lv_ctx_aarch64.S. Neither side may change without the other. */

static void lv_task_trampoline(void);

#ifdef LV_CTX_UCONTEXT
/* G1 fallback (build-time LV_CTX_UCONTEXT): correct, slow (sigprocmask syscall
 * per switch on glibc), acceptable as a bring-up/porting path only. The
 * trampoline reads g_sched.current, so makecontext's int-args limitation costs
 * nothing — no argument shim needed. */
static void ctx_to_task(lv_task_t* t)  { swapcontext(&g_sched.sched_uc, &t->uc); }
static void ctx_to_sched(lv_task_t* t) { swapcontext(&t->uc, &g_sched.sched_uc); }
static void task_seed(lv_task_t* t) {
    getcontext(&t->uc);
    t->uc.uc_stack.ss_sp   = t->stack_base + g_page;
    t->uc.uc_stack.ss_size = t->stack_size;
    t->uc.uc_link          = NULL;   /* the trampoline never returns; it switches out */
    makecontext(&t->uc, lv_task_trampoline, 0);
    t->sp = NULL;
}
#else
/* Primary path: the ~12-insn asm switch (G1). G4: lv_ctx_switch stays an opaque
 * out-of-line call in a .S file — never inline, never LTO-fold (rationale at the
 * top of lv_ctx_x86_64.S; G8's red-zone safety derives from the same non-leaf
 * call boundary). */
extern void lv_ctx_switch(void** save_sp, void* load_sp);

static void ctx_to_task(lv_task_t* t)  { lv_ctx_switch(&g_sched.sched_sp, t->sp); }
static void ctx_to_sched(lv_task_t* t) { lv_ctx_switch(&t->sp, g_sched.sched_sp); }

# if defined(__x86_64__)
extern void lv_ctx_fpenv(void* out8);   /* current fcw@+0 / mxcsr@+4 (lv_ctx_x86_64.S) */

/* S1 (fenced) — seed a fresh stack so the first switch-in `ret`s into the
 * trampoline. Layout mirrors lv_ctx_x86_64.S's saved frame exactly; the slot
 * above the trampoline's return target is a POISONED sentinel — the trampoline
 * never returns, and walking past it is a bug we want loud. The fp-env slot is
 * seeded with the SPAWNER's live environment (pump parity: dispatched callbacks
 * used to run under the caller's fp env). Alignment: the trampoline begins with
 * rsp ≡ 8 (mod 16) — a normal post-call frame, so its own calls stay aligned. */
static void task_seed(lv_task_t* t) {
    uint64_t* p = (uint64_t*)(void*)(t->stack_base + g_page + t->stack_size);
    *--p = 0xDEADBEEFDEADBEEFull;                      /* poison: tramp's "return addr"  */
    *--p = (uint64_t)(uintptr_t)&lv_task_trampoline;   /* the switch's ret target        */
    *--p = 0;                                          /* rbp (frame-chain terminator)   */
    *--p = 0;                                          /* rbx                            */
    *--p = 0;                                          /* r12                            */
    *--p = 0;                                          /* r13                            */
    *--p = 0;                                          /* r14                            */
    *--p = 0;                                          /* r15                            */
    --p; lv_ctx_fpenv(p);                              /* fcw@+0, mxcsr@+4               */
    t->sp = p;
}
# elif defined(__aarch64__)
/* S1 (fenced) — aarch64 seed: 160-byte frame per lv_ctx_aarch64.S. x30 (lr) is
 * the ret target; x29 = 0 terminates the frame chain; d8-d15/x19-x28 zeroed. */
static void task_seed(lv_task_t* t) {
    uint64_t* p = (uint64_t*)(void*)(t->stack_base + g_page + t->stack_size) - 20;
    memset(p, 0, 160);
    p[11] = (uint64_t)(uintptr_t)&lv_task_trampoline;  /* x30 slot (sp+88) */
    t->sp = p;
}
# else
#  error "lv_task: no context switch for this arch — port a lv_ctx_*.S or build with -DLV_CTX_UCONTEXT (doc 1 §5)"
# endif
#endif /* LV_CTX_UCONTEXT */

/* ============================== §4 Stacks ================================== */

static void lv_die(const char* msg) {
    ssize_t r = write(2, msg, strlen(msg)); (void)r;
    abort();
}

static lv_task_t* task_new(void) {
    size_t total = (size_t)g_page + g_sched.default_stack;
    uint8_t* base = (uint8_t*)mmap(NULL, total, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (base == MAP_FAILED) lv_die("lv_task: task stack mmap failed\n");
    /* guard page below the usable range: overflow faults instead of corrupting
     * the neighboring mapping. Lazy commit does the rest — RSS = touched pages. */
    if (mprotect(base, (size_t)g_page, PROT_NONE) != 0)
        lv_die("lv_task: guard-page mprotect failed\n");
    lv_task_t* t = (lv_task_t*)calloc(1, sizeof *t);
    if (!t) lv_die("lv_task: task struct alloc failed\n");
    t->stack_base = base;
    t->stack_size = g_sched.default_stack;
#if LV_TASK_ASAN
    t->asan_base = base + g_page; t->asan_size = t->stack_size;   /* S1 (fenced) */
#endif
#if LV_TASK_VALGRIND
    /* S1 (fenced): without registration valgrind reports wild "invalid write
     * below stack" on the first switch (deregister pairs at unmap — none in M0;
     * pooled stacks stay mapped for the thread's life; doc 2 §7 owns teardown). */
    if (RUNNING_ON_VALGRIND)
        t->vg_id = (unsigned)VALGRIND_STACK_REGISTER(base + g_page, base + total);
#endif
    g_sched.n_mapped++;
    return t;
}

/* hwm_committed (§8): the deepest single task stack observed — resident bytes
 * via mincore, sampled only at pool return (off every switch/park hot path).
 * This is the number LANG_TASK_STACK tuning wants; madvise below then drops the
 * pages deterministically (MADV_DONTNEED, not MADV_FREE — lazy accounting would
 * make the leak harness's +0B runs flaky). Gated on the cached stats flag so the
 * completion path pays no syscall when nobody is reading the line (§12's
 * spawn/complete >= 2M/s gate is measured with stats off). */
static void sample_committed_hwm(lv_task_t* t) {
    if (!g_sched.stats_on) return;
    size_t pages = t->stack_size / (size_t)g_page;
    if (!g_sched.mincore_scratch) {                    /* one-time warm-up alloc */
        g_sched.mincore_scratch = (unsigned char*)malloc(pages);
        g_sched.mincore_pages = g_sched.mincore_scratch ? pages : 0;
    }
    if (!g_sched.mincore_scratch || pages > g_sched.mincore_pages) return;
    if (mincore(t->stack_base + g_page, t->stack_size, g_sched.mincore_scratch) != 0) return;
    size_t resident = 0;
    for (size_t i = 0; i < pages; i++) resident += (size_t)(g_sched.mincore_scratch[i] & 1);
    size_t bytes = resident * (size_t)g_page;
    if (bytes > g_sched.hwm_committed) g_sched.hwm_committed = bytes;
}

/* S1 (fenced) — the DONE-task pool-return rule (§4): a DONE task's stack returns
 * to the pool only AFTER the scheduler has switched off it — you cannot free (or
 * hand out for reuse) the stack you are standing on. Called from switch_to's
 * epilogue, which by construction runs on the scheduler's own stack. G12's flip
 * side: only DONE tasks ever reach here — a PARKED task's frames ARE the
 * ownership record for every ARC ref they hold; no kill API exists (B2 unwinds). */
static void pool_return(lv_task_t* t) {
    assert(t->state == LV_TASK_DONE && "pool_return: only DONE tasks return their stack");
    sample_committed_hwm(t);
    t->state = LV_TASK_POOLED;
    t->next = g_sched.pool;
    g_sched.pool = t;
    g_sched.n_pooled++;
    if (g_sched.n_pooled > LV_TASK_POOL_HOT)   /* §4 watermark: hot 8 stay committed */
        madvise(t->stack_base + g_page, t->stack_size, MADV_DONTNEED);
}

/* ---- §4 overflow diagnosis: guard-page SIGSEGV handler ----
 * Async-signal-safe by construction: TLS list walk (the faulting thread owns its
 * lists — no locks), write(2), integer formatting by hand, then re-raise with
 * default disposition by returning to the faulting insn. Never attempt recovery
 * (the C stack is gone). Runs on a per-thread sigaltstack — the faulting thread's
 * normal stack is the one that's full; without an altstack the handler itself
 * would fault. */
static int guard_hit(const lv_task_t* t, const uint8_t* addr) {
    return t && addr >= t->stack_base && addr < t->stack_base + g_page;
}

static char* fmt_u64(char* p, unsigned long long v) {   /* appends decimal, returns new end */
    char tmp[20]; int n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = tmp[--n];
    return p;
}

static void lv_task_segv(int sig, siginfo_t* si, void* uctx) {
    (void)uctx;
    const uint8_t* addr = (const uint8_t*)si->si_addr;
    const lv_task_t* hit = NULL;
    if (guard_hit(g_sched.current, addr)) hit = g_sched.current;
    const lv_task_t* lists[3] = { g_sched.runq_head, g_sched.parked_head, g_sched.pool };
    for (int i = 0; !hit && i < 3; i++)
        for (const lv_task_t* t = lists[i]; t; t = t->next)
            if (guard_hit(t, addr)) { hit = t; break; }
    if (hit) {
        char buf[128]; char* p = buf;
        static const char m1[] = "task stack overflow (task #";
        static const char m2[] = ", stack ";
        static const char m3[] = "B; raise LANG_TASK_STACK)\n";
        memcpy(p, m1, sizeof m1 - 1); p += sizeof m1 - 1;
        p = fmt_u64(p, hit->id);
        memcpy(p, m2, sizeof m2 - 1); p += sizeof m2 - 1;
        p = fmt_u64(p, hit->stack_size);
        memcpy(p, m3, sizeof m3 - 1); p += sizeof m3 - 1;
        ssize_t r = write(2, buf, (size_t)(p - buf)); (void)r;
    }
    /* re-raise: restore default disposition and return — the faulting insn
     * re-executes and the process dies with the real signal (core intact). */
    signal(sig, SIG_DFL);
}

static void install_segv_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = lv_task_segv;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
}

/* =========================== §2 Setup and flags ============================= */

/* Tasks are the DEFAULT since the M5 flip (doc 5 §5, S5): LANG_PUMP=1 is the
 * escape hatch mapping to the old pump verbatim (same presence convention as
 * LANG_ARC_TRACE / LANG_THREAD_STATS, lv_runtime.c:187,958; read once for the
 * process lifetime). LANG_TASKS, the M1-M4 opt-in, is now a no-op — the flag
 * and the pump path itself are deleted together at M6.
 * Off => the engine keeps the pump and never calls any other entry here. */
int lv_tasks_enabled(void) {
    static int cached = -1;   /* -1 = not yet checked */
    if (cached < 0)
        cached = getenv("LANG_PUMP") ? 0 : 1;
    return cached;
}

/* Per-thread, idempotent (§2): stack sizing (LANG_TASK_STACK override, bytes,
 * page-rounded, min 64 KiB), the process-wide SIGSEGV handler (once), and this
 * thread's sigaltstack. Main thread: from lv_rt_init / engine start. Workers:
 * from the spawn trampoline beside lv_thread_ctx_init (doc 2 §7). */
void lv_sched_init(void) {
    if (g_sched.initialized) return;

    long pg = sysconf(_SC_PAGESIZE);
    if (pg <= 0) pg = 4096;
    g_page = pg;                          /* idempotent: same value every thread */

    size_t sz = LV_TASK_STACK_DEFAULT;
    const char* env = getenv("LANG_TASK_STACK");
    if (env && *env) {
        char* end = NULL;
        unsigned long long v = strtoull(env, &end, 10);
        if (end != env && v > 0)
            sz = (v < LV_TASK_STACK_MIN) ? LV_TASK_STACK_MIN : (size_t)v;
    }
    sz = (sz + (size_t)pg - 1) & ~((size_t)pg - 1);   /* page-round up */
    g_sched.default_stack = sz;

    /* overflow diagnosis (§4): handler once per process, altstack per thread
     * (mandatory — the faulting thread's normal stack is the one that's full). */
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, install_segv_handler);
    size_t sslen = (size_t)SIGSTKSZ;
    g_sched.altstack = malloc(sslen);     /* kept reachable for the thread's life */
    if (g_sched.altstack) {
        stack_t ss; ss.ss_sp = g_sched.altstack; ss.ss_size = sslen; ss.ss_flags = 0;
        sigaltstack(&ss, NULL);
    }
    g_sched.stats_on = getenv("LANG_TASK_STATS") ? 1 : 0;
    g_sched.initialized = 1;
}

/* B2 §8: the uncancellable-join report threshold (ms; 0 disables). Read at USE
 * time, not init — the same late-read rule as lv_task_stats_report, so a fork
 * child (selftest harness) can pin it after the parent already initialized.
 * The read sits on the report path only: a quiescent poll over an already-old
 * join park — never a switch/park hot path. */
static int64_t uncancellable_ms(void) {
    const char* s = getenv("LANG_TASK_UNCANCELLABLE_MS");
    if (!s || !*s) return 5000;
    return atoll(s);
}

/* Install the engine hooks once per thread before lv_sched_run (§2/§6). */
void lv_sched_hooks(int (*poll)(const void* obj), int (*loop_step)(void)) {
    g_sched.poll = poll;
    g_sched.loop_step = loop_step;
}

/* Install the pending-throw probe (G11). NULL leaves the park/spawn debug
 * assertion disabled — used by consumers that carry no throw flag. */
void lv_sched_throw_probe(int (*throwing)(void)) {
    g_sched.throwing = throwing;
}

/* The RUNNING task, or NULL when executing on the scheduler context (§2). */
lv_task_t* lv_task_self(void) {
    return g_sched.current;
}

/* ============================ Intrusive queues ============================== */

static void runq_append(lv_task_t* t) {
    t->next = NULL;
    if (g_sched.runq_tail) g_sched.runq_tail->next = t;
    else                   g_sched.runq_head = t;
    g_sched.runq_tail = t;
}

static lv_task_t* runq_pop(void) {
    lv_task_t* t = g_sched.runq_head;
    if (!t) return NULL;
    g_sched.runq_head = t->next;
    if (!g_sched.runq_head) g_sched.runq_tail = NULL;
    t->next = NULL;
    return t;
}

static void parked_append(lv_task_t* t) {
    t->next = NULL;
    if (g_sched.parked_tail) g_sched.parked_tail->next = t;
    else                     g_sched.parked_head = t;
    g_sched.parked_tail = t;
}

/* B2: unlink a specific task from the parked list (cancel-wake). O(parked),
 * same order of work as one poll pass; called from task context (cancelAll runs
 * on the closing task) — safe because everything here is thread-local (A-1)
 * and poll_parked only runs on the scheduler context, never concurrently. */
static int parked_remove(lv_task_t* t) {
    lv_task_t* prev = NULL;
    for (lv_task_t* it = g_sched.parked_head; it; prev = it, it = it->next) {
        if (it != t) continue;
        if (prev) prev->next = it->next; else g_sched.parked_head = it->next;
        if (g_sched.parked_tail == it) g_sched.parked_tail = prev;
        it->next = NULL;
        g_sched.n_parked--;
        return 1;
    }
    return 0;
}

/* ==================== B2 (doc 06 §4): the id -> task registry ===============
 * Ids are the spawn ordinal — monotone, never reused, so a stale id can only
 * miss (read as "done"), never alias a different task. Lookup is a linear scan:
 * the registry holds live GROUP CHILDREN only, small by construction. */

static void reg_add(lv_task_t* t) {
    if (g_sched.reg_len == g_sched.reg_cap) {
        uint32_t cap = g_sched.reg_cap ? g_sched.reg_cap * 2 : 8;
        void* p = realloc(g_sched.reg, cap * sizeof *g_sched.reg);
        if (!p) lv_die("lv_task: registry alloc failed\n");
        g_sched.reg = p;
        g_sched.reg_cap = cap;
    }
    g_sched.reg[g_sched.reg_len].id = t->id;
    g_sched.reg[g_sched.reg_len].t  = t;
    g_sched.reg_len++;
    t->registered = 1;
}

static lv_task_t* reg_find(uint64_t id) {
    for (uint32_t i = 0; i < g_sched.reg_len; i++)
        if (g_sched.reg[i].id == id) return g_sched.reg[i].t;
    return NULL;
}

static void reg_remove(lv_task_t* t) {
    for (uint32_t i = 0; i < g_sched.reg_len; i++) {
        if (g_sched.reg[i].t != t) continue;
        g_sched.reg[i] = g_sched.reg[--g_sched.reg_len];   /* swap-pop */
        break;
    }
    t->registered = 0;
}

/* ==================== S1 (fenced): trampoline + switches ====================
 * The ASan fiber-annotation protocol (G6): every departure brackets with
 * start_switch_fiber (saving THIS context's fake stack, naming the TARGET's
 * bounds) and every arrival completes with finish_switch_fiber (consuming the
 * fake stack saved when this context last left — NULL on a fresh stack).
 * Missing brackets => false "stack-use-after-return" storms in later legs that
 * look like engine bugs. A dying task passes NULL as the save slot so ASan
 * destroys its fake stack (the real stack returns to the pool and is
 * re-annotated fresh on reuse). */

/* Fresh-task entry. Reached by the first switch-in `ret`ing into it (asm path;
 * frame seeded by task_seed) or via makecontext (ucontext path). Runs fn(a,b),
 * marks DONE, and switches to the scheduler — it NEVER returns (the seeded
 * return-address slot above it is a poisoned sentinel; walking past it is a bug
 * we want loud). */
static void lv_task_trampoline(void) {
    lv_task_t* t = g_sched.current;
#if LV_TASK_ASAN
    /* First entry on this (possibly pool-reused) stack: complete the scheduler's
     * start_switch. The out-params capture the SCHEDULER stack's bounds — the
     * target every task->scheduler departure below must name. (Per-thread; the
     * recapture on every fresh task is harmless and keeps it correct.) */
    __sanitizer_finish_switch_fiber(NULL, &g_sched.asan_sched_bottom, &g_sched.asan_sched_size);
#endif
    t->fn(t->a, t->b);
    /* Completion with the pending-throw flag SET is a defined path (doc 2 §8),
     * not a G11 violation: it is the uncaught carrier. A worker body's raw
     * closure throws uncaught (the spawn try/catch lives on the SPAWNER side,
     * Resolver.cpp's watch callback — lv_worker_main's throwing check is the
     * normal fail path), and under C2 a loop callback's uncaught throw is
     * program-uncaught. The scheduler's gate in lv_sched_run stops dispatching
     * and returns with the flag intact so the engine's standard uncaught path
     * fires (lv_entry / lv_worker_main). What G11 forbids is PARKING with the
     * flag set — asserted in lv_task_park_on, guaranteed by lvrt_await's
     * throwing early-out. */
    if (t == g_sched.main_task) { g_sched.main_done = 1; g_sched.main_task = NULL; }
    if (t->registered) reg_remove(t);   /* B2: done leaves the id registry — a later
                                           cancel/join of this id reads "done" */
    t->state = LV_TASK_DONE;
    g_sched.n_completed++;
    g_sched.n_live--;
#if LV_TASK_ASAN
    /* final departure: NULL save slot => destroy this fiber's fake stack now. */
    __sanitizer_start_switch_fiber(NULL, g_sched.asan_sched_bottom, g_sched.asan_sched_size);
#endif
    g_sched.n_switches++;
    ctx_to_sched(t);
    __builtin_trap();   /* unreachable: a DONE task is never resumed */
}

/* Scheduler -> task. Returns when the task parks, yields, or completes; the
 * DONE epilogue (pool return) runs here — on the scheduler stack, per §4's
 * fenced rule — never on the stack being returned. */
static void switch_to(lv_task_t* t) {
    t->state = LV_TASK_RUNNING;
    g_sched.current = t;
    g_sched.n_switches++;
#if LV_TASK_ASAN
    __sanitizer_start_switch_fiber(&g_sched.sched_fake, t->asan_base, t->asan_size);
#endif
    ctx_to_task(t);
#if LV_TASK_ASAN
    __sanitizer_finish_switch_fiber(g_sched.sched_fake, NULL, NULL);
#endif
    g_sched.current = NULL;
    if (t->state == LV_TASK_DONE)
        pool_return(t);
}

/* Task -> scheduler (park and yield paths; the DONE path departs inside the
 * trampoline above with the destroy-fake variant). */
static void leave_to_sched(lv_task_t* t) {
    g_sched.n_switches++;
#if LV_TASK_ASAN
    __sanitizer_start_switch_fiber(&t->fake_stack, g_sched.asan_sched_bottom, g_sched.asan_sched_size);
#endif
    ctx_to_sched(t);
#if LV_TASK_ASAN
    __sanitizer_finish_switch_fiber(t->fake_stack, NULL, NULL);
#endif
}

/* ==================== §6/§7 spawn, yield, park, scheduler =================== */

void lv_task_spawn(lv_task_fn fn, void* a, void* b) {
    assert(g_sched.initialized && "lv_sched_init() must precede lv_task_spawn");
    assert((!g_sched.throwing || !g_sched.throwing()) &&
           "G11: spawn with a pending throw");
    lv_task_t* t;
    if (g_sched.pool) {                        /* pooled: no allocation (§3) */
        t = g_sched.pool;
        g_sched.pool = t->next;
        g_sched.n_pooled--;
    } else {
        t = task_new();
    }
    t->next = NULL;
    t->parked_on = NULL;
    t->parked_on2 = NULL;              /* B2: full park/cancel state reset — a pooled
                                          stack must never inherit a mark or shield */
    t->join_ids = NULL; t->join_n = 0;
    t->park_ns = 0;
    t->wake_reason = LV_PARK_SETTLED;
    t->woke_ix = 0;
    t->cancelled = 0;
    t->reported = 0;
    t->shield = 0;
    t->registered = 0;
    t->fn = fn; t->a = a; t->b = b;
    t->id = (uint64_t)++g_sched.n_spawned;
#if LV_TASK_ASAN
    t->fake_stack = NULL;                      /* fresh stack: first arrival passes NULL */
#endif
    task_seed(t);
    t->state = LV_TASK_RUNNABLE;
    runq_append(t);
    g_sched.n_live++;
    if (g_sched.n_live > g_sched.n_peak_live) g_sched.n_peak_live = g_sched.n_live;
}

void lv_task_yield(void) {
    lv_task_t* t = g_sched.current;
    assert(t != NULL && "lv_task_yield: no current task (scheduler context)");
    t->state = LV_TASK_RUNNABLE;
    runq_append(t);            /* tail — FIFO fairness, no starvation among runnables (§6) */
    leave_to_sched(t);
}

/* ---- B2 park-shape helpers (doc 06 §5) — every access to the park keys
 * (parked_on / parked_on2 / join_ids) goes through these (the §8 reviewer
 * rule), so a future general-N park is a local change. A park is
 * promise-shaped (parked_on set, parked_on2 optional) or join-shaped
 * (join_ids set); never both. ---- */

static int64_t lv_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* Is every id in a join set done? (unknown id == done — ids never reused). */
static int join_all_done(const uint64_t* ids, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        if (reg_find(ids[i])) return 0;
    return 1;
}

/* Does any park key of `t` report settled RIGHT NOW? Promise shape: poll each
 * key (G15: the hook reads bool immediates only), recording woke_ix so the
 * wake carries WHICH key fired (first-declared-wins on a simultaneous settle).
 * Join shape: the all-done registry check. Also the §8 settle-wins probe
 * lv_task_cancel uses: a settled park is delivered as a settle, never a cancel. */
static int park_keys_settled(lv_task_t* t) {
    if (t->join_ids)
        return join_all_done(t->join_ids, t->join_n);
    if (!g_sched.poll) return 0;
    if (t->parked_on && g_sched.poll(t->parked_on)) { t->woke_ix = 0; return 1; }
    if (t->parked_on2 && g_sched.poll(t->parked_on2)) { t->woke_ix = 1; return 1; }
    return 0;
}

static void park_keys_clear(lv_task_t* t) {
    t->parked_on = NULL;
    t->parked_on2 = NULL;
    t->join_ids = NULL; t->join_n = 0;
    t->park_ns = 0;
    t->reported = 0;
}

/* The one park implementation behind lv_task_park_on / _any2 / _join. Keys are
 * BORROWED: the parked task's own frame holds the awaited references/buffer,
 * so nothing here can dangle while parked (§7/G12).
 *
 * B2 §2 delivery rule, both halves:
 *   - park ENTRY: a pending unshielded mark is consumed here, before parking —
 *     "the mark takes effect at its next park". A shielded mark holds.
 *   - while PARKED: lv_task_cancel dequeues+wakes (unless shielded, or a key
 *     already polls settled — settle wins that park; the mark holds for the
 *     next one).
 * The mark is CONSUMED at delivery (cancelled=0): a task that catches
 * CancelledException may legitimately park again (a refuser — §8's close()
 * report is the audit), and close()'s bounded re-cancel re-marks it. */
static int park_common(lv_task_t* t) {
    assert(t != NULL && "lv_task_park: parking the scheduler is a fatal misuse");
    assert((!g_sched.throwing || !g_sched.throwing()) &&
           "G11: park with a pending throw");   /* pump parity: Eval.cpp:1077 checked first */
    if (t->cancelled && !t->shield) {
        t->cancelled = 0;                       /* consumed at delivery */
        park_keys_clear(t);
        g_sched.n_cancel_wakes++;
        return LV_PARK_CANCELLED;
    }
    t->wake_reason = LV_PARK_SETTLED;
    t->woke_ix = 0;
    t->state = LV_TASK_PARKED;
    parked_append(t);
    g_sched.n_parked++;
    if (g_sched.n_parked > g_sched.n_peak_parked) g_sched.n_peak_parked = g_sched.n_parked;
    leave_to_sched(t);
    /* resumed: poll_parked (settle), drain_wake_all (G2), or lv_task_cancel */
    int r = t->wake_reason;
    if (r == LV_PARK_CANCELLED) t->cancelled = 0;   /* consumed at delivery */
    park_keys_clear(t);
    return r;
}

int lv_task_park_on(const void* obj) {
    lv_task_t* t = g_sched.current;
    assert(t != NULL && "lv_task_park_on: parking the scheduler is a fatal misuse");
    t->parked_on = obj;
    return park_common(t);
}

int lv_task_park_any2(const void* a, const void* b, int* which) {
    lv_task_t* t = g_sched.current;
    assert(t != NULL && "lv_task_park_any2: parking the scheduler is a fatal misuse");
    t->parked_on = a;
    t->parked_on2 = b;
    int r = park_common(t);
    if (r == LV_PARK_SETTLED && which) *which = t->woke_ix;
    return r;
}

int lv_task_park_join(const uint64_t* ids, int n) {
    lv_task_t* t = g_sched.current;
    assert(t != NULL && "lv_task_park_join: parking the scheduler is a fatal misuse");
    if (n <= 0 || join_all_done(ids, (uint32_t)n))
        return LV_PARK_SETTLED;                 /* fast path: nothing to wait for —
                                                   no scheduling point, like a ready
                                                   await (doc 5 C4) */
    t->join_ids = ids;
    t->join_n = (uint32_t)n;
    t->park_ns = lv_now_ns();                   /* §8 report timing (report only,
                                                   never wake correctness) */
    return park_common(t);
}

/* B2 §8: the uncancellable report — a join that has waited past the threshold
 * with a cancel-marked child still live is REPORTED once per park (loud, not
 * hung; the join still completes when the refuser parks or returns). Runs on
 * the scheduler during a poll pass, so a compute-bound refuser that never
 * yields cannot be reported — that is the honest preemption limit (§9). */
static void join_report_check(lv_task_t* t) {
    if (t->reported || !t->park_ns) return;
    int64_t ms = uncancellable_ms();
    if (ms <= 0 || lv_now_ns() - t->park_ns < ms * 1000000) return;
    int64_t stuck = 0;
    for (uint32_t i = 0; i < t->join_n; i++) {
        lv_task_t* c = reg_find(t->join_ids[i]);
        if (c && c->cancelled) stuck++;
    }
    if (!stuck) return;    /* children merely slow, not refusing a cancel */
    t->reported = 1;
    g_sched.n_uncancellable_reports++;
    fprintf(stderr, "[tasks] uncancellable=%lld: group join waiting on cancelled task(s)\n",
            (long long)stuck);
}

/* Walk the parked list in park order polling each task's key shape; settled
 * tasks move to the runq (FIFO position = settle-scan order — deterministic
 * because the parked list and the poll points are). O(parked) per quiescent
 * cycle (G10) — the settle-hook redesign is documented (overview §9) and
 * UNBUILT until a profile demands it. The poll hook owns G15. */
static int poll_parked(void) {
    int woke = 0;
    lv_task_t* prev = NULL;
    lv_task_t* t = g_sched.parked_head;
    while (t) {
        lv_task_t* next = t->next;
        if (park_keys_settled(t)) {            /* settled OR failed — both wake (§2 bitmask) */
            if (prev) prev->next = next; else g_sched.parked_head = next;
            if (g_sched.parked_tail == t) g_sched.parked_tail = prev;
            g_sched.n_parked--;
            t->wake_reason = LV_PARK_SETTLED;
            t->state = LV_TASK_RUNNABLE;
            runq_append(t);
            woke = 1;
        } else {
            if (t->join_ids) join_report_check(t);   /* B2 §8: loud, not hung */
            prev = t;
        }
        t = next;
    }
    return woke;
}

/* G2's mechanism: every parked task wakes with wake_reason=DRAINED, in park
 * order; the ENGINE decides what that means (parity: silent value read; M5:
 * throw — C3). Cannot livelock: each park gets at most one drain wake per park
 * — a program that re-parks forever on a dead promise loops under the pump too
 * (doc 5 §3, bug-for-bug equivalence). `drained_wakes` rides the stats line as
 * the G2 smell the harness can grep long before M5 makes it throw. */
static void drain_wake_all(void) {
    lv_task_t* t = g_sched.parked_head;
    while (t) {
        lv_task_t* next = t->next;
        t->wake_reason = LV_PARK_DRAINED;
        t->state = LV_TASK_RUNNABLE;
        runq_append(t);
        g_sched.drained_wakes++;
        t = next;
    }
    g_sched.parked_head = g_sched.parked_tail = NULL;
    g_sched.n_parked = 0;
}

/* ==================== B2 (doc 06 §2/§4): cancel, shield, register =========== */

uint64_t lv_task_spawn_registered(lv_task_fn fn, void* a, void* b) {
    lv_task_spawn(fn, a, b);
    lv_task_t* t = g_sched.runq_tail;          /* just enqueued (the main_task idiom) */
    reg_add(t);
    return t->id;
}

int lv_task_cancel(uint64_t id) {
    lv_task_t* t = reg_find(id);
    if (!t) return 0;                           /* unknown/done: idempotent no-op (§8) */
    if (t->cancelled) return 1;                 /* double-cancel: no-op (§8) */
    t->cancelled = 1;
    g_sched.n_cancelled++;
    if (t->state == LV_TASK_PARKED && !t->shield) {
        /* §8 settle-wins: if a park key already polls settled, leave the task
         * parked — the normal poll delivers the settle; the mark holds for its
         * next park. Otherwise dequeue and wake it cancelled. */
        if (!park_keys_settled(t) && parked_remove(t)) {
            t->wake_reason = LV_PARK_CANCELLED;
            t->state = LV_TASK_RUNNABLE;
            runq_append(t);
            g_sched.n_cancel_wakes++;
        }
    }
    /* RUNNING/RUNNABLE (or shielded-parked): the mark is consumed at the next
     * (unshielded) park entry — a running task is never preempted (§2). */
    return 1;
}

void lv_task_shield(int delta) {
    lv_task_t* t = g_sched.current;
    if (!t) return;                             /* scheduler context: no-op (native guard) */
    if (delta > 0) {
        t->shield = (uint16_t)(t->shield + delta);
    } else {
        assert(t->shield >= (uint16_t)(-delta) && "lv_task_shield: mask underflow");
        t->shield = (uint16_t)(t->shield + delta);
    }
}

/* The carrier loop (§6). The scheduler context is the thread's original OS
 * stack; task 0 (the program top-level) is an ordinary task. Replaces the
 * engine's pump/drain sites; returns when the thread's program lifetime ends. */
void lv_sched_run(lv_task_fn main_fn, void* a, void* b) {
    assert(g_sched.initialized && "lv_sched_init() must precede lv_sched_run");
    assert(g_sched.current == NULL && "lv_sched_run re-entered from a task");
    lv_task_spawn(main_fn, a, b);                    /* task 0 */
    g_sched.main_task = g_sched.runq_tail;           /* just enqueued (doc 2 §8/§5) */
    g_sched.main_done = 0;
    for (;;) {
        lv_task_t* t;
        while ((t = runq_pop()) != NULL) {           /* completion order — C1 */
            switch_to(t);                            /* returns on park/done/yield */
            /* §8 gate (doc 2): a task returned with the pending-throw flag set
             * — the uncaught carrier. Stop dispatching (the pump-drain's
             * notThrowing gate, LlvmGen.cpp:705) and return with the flag
             * intact; the engine's entry (lv_entry / lv_worker_main) owns the
             * report. Parked tasks stay pinned (G12 — B2 unwinds, never free). */
            if (g_sched.throwing && g_sched.throwing()) {
                lv_task_stats_report();
                return;
            }
        }
        if (poll_parked())                           /* settle-scan: parked, in order */
            continue;                                /* something became runnable    */
        if (!g_sched.loop_step || !g_sched.loop_step()) {
            /* loop had NO work: */
            if (!g_sched.parked_head) break;         /* quiescent: program over      */
            drain_wake_all();                        /* G2: wake parked, drained=1,  */
            continue;                                /*     park order; then resched */
        }
    }
    lv_task_stats_report();
}

int lv_sched_main_completed(void) {
    return g_sched.main_done;
}

/* doc 2 §7 — worker-thread teardown (see lv_task.h). Pooled stacks only: a
 * task parked at the §8 gate exit still pins its stack and every ARC ref in
 * its frames (G12); freeing it here would be the exact corruption class the
 * fenced pool-return rule exists to prevent. */
void lv_sched_teardown(void) {
    assert(g_sched.current == NULL && "lv_sched_teardown from inside a task");
    lv_task_t* t = g_sched.pool;
    while (t) {
        lv_task_t* next = t->next;
#if LV_TASK_VALGRIND
        if (RUNNING_ON_VALGRIND) VALGRIND_STACK_DEREGISTER(t->vg_id);
#endif
        munmap(t->stack_base, (size_t)g_page + t->stack_size);
        g_sched.n_mapped--;
        g_sched.n_pooled--;
        free(t);
        t = next;
    }
    g_sched.pool = NULL;
    free(g_sched.reg);                          /* B2: registry array (live entries, if
                                                   any, are pinned parked tasks — their
                                                   structs stay per G12; only the index
                                                   array is freed) */
    g_sched.reg = NULL; g_sched.reg_len = g_sched.reg_cap = 0;
    free(g_sched.mincore_scratch);
    g_sched.mincore_scratch = NULL; g_sched.mincore_pages = 0;
    if (g_sched.altstack) {   /* disarm BEFORE freeing the storage it names */
        stack_t ss; ss.ss_sp = NULL; ss.ss_size = 0; ss.ss_flags = SS_DISABLE;
        sigaltstack(&ss, NULL);
        free(g_sched.altstack);
        g_sched.altstack = NULL;
    }
}

/* ========================= §8 Stats and diagnostics ========================= */

void lv_task_stats_report(void) {
    if (!getenv("LANG_TASK_STATS")) return;   /* mirrors LANG_THREAD_STATS (lv_runtime.c:958) */
    fprintf(stderr,
        "[tasks] spawned=%lld completed=%lld switches=%lld peak_live=%lld peak_parked=%lld\n"
        "        stacks: mapped=%lld pooled=%lld hwm_committed=%.1fMiB drained_wakes=%lld\n"
        "        cancel: marked=%lld delivered=%lld uncancellable_reports=%lld\n",
        (long long)g_sched.n_spawned, (long long)g_sched.n_completed,
        (long long)g_sched.n_switches, (long long)g_sched.n_peak_live,
        (long long)g_sched.n_peak_parked,
        (long long)g_sched.n_mapped, (long long)g_sched.n_pooled,
        (double)g_sched.hwm_committed / (1024.0 * 1024.0),
        (long long)g_sched.drained_wakes,
        (long long)g_sched.n_cancelled, (long long)g_sched.n_cancel_wakes,
        (long long)g_sched.n_uncancellable_reports);
}

#endif /* !_WIN32 */
