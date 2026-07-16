# LA-30 True Suspension — Overview (doc 0 of 6)

**Track:** LA-30 — stackful tasks under the uncolored `await`.
**Status:** PROPOSED — design complete, awaiting owner green-light. No implementation before §7's gate.
**Date:** 2026-07-11 (audited against master @ `c79932e`).
**Docs in this track:** this overview + `techdesign-01-task-substrate.md` (IMPLEMENTED
2026-07-11 → moved to `designs/complete/`), `-02-llvm-leg.md`,
`-03-interpreter-legs.md`, `-04-emitcpp-leg.md`, `-05-semantics-and-flip.md`,
`-06-b2-cancellation.md` (gated follow-on).

---

## 1. Thesis

Today `await` **pumps the event loop** from inside the awaiting frame: the awaiter re-enters
loop dispatch, and every callback that fires runs **nested on the same stack** until the
awaited promise is ready (`src/Eval.cpp:1072`, `src/IrInterp.cpp:376`,
`src/LlvmGen.cpp:2811` — all three carry the "pump the loop until ready (§14)" comment;
`src/Ir.hpp:66` codifies it on the op itself). That was the right v1: zero new runtime, no
stack switching, identical shape on all engines.

It has three structural costs, all already visible in the Atlantis kernel design
(`designs/atlantis/techdesign-01-kernel.md` §8):

1. **Stack-ordered resumption.** A pumping awaiter can only resume when everything that
   started *above* it on the stack has finished or parked deeper. A fast response waits
   behind a slow one that merely started later; in-flight concurrency = stack depth.
2. **Nothing is killable.** The kernel concedes "a wall-clock kill switch is impossible
   until true workers" (kernel §7, line ~621) and legislates `await nextTick()` chunking as
   MANDATORY hygiene because a handler that computes without yielding starves the loop.
3. **Imprecise failure routing.** An uncaught throw inside a loop callback surfaces at
   *whichever `await` happens to be pumping innermost* — not at the await of the promise
   that callback was resolving (doc 5 §2, correction C2, has the repro).

LA-30 replaces the pump with **stackful tasks**: each dispatched callback (and the program
top-level) runs on its own small stack; `await` **parks the task** and switches to a
per-thread scheduler; completion moves the parked task to a FIFO run queue. Resumption
becomes **completion-ordered**, stacks stay flat, every task has an identity (the hook
cancellation/timeouts need — doc 6).

**The surface does not change.** No `async` keyword, no colored signatures, no new syntax,
no prelude API change. This is the payoff of having kept `await` uncolored (the same
refusal that killed implicit-promise-wrapping): pump → tasks is a pure runtime swap,
exactly the migration Java made with Loom. User code that was correct under the pump —
and did not depend on undocumented pump internals — stays correct.

## 2. Current anatomy (what the swap touches)

Evidence, so no implementer has to re-derive it:

| piece | where | shape today |
|---|---|---|
| `await` op | `src/Ast.hpp:119`, `src/Ir.hpp:66`, lowered at `src/Lower.cpp:1615-1618` | one op, promise reg in, value reg out |
| oracle pump | `src/Eval.cpp:1072-1105` | inline `while` over `RuntimeLoop::nextBatch()`, invokes closures, rethrows Worker `failed` |
| IR pump | `src/IrInterp.cpp:376-400` | same shape |
| LLVM pump | `src/LlvmGen.cpp:2811-2855` | inline BB graph calling `lvrt_loop_has_work` / `lvrt_loop_step` (§2.9 fns, `LlvmGen.cpp:348`); rethrow via `lvrt_raise_str` (`:282`) |
| emit-C++ | `src/CGen.cpp` | **no `Await` case at all** — clean "unsupported" default skip (`CGen.cpp:677` comment); no loop, no timers |
| top-level drain | `src/Eval.cpp:1455`, `src/IrInterp.cpp:654`, `runtime/lv_entry.c:36-37` | `while (hasWork) step` after the program body (RuntimeLoop.hpp:12-14 program-lifetime rule) |
| loop registry | `src/RuntimeLoop.{hpp,cpp}` (interpreters, singleton), `runtime/lv_loop.c` (LLVM leg, per-thread) | registry owns timers/watches; **the engine drives dispatch** |
| Promise | prelude, `src/Resolver.cpp:1073-1091` | `ready`/`value` fields + single `cont` slot for `then` |
| Worker | prelude, `src/Resolver.cpp:1111-1120` | `: Promise<T>`; `reject(IException)` sets `failed`/`failMessage`/`ready` |
| spawn seam | prelude, `src/Resolver.cpp:1146ff` | one body, two legs behind the `sysThreadStart` probe (−1 = cooperative 0-delay timer on interpreters; fd = pthread + join watch on LLVM) |
| exceptions | `src/LlvmGen.cpp:162,367-370` (`gThrowing`, thread-local), `src/CGen.cpp:208-215`, `Eval.cpp` `throwing_` member | **pending-throw flag model on every engine — no landingpads, no Itanium EH** |
| threads | `runtime/lv_thread.{h,c}`, `lv_thread_ctx_init` (`lv_thread.h:36`) | per-thread TLS heap/arena/throw state; copy-always boundaries; handles never cross threads (A-1) |

