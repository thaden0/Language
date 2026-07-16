# LA-30 True Suspension — Task Substrate (doc 1 of 6)

**Owns:** `runtime/lv_task.h`, `runtime/lv_task.c`, `runtime/lv_ctx_x86_64.S`,
`runtime/lv_ctx_aarch64.S`, `runtime/selftest.c` (additions only), `CMakeLists.txt` (one
hunk). **Depends on:** nothing (M0 is engine-free). **Consumed by:** docs 2 and 3.
**Status:** IMPLEMENTED — master 2026-07-11 (`c39d1b9` foundation + `621fe7f` S1 fenced
work). **M0 fully verified** across all three builds §12 requires: default selftest green;
`LANG_RT_SANITIZE=ON` green (ASan/UBSan, zero reports over 1M switches); valgrind green
(`--error-exitcode=1 --track-origins=yes`, 0 errors from 0 contexts — proving the
`VALGRIND_STACK_REGISTER` annotations, else valgrind floods on the first switch). Perf:
switch bench 35 M/s (gate ≥20), spawn/complete 8.99 M/s pooled (gate ≥2), pool `mapped=4`
(≤9). A feature-detected `runtime_selftest_valgrind` ctest makes the valgrind lane
permanent (self-skips its fork/deliberate-fault tests under `RUNNING_ON_VALGRIND`). Docs
2/3 consume this. **Gotchas owned:** G1, G2(mech), G4, G5, G6, G8, G10, G11, G12, G15.

---

## 1. Scope and stance

One C substrate, two consumers: the `lvrt` archive (LLVM-emitted programs,
CMakeLists.txt Track-B block ~§868-885) and the `langfront` library (the compiler binary,
for the oracle/IR legs — doc 3 hosts interpreter recursion on these same fibers). Plain
C17, no dependencies, no LLVM — testable standalone through `runtime_selftest`, exactly
the Track-B discipline (`B-M1: the runtime core is plain C ... tested standalone`).

