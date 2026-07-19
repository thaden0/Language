/* Track W — the wasm task substrate (W-M2, techdesign-04-async-jspi.md).
 *
 * Realizes the lv_task.h interface over host imports + JSPI, replacing
 * lv_task_stub.c in the wasm archive (doc 04 §2). One model, two
 * realizations: natively a park is lv_ctx_switch under lv_task.c's per-thread
 * scheduler; here the ENGINE performs the identical operation — a
 * WebAssembly.Suspending import suspends the whole current activation on a JS
 * promise, and the host resumes it in completion order. The scheduler moves
 * into the host event loop (doc 04 §1's inversion): there is no carrier loop
 * on this target — every runnable unit enters wasm as its own
 * WebAssembly.promising export call, and quiescence detection is the host's
 * turn-end scan (§3.3, realized by lv_host.js calling lv_park_poll below).
 *
 * THE SHADOW-STACK DISCIPLINE (the one thing §3.1's "the browser supplies the
 * task stacks" did not anticipate): JSPI suspends the ENGINE's wasm stack —
 * locals, call frames, the wasm value stack — but clang-compiled C also keeps
 * address-taken locals on a SHADOW STACK in linear memory, addressed through
 * the module global __stack_pointer. That global is shared module state: it is
 * NOT saved or restored by suspend/resume. Two live activations on one shadow
 * stack would clobber each other's frames, so this file enforces:
 *
 *   1. every dispatched activation (spawned task, timer callback, probe) runs
 *      its payload on its OWN pooled stack region (lv_stack_take below),
 *      switched to at activation entry — the exact analog of lv_task.c's
 *      mmap'd per-task stacks, with malloc standing in for mmap;
 *   2. every suspension point (the park/step/yield/probe imports) saves
 *      __stack_pointer and g_current into wasm locals before the call and
 *      restores both immediately after it returns — a resumed activation must
 *      re-establish its own stack position before calling anything;
 *   3. brief host-entry work that CANNOT suspend (the settle-scan's poll, the
 *      activation wrappers' take/give/throw-gate bookkeeping) runs on one
 *      static service stack — safe precisely because nothing on it ever lives
 *      across a suspension;
 *   4. the main activation (lv_entry_main) keeps the wasm-ld default stack:
 *      it is entered exactly once, rules 1-3 keep everyone else off it, and
 *      it must be the FIRST export entered — __stack_pointer is pristine
 *      only until some activation suspends (lv_host.js's run() enters main
 *      synchronously before anything else can).
 *
 * The sp accessors use the wasi-libc idiom (.globaltype + global.get/set) —
 * this is data movement, not a register-context switch; doc 02 §6's "no .S
 * files" stance stands: suspension itself is entirely the engine's.
 */
#ifndef __wasm__
#error "lv_task_wasm.c is the wasm32 leg of the task substrate (see lv_task.c)"
#endif

#include "lv_task.h"
#include "lv_abi.h"    /* LvValue, lvrt_retain/release, lvrt_uncaught, lv_rt_exit_code */
#include "lv_plat.h"   /* lv_plat_write, lv_plat_exit */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ============================ imports (module "lv") ========================
 * The suspending ones are wrapped in WebAssembly.Suspending by lv_host.js —
 * the ONLY place import names are spelled on the host side (doc 03 §3).
 *   park(task)  -> wake code: the host stores the resolver keyed by the task
 *                  record; the turn-end scan (lv_park_poll) or a cancel wake
 *                  (lv.wake) or the loud drain (§3.4) resolves it.
 *   step()      -> suspend until the host completes one timer dispatch (or
 *                  return immediately when nothing is armed) — the wasm
 *                  realization of "one loop batch", consumed by
 *                  lv_loop_wasm.c for lv_main's emitted tail drain.
 *   yield()     -> resolve on a fresh microtask: FIFO fairness among queued
 *                  activations, the runq-append analog.
 *   spawn(task) -> host queues a microtask that enters lv_task_run(task).
 *   wake(task,code) -> resolve a parked task NOW (cancel delivery).
 *   probe_fetch()   -> §6's fetch-shaped Suspending probe (data: URI). */
