# Refactor 1 — Session 05: LlvmGen TU Split (fable)

> Goal: split `LlvmGen.cpp` (3,686 lines, ~99 fns) into op-lowering vs.
> runtime-glue translation units. Marked **fable** per policy: this file
> contains the destination-ownership and ARC lowering; even pure motion of
> that code demands the highest care. No behavior change — enforced by a
> byte-identical-output gate. Date: 2026-07-23. Depends on: 01.
> Parallel with: 02, 03, 04.

## Files owned by this session

- `src/LlvmGen.cpp` (shrink), `src/LlvmGenInternal.hpp`,
  `src/LlvmGenOps.cpp`, `src/LlvmGenGlue.cpp` (fill)
- `src/LlvmGen.hpp` (comment-only edits)

## Split mapping

| Destination | What moves |
|---|---|
| `LlvmGenOps.cpp` | the per-IR-op lowering routines (one routine per `Op`), including the destination-ownership / ARC retain-release emission — moved VERBATIM |
| `LlvmGenGlue.cpp` | runtime-glue emission: string/closure/exception glue binding to `NativeRuntime`, constant/string pools, module preamble/declarations |
| `LlvmGen.cpp` (stays) | `emitIr()` driver, function-frame setup, register/SSA-name bookkeeping shared by both |

Helper placement rule as in sessions 03/04 (`llvmDetail::` in
`LlvmGenInternal.hpp` for the ~2 shared file-statics; single-user helpers
move with their user).

## Method

1. **Golden capture first.** Before any edit, emit `.ll` for a fixed corpus:
   at least 25 programs covering every op family — arith/compare, string ops,
   closures + captures, class dispatch (mono + dynamic), struct/columnar
   arrays, Map/Array ops, exceptions, using/cleanup chains, tasks/async,
   TLS/socket natives, globals init. Store the outputs out-of-tree
   (scratchpad), keyed by program.
2. Move code per the table, compiling after each step.
3. **Golden gate.** Re-emit the corpus; every `.ll` file must be
   **byte-identical** to its golden capture. Any diff — even ordering — means
   the motion changed emission order; revert that step and re-do it
   preserving definition/traversal order.

## Validation

- Byte-identical `.ll` gate above (this is the primary gate).
- Full `ctest -j4` green including the LLVM-leg tests (flake policy per
  overview).
- One end-to-end `--build` of a nontrivial program (e.g. a routing-corpus or
  JSON test) runs with identical output.

## Ending state (fixed)

Three TUs + internal header implement `LlvmGen`; `.ll` output byte-identical;
behavior identical. The ARC/destination-ownership code has moved as an
unbroken block and is otherwise untouched.

## Constraints

- Do not "improve" anything while moving — no renames, no comment rewrites,
  no reordering of emission, no constant-pool changes. Verbatim motion only.
- `NativeRuntime.cpp` is out of scope (phase-2 ticket).