The substrate knows nothing about Promises, `LvValue`, or interpreters. Engine-specific
knowledge enters through two installed callbacks (§6): a *poll* predicate ("is this opaque
parked-on object settled?") and a *dispatch* thunk ("run this callback value in a task").
That keeps `lv_task.c` identical for both consumers and keeps the loop dumb
(RuntimeLoop.hpp:16-19 stance).

## 2. Public API (`runtime/lv_task.h`, complete)

```c
/* LA-30 — stackful task substrate. Thread-pinned; no migration, ever (overview §9).
 * NOT part of lv_abi.h: generated code never calls these — only the runtime's own
 * files and the engines do (same discipline as lv_thread.h). */
#ifndef LV_TASK_H
#define LV_TASK_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct lv_task lv_task_t;
typedef void (*lv_task_fn)(void* a, void* b);

/* Read LANG_TASKS once (process lifetime). 0 => every other call is forbidden. */
int  lv_tasks_enabled(void);

/* Per-thread. Idempotent. Main thread: from lv_rt_init / engine start.
 * Worker threads: from the spawn trampoline beside lv_thread_ctx_init (doc 2 §7). */
void lv_sched_init(void);

/* The carrier loop: runs task 0 (`main_fn`), then schedules until quiescent
 * (run queue empty, loop has no work, no parked task settles). Replaces the
 * engine's pump/drain sites. Returns when the thread's program lifetime ends. */
void lv_sched_run(lv_task_fn main_fn, void* a, void* b);

/* Engine hooks, installed once per thread before lv_sched_run:
 *   poll(obj)   -> bitmask: 1 = settled (`ready`), 2 = failed. MUST be ARC-silent
 *                  (read bool immediates only — G15).
 *   loop_step() -> dispatch one loop batch; returns 0 when the loop had no work.
 *                  Every USER callback inside must go through lv_task_spawn. */
void lv_sched_hooks(int (*poll)(const void* obj), int (*loop_step)(void));

lv_task_t* lv_task_self(void);              /* NULL on the scheduler context   */
void lv_task_spawn(lv_task_fn fn, void* a, void* b);   /* enqueue a pooled task */
void lv_task_yield(void);                   /* runq-append self, switch out    */

/* Park the current task on `obj` until poll(obj) reports settled, or the thread
 * quiesces first. Returns LV_PARK_SETTLED or LV_PARK_DRAINED. Asserts: called
 * from a task (not the scheduler), and the caller's pending-throw flag is clear
 * (G11 — asserted via the flag hook below). */
int  lv_task_park_on(const void* obj);
#define LV_PARK_SETTLED  1
#define LV_PARK_DRAINED  2

/* G11 enforcement: engines install "is a throw pending on this thread?". Debug
 * builds assert !throwing at every park/spawn boundary; release builds skip. */
void lv_sched_throw_probe(int (*throwing)(void));

/* Stats ([tasks] line, LANG_TASK_STATS — mirrors LANG_THREAD_STATS, doc §8). */
void lv_task_stats_report(void);

#ifdef __cplusplus
}
#endif
#endif
```

Deliberate absences: no `lv_task_cancel` / `lv_task_kill` (B2, doc 6 — and G12 makes
"kill" impossible anyway: parked frames hold ARC references, so teardown must *unwind*,
never free); no cross-thread wake (A-1 makes all waiters thread-local); no priorities
(FIFO is the contract, C1).

## 3. Task object and lists

```c
struct lv_task {
    void*      sp;            /* saved stack pointer; register file lives ON the stack */
    uint8_t*   stack_base;    /* mmap base (guard page is [base, base+PAGE)) */
    size_t     stack_size;    /* usable bytes above the guard */
    lv_task_t* next;          /* intrusive: runq | parked | pool (exclusive states) */
    const void* parked_on;    /* poll key while PARKED */
    lv_task_fn fn; void* a; void* b;
    uint32_t   state;         /* RUNNABLE, RUNNING, PARKED, DONE, POOLED */
    uint32_t   drained : 1;   /* this park was woken by drain, not settle */
#if LV_TASK_ASAN
    void* fake_stack; const void* asan_base; size_t asan_size;
#endif
#if LV_TASK_VALGRIND
    unsigned vg_id;
#endif
};
```

Per-thread scheduler state is one `LV_TLS` struct (`lv_abi.h:27` macro): `current`,
`runq_head/tail` (FIFO), `parked_head/tail` (FIFO — park order is the drain-wake order),
`pool` (LIFO), the two hooks, counters. Intrusive singly-linked lists; a task is on at
most one list (states are exclusive) so one `next` pointer suffices. No allocation on any
scheduling path after pool warm-up.

## 4. Stacks

- `mmap(NULL, guard + size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS)` then
  `mprotect(base, PAGE, PROT_NONE)` for the guard below. Lazy commit does the rest: a
  64-bit address space makes *virtual* size nearly free; RSS = touched pages only.
- **Defaults:** compiled leg 1 MiB usable; interpreter legs 8 MiB (fat C++ frames — G9,
  measured in doc 3 §6). Override: `LANG_TASK_STACK` (bytes, page-rounded, min 64 KiB).
  One env var, per-consumer default chosen by the `lv_sched_init` caller.
- **Pool:** per-thread LIFO free-list. Watermark 8 stacks hot; beyond it,
  `madvise(MADV_DONTNEED)` on return (drops RSS deterministically — the leak harness
  reads predictable numbers; `MADV_FREE`'s lazy accounting would make `+0B` runs
  flaky). Refault cost on reuse is a page fault per touched page — acceptable, and only
  past the hot watermark.
- **Overflow:** guard-page fault. `lv_rt_init` (and each worker bootstrap) installs a
  `SIGSEGV` handler on a `sigaltstack` (the faulting thread's normal stack is the one
  that's full — an altstack is mandatory or the handler itself faults). The handler is
  async-signal-safe: walk the thread's task list (TLS, no locks — the faulting thread is
  the owner), if `si_addr` lands in a guard page `write(2)` a fixed diagnostic —
  `task stack overflow (task #N, stack 1048576B; raise LANG_TASK_STACK)` — then
  re-raise with default disposition. Never attempt recovery (the C stack is gone).
- A DONE task's stack returns to the pool only after switching *off* it (the epilogue
  runs on the scheduler stack — you cannot free the stack you are standing on).
  **(This rule is part of sensitive section S1 — overview §12; it is authored under S1's
  gate, fenced in §5.)**