__attribute__((import_module("lv"), import_name("park")))
extern int32_t lv_import_park(lv_task_t* t);
__attribute__((import_module("lv"), import_name("step")))
extern int32_t lv_import_step(void);
__attribute__((import_module("lv"), import_name("yield")))
extern int32_t lv_import_yield(void);
__attribute__((import_module("lv"), import_name("spawn")))
extern void lv_import_spawn(lv_task_t* t);
__attribute__((import_module("lv"), import_name("wake")))
extern void lv_import_wake(lv_task_t* t, int32_t code);
__attribute__((import_module("lv"), import_name("probe_fetch")))
extern int32_t lv_import_probe_fetch(void);

/* ========================= shadow-stack accessors ========================= */
__asm__(".globaltype __stack_pointer, i32\n");

static inline void* lv_sp_get(void) {
    void* p;
    __asm__ volatile("global.get __stack_pointer\n"
                     "local.set %0" : "=r"(p));
    return p;
}
static inline void lv_sp_set(void* p) {
    __asm__ volatile("local.get %0\n"
                     "global.set __stack_pointer" : : "r"(p));
}

/* ============================== task records ==============================
 * The wasm lv_task_t: no stack base, no context slot — the engine owns both.
 * Park keys mirror lv_task.c §7 exactly (promise-shaped: parked_on +
 * optional parked_on2; join-shaped: join_ids/join_n; never both). */
enum { LV_TASK_RUNNABLE = 1, LV_TASK_RUNNING, LV_TASK_PARKED, LV_TASK_DONE };

struct lv_task {
    uint64_t        id;
    lv_task_fn      fn; void* a; void* b;
    const void*     parked_on;     /* BORROWED per G12 + retained across the
                                      suspension per doc 04 §3.3's ARC note   */
    const void*     parked_on2;
    const uint64_t* join_ids;      /* BORROWED: the parking frame owns it     */
    uint32_t        join_n;
    int             woke_ix;       /* any2: which key settled (scan-time)     */
    int             state;
    uint16_t        shield;        /* B2 §8 cancel mask                       */
    uint8_t         cancelled;     /* B2 mark, consumed at delivery           */
    uint8_t         registered;
};

static lv_task_t  g_main;             /* task 0 — lv_sched_run's inline body  */
static lv_task_t* g_current;          /* re-established after every resume    */
static int        g_main_done;
static int        g_initialized;
static uint64_t   g_nspawned;

/* engine hooks (installed by lv_entry.c before lv_sched_run) */
static int (*g_poll)(const void*);
static int (*g_loop_step)(void);      /* stored for interface parity; the host
                                         owns the loop on this target         */
static int (*g_throwing)(void);

static void lv_fatal(const char* msg) {
    lv_plat_write(2, msg, (int64_t)strlen(msg));
    lv_plat_write(2, "\n", 1);
    __builtin_trap();
}

/* ====================== pooled activation stacks (rule 1) ==================
 * 256 KiB per region (native pools ~1 MiB mmaps; linear memory is dearer and
 * the corpus' deepest wasm frames are far below this). Freed regions are
 * pooled, exactly lv_task.c §3's reuse discipline. LANG_TASK_* env overrides
 * do not exist here — getenv is pinned to NULL on this target (doc 03). */
#define LV_WASM_TASK_STACK  (256 * 1024)
#define LV_WASM_SVC_STACK   (32 * 1024)

typedef struct LvStackNode { struct LvStackNode* next; } LvStackNode;
static LvStackNode* g_stack_pool;

static _Alignas(16) char g_svc_stack[LV_WASM_SVC_STACK];
#define LV_SVC_TOP ((void*)(g_svc_stack + sizeof g_svc_stack))

static void* lv_stack_take(void) {
    if (g_stack_pool) {
        void* p = g_stack_pool;
        g_stack_pool = g_stack_pool->next;
        return p;
    }
    void* p = malloc(LV_WASM_TASK_STACK);
    if (!p) lv_fatal("task stack alloc failed");
    return p;
}
static void lv_stack_give(void* p) {
    LvStackNode* n = (LvStackNode*)p;
    n->next = g_stack_pool;
    g_stack_pool = n;
}
/* sp must be 16-aligned; malloc only guarantees 8 on wasm32. */
static void* lv_stack_top(void* base) {
    return (void*)(((uintptr_t)base + LV_WASM_TASK_STACK) & ~(uintptr_t)15);
}

/* ========================= park keys (lv_task.c §7 mirror) ================= */

