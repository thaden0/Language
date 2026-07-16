# LA-30 B2 — Cancellation, Timeouts, Structured Concurrency (doc 6 of 6)

**Status: IMPLEMENTED IN FULL, master `8daab3e` (+ `a6448f7` build-asan untrack),
2026-07-12.** Owner green-lit both gates (M5 landed + B2's own separate go); the
sensitive substrate extension (§5, §8's shield/registry) was authored by an
escalated model under the doc-0 §12 gate ritual. See §11 for the implementation
log — three deliberate divergences from this sketch, all in the direction of
"the substrate already made it unnecessary," not scope cuts.
**Owns:** prelude region of `src/Resolver.cpp`, new natives (`runtime/lv_loop.c`),
doc-1 substrate extensions (`runtime/lv_task.{h,c}`). **Gotchas owned:** G12
(binding), plus §8's table — all exercised in `runtime/selftest.c` and
`tests/corpus/tasks/b2_*`.

---

## 1. What B2 delivers (and to whom)

The Atlantis kernel's two standing concessions get their mechanism:

- kernel §7 (~line 621): "a wall-clock kill switch is impossible until true workers" →
  **`awaitTimeout` with teeth** (stop waiting AND stop task-backed work).
- kernel §8's starvation rules → narrow further: a stuck *task* becomes cancellable at
  its next park; only a compute-bound non-awaiting body remains unkillable (honest limit,
  same as Go/Loom — preemption is out of scope, stated in §9).

Plus the general-purpose payoff: **structured concurrency** with zero new syntax, via
the existing `using`/`IDisposable` rule (reference.md §5.2 — scope-owned resources,
close on every exit edge, reverse declaration order). A task group IS a resource.

## 2. Cancellation model (the one rule)

**Cancellation is an exception delivered at park points, and only there.**

- New prelude class `CancelledException : Exception` (ordinary `IException` carrier —
  catchable by type/interface like everything else; no second error channel — the same
  collapse LA-19's request makes for rejection).
- Cancelling a task marks it; if PARKED it is woken with a `cancelled` result code; the
  park site (the engine's await path) raises `CancelledException` at the `await` —
  the same rethrow point machinery as Worker-failed and C3-drained. A RUNNING task is
  never preempted: the mark takes effect at its next park. A DONE/settled race: settle
  wins, cancel becomes a no-op (§8).
- **G12 is why this is the only possible model:** a parked task's frames hold ARC
  references; freeing a stack without unwinding leaks (or corrupts) — so cancellation
  MUST run the task to completion through its normal unwind (`using` closers, `catch`
  blocks, ARC releases all fire). Go can free stacks because a GC scans them; ARC
  cannot. There is no `kill`, there will never be a `kill`.

Delivery sites inherit awaits' semantics: cancellation at an `await` inside a `try` is
catchable (a task may refuse cancellation — with the visibility that implies); an
uncaught `CancelledException` in a group-owned task is **absorbed by its group's join**
(cancellation is not a program error), while in a non-group callback it rides the normal
uncaught path (you cancelled something unowned — loud is right).

## 3. Surface (v1 of B2 — deliberately small)

No raw task handles. The bazooka stays in the marked room: user code gets **groups and
timeouts**, not tasks.

```lev
class TaskGroup {                       // reference type, IDisposable
    new TaskGroup() { ... }
    void run(() => void body);          // start a child task, owned by this group
    void cancelAll();                   // mark every live child
    void close();                       // cancelAll(), then await all children
}

T? awaitTimeout<T>(Promise<T> work, int ms);   // std free function
```

- `using TaskGroup g = TaskGroup();` gives structured concurrency: every exit edge of
  the block (fall-off, return, throw, break — §5.2's guarantee) runs `close()`, which
  cancels stragglers and **joins everything** — no orphan task survives its lexical
  scope. Group nesting composes by scoping; reverse-order closing is inherited from
  `using`.
- `g.run` is named `run`, not `spawn` — `spawn` is thread-backed `Worker<T>` (Track 10)
  and the two must never blur: a group child is a same-thread task (cheap, cancellable);
  a `spawn` is a thread (parallel, uncancellable). The naming carries the semantics.
- `awaitTimeout(work, ms)`: parks on {work, timer} (multi-park, §5); on timeout —
  returns `None` and, **iff** `work` is a promise whose producer is a group-owned task
  of the caller's group, cancels it. A `Worker` (OS thread) is *never* cancelled by
  timeout — the kernel's own doc already tells the truth here ("threads cannot be
  wall-clock killed"); the function stops *waiting*, and the docs say exactly that.
  Expected-absence via `T?` (union rule), not an exception — timeout is an outcome,
  not a failure (§12.6 line).

## 4. Natives and prelude floor (sketch)

Three natives over doc-1 extensions: `sysTaskRun(closure) -> int` (task id, group
bookkeeping in prelude), `sysTaskCancel(int id)`, `sysTaskJoinAll(Array<int>) ->
Promise<int>`-shaped park. `TaskGroup` itself is prelude code holding `Array<int>` of
live ids — the same "natives are a floor, the library is language" discipline as
everything else. Ids, not object handles, cross the native boundary (no ARC entanglement
in the task registry; the registry maps id → `lv_task_t*`, thread-local).

## 5. Substrate extension: multi-park (the one hard bit)

`awaitTimeout` (and channel `select`, §6) needs "park on N conditions, first settle
wins". Doc-1's `parked_on` single key becomes, for these sites only, a small inline
array of park nodes (fixed N=2 covers timeout; general select gets `N` nodes):

- Park: enroll one node per condition (promise poll key, timer key, channel key).
- Wake: first settled condition wins; **the task must be dequeued from every other
  waitq/parked entry before it runs** — the classic select complication; with intrusive
  nodes each dequeue is O(1), and it happens on the scheduler (single-threaded, no
  races by A-1).
- The losing timer must be cancelled (or it fires into a dead node — nodes carry a
  generation counter; a stale fire is a no-op, and the `[tasks]` stats count
  `stale_wakes` so leaks of this kind are visible).

## 6. Channel `select` (sketch only — its own doc when green-lit)

`select` over N channel-receives + optional timeout arm = multi-park generalized;
arm priority = declaration order on simultaneous readiness (deterministic, matches
first-declared-wins everywhere else in the language). Needs `Channel<T>` to expose its
waitq as a park key — a runtime detail, no surface change. The threads design already
deferred fan-in to "N channels + a loop-level select, a later library layer"
(techdesign-threads.md §363-364) — this is that layer's substrate. Design the surface
(expression? statement? match-like arms?) in its own doc; do not improvise syntax here.

## 7. Kernel adoption map (for the Atlantis owner, post-B2)

Per-request `TaskGroup` in the pipeline (request-scoped `using` block) ⇒ handler
timeout = `awaitTimeout` around the handler's promise with the group cancel ⇒ kernel
§7's kill switch, drain = `cancelAll` on live request groups + the existing
close-after-current-response seam. SSE pingers and per-connection timers (§8's "cancel
on close" rule) become group-owned — the rule stops being convention enforced by
review and becomes structure enforced by scoping.

## 8. Predictable gotchas

| trap | note |
|---|---|
| cancel delivered inside `close()`/unwind | a closer that awaits could be re-cancelled forever → **shield rule:** cancellation delivery is masked while a task is running `IDisposable.close()` on the unwind path (mask counter in the task struct; scheduler checks before delivering). Without this, `using` + cancel livelocks |
| double-cancel | idempotent by state check (marked ∨ done → no-op) |
| cancel-vs-settle race | settle wins; cancel of a settled park is a no-op (§2) — assert both orders in corpus |
| select loser dequeue | §5's generation counter; `stale_wakes` stat is the audit |
| catching `CancelledException` and continuing forever | legal (a task may refuse) but a group's `close()` then waits forever → `close()` re-cancels on a bounded retry then **reports** (`[tasks] uncancellable=N` + drain report naming the task's entry) — loud, not hung, and the group still joins when the task eventually parks |
| group child spawning a `Worker` | the thread is NOT group-owned (uncancellable, different lifetime); docs must say groups own tasks, threads own themselves |
| B1 code assuming single-key park | doc-1 reviewers: keep `parked_on` access behind helpers so the multi-node change is mechanical |

## 9. Non-goals (stated so nobody "finishes" them)

No preemption of compute-bound tasks. No cross-thread cancellation of `Worker`s. No raw
user-facing task handles or task-locals. No async `IDisposable.close`. No priorities.
Each would need its own design against info.md's gates; none is implied by B2.

## 10. Acceptance sketch (binding when green-lit)

Corpus: group joins on all exit edges (fall-off/return/throw/break × close-order);
cancel-at-park raises catchable `CancelledException`; timeout returns `None` and
cancels task-backed work but never threads; shield rule (closer awaits under cancel);
select loser-dequeue under simultaneous settle; `+0B` under cancel storms
(10k cancels — G12's unwind discipline is what the churn corpus is really testing here).
Perf: cancel/join overhead unmeasurable when unused (the mask/mark fields are cold).

**STOP:** any need for preemption to make a kernel scenario work (redesign the scenario);
any temptation to expose `lv_task_t` to the language; any `close()` path that can hang
without the §8 report firing.

## 11. Implementation log (2026-07-12)

Landed as sketched, with three divergences discovered during implementation — each
the substrate turning out to make part of the sketch unnecessary, not a scope cut:

1. **§5's generation counters were not needed.** The sketch worried about "the
   losing timer firing into a dead node." In the shipped design, `awaitTimeout`'s
   timer arm is an **ordinary loop timer resolving an ordinary Promise** — there is
   no raw timer→park edge for a stale fire to corrupt. `lv_task_park_any2`'s two
   keys are both promise-shaped (poll `ready`/`failed` on an `Object*`), so an
   abandoned arm's timer callback just resolves a `Promise` nobody is parked on
   anymore — inert, not stale. `stale_wakes` accordingly does not exist as a stat;
   `[tasks]` instead reports `cancel: marked=/delivered=/uncancellable_reports=`.
2. **`sysTaskJoinAll` parks directly and returns a count, not `Promise<int>`.**
   The sketch's `-> Promise<int>`-shaped park was written before the multi-park
   substrate existed; once `lv_task_park_join(ids, n)` was available as a direct
   blocking-park primitive (mirroring `lv_task_park_on`), wrapping it in a Promise
   would have added an indirection with no consumer — `TaskGroup.close()` just
   calls it and inspects the return.
3. **`awaitTimeout` v1 does not implement the "cancels task-backed work" half of
   §3's promise.** No `promise → producer-task-id` map exists (a `Promise<T>` is
   an ordinary prelude object; nothing connects it back to the `sysTaskRun` id that
   resolves it). `awaitTimeout` therefore only stops *waiting* — the compose-with-
   `cancelAll()` escape hatch §3 already names for `Worker` applies equally here in
   v1. Building the producer map is future work if a kernel scenario needs it
   (§7's adoption map still holds: pair `awaitTimeout` with the enclosing group's
   `cancelAll()` for the kill-switch shape).

Everything else — the cancellation-at-park-only rule (§2), the shield rule (§8),
double-cancel/settle-race idempotence (§8), the uncancellable report, and the §10
acceptance bar (group joins on all exit edges, `+0B` under cancel storms, shield
under closer-awaits) — landed exactly as designed. Verified on oracle/IR/LLVM
(`tests/corpus/tasks/b2_*`) and in `runtime_selftest` under default/ASan+UBSan/
valgrind builds.
