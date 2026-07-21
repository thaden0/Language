# Refactor 1 — Session 02: RuntimeCore Extraction (opus)

> Goal: one shared implementation of the runtime semantics that `Evaluator`
> (the AST oracle) and `IrInterp` (the `--run` executor) currently duplicate,
> so dispatch/arith/task semantics can never drift between engines again.
> Date: 2026-07-22. Depends on: 01. Parallel with: 03, 04, 05.

## Files owned by this session

- `src/RuntimeCore.hpp`, `src/RuntimeCore.cpp` (fill)
- `src/Eval.hpp`, `src/Eval.cpp` (edit: delete duplicates, call RuntimeCore)
- `src/IrInterp.hpp`, `src/IrInterp.cpp` (same)
- `src/RuntimeValue.hpp` (edit only if a helper's natural home is here)

## What moves into RuntimeCore

Clone-scan evidence: 35 identifier-blind duplicate windows between the two
files. The proven twins, by current location:

1. **Arity-aware method lookup** — `Evaluator::findMethod`
   (`Eval.cpp:278`) and `IrInterp::findMethodByName` (`IrInterp.cpp:27`).
   These are line-for-line clones *by construction*: the comment at
   `IrInterp.cpp:29` records that bug.md #13 was precisely a drift between
   them. Unify as:
   `const Stmt* rtFindMethod(Symbol* cls, const std::string& name, int argc = -1);`
   The bug.md #13 arity-disambiguation behavior is the specification; the
   walk order over base classes must be preserved exactly.
2. **Object arithmetic** — `Evaluator`'s operator-method resolution in
   `evalBinary` (`Eval.cpp:1146` region) and `IrInterp::objectArith`
   (`IrInterp.cpp:710`). Shared piece: operator-token → method-symbol mapping,
   the resolved-decl-else-lookup pattern, and the fallback to a user `==`
   (incl. synthesized struct equality). Engine-specific piece (how operands
   are evaluated / how the method is invoked) stays in each engine and is
   passed as a callback or handled by the caller around the shared core.
3. **Member get/set over `Value`** — `IrInterp::getMember`/`setMember`
   (`IrInterp.cpp:619/635`) and the Evaluator's equivalents: the
   kind-switch on Object/Array/Map/Block/String targets. Accessor (`get`/`set`
   method) resolution stays engine-side; raw slot/field/key access is shared.
4. **Task/promise state transitions** — both engines implement
   `taskPollPromise` / `taskLoopStep` / `taskRunProgram` and register them via
   `lv_sched_hooks` (`Eval.cpp:2094`, `IrInterp.cpp:792`). Extract the
   *promise-state* logic (poll result encoding, settled/pending transitions,
   loop-drain step protocol against `RuntimeLoop::nextBatch`) into free
   functions both engines call. The frame save/restore (`saveTaskState`) is
   engine-specific state and does NOT move.

## Method

1. **Divergence audit first.** For each pair above, produce a side-by-side
   diff before moving anything. Classify every difference as
   (a) cosmetic, (b) accidental drift — a latent bug in one engine, or
   (c) intentional oracle-vs-IR difference.
   - (b) findings are filed to `/bug.md` per the bug workflow **and** the
     RuntimeCore version implements the *correct* side, with the bug entry
     noting refactor_1/02 fixed it structurally.
   - (c) findings stay engine-side; the doc implementer records each one as a
     comment at the call site (`// engine-specific: <why>`).
2. Move code into `RuntimeCore` as free functions over
   `Value`/`Symbol`/`Sema` — no new class, no virtuals, no engine state.
   Anything needing engine behavior takes it as an explicit parameter.
3. Delete the originals; both engines call RuntimeCore. No signature changes
   to any public engine API.

## Validation

- Full `ctest -j4` green (known flake policy per overview).
- Engine-equivalence spot check: run at least 10 nontrivial corpus programs
  (method dispatch through inheritance, operator overloads, struct equality,
  Map/Array member access, async/task programs) under **both** `--run`
  (IrInterp) and the comptime/oracle path where applicable, plus an LLVM
  build of the same programs; outputs must match pre-refactor outputs.
- The clone scan re-run must show the `Eval.cpp` ↔ `IrInterp.cpp` pair drop
  from 35 windows to < 8.

## Ending state (fixed)

`RuntimeCore.{hpp,cpp}` owns items 1–4; neither engine contains a private
copy of any of them; divergence audit findings are either in `/bug.md` or
documented as intentional at the call site. No public API changed.

## Known constraints

- `Map<K, Struct>` class-field corruption (bug #47) and boxed-`.at()` alias
  semantics (#74, fixed in `lvrt_copyval`) are pre-existing behaviors —
  RuntimeCore must not change either; the audit compares against current
  behavior, not desired behavior.
- Do not touch `RuntimeNatives.cpp`, `CGen.cpp`, or `NativeRuntime.cpp` —
  their duplication is engine-consolidation phase 2 (ticketed).