static void lv_key_retain(const void* k) {
    if (!k) return;
    LvValue v; v.tag = LV_OBJ; v.payload = (int64_t)(intptr_t)k;
    lvrt_retain(&v);
}
static void lv_key_release(const void* k) {
    if (!k) return;
    LvValue v; v.tag = LV_OBJ; v.payload = (int64_t)(intptr_t)k;
    lvrt_release(&v);
}
static void lv_park_keys_clear(lv_task_t* t) {
    t->parked_on = NULL;
    t->parked_on2 = NULL;
    t->join_ids = NULL; t->join_n = 0;
}

/* ====================== B2 registry (id -> record, §4) =====================
 * Ids never reused; done leaves the registry, so unknown == done. */
typedef struct { uint64_t id; lv_task_t* t; } LvRegEnt;
static LvRegEnt* g_reg;
static uint32_t  g_reg_len, g_reg_cap;

static void lv_reg_add(lv_task_t* t) {
    if (g_reg_len == g_reg_cap) {
        g_reg_cap = g_reg_cap ? g_reg_cap * 2 : 16;
        g_reg = realloc(g_reg, g_reg_cap * sizeof *g_reg);
        if (!g_reg) lv_fatal("task registry alloc failed");
    }
    g_reg[g_reg_len].id = t->id;
    g_reg[g_reg_len].t  = t;
    g_reg_len++;
    t->registered = 1;
}
static lv_task_t* lv_reg_find(uint64_t id) {
    for (uint32_t i = 0; i < g_reg_len; i++)
        if (g_reg[i].id == id) return g_reg[i].t;
    return NULL;
}
static void lv_reg_remove(lv_task_t* t) {
    for (uint32_t i = 0; i < g_reg_len; i++) {
        if (g_reg[i].t != t) continue;
        g_reg[i] = g_reg[--g_reg_len];          /* swap-pop */
        break;
    }
    t->registered = 0;
}

static int lv_join_all_done(const uint64_t* ids, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        if (lv_reg_find(ids[i])) return 0;
    return 1;
}

/* Does any park key of `t` report settled RIGHT NOW? Verbatim lv_task.c
 * park_keys_settled: promise keys through the poll hook (G15: bool immediates
 * only, ARC-silent), woke_ix first-declared-wins; join shape via the
 * registry. Doubles as the §8 settle-wins probe in lv_task_cancel. */
static int lv_park_keys_settled(lv_task_t* t) {
    if (t->join_ids)
        return lv_join_all_done(t->join_ids, t->join_n);
    if (!g_poll) return 0;
    if (t->parked_on && g_poll(t->parked_on)) { t->woke_ix = 0; return 1; }
    if (t->parked_on2 && g_poll(t->parked_on2)) { t->woke_ix = 1; return 1; }
    return 0;
}

/* The settle-scan export (doc 04 §3.3): the host calls this for every parked
 * entry after each turn back into JS; nonzero => resolve that park SETTLED.
 * O(parked) per turn, same complexity stance as native poll_parked; the
 * settle-hook import stays the documented UNBUILT escape hatch. As-built
 * note: the design text scanned promise pointers via an exported
 * lvrt_promise_state; parking the TASK RECORD instead lets the one scan
 * cover all three park shapes (promise / any2 / join), so this export
 * generalizes that call rather than adding link flags for it.
 *
 * Runs on the service stack (rule 3): the host may call it while the last
 * activation to run sits suspended arbitrarily deep in its own region, so
 * polling at "wherever sp happens to be" could underflow that region. Never
 * suspends — G15 keeps the poll a pure read. */
__attribute__((noinline))
static int32_t lv_park_poll_svc(lv_task_t* t) {
    return lv_park_keys_settled(t) ? 1 : 0;
}
__attribute__((export_name("lv_park_poll")))
int32_t lv_park_poll(lv_task_t* t) {
    void* saved = lv_sp_get();
    lv_sp_set(LV_SVC_TOP);
    int32_t r = lv_park_poll_svc(t);
    lv_sp_set(saved);
    return r;
}

/* ============================ park (§3.1) =================================
 * B2 §2 delivery, both halves, verbatim from lv_task.c park_common:
 * a pending unshielded mark is consumed at park ENTRY; while parked,
 * lv_task_cancel wakes with CANCELLED via lv.wake (unless shielded or a key
 * already polls settled — settle wins). The mark is CONSUMED at delivery. */
