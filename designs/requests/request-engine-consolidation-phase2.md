# Request: Engine Consolidation — Phase 2

> Filed 2026-07-20 by the refactor_1 track (see
> `designs/refactor_1/techdesign-00-overview.md`). refactor_1 is
> structure-only; the items below change behavior or generated output and
> therefore need their own design track.

## What is requested

Three consolidations that finish what refactor_1's `RuntimeCore` starts:

1. **Narrow `Evaluator` to comptime-only.** Route all program execution
   through `Lowerer` + `IrInterp` (already the `--run` default), leaving the
   AST evaluator solely as the metaprogramming/comptime engine for
   `RuleEngine`. Removes one full runtime from the system.
2. **Single-source the CGen mini-runtime.** `CGen.cpp` emits a textual copy
   of the `RuntimeValue.hpp` value model and `arithPrim`/`valueToString`
   (21 clone windows measured). Generate that emitted runtime from one
   shared fragment (an `#include`-able or build-embedded source of truth)
   instead of a hand-maintained parallel copy. Same for the
   `NativeRuntime.cpp` mirrors linked into LLVM binaries.
3. **Unify the native-builtin tables.** `Lower.cpp` and
   `RuntimeNatives.cpp` restate the native surface (19 clone windows);
   derive both from one table (name, arity, ownership annotation, per-leg
   availability).

## What we are working on that requires it

The self-host schedule (portable pivot, G-gates → 2027-01). Every runtime
copy is a porting tax: the Leviathan-written compiler must reproduce each
one. Phase 2 reduces the portable semantics to exactly one interpreter +
one shared runtime definition.

## Wider things this allows

- Engine-divergence bug family (e.g. bug.md #13 class) becomes structurally
  impossible for the consolidated surface.
- New natives are declared once and appear in the interpreter, the lowerer's
  knowledge, and both compiled runtimes consistently.
- The conformance corpus (refactor_1 session 07) shrinks its skip lists as
  leg differences disappear.

## Requirements

**Minimum:** item 2 for `arithPrim`/`valueToString` only (the bit-for-bit
scalar semantics), with a byte-identity gate on CGen output for a corpus.

**Maximum:** all three items; `Evaluator` retains no code path reachable
from program execution; one declarative native table; conformance corpus
green on all legs with no consolidation-related skips.

## Known risks to design around

- `Evaluator` is the comptime oracle for `RuleEngine` — narrowing must not
  change comptime results (`import()`, attr evaluation, folding).
- CGen output is compiled by g++; the shared fragment must stay
  C++-compilable in both contexts (host build and emitted source).
- The interp/LLVM behavioral-identity guarantee (archectecture.md §3) is the
  invariant to protect throughout.