## 5. Context switch

> **⟦SENSITIVE-GATE — ENTER S1⟧** Model-gated (overview §12). **STOP before writing any
> fenced code below** (through the EXIT gate at the end of this section; S1 also covers
> §4's pool-return rule and §10's annotations). Declare
> `SENSITIVE SECTION ENTERED: S1 — context switch. Halting for model escalation; not writing fenced code.`
> and hand back. Escalated model only.

**Primary: ~40 lines of assembly per arch** (`lv_ctx_x86_64.S`, `lv_ctx_aarch64.S`),
the boost::context / libtask shape:

```asm
/* void lv_ctx_switch(void** save_sp, void* load_sp)
 * x86-64 SysV. Callee-saved regset only: the compiler already spilled
 * caller-saved state around this (non-leaf) call. ~12 insns, no syscalls. */
.globl lv_ctx_switch
lv_ctx_switch:
    pushq %rbp; pushq %rbx; pushq %r12; pushq %r13; pushq %r14; pushq %r15
    subq  $8, %rsp            /* mxcsr (4) + x87 cw (2) + pad */
    stmxcsr 4(%rsp); fnstcw (%rsp)
    movq  %rsp, (%rdi)        /* *save_sp = sp */
    movq  %rsi, %rsp          /* sp = load_sp  */
    fldcw (%rsp); ldmxcsr 4(%rsp)
    addq  $8, %rsp
    popq  %r15; popq %r14; popq %r13; popq %r12; popq %rbx; popq %rbp
    ret                       /* returns into the target context */
```

A fresh task's stack is seeded so the first switch `ret`s into a trampoline
(`lv_task_entry`), which calls `fn(a,b)`, marks DONE, and switches to the scheduler —
the trampoline never returns (its return address slot is a poisoned sentinel;
walking past it is a bug we want loud).

aarch64 variant: `x19-x28`, `fp`, `lr`, `d8-d15`, `sp`. (qemu cross lane — M4 audit.)

**Rules (G4, G8):**
- The switch lives in a `.S` file, is never inlined, never LTO-folded into a caller
  (asm files are opaque to LTO — keep it that way; no "portable inline-asm rewrite").
  A compiler that inlined an rsp-swap into a frame would corrupt both contexts.
- x86-64 red zone: safe *because* `lv_ctx_switch` is an ordinary non-leaf call — the
  caller may not keep live data below `rsp` across any call. Do not "optimize" the
  switch into a leaf-visible intrinsic; that safety evaporates.
- FP env (`mxcsr`/x87 cw) is saved: 2 cheap insns buy immunity from any future native
  that toggles rounding modes.

**Fallback: `LV_CTX_UCONTEXT`** (build-time) using `makecontext`/`swapcontext` for
unported POSIX arches. Costs a `sigprocmask` **syscall per switch** on glibc (G1 — the
historical reason every serious runtime hand-rolls) and `makecontext`'s int-args
signature needs a shim. Correct, slow, acceptable as a bring-up/porting path only.

**Win32 (G5): use the Fiber API** (`ConvertThreadToFiber`/`CreateFiber`/`SwitchToFiber`)
behind the same `lv_task.h` seam via `lv_plat_win32.c` — never hand-rolled asm there.
Reasons an asm switch is wrong on Windows even though the instructions are trivial:
the TIB's `StackBase`/`StackLimit` must track the active stack or `__chkstk` probes,
SEH dispatch, and CFG checks misbehave in ways that surface far from the switch.
Fibers maintain the TIB; we don't. Scheduling this leg: M4 **iff** the wine lane
(`tests/run_wine_cross.sh`) executes async corpus — audit first; if it doesn't, file the
explicit deferral and keep win32 on the pump until it does (doc 5 §5's flip then keys the
win32 lane to `LANG_PUMP` semantics — the lane must not silently diverge).

> **⟦SENSITIVE-GATE — EXIT S1⟧** On completing the fenced work (the asm switch here, plus
> §4's DONE-task pool-return rule and §10's sanitizer/valgrind annotations) and passing
> §12's M0 bar, **STOP.** Declare
> `SENSITIVE SECTION COMPLETE: S1 — context switch. <what landed + how verified>. Halting for review.`
> and hand back before resuming non-fenced work. The scheduler (§6) below is NOT fenced —
> ordinary work resumes there.

