# LA-30 True Suspension — Interpreter Legs: Oracle + IR (doc 3 of 6)

**Owns:** `src/Eval.cpp`, `src/IrInterp.cpp`. **Depends on:** doc 1 (M0); the substrate
compiles into `langfront` so both engines link it in-process. **Status:** IMPLEMENTED —
master 2026-07-12 (S2 interpreter-leg path + S3 G7 audit authored under the model gate,
owner-escalated). **M2+M3 verified:** full existing corpus (all 148 ctest lanes,
`threads_serial/` included) green AND byte-identical in both modes; scratch-probe
divergences exactly the enumerated C1/C2 shapes; wall-clock within 1.2% of pump (gate:
3%); valgrind-clean on both engines in tasks mode; `drained_wakes` audited 0 across the
existing corpus. Depth probe landed at `tests/corpus/tasks/recursion_depth.lev`
(tasks-mode-only — see §5). Implementation log in §11.
**Gotchas owned:** G7, G9, G11 (leg detail), G14.

---

## 1. The structural fact this doc exists for

On the interpreters, **the Leviathan stack IS the C++ call stack**: `Evaluator::eval`
recurses per expression/call, `IrInterp` recurses per function activation. Suspending a
Leviathan task therefore means suspending interpreter recursion mid-flight — which is
exactly what a fiber does. The same `lv_task` substrate hosts it: a task's stack carries
the interpreter's own C++ frames for that task. No interpreter data structure changes; the
suspension is beneath it.

The alternative (heap-reified interpreter frames, a "goroutine as data") was considered
and rejected: it rewrites both interpreters' cores, diverges from the substrate the
compiled leg uses, and buys nothing the fiber doesn't. Uniformity wins — one suspension
mechanism, four consumers (oracle task, IR task, LLVM task, worker body).

## 2. What changes, mechanically

Three sites per engine, mirrors of doc 2's shape:

1. **The `Await` case** (`Eval.cpp:1072-1105`, `IrInterp.cpp:376-400`): keep the
   tag/comptime/pending-throw early-outs and the entire post-wait tail (failed → rethrow
   with `failMessage`; read `value`) **verbatim**; replace only the middle — the
   `while (!throwing_) { nextBatch(); invoke... }` pump — with:

   ```cpp
   if (lv_tasks_enabled()) {
       int r = lv_task_park_on(p.obj.get());          // borrowed key — frame holds the ref
       if (r == LV_PARK_DRAINED && !parityMode())
           return throwRuntime("await: event loop drained with promise unresolved");
       // parity mode: fall through to the tail, silent — G2
   } else { /* today's pump loop, verbatim, until M6 */ }
   ```

   > **Note — this `Await`-case parity path (the `!parityMode()` drained branch and the
   > verbatim post-wait `value` read) is part of sensitive section S2** (overview §12;
   > fenced on the runtime side in doc 02 §3). Author it under S2's gate, not as
   > mechanical work — the retain discipline of the `value` read and the drained-parity
   > semantics must match the LLVM leg byte-for-byte. The tag/comptime/pending-throw
   > early-outs and hook installation (§2.3) around it are ordinary, non-fenced work.

2. **The top-level drain** (`Eval.cpp:1455-1456`, `IrInterp.cpp:654-655` — the
   RuntimeLoop.hpp:12-14 program-lifetime rule): becomes `lv_sched_run(programBodyTask)`
   in tasks mode; the program body itself is task 0, so a top-level `await` parks like
   any other. The pump-mode drain stays verbatim behind the flag until M6.

3. **Hook installation** (once per engine run, before `lv_sched_run`):
   - `poll`: `[](const void* obj) { auto* o = (Obj*)obj; int s = 0;
     if (truthyBool(o->fields, "ready"))  s |= 1;
     if (truthyBool(o->fields, "failed")) s |= 2; return s; }` — two `fields.find`
     reads of `bool` values; no `Value` copies of object-typed fields (G15).
   - `loop_step`: pull one `RuntimeLoop::nextBatch()` and `lv_task_spawn` each job's
     closure+argument through a thunk that calls `callClosure` (oracle) /
     `invokeClosure` (IR). Returns 0 when `!loop.hasWork()`. `RuntimeLoop` itself is
     untouched — it stays the dumb registry (its singleton is fine: these engines are
     single-threaded; cooperative workers were always loop tasks).
   - `throw_probe`: returns the engine's `throwing_` (G11).