static int lv_park_common(lv_task_t* t) {
    if (g_throwing && g_throwing())
        lv_fatal("G11: park with a pending throw");   /* lvrt_await's early-out
                                                         proves this unreachable */
    if (t->cancelled && !t->shield) {
        t->cancelled = 0;                             /* consumed at delivery */
        lv_park_keys_clear(t);
        return LV_PARK_CANCELLED;
    }
    t->woke_ix = 0;
    t->state = LV_TASK_PARKED;
    /* doc 04 §3.3 ARC note: retain the promise keys on the C side across the
     * suspension — the JS map holds a raw pointer, never lifetime. (The
     * suspended awaiting frame also holds its reference, as on native; this
     * retain is the belt to that brace.) Join ids stay borrowed: the parking
     * frame owns the buffer (G12) and that frame is exactly what the engine
     * keeps alive while suspended. */
    const void* k1 = t->parked_on;
    const void* k2 = t->parked_on2;
    lv_key_retain(k1);
    lv_key_retain(k2);

    void* sp = lv_sp_get();                           /* rule 2 */
    int r = lv_import_park(t);                        /* JSPI: activation parks */
    lv_sp_set(sp);
    g_current = t;

    lv_key_release(k1);
    lv_key_release(k2);
    if (r == LV_PARK_CANCELLED) t->cancelled = 0;     /* consumed at delivery */
    t->state = LV_TASK_RUNNING;
    lv_park_keys_clear(t);                            /* woke_ix survives (any2) */
    return r;
}

int lv_task_park_on(const void* obj) {
    lv_task_t* t = g_current;
    if (!t) lv_fatal("lv_task_park_on: parking outside any activation");
    t->parked_on = obj;
    return lv_park_common(t);
}

int lv_task_park_any2(const void* a, const void* b, int* which) {
    lv_task_t* t = g_current;
    if (!t) lv_fatal("lv_task_park_any2: parking outside any activation");
    t->parked_on = a;
    t->parked_on2 = b;
    int r = lv_park_common(t);
    if (r == LV_PARK_SETTLED && which) *which = t->woke_ix;
    return r;
}

int lv_task_park_join(const uint64_t* ids, int n) {
    lv_task_t* t = g_current;
    if (!t) lv_fatal("lv_task_park_join: parking outside any activation");
    if (n <= 0 || lv_join_all_done(ids, (uint32_t)n))
        return LV_PARK_SETTLED;                       /* ready join = ready await:
                                                         no scheduling point (C4) */
    t->join_ids = ids;
    t->join_n = (uint32_t)n;
    /* §8's uncancellable report is env-gated (LANG_TASK_UNCANCELLABLE_MS) and
     * getenv is pinned NULL on this target, so the report leg is dead here by
     * the same rule that disables it natively with the var unset. */
    return lv_park_common(t);
}

/* ===================== spawn / yield / cancel / shield ===================== */

static lv_task_t* lv_task_spawn_rec(lv_task_fn fn, void* a, void* b) {
    if (!g_initialized) lv_fatal("lv_sched_init() must precede lv_task_spawn");
    if (g_throwing && g_throwing())
        lv_fatal("G11: spawn with a pending throw");
    lv_task_t* t = calloc(1, sizeof *t);
    if (!t) lv_fatal("task alloc failed");
    t->fn = fn; t->a = a; t->b = b;
    t->id = ++g_nspawned;
    t->state = LV_TASK_RUNNABLE;
    return t;
}

void lv_task_spawn(lv_task_fn fn, void* a, void* b) {
    /* FIFO: the host queues one microtask per spawn; microtasks outrank timer
     * macrotasks, preserving "the runq drains before the loop steps". */
    lv_import_spawn(lv_task_spawn_rec(fn, a, b));
}

uint64_t lv_task_spawn_registered(lv_task_fn fn, void* a, void* b) {
    lv_task_t* t = lv_task_spawn_rec(fn, a, b);
    lv_reg_add(t);                       /* like lv_task.c: register at spawn */
    lv_import_spawn(t);
    return t->id;
}