## 6. Scheduler

```c
void lv_sched_run(lv_task_fn main_fn, void* a, void* b) {
    lv_task_spawn(main_fn, a, b);                    /* task 0 */
    for (;;) {
        lv_task_t* t;
        while ((t = runq_pop()))                     /* completion order — C1 */
            switch_to(t);                            /* returns on park/done/yield */
        if (poll_parked())                           /* settle-scan: parked, in order */
            continue;                                /* something became runnable    */
        if (!sched.loop_step || !sched.loop_step())  /* one batch; enqueues tasks    */
        {                                            /* loop had NO work:            */
            if (!sched.parked_head) break;           /* quiescent: program over      */
            drain_wake_all();                        /* G2: wake parked, drained=1,  */
            continue;                                /*     park order; then resched */
        }
    }
    lv_task_stats_report();
}
```

- **`poll_parked()`** walks the parked list in park order calling `poll(parked_on)`;
  settled tasks move to the runq (their FIFO position = settle-scan order — deterministic
  because the parked list and the poll points are). Runs when the runq empties and after
  every loop batch. O(parked) per quiescent cycle (G10) — at kernel scale (10^3-10^4
  parked) this is a µs-range scan of two bool fields per entry; the settle-hook redesign
  is *documented* (overview §9) and *unbuilt* until a profile demands it.
- **`drain_wake_all()`** implements G2's mechanism: every parked task is woken with
  `drained=1`, in park order; `lv_task_park_on` then returns `LV_PARK_DRAINED` and the
  *engine* decides what that means (parity: silent value read; M5: throw — doc 5 C3).
  Drained wakes cannot livelock: each park gets at most one drain wake per park, exactly
  as each pump iteration re-checks `hasWork` once and breaks — a program that re-parks
  forever on a dead promise loops under the pump too (bug-for-bug equivalence, doc 5 §3).
- A task that yields (`lv_task_yield`) goes to the runq **tail** — FIFO fairness, no
  starvation among runnables; `await` on a ready promise takes the fast path and never
  enters the scheduler at all (C4, doc 5).
- The scheduler context is the thread's original OS stack. Task 0 (program top-level) is
  an ordinary task: when it parks at a top-level `await`, dispatched work proceeds —
  observable order identical to the pump for non-nested cases, corrected (C1) for nested.

## 7. Park/wake and the poll seam

`lv_task_park_on(obj)`:
1. Debug-assert `lv_task_self() != NULL` (parking the scheduler is a fatal misuse) and
   `!throw_probe()` (G11 — an engine must never park with a pending throw; the pump's
   own structure guaranteed this: `Eval.cpp:1077` checks `throwing_` before pumping).
2. Append self to the parked list with `parked_on = obj`, switch to scheduler.
3. On resume: return `LV_PARK_DRAINED` if `drained`, else `LV_PARK_SETTLED`.

The **poll hook** owns G15: it may only read `bool` immediates (`ready`, `failed`).
On the LLVM leg that is `lvrt_promise_state` (doc 2 §3) — field reads of immediates
retain nothing, so a million polls churn zero refcounts and the `+0B` churn corpus stays
exact. On the interpreters it is a `fields.find("ready")` pair (doc 3 §4). Never poll
`value` (object-typed — a retain per poll would both churn and leak-on-drain).

Lifetimes: the parked task's own frame holds the awaited promise reference (the operand
of `await`), so `parked_on` is a **borrowed** pointer that cannot dangle while the task
is parked. The parked set never retains. (This is also why G12 holds: the frames ARE the
ownership record; only unwinding releases them.)

## 8. Stats and diagnostics

`LANG_TASK_STATS` (mirrors `LANG_THREAD_STATS`, `lv_runtime.c:958`): at thread drain,
one line to stderr —

```
[tasks] spawned=1204 completed=1204 switches=48210 peak_live=37 peak_parked=31
        stacks: mapped=40 pooled=8 hwm_committed=1.4MiB drained_wakes=0
```