## 3. The G7 audit — engine state vs. parked tasks

> **⟦SENSITIVE-GATE — ENTER S3⟧** Model-gated (overview §12). **STOP before performing this
> audit or writing any save/restore fix it uncovers** (through the EXIT gate at the end of
> this section). Declare
> `SENSITIVE SECTION ENTERED: S3 — interpreter G7 audit. Halting for model escalation; not writing fenced code.`
> and hand back. Escalated model only — the audit's *value depends on being exhaustive*,
> which is exactly the property a mechanical pass is most likely to miss.

The one real hazard on this leg: **`Evaluator`/`IrInterp` are single objects shared by
every task's recursion.** Any *member* mutated as recursion descends is corruption bait —
unless it is strictly save/restore-scoped (then each fiber's own C++ frames carry the
save/restore chain and interleaving is invisible), or throw-flag-shaped (linear, and
provably clear at parks).

**The audit's load-bearing correction (found at implementation, 2026-07-12).** The
pre-implementation sketch classified `env_`/`thisObj_`/`thisClass_` as safe because
"restore frames live on the fiber's own stack." That verdict was **wrong** under
completion-ordered resumption: the LIFO save/restore in `callFunction`/`callClosure`
(`Eval.cpp` — wholesale `std::move` swaps) is sound only while dispatch is strictly
*nested*, which is exactly the property the pump had and tasks remove. Concretely: task
A parks inside a call with `env_` = A's frames; task B starts and installs its own; B
**parks** (instead of completing); A resumes → A executes with B's environment — silent,
schedule-dependent corruption (reproduced in bring-up as a thought experiment; the
`interleave_probe` scratch case exercises the exact shape and passes only with the fix).

**The fix (S3): park-site save/restore.** The park is the ONLY point a fiber loses the
engine, so `Evaluator::saveTaskState()`/`restoreTaskState()` move the volatile member
set into the parking fiber's own frame around `lv_task_park_on` and reset the members to
pristine (a fresh task starts from a clean context). Invariant restored: *park-time view
== resume-time view*, whatever ran in between. Pristine-start is deliberately NOT the
pump's behavior — nested dispatch leaked the awaiter's `rawField_`/`inCtor_`/
`primThis_`/`curNamespace_` into callbacks, an undocumented stack accident of the same
family as C2, not a contract.

Verified audit of `Evaluator` (every mutable member classified — the review rule stands:
extend this table before merging any new member):