Two facts discovered in this audit that make LA-30 markedly cheaper than the generic
"fibers in a C++ runtime" project:

- **No Itanium EH anywhere.** Every engine uses a pending-throw flag, so there is no
  `__cxa_eh_globals` to swap per task, no exception in flight across a switch, no unwinder
  walking off a task stack. The entire classic fiber/EH hazard class is absent by
  construction. The residual obligation is one invariant: *a task never parks with the
  throw flag set* (G11).
- **Handles never cross threads (A-1).** All waiters of a promise live on the promise's
  own thread, so the scheduler, run queue, and parked set are strictly thread-local — no
  locks, no cross-thread wake protocol beyond the eventfd seam threads already use.

## 3. Target model (one page)

- A **task** = a stack (mmap'd, guard page, pooled) + a saved context (callee-saved regs +
  sp). Tasks are **pinned to their spawning thread forever** — no migration, no work
  stealing. This preserves the TLS heap/arena/allocator discipline, the non-atomic ARC
  fast path, `errno`, OpenSSL per-thread state, and makes cached-TLS-across-switch a
  non-issue. (Rejecting migration is load-bearing; see §9.)
- Each thread runs **one scheduler** on its original OS stack: FIFO run queue, then the
  existing loop (`nextBatch` / `lvrt_loop_step`) as the source of new runnables. The
  program top-level (`lv_main`, or the interpreter's program body) is **task 0**.
- Every **user callback the loop dispatches** (timer fire, fd watch, worker-join watch)
  runs in a pooled task. Runtime-internal handlers stay on the scheduler stack.
- `await`: ready → proceed **without yielding** (fast path, same as pump). Not ready →
  park `(task, promiseObj)` in the thread's parked set and switch out. The scheduler
  **polls the parked set on quiescence** (run queue empty, and after each loop step) by
  reading the promise's `ready`/`failed` fields — both `bool` immediates, so the read is
  ARC-silent. No prelude change, no settle hook, `resolve()` stays pure language (the
  "loop stays dumb" stance, RuntimeLoop.hpp:16-19). A settle-hook native is the documented
  optimization escape if O(parked) polling ever measures hot (G10) — do not build it
  speculatively.
- Loop drained with tasks still parked: parity mode reproduces today's silent behavior;
  flip mode makes it loud (correction C3, doc 5).
- Cross-thread: unchanged. Worker completion still signals its eventfd; the owning loop's
  watch callback (now a task) rebuilds the result and resolves the Worker on the owner
  thread. `Channel<T>` unchanged.

## 4. What does NOT change

For reviewer confidence, the invariant list:

- Surface syntax and types: `await expr`, `Promise<T>`, `Worker<T> : Promise<T>`,
  `Channel<T>`, `spawn` — untouched. No coloring, ever (foreclosed; §9).
- `await` on a non-promise value = identity (parity with `Eval.cpp:1078`).
- `await` inside hermetic comptime = compile error (`Eval.cpp:1073`); comptime never
  reaches the scheduler (G14).
- `then(cb)` timing: `resolve()` invokes `cont` synchronously in the resolver's context,
  exactly as the prelude writes it today.
- Worker rejection carrier (`failed` + `failMessage` string) — LA-19 is *adjacent*, not
  absorbed (doc 5 §7).
- Dispatch order out of the loop: ready fds in fd order, then due timers in due/creation
  order (RuntimeLoop.hpp:60-64) — becomes enqueue order, preserved.
- Cooperative workers on the interpreters stay 0-delay-timer-driven in spawn order —
  the oracle remains the deterministic semantic reference.
- Copy-always thread boundaries, flatten/rebuild, eventfd join protocol, `+0B` leak
  discipline — untouched.
- The frozen ELF backend: **not a target, no lane, nothing gated on it** (standing rule).

## 5. Deliberate semantic corrections (summary; normative text in doc 5)

Three places where the pump's behavior is an artifact, not a contract. Parity is
maintained through M4 (differential bring-up); M5 flips them **deliberately, with corpus
updates and docs**, as documented corrections:

- **C1 — resumption order.** Completion-ordered (per-thread FIFO), not innermost-pump
  order. The only order the docs ever promised is "interleaving happens at await points";
  pump LIFO was never specified.
- **C2 — failure routing.** An uncaught throw in a loop callback no longer surfaces at an
  unrelated pumping `await`; it is program-uncaught (or `reject` when the callback is a
  spawn leg, which already `try/catch`es — unchanged).
- **C3 — drained-loop await.** Today an `await` whose promise nothing can resolve
  **silently yields the promise's default-constructed `value`** when the loop runs dry
  (`Eval.cpp:1083` "nothing will resolve it" → falls through to the field read). That is
  fabricated data — the disease `None` exists to cure (info.md §9). Corrected: a
  catchable `RuntimeException("await: event loop drained with promise unresolved")` at
  the await; on a worker thread it lands in `spawn`'s existing `catch` → `reject` →
  rethrows at the spawner's await. Loud, local, and uniform.

## 6. Doc map and file ownership (disjoint — parallel agents safe)

| doc | owns files | can start |
|---|---|---|
| 01 task substrate | `runtime/lv_task.h`, `runtime/lv_task.c`, `runtime/lv_ctx_x86_64.S`, `runtime/lv_ctx_aarch64.S`, `runtime/selftest.c` (additions), `CMakeLists.txt` (one hunk) | at T0 |
| 02 LLVM leg | `src/LlvmGen.cpp` (Await case + entry emission), `runtime/lv_loop.c`, `runtime/lv_entry.c`, `runtime/lv_thread.c`, `runtime/lv_abi.h` (decls), `runtime/lv_runtime.c` (`lvrt_await`, promise-state helper, `[tasks]` stats) | after M0 |
| 03 interpreter legs | `src/Eval.cpp`, `src/IrInterp.cpp` | after M0 (parallel with 02) |
| 04 emit-C++ leg | `src/CGen.cpp` — **v1: no code change** (explicit deferral) | doc-only |
| 05 semantics + flip | `docs/reference.md`, `info.md`, `tests/corpus/tasks/*`, kernel-doc handoff list; `src/{Token,Parser,Ast}.*` for `fork` removal (coordinate — front-end recently touched by LA-28) | corpus at T0; flip at M5 |
| 06 B2 cancellation | prelude region of `src/Resolver.cpp`, future natives | **GATED** — design only |

## 7. Milestones and timeline

T0 = owner green-light. Illustrative calendar assumes T0 = 2026-07-13.

| id | date | deliverable | gate |
|---|---|---|---|
| M0 | T0+2 (07-15) | substrate: `lv_task.*` + context switch + scheduler skeleton; `runtime_selftest` additions green under ASan/UBSan (`LANG_RT_SANITIZE=ON`) and valgrind; 1M-switch bench | switch works or STOP |
| M1 | T0+4 (07-17) | LLVM leg behind `LANG_TASKS=1`; **full existing corpus green in BOTH modes** (parity incl. C1-C3 pump behaviors); new `tests/corpus/tasks/` green in tasks mode on LLVM | parity or STOP |
| M2 | T0+6 (07-19) | oracle leg, same double-mode bar | |
| M3 | T0+7 (07-20) | IR leg; oracle=IR=LLVM differential on `tasks/` | |
| M4 | T0+9 (07-22) | cross-lane audit: qemu (aarch64) and wine (win32) lanes — port `lv_ctx_aarch64.S` / win32 fibers **iff those lanes execute async corpus** (audit first; doc 1 §5); perf gates (doc 5 §9) | |
| M5 | T0+11 (07-24) | **flip**: `LANG_TASKS` default ON, `LANG_PUMP=1` escape hatch; corrections C1-C3 live with corpus updates; reference.md/info.md patched; kernel handoff list filed | |
| M6 | T0+18 (07-31) | after soak: delete every pump path (LlvmGen BB graph, Eval/IrInterp pump loops, lv_entry raw drain, `LANG_PUMP`); grep-clean | |

M2/M3 parallelize with M1's tail; ownership above is disjoint by design.

## 8. Flag strategy

`LANG_TASKS` (env, read once at `lv_rt_init` / engine start — the `LANG_ARC_TRACE` /
`LANG_THREAD_STATS` convention, `runtime/lv_runtime.c:187,958`). Off = today's pump,
byte-identical. M5 inverts the default and adds `LANG_PUMP=1`; M6 deletes both flag and
pump. The flag is process-global: engines never mix models within one run, so a program is
always entirely-pump or entirely-tasks (G-invariant for differential testing).

## 9. Considered and rejected

- **Stackless coroutines / `llvm.coro`** — a per-function CPS/state-machine transform is
  coloring at the IR level, must be reimplemented per engine (impossible for the
  tree-walk oracle), and is the single most invasive lowering that exists. Rejected.
- **Colored `async` surface** — contagion through signatures, the disease `any` was
  rejected for; kernel §8.4 explicitly brags "no coloring". Foreclosed, not deferred.
- **Growable/moving stacks** — moving needs precise pointer maps; interior pointers are
  everywhere (C++ frames on the interpreters, address-taken slots in LLVM output).
  Fixed-size + guard page + lazy commit is what C-compatible runtimes do. Rejected.
- **Segmented stacks** — the hot-split problem; abandoned by Go and Rust both. Rejected.
- **Task migration / work stealing** — would put atomic ops on the ARC fast path and
  break the per-thread heap/arena reap protocol (`lv_thread_ctx_unmap`). Copy-always
  exists precisely so this is never needed. Rejected permanently, not deferred.
- **`ucontext` as the primary switch** — glibc's `swapcontext` performs a `sigprocmask`
  syscall per switch (~50-100× a register swap). Kept only as the portability fallback
  (G1).
- **Settle-hook native in `resolve()`** — O(1) wake but couples the prelude to the
  scheduler. Poll-on-quiescence is O(parked) per quiescent cycle, ARC-silent, and keeps
  `resolve()` pure language. Hook stays a documented escape (G10).
- **OS-thread-per-task** — that is just `spawn`, which we have; memory-per-connection is
  the thing tasks fix.

## 10. Predictable-gotchas index (G1–G15)

Each is detailed in its owning doc; this is the cross-track index. **Read before coding.**

| id | one-liner | doc |
|---|---|---|
| G1 | glibc `swapcontext` = syscall per switch; asm is the primary path | 1 |
| G2 | drained-loop await silently fabricates a default value today — parity until M5, then C3 | 1,5 |
| G3 | pump rethrow targets the innermost pumping await (misrouting) — parity until M5, then C2 | 5 |
| G4 | the context switch must stay an opaque out-of-line call — never inline/LTO into callers | 1 |
| G5 | Win32: use Fibers, never hand-rolled asm (TIB StackBase/Limit, SEH, `__chkstk`) | 1 |
| G6 | ASan/valgrind/TSan fiber annotations from M0, or every later lane fails mysteriously | 1 |
| G7 | interpreter shared-state audit: anything not on the fiber's own C++ stack must be park-safe | 3 |
| G8 | x86-64 red zone: safe only because the switch is a non-leaf call boundary — document, don't rely | 1 |
| G9 | interpreter tasks need far bigger stacks than compiled tasks (fat C++ frames) | 3 |
| G10 | poll-on-quiescence is O(parked); settle-hook is the measured escape, not the default | 1 |
| G11 | never park with the pending-throw flag set — assert at every park site | 1,2,3 |
| G12 | a parked task pins its stack and every ARC ref in its frames; cancellation must UNWIND, never free | 1,6 |
| G13 | corpus may assert only contract-level order (C1), never incidental interleavings | 5 |
| G14 | comptime evaluation must never reach the scheduler — assert | 3 |
| G15 | parked-set polling must read only `bool` immediates (`ready`/`failed`) — object-field reads would churn ARC | 1,2 |

## 11. STOP-and-escalate

Standing protocol (same as Track 10 / regex-library): an implementing agent that hits any
of these **stops, files the finding (bug.md if it's a defect), and escalates — no
improvisation**:

- Any existing-corpus divergence in tasks mode that is not one of C1/C2/C3's enumerated
  shapes.
- The `+0B` churn discipline breaks in tasks mode (a switch/park path leaks or
  double-frees).
- The asm switch cannot be made ASan/valgrind-clean on x86-64 (M0 gate).
- Any temptation to add `Await` to `src/CGen.cpp` "while in there" (doc 4 forbids it).
- Any temptation to touch `src/X64Gen.cpp` — ELF is frozen and not a target; nothing in
  this track may be gated on it.
- Reaching a **sensitive section** (§12) without executing its gate ritual — that is a
  STOP condition, not a judgment call.

## 12. Sensitive sections — model-gated STOP protocol

A handful of loci carry almost all of the track's risk: a subtle error there fails
**silently, schedule-dependently, and far from its cause** — the failure mode that eats
the most debugging time and is hardest to catch in review. These sections are
**model-gated**: they must be authored by the escalated model, never by whichever model
is doing the surrounding mechanical work. To enforce that without relying on in-the-moment
judgment, each is fenced with **gate markers** (grep token `⟦SENSITIVE-GATE⟧`) in its
owning doc.

### 12.1 The ritual (hard stops — two per section, no exceptions)

Every sensitive section is bracketed by an **ENTER gate** and an **EXIT gate**. An
implementing agent:

1. **Works normally up to an ENTER gate.** Everything *outside* the fences proceeds under
   the ordinary §11 STOP-and-escalate rules — no special handling.
2. **At an ENTER gate: STOP before writing a single line of the fenced code.** Declare,
   verbatim:
   `SENSITIVE SECTION ENTERED: <id> — <name>. Halting for model escalation; not writing fenced code.`
   Then end the turn and hand back. Do **not** proceed, do **not** "just scaffold it,"
   do **not** write the region and flag it afterward — the entire point is that a
   non-escalated model never authors these bytes.
3. **The escalated model implements the fenced region — and only it** — then verifies it
   against that section's own acceptance bar (the last column of §12.2).
4. **At the EXIT gate: STOP.** Declare, verbatim:
   `SENSITIVE SECTION COMPLETE: <id> — <name>. <one line: what landed + how verified>. Halting for review.`
   Then end the turn and hand back **before** resuming any non-fenced work.

Two stops, always: one so the right model *starts* it, one so the work is *reviewed*
before surrounding work builds on it. Skipping either — or authoring fenced code as a
non-escalated model — is a §11 STOP violation.

### 12.2 The registry

| id | section | owning doc | why sensitive | acceptance to clear the EXIT gate |
|---|---|---|---|---|
| **S1** | context switch + stack-pool lifetime + sanitizer/valgrind fiber annotations | 01 §5 (fenced), §4 pool-return rule, §10 (noted) | ~12 instructions whose errors corrupt the *next* task to run — far from cause, intermittent; missing ASan/valgrind annotations produce false-positive storms that masquerade as engine bugs in every later leg | selftest green in three builds (default, `LANG_RT_SANITIZE=ON`, valgrind); switch bench ≥ 20M/s (M0 gate) |
| **S2** | `lvrt_await` `value`-read retain discipline + drained-await parity | 02 §3 (fenced), 03 §2 (noted) | a one-off retain leak here bleeds across *every* awaiting program; getting parity wrong before M5 breaks the differential testing that legitimizes the M5 corrections | `+0B` churn corpus exact; both-mode existing corpus byte-identical (M1/M2) |
| **S3** | interpreter shared-state (G7) audit + any save/restore fix it uncovers | 03 §3 (fenced) | a "current-X" member without save/restore corrupts silently on the first task interleave; the failure is schedule-dependent and reads as a miscompile | G7 table proven **exhaustive** (every mutated member classified); `threads_serial/` byte-identical in both modes |
| **S4** | additive changes to `lv_thread.c` (worker scheduler bootstrap) | 02 §7 (fenced) | touches Track 10's already-closed eventfd join race ("never leaves the awaiter parked forever") — validated landed work, disproportionate cost to regress | thread-leak harness `+0B`, no leaked pthread, join-race corpus green |
| **S5** | the M5 flip (default inversion + C1–C3 going live + cross-lane pinning) | 05 §5 (fenced) | the one milestone that changes observable behavior on purpose; a silently-diverged qemu/wine lane poisons differential testing indefinitely | flip commit is semantics-only; every lane green-on-tasks **or** explicitly `LANG_PUMP`-pinned with a port-milestone comment |

S1 and S4 are pure-runtime and can be escalated in isolation. S2 and S3 span engine legs.
S5 is a process gate as much as a code one (doc 05 §5). **Doc 04 (emit-C++) and doc 06
(B2) contain no sensitive sections** — the former writes no code by decision, the latter
is design-gated until its own green-light.

## 13. Relations

- **LA-19 (promise rejection, `designs/requests/request-promise-rejection.md`)** — not
  absorbed; C2/C3 make its routing story precise and its full `reject(e)` object carrier
  trivial afterwards. Recommend closing LA-19 immediately after M5 (doc 5 §7).
- **Atlantis kernel** — §8's MANDATORY chunking relaxes to hygiene; §7's "kill switch
  impossible" gets its mechanism in B2 (doc 6). Handoff list in doc 5 §6.
- **Self-host** — the compiler itself is synchronous; this track competes for schedule,
  not for semantics. Sequencing is the owner's call at the green-light gate.
- **B2 (doc 6)** — cancellation, `awaitTimeout` with teeth, `TaskGroup` structured
  concurrency, channel select. Gated on M5 + separate green-light.