`drained_wakes` deliberately rides the stats line: a nonzero value in a program that
"worked" is the G2 smell (silent fabricated defaults under parity mode) — the harness
can grep it long before M5 makes it throw.

## 9. Selftest additions (`runtime/selftest.c`)

Engine-free C tests, same harness that proved the archive (Track B):

1. spawn/park/wake round-trip; FIFO order of 3 tasks asserted.
2. 1M ping-pong switches; wall-clock printed (perf gate: ≥ 20M switches/s on x86-64 asm;
   ucontext build exempt).
3. pool reuse: 10k spawn/complete with watermark 8 → `mapped ≤ 9`.
4. drain-wake: park on a never-settling key; assert `LV_PARK_DRAINED`, park order.
5. guard-page: child process overflows deliberately; parent asserts the diagnostic line
   and the death signal.
6. poll determinism: two parked tasks, settle both between polls, assert wake order =
   park order.
7. stats line shape (grep-able by the harness).
8. (ASan build) switch annotations: the whole suite under `LANG_RT_SANITIZE=ON`.

## 10. Sanitizer/valgrind integration (G6 — do this at M0, not later)

> **Part of sensitive section S1** (overview §12) — these annotations are authored under
> S1's gate (fenced in §5) because getting them wrong is what makes every *later* leg's
> sanitizer runs lie. Do not land them as non-fenced mechanical work.

- **ASan:** every switch is bracketed by `__sanitizer_start_switch_fiber` /
  `__sanitizer_finish_switch_fiber` (the `fake_stack` slot in the task struct). Missing
  brackets ⇒ false "stack-use-after-return" storms in *later* legs that look like engine
  bugs. `LV_TASK_ASAN` keys off `__has_feature(address_sanitizer)` and the existing
  `LANG_RT_SANITIZE` option builds it.
- **Valgrind:** `VALGRIND_STACK_REGISTER` per mmap'd stack, `DEREGISTER` at unmap,
  guarded by `RUNNING_ON_VALGRIND` (header vendored the usual way; zero cost off).
  Without it valgrind reports wild "invalid write below stack" on the first switch —
  the `run_memverify.sh` lane would drown.
- **TSan (optional lane):** `__tsan_create_fiber`/`__tsan_switch_to_fiber` behind
  `LV_TASK_TSAN`; tasks are single-threaded so this is annotation-only. File as a
  follow-up if the TSan lane lands first.

## 11. Predictable gotchas (owned here)

| id | trap | mitigation |
|---|---|---|
| G1 | `swapcontext` syscall tax | asm primary; ucontext behind `LV_CTX_UCONTEXT` only |
| G2 | drain-wake must reproduce pump's silent path until M5 | `drained` bit + engine-side choice; `drained_wakes` stat |
| G4 | inlined/LTO'd switch corrupts both contexts | `.S` file, opaque; never rewrite as inline asm |
| G5 | Win32 TIB/SEH/`__chkstk` | Fiber API via `lv_plat_win32.c`; audit wine lane before M5 |
| G6 | sanitizer false positives drown later legs | annotations land in M0 with selftest #8 |
| G8 | red-zone assumptions | safety derives from non-leaf call ABI; document, never "optimize" |
| G10 | O(parked) poll | measure first; settle-hook is the escape, not the default |
| G11 | park with pending throw | `lv_sched_throw_probe` + debug assert at every park |
| G12 | freeing a parked stack leaks/corrupts ARC | no kill API exists; DONE-only pool return; B2 must unwind |
| G15 | poll churns ARC | poll reads bool immediates only; churn corpus (+0B) is the acceptance |

## 12. Acceptance (M0)

- `runtime_selftest` green: default build, `LANG_RT_SANITIZE=ON` build, valgrind run.
- Switch bench ≥ 20M/s (x86-64 asm), spawn/complete ≥ 2M/s pooled.
- No engine files touched. `lv_task.c` compiles into both `lvrt` and `langfront`
  (CMake hunk: add to the Track-B archive sources + `target_sources(langfront ...)`,
  plus `enable_language(ASM)`).

**STOP conditions:** asm switch not sanitizer-clean on x86-64; any need to touch engine
files to make M0 pass (the seam is wrong if so); any allocation appearing on the
switch/park hot path.