| member | discipline | verdict |
|---|---|---|
| `env_`, `thisObj_`, `thisClass_`, `curNamespace_`, `primThis_`, `hasPrimThis_`, `inCtor_`, `returning_`, `returnValue_`, `breaking_`, `continuing_`, `rawField_` | **park-swapped** (`TaskState`, Eval.hpp) — moved into the parking fiber's frame, members reset pristine, restored on resume | safe by the swap; the LIFO call-site swaps stay correct *within* one fiber |
| `throwing_` | linear flag; parks only happen in `Await` after the early-out; probe-asserted (G11); a task-ending throw is stashed first-wins (`taskCaptureTermination`) and re-raised after `lv_sched_run` returns | safe — the flag never crosses a task boundary |
| `thrownValue_` | dead whenever `throwing_` is false (catch binds it into an env frame before running the handler); park-swapped anyway so the invariant is total | safe |
| `exiting_` | rides `throwing_`; stash captures it; a stashed **exit** re-raises at every subsequent park-resume ("exit stops every task, parked ones included" — exit-codes §4), unlike a plain throw (C2: never delivered to an unrelated await) | safe; parity verified on the exit-in-callback probe |
| `comptime_`, `hermetic_`, `steps_`, `budgetExhausted_` | resolver-phase only; `Await` under comptime throws before any park (`Eval.cpp`), `assert(!comptime_)` at the park (G14) | safe — scheduler unreachable |
| `importCtx_`, `importCache_`, `importedAssets_` | comptime-only (LA-20 intercept checks `comptime_`), cache append-only | safe via G14 |
| `globals_` | name→Value map: **program state, shared across tasks BY DESIGN** (doc 5 §1: "state on the current thread may be observed and mutated by other tasks") | safe — this is the language's own semantics, not engine state |
| `out_` | append-only; order = the (deterministic) schedule | safe |
| `exitCode_`, `sema_`, `sink_` | written at `run()` end / const / append-only diagnostics | safe |
| `taskTerm*_`, `gTaskEval_` | scheduler-context bookkeeping, never read mid-task except the intentional stash gates | safe |
| recursion-depth guard | **none exists** in either engine (grep-verified) | n/a — the sketch's "likely first offender" is absent |
| natives' statics (exit-request latch, TLS session table) | process/global latches; the exit latch makes every post-exit task unwind at its first native call (approximating the pump's hard stop); TLS table is fd-keyed (doc 2 §9's verdict) | safe — already interleaving-exposed under the pump |

`IrInterp` audit: the register file and `pc` are **locals of `call`/`frameExec`**
(fiber-carried — the whole point of hosting recursion on fibers); `globals_`/`out_` as
above; `thrown_` dead-outside-throw-window, park-swapped anyway; `taskTerm*_` mirrors
the oracle; `mem_`/`tracked_`/`violations_` (the mem-verify/ownership bookkeeping) are
diagnostics whose order follows the deterministic schedule — `corpus_mem_verify` and
`corpus_ir_verify` pass in both modes.

The heuristic form of G7 for reviewers: *"could this member's value at park-time differ
from its value at resume-time, and would that change behavior?"* If yes → per-task or
save/restore. **Do not** shortcut with "the pump already interleaved it": the pump's
reentrancy was strictly stack-shaped, and LIFO-swap members that were safe under nesting
are exactly the ones fiber interleaving corrupts — that shortcut is how the sketch's
original `env_` verdict went wrong.

> **⟦SENSITIVE-GATE — EXIT S3⟧** On completing the G7 audit (every mutated `Evaluator`/
> `IrInterp` member classified — the table proven exhaustive) plus any save/restore fix it
> found, and passing S3's bar (`threads_serial/` byte-identical in both modes), **STOP.**
> Declare
> `SENSITIVE SECTION COMPLETE: S3 — interpreter G7 audit. <what landed + how verified>. Halting for review.`
> and hand back before resuming non-fenced work.

## 4. Determinism (the oracle stays the reference)

- Single scheduler, single thread, FIFO runq; runnables originate from `nextBatch`,
  whose order is specified (ready fds in fd order, then timers in due/creation order —
  RuntimeLoop.hpp:60-64) and from settle-scans of the parked list in park order (doc 1
  §6). Every source is deterministic ⇒ the whole schedule is. Corpus stays exact-output.
- Cooperative workers keep their identity: `spawn`'s interpreter leg is a 0-delay timer
  in spawn order (`Resolver.cpp:1155-1160`) — under tasks that timer's callback runs as
  a task, in the same relative order. `threads_serial/` corpus must not change by a byte
  through M4.
- The oracle remains what the COW/ARC engines are differentially checked against; tasks
  do not add nondeterminism to it. (Real-thread LLVM runs keep their existing tolerance:
  scheduling across OS threads was never byte-pinned; per-thread schedules now are.)

## 5. Stack sizing (G9)

Interpreter frames are fat: an `eval` recursion step is several C++ frames
(`eval` → `evalCall` → `callClosure` → `eval`...) with `Value`/`shared_ptr` locals.

**Measured (2026-07-12, the depth probe's `hwm_committed` at depth 1000):** ~**5.9 KiB
per Leviathan frame on the oracle**, ~**1.4 KiB on the IR engine** — so 10k frames need
~59 MiB, and the design's provisional 8 MiB default overflowed the probe as this section
anticipated. Per the rule below, the interpreter default was raised to **128 MiB
virtual** (≈2× the oracle's 10k-frame need; `-DLV_TASK_STACK_DEFAULT` on the `langfront`
target in CMakeLists; `LANG_TASK_STACK` still overrides; compiled default untouched at
1 MiB). Lazy commit keeps RSS at touched pages, so the width costs nothing until used —
verified: the probe run commits 59.1 MiB (oracle) / 14.2 MiB (IR) on exactly one task.

The probe (`tests/corpus/tasks/recursion_depth.lev`: depth 10k inside a spawned callback
AND inside a timer callback) records touched-page HWM in `[tasks]` stats. **Bring-up
finding:** depth 10k segfaults under the **pump too** (all nested dispatch shared the
one ~8 MiB OS stack) — a pre-existing ceiling, so the probe is a *tasks-mode-only*
corpus file (doc 5's DIVERGENCES quarantine shape: pump = stack overflow, tasks = the
correct output). Tasks turn that shared, unsized ceiling into a per-task, sized,
diagnosable budget. The guard page turns the miss loud (doc 1 §4's diagnostic names the
env var — observed: `task stack overflow (task #2, stack 8388608B; raise
LANG_TASK_STACK)`); an overflow diagnostic during bring-up is the mechanism working, not
a defect.

## 6. Comptime and `import()` (G14)

Hermetic comptime evaluation (`comptime_ && hermetic_`) runs during resolve, on the
compiler's own stack, before any scheduler exists for the program — and `await` there is
already a compile-time error (`Eval.cpp:1073-1075`). Obligations: keep that error
*before* any park branch (order of checks in §2.1 preserves it); assert `!comptime_` at
the park; never call `lv_sched_init`/`lv_sched_run` from resolver-phase evaluation.
`std::import()` and the metaprogramming rules engine are unaffected.

## 7. What is deliberately NOT done here

- No change to `RuntimeLoop` (registry stays; singleton stays — single-threaded engines).
- No fiber-per-*call* or per-`then`-callback micro-tasking beyond what the loop
  dispatches: `then(cb)` on a ready promise still invokes `cb` synchronously in the
  resolver's context (prelude behavior, `Resolver.cpp:1087-1090`) — the prelude is
  untouched.
- No attempt to make the oracle model LLVM's real-thread interleavings — the serial
  worker model is a feature (deterministic reference), not a gap.
- No prelude edits at all on this leg. If a prelude edit appears necessary, the design
  is wrong somewhere — STOP.

## 8. Corpus

- M2 (oracle) / M3 (IR): full existing corpus green in both modes; `tasks/` corpus green
  in tasks mode; `threads_serial/` byte-identical across modes; churn corpus `+0B`.
- M3 closes with the three-engine differential on `tasks/`: oracle = IR = LLVM
  byte-identical (the schedules are deterministic on all three for these programs —
  that identity is the whole value of doing interpreters second).
- Debugging note for implementers: a fiber-hosted interpreter under gdb has real stacks —
  `bt` works inside a running task; parked tasks are invisible to `bt` (their stacks are
  live but not the current one). A `lv_task_dump_all()` debug helper (walk parked/runq,
  print `parked_on` + entry fn) is cheap and worth adding to the substrate if M2 debugging
  wants it — file under doc 1 ownership if so.

## 9. Predictable gotchas (leg-local)

| trap | note |
|---|---|
| a member that is "current-X" without save/restore | the G7 table + review rule; the depth-guard counter is the likely first offender |
| parking inside comptime | G14 asserts; the existing `:1073` error must stay ahead of the park branch |
| 1 MiB default stacks "because that's what doc 2 uses" | G9 — interpreter frames are ~an order fatter; 8 MiB + depth probe |
| "simplify" cooperative workers into direct `lv_task_spawn` | changes spawn ordering vs. the 0-delay-timer contract; keep the prelude leg untouched |
| letting `poll` copy the `value` field for convenience | G15 — bool immediates only |
| forgetting the IR leg's `raise()` differs from oracle's `throwRuntime` | keep each engine's own throw idiom in the drained branch (C3 wording must be byte-identical across engines — take it from doc 5 §2) |

## 10. Acceptance

M2/M3 per §8. Perf: interpreter corpus wall-clock within 3% of pump mode (fibers add two
switches per parked await; the pump's nested dispatch wasn't free either — expect a wash;
regressions beyond that mean an accidental hot-path allocation — STOP and profile).

**STOP:** any prelude edit needed; any `RuntimeLoop` API change needed; any
corpus divergence outside C1/C2/C3; `threads_serial/` divergence of any kind.

## 11. Implementation log (landed master 2026-07-12)

Authored under the model gate (owner-escalated 2026-07-12) — the S2 interpreter-leg
parity path and the S3 G7 audit are the fenced regions; everything else per §2's
mechanical plan. What landed, exactly:

- **`src/Eval.cpp` / `src/Eval.hpp`** — `Await` tasks branch (ready fast-path per doc 5
  C4; park with S3 state swap; drained = G2-parity silent fall-through with the C3
  throw text staged in a comment for the M5 edit; stashed-**exit** re-raise on resume);
  `run()` split into `execTopLevel` (verbatim items loop, both modes) + mode dispatch
  (`lv_sched_run` with task 0 = program body vs. the verbatim pump drain); substrate
  hooks `taskPollPromise` (G15 bool-immediate reads), `taskLoopStep` (one `nextBatch`,
  jobs `lv_task_spawn`ed in batch order, gated on the termination stash = the pump
  drain's `!throwing_` gate), `taskThrowProbe` (G11), thunks + first-wins
  `taskCaptureTermination`.
- **`src/IrInterp.cpp` / `src/IrInterp.hpp`** — the same three sites, mirrored;
  `thrown_` park-swapped; ginit+entry as task 0 preserving the pump's
  no-check-between-calls shape.
- **`CMakeLists.txt`** — `LV_TASK_STACK_DEFAULT = 128 MiB` on `langfront` (G9; §5's
  measured numbers).
- **`tests/corpus/tasks/recursion_depth.lev(+.expected)`** — the §5 depth probe
  (tasks-mode-only; pump segfaults at this depth — pre-existing OS-stack ceiling).
- **Untouched, as designed:** `RuntimeLoop` (registry + singleton), the prelude,
  `src/CGen.cpp` (doc 4), `src/X64Gen.cpp` (frozen), Checker/Lower.

Verification (the §8/§10 bars): all **148 ctest lanes green in BOTH modes** and
byte-identical (every corpus file exact-matches `.expected` under pump and tasks —
includes `threads_serial/`, exit-codes, mem-verify/ir-verify); scratch probes confirm
real parking (`peak_parked≥1`, `drained_wakes` correct), C1 completion-order and C2
uncaught-routing appear **only** in their enumerated shapes, drained-await and
exit-in-callback are parity-exact; wall-clock: oracle −0.3%, IR +1.2% (gate 3%);
valgrind-clean on both engines in tasks mode (S1's fiber annotations at work).

Deviations from the sketch, all argued in place: (1) §3's `env_` "safe" verdict was
wrong — replaced by the park-site swap (the S3 fix; see §3); (2) fresh tasks start
pristine rather than inheriting the awaiter's leaked `rawField_`/`inCtor_`/etc. (an
undocumented pump artifact, C2-family); (3) a stashed `env.exit` re-raises at every
park-resume so exit stops parked tasks too (exit-codes §4 parity — verified); (4) the
interpreter stack default is 128 MiB, not 8 MiB (§5's own escalation rule, measured);
(5) the substrate's landed G11 completion assert means thunks stash-and-clear rather
than doc 2 §8's "leave the flag set" sketch — same observable behavior, routed through
`taskCaptureTermination`.

**S2 scope note:** S2's *interpreter-leg* half (this doc's §2 note) is done; S2 as a
whole stays open until doc 02 lands `lvrt_await`'s LLVM-side retain discipline — the
"byte-for-byte across engines" clause is satisfiable today only among oracle/IR (they
are byte-identical), and doc 02 must match them when it lands.