int lv_task_cancel(uint64_t id) {
    lv_task_t* t = lv_reg_find(id);
    if (!t) return 0;                    /* unknown/done: idempotent no-op (§8) */
    if (t->cancelled) return 1;          /* double-cancel: no-op (§8)           */
    t->cancelled = 1;
    if (t->state == LV_TASK_PARKED && !t->shield) {
        /* §8 settle-wins: a park whose key already polls settled is delivered
         * as a settle by the next scan; the mark holds for its next park. */
        if (!lv_park_keys_settled(t))
            lv_import_wake(t, LV_PARK_CANCELLED);
    }
    return 1;
}

void lv_task_shield(int delta) {
    lv_task_t* t = g_current;
    if (!t) return;                      /* no current activation: no-op */
    t->shield = (uint16_t)(t->shield + delta);
}

void lv_task_yield(void) {
    lv_task_t* t = g_current;
    if (!t) lv_fatal("lv_task_yield: no current activation");
    void* sp = lv_sp_get();              /* rule 2 */
    (void)lv_import_yield();
    lv_sp_set(sp);
    g_current = t;
}

/* ================== dispatched activations (rule 1 wrappers) ==============
 * The export/enter shape shared by spawned tasks, timer dispatches
 * (lv_loop_wasm.c rides lv_wasm_dispatch_run), and the probe. The wrapper
 * functions keep PURE-LOCAL frames (no address-taken locals): they run at
 * whatever __stack_pointer value the host left, and must not write shadow
 * memory there. Setup/teardown runs on the service stack; the payload —
 * everything that may suspend — runs on its own pooled region. */

__attribute__((noinline))
static void lv_activation_body(lv_task_t* t) {
    g_current = t;
    t->state = LV_TASK_RUNNING;
    t->fn(t->a, t->b);
    /* Completion with the pending-throw flag SET is the defined uncaught
     * carrier (doc 2 §8) — the gate below owns it, on the service stack. */
    if (t->registered) lv_reg_remove(t);
    t->state = LV_TASK_DONE;
    g_current = NULL;
}

/* doc 2 §8's throw gate, host-loop edition: a task/callback body returned
 * with the pending-throw flag set. Stop dispatching — on this target that is
 * "stop the host", i.e. exit with the runtime's exit code, reporting first
 * exactly when the native entry would (task 0 incomplete; a completed task 0
 * already ran its own uncaught/meter tail — C2's contract). Never returns
 * when it fires: lv_plat_exit throws the host's exit sentinel. */
__attribute__((noinline))
static void lv_throw_gate_svc(void) {
    if (!(g_throwing && g_throwing())) return;
    if (!g_main_done) lvrt_uncaught();
    lv_plat_exit(lv_rt_exit_code());
}

__attribute__((export_name("lv_task_run")))
void lv_task_run(lv_task_t* t) {
    void* entry_sp = lv_sp_get();
    lv_sp_set(LV_SVC_TOP);
    void* region = lv_stack_take();
    lv_sp_set(lv_stack_top(region));
    lv_activation_body(t);               /* may suspend — own region only */
    lv_sp_set(LV_SVC_TOP);
    lv_stack_give(region);
    free(t);
    lv_throw_gate_svc();
    lv_sp_set(entry_sp);
}

/* Runtime-internal seam for lv_loop_wasm.c: run one dispatched callback as
 * its own activation (transient task identity — natives like sysTaskShield
 * need a current task; native timer callbacks are spawned tasks the same
 * way). The record lives on the PAYLOAD stack so it survives suspensions. */
__attribute__((noinline))
static void lv_dispatch_body(lv_task_fn fn, void* a, void* b) {
    lv_task_t rec;
    memset(&rec, 0, sizeof rec);
    rec.id = ++g_nspawned;
    rec.fn = fn; rec.a = a; rec.b = b;
    lv_activation_body(&rec);
}

void lv_wasm_dispatch_run(lv_task_fn fn, void* a, void* b) {
    void* entry_sp = lv_sp_get();
    lv_sp_set(LV_SVC_TOP);
    void* region = lv_stack_take();
    lv_sp_set(lv_stack_top(region));
    lv_dispatch_body(fn, a, b);          /* may suspend — own region only */
    lv_sp_set(LV_SVC_TOP);
    lv_stack_give(region);
    lv_throw_gate_svc();
    lv_sp_set(entry_sp);
}

/* Runtime-internal seam for lv_loop_wasm.c's lvrt_loop_step: suspend until
 * the host completes one timer dispatch (or return at once when nothing is
 * armed — the host decides; see lv_host.js `step`). */
