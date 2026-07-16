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

/* Tasks are the default since the M5 flip; LANG_PUMP=1 (read once, process
 * lifetime) selects the old pump. 0 => every other call is forbidden. */
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
 * quiesces first. Returns LV_PARK_SETTLED, LV_PARK_DRAINED, or (B2) LV_PARK_
 * CANCELLED — a pending cancel mark is DELIVERED (and consumed) at park points,
 * and only there (doc 06 §2). Asserts: called from a task (not the scheduler),
 * and the caller's pending-throw flag is clear (G11 — asserted via the flag
 * hook below). */
int  lv_task_park_on(const void* obj);
#define LV_PARK_SETTLED    1
#define LV_PARK_DRAINED    2
#define LV_PARK_CANCELLED  3   /* B2 doc 06 §2: woken (or refused entry) by lv_task_cancel */

/* --- LA-30 B2 (doc 06 §4/§5) — cancellation, registry, multi-park ---------
 * Group-owned tasks cross the native boundary as IDs, never handles (§4: no
 * ARC entanglement in the registry; the registry maps id -> lv_task_t*,
 * thread-local, ids never reused). All of these share lv_task_park_on's
 * asserts and G12's discipline: cancellation UNWINDS a task through its
 * normal park-site raise — there is no kill, there will never be a kill. */

/* Spawn + register: like lv_task_spawn, returning the task's registry id. */
uint64_t lv_task_spawn_registered(lv_task_fn fn, void* a, void* b);

/* Mark task `id` cancelled. Delivery: if PARKED and unshielded (and no park
 * key already polls settled — settle wins, §8), it is dequeued and woken with
 * LV_PARK_CANCELLED; otherwise the mark is consumed at its next park entry.
 * Returns 1 if a live task was marked (or already carried the mark), 0 if the
 * id is unknown/done — idempotent both ways (§8 double-cancel). */
int  lv_task_cancel(uint64_t id);

/* §8 shield rule: while a task's shield counter is >0, cancel delivery is
 * masked (the mark holds but neither wakes it nor fires at park entry) — the
 * TaskGroup.close() unwind uses this so `using` + cancel cannot livelock.
 * delta is +1/-1; counter semantics so nested closes compose. Current task
 * only; a no-op from the scheduler context (natives guard). */
void lv_task_shield(int delta);

/* Multi-park, N=2 (doc 06 §5 — awaitTimeout's {work, timer}): park until
 * either key polls settled. On LV_PARK_SETTLED, *which is 0 or 1 (the settled
 * key; key a is checked first on a simultaneous settle — first-declared-wins).
 * DRAINED/CANCELLED leave *which untouched. Both keys BORROWED (G12: the
 * parked task's own frame holds both references). */
int  lv_task_park_any2(const void* a, const void* b, int* which);

/* Group join (doc 06 §3): park until every id in `ids` is done (unknown ids
 * count as done — ids are never reused). ids is BORROWED for the duration of
 * the park (the caller's frame owns the buffer). n == 0 or all-done returns
 * LV_PARK_SETTLED without parking. The §8 uncancellable report fires (once
 * per park, stderr) when the join has waited past LANG_TASK_UNCANCELLABLE_MS
 * (default 5000) with a cancel-marked child still live — loud, not hung; the
 * join still completes when the refuser eventually parks or returns. */
int  lv_task_park_join(const uint64_t* ids, int n);

/* G11 enforcement: engines install "is a throw pending on this thread?". Debug
 * builds assert !throwing at every park/spawn boundary; release builds skip. */
void lv_sched_throw_probe(int (*throwing)(void));

/* Stats ([tasks] line, LANG_TASK_STATS — mirrors LANG_THREAD_STATS, doc §8). */
void lv_task_stats_report(void);

/* --- LA-30 doc 2 (LLVM leg) — runtime-internal seam decls (doc 2 §2). Not in
 * lv_abi.h: generated code calls only lvrt_await; these cross between the
 * runtime's own .c files (lv_runtime.c / lv_loop.c / lv_entry.c / lv_thread.c). */

/* Poll hook (defined in lv_runtime.c): bit0 = `ready`, bit1 = `failed`. Reads
 * ONLY bool immediates through the raw copy-out getfield — ARC-silent (G15). */
int  lvrt_promise_state(const void* obj);

/* loop_step hook (defined in lv_loop.c): one loop batch with every USER
 * callback task-spawned instead of invoked inline; returns 0 when the loop had
 * no work. ONLY the scheduler calls this — the raw lvrt_loop_step keeps inline
 * dispatch in both modes (lv_main's emitted tail drain pumps it; doc 2 §6). */
int  lvrt_loop_step_tasks(void);

/* Did lv_sched_run's task 0 run to completion? 0 when the scheduler exited via
 * the doc-2 §8 throw gate while task 0 was still parked — its own uncaught/
 * meter tail never ran, so the engine's entry decides what to report. */
int  lv_sched_main_completed(void);

/* Worker-thread teardown (doc 2 §7; the "doc 2 owns teardown" note at the
 * valgrind registration site): munmap every POOLED stack and free its task
 * struct before the thread dies — pooled stacks are TLS-reachable only, so a
 * worker that skips this leaks its stacks for the process lifetime. Tasks
 * still parked at the §8 gate exit stay pinned (G12: unwind, never free — B2's
 * job); teardown reports nothing and skips them. Main thread never calls this
 * (process exit reclaims; pooled stacks legitimately live for its lifetime). */
void lv_sched_teardown(void);

#ifdef __cplusplus
}
#endif
#endif
