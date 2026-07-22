# Refactor 1 — Session 07: IR Spec + Conformance Corpus (opus)

> Goal: make the IR the written contract between the front end and the back
> ends, and stand up a backend-agnostic conformance suite. This is the
> self-hosting on-ramp: a Leviathan-written front end will be validated by
> targeting the documented IR and passing the same corpus.
> Date: 2026-07-25. Depends on: 06 (paths), independent of 08.

## Files owned by this session

- `docs/ir-spec.md` (new)
- `tests/conformance/` (new: `*.lev` programs + `*.expected` files + runner)
- ctest wiring for the runner (CMake test additions only)

## Part A — `docs/ir-spec.md`

For **every** opcode in `Ir.hpp`'s `Op` enum, document:

- operand meanings (`a b c d` of `Inst`, plus `sname`/`decl`/`tk` side
  fields where used)
- semantics in one paragraph: effect on registers, heap, control flow
- value-kind requirements and what happens on mismatch (trap, coercion,
  or undefined — as *currently implemented by IrInterp*, which is the
  reference semantics)
- which backends consume it specially (e.g. columnar ops, task ops)

Also document: the register model, constant table, function/global layout,
`computeFnGlobalRefs` init-ordering contract, and the calling convention
between `Inst` streams and natives (`taskNative` dispatch).

Source of truth precedence when engines disagree: IrInterp defines IR
semantics. A discovered disagreement is filed to `/bug.md` (bug workflow),
and the spec records the IrInterp behavior with a `> bug #N` annotation.
Nothing in this session *changes* engine code.

## Part B — conformance corpus

1. Layout: `tests/conformance/<area>/<name>.lev` +
   `<name>.expected` (exact stdout) + optional `<name>.exit` (expected exit
   code, default 0). Areas (minimum 4 programs each): arith-string, control,
   classes-dispatch, structs-columnar, collections, closures-lambdas,
   exceptions, using-cleanup, tasks-async, enums-match, generics, metaprog
   (rules/macros visible at runtime), natives-fs (tmpdir-confined).
2. Runner `tests/conformance/run.sh <leg> <program>`: legs are
   `interp` (`--run`), `llvm` (build via clang, run binary), `cgen` (build
   via g++, run binary). **No X64 leg — ever.**
3. ctest registration: one test per (leg × program). Programs whose natives
   a leg does not support are listed in a per-leg `skip.txt` with a one-line
   reason each — an empty reason fails review; skips are visible, never
   silent.
4. Seed rule: corpus programs must be self-contained single files, no
   network, deterministic output (no time/RNG unless seeded/fixed).

## Validation

- All (leg × program) tests green under `ctest -j4` (flake policy per
  overview) on interp, llvm, and cgen legs, minus documented skips.
- `docs/ir-spec.md` covers 100% of the `Op` enum — the session includes a
  grep-based completeness check (every enumerator name appears as a spec
  heading) wired into the `layering`-style script or a tiny new ctest.

## Ending state (fixed)

Every opcode documented with IrInterp as reference semantics; ≥52 corpus
programs running on 3 legs under ctest; per-leg skip lists exhaustive and
justified; zero engine-code changes. Divergences found are bugs in
`/bug.md`, not fixes in this session.

## Self-host note (context, not tasks)

With this landed, the first self-host gate is a Leviathan-written lexer
validated by token-stream diff against the C++ lexer over this corpus; the
Checker's TU boundaries from session 03 define the porting units after that.
That work is outside refactor_1 and proceeds under the portable-pivot G-gates.