int lv_task_loop_step_wait(void) {
    lv_task_t* cur = g_current;
    void* sp = lv_sp_get();              /* rule 2 */
    int r = lv_import_step();
    lv_sp_set(sp);
    g_current = cur;
    return r;
}

/* ====================== the doc 04 §6 composition probe ====================
 * One activation that suspends on a GENUINE host promise (lv.probe_fetch —
 * fetch of a data: URI on the host side) and then again through the yield
 * lane, while corpus activations sit parked in the same instance. Proves
 * Suspending imports compose with the park machinery ahead of doc 05's real
 * fetch bridge. Reachable only from the host (tests/wasm_node_run.mjs's
 * LV_PROBE_FETCH mode) — never from language code. */
__attribute__((noinline))
static int32_t lv_probe_body(void) {
    lv_task_t rec;
    memset(&rec, 0, sizeof rec);
    rec.id = ++g_nspawned;
    rec.state = LV_TASK_RUNNING;
    lv_task_t* prev = g_current;
    g_current = &rec;
    void* sp = lv_sp_get();              /* rule 2 */
    int32_t v = lv_import_probe_fetch(); /* Suspending: a real fetch settles it */
    lv_sp_set(sp);
    g_current = &rec;
    lv_task_yield();                     /* park-lane suspension above the fetch */
    g_current = prev;
    return v;
}

__attribute__((export_name("lv_probe_fetch")))
int32_t lv_probe_fetch(void) {
    void* entry_sp = lv_sp_get();
    lv_sp_set(LV_SVC_TOP);
    void* region = lv_stack_take();
    lv_sp_set(lv_stack_top(region));
    int32_t v = lv_probe_body();
    lv_sp_set(LV_SVC_TOP);
    lv_stack_give(region);
    lv_sp_set(entry_sp);
    return v;
}

/* ========================== the lv_task.h surface ========================= */

int lv_tasks_enabled(void) {
    /* Tasks are the model on wasm (doc 04). LANG_PUMP cannot exist here:
     * getenv is pinned NULL on this target (doc 03), matching "read once,
     * process lifetime" trivially. */
    return 1;
}

void lv_sched_init(void) { g_initialized = 1; }

void lv_sched_hooks(int (*poll)(const void*), int (*loop_step)(void)) {
    g_poll = poll;
    g_loop_step = loop_step;
}

void lv_sched_throw_probe(int (*throwing)(void)) { g_throwing = throwing; }

lv_task_t* lv_task_self(void) { return g_current; }

/* Task 0 runs INLINE in the current (main) activation: lv_entry_main is
 * already the promising export the host called, so its whole stack — awaits
 * included — is suspendable as-is. The native carrier loop's "schedule until
 * quiescent" half moves to the host (§1): timers and spawned tasks keep
 * arriving as their own activations after this returns, and lv_host.js's
 * run() settles only when the parked map and timer set are empty (§3.4). */
void lv_sched_run(lv_task_fn main_fn, void* a, void* b) {
    if (!g_initialized) lv_fatal("lv_sched_init() must precede lv_sched_run");
    g_main.id = ++g_nspawned;
    g_main.state = LV_TASK_RUNNING;
    g_main_done = 0;
    g_current = &g_main;
    main_fn(a, b);
    /* Completion with the throw flag set is the uncaught carrier — task 0's
     * own emitted tail already ran lvrt_uncaught (which is why main_done
     * gates lv_entry.c's extra report, same as the native trampoline). */
    g_main_done = 1;
    g_main.state = LV_TASK_DONE;
    g_current = NULL;
}

int lv_sched_main_completed(void) { return g_main_done; }

void lv_task_stats_report(void) {
    /* LANG_TASK_STATS is env-gated and getenv is pinned NULL here (doc 03):
     * dead by the same rule that silences it natively with the var unset. */
}

void lv_sched_teardown(void) {
    /* Worker-thread only on native; no workers on this target (doc 04 §7).
     * Return the pool for completeness — process teardown reclaims anyway. */
    LvStackNode* n = g_stack_pool;
    while (n) {
        LvStackNode* next = n->next;
        free(n);
        n = next;
    }
    g_stack_pool = NULL;
    free(g_reg);
    g_reg = NULL; g_reg_len = g_reg_cap = 0;
}
