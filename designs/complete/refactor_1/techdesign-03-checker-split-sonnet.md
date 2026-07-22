# Refactor 1 — Session 03: Checker TU Split (sonnet)

> Goal: split `Checker.cpp` (5,336 lines, ~301 fns — the largest file in the
> compiler) into single-concern translation units implementing the SAME
> `Checker` class. Pure code motion: no signature, logic, or behavior change.
> This is also self-host prep — each unit becomes an independently portable
> piece. Date: 2026-07-22. Depends on: 01. Parallel with: 02, 04, 05.

## Files owned by this session

- `src/Checker.cpp` (shrink), `src/Checker.hpp` (comment-only edits)
- `src/CheckerInternal.hpp`, `src/CheckerInfer.cpp`, `src/CheckerFlow.cpp`,
  `src/CheckerDispatch.cpp`, `src/CheckerGenerics.cpp`, `src/CheckerReify.cpp`
  (fill; created by session 01)

## Split mapping

Move each method's *definition* (declarations stay in `Checker.hpp`):

| Destination | What moves |
|---|---|
| `CheckerInfer.cpp` | the `typeOf*` inference core (`typeOfMember`, `typeOfCall`, `typeOfBinary`, and every sibling `typeOf*`) |
| `CheckerFlow.cpp` | flow-sensitive narrowing: `analyzeCond`, everything operating on `Fact`, `unionMinus`, `invalidatePath` |
| `CheckerDispatch.cpp` | `buildOverrideIndex`, `resolveDispatch`, `dispatchesDynamically`, `tryResolveMethodRef` (method-reference eta-expansion) |
| `CheckerGenerics.cpp` | monomorphization: `markSpecializationSites`, `materializeSpecializations`, `specializeValueStruct` and their helpers |
| `CheckerReify.cpp` | expression reification: `reifyNode`, `makeExprNode`, `buildBindsExpr` and their helpers |
| `Checker.cpp` (stays) | `run()`, declaration/statement walking, diagnostics plumbing, everything not listed above |

**Helper placement rule:** a helper (method or file-static) used by exactly
one destination moves with it. `Checker.cpp` currently has ~19 file-static /
anonymous-namespace helpers — any static needed by more than one TU loses
`static`, gains a `checkerDetail` namespace, and its declaration goes in
`CheckerInternal.hpp`. No static may be duplicated.

## Method

1. Enumerate every function definition in `Checker.cpp` with its line range;
   assign each to a destination per the table. A function whose assignment is
   ambiguous under the table → STOP and escalate (do not guess).
2. Move in table order, one destination file per commit-sized step, compiling
   after each.
3. `Checker.hpp` gets `// defined in CheckerX.cpp` comments on grouped
   declarations. Nothing else in the header changes.

## Validation

- Full clean build with zero new warnings; `ctest -j4` green (flake policy
  per overview).
- `git diff` review confirms pure motion: every hunk is delete-here/add-there;
  the only permitted textual changes are `static` removal + `checkerDetail::`
  qualification for shared helpers, and `#include` lines.
- Resulting `Checker.cpp` ≤ ~1,800 lines; no new TU exceeds ~1,500.

## Ending state (fixed)

Six TUs implement `Checker` per the table; `CheckerInternal.hpp` holds all
cross-TU helper declarations; behavior identical.

## STOP-and-escalate

Escalate on: ambiguous function assignment; a shared static whose
de-static-ing changes overload resolution; any test regression; any need to
change a function signature or reorder logic to make the split compile.
