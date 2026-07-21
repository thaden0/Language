# Refactor 1 — Maintainability & Self-Hosting On-Ramp (Overview)

> Status: AUTHORED 2026-07-20. Not implemented.
> Scope: the C++ host compiler in `src/` only. No `.lev` prelude, no trident,
> no library changes. **Zero behavior changes** — every session in this track
> is structure-only and must leave observable compiler behavior identical.

## Why

`src/` is a flat directory of 50 files (~38,800 lines) whose layering is
logical, not physical (see `docs/archectecture.md`). Three concrete problems:

1. **Nothing enforces the layering.** Any backend can include any semantic
   header; only convention stops it.
2. **A few files do 3–5 jobs.** `Checker.cpp` (5,336 lines, ~301 fns),
   `Rules.cpp` (2,864), `LlvmGen.cpp` (3,686), `Parser.cpp` (2,351).
3. **Runtime semantics are defined four times** — `Eval.cpp`, `IrInterp.cpp`,
   `CGen.cpp`'s emitted mini-runtime, and `NativeRuntime.cpp`. A winnowing-style
   clone scan (8-significant-line windows, identifier-blind) measured:

   | Clone pair | Dup windows | What it is |
   |---|---:|---|
   | `Eval.cpp` ↔ `IrInterp.cpp` | 35 | method lookup, `objectArith`, member get/set, task machinery |
   | `CGen.cpp` ↔ `RuntimeValue.hpp` | 21 | emitted mini-runtime mirrors the value model |
   | `Lower.cpp` ↔ `RuntimeNatives.cpp` | 19 | native-builtin tables restated |
   | `CGen.cpp` ↔ `LlvmGen.cpp` | 14 | shared backend glue |

   The copies have already drifted once: the comment at `IrInterp.cpp:29`
   records that bug.md #13 was a divergence between `findMethodByName` and the
   oracle's `findMethod`. Every dispatch-semantics change today must be made
   twice, perfectly.

This track also serves the self-host schedule (G1–G5 → 2027-01): a split
Checker can be ported to Leviathan piecewise, a documented IR is the porting
contract, and a backend-agnostic conformance corpus is how a Leviathan-written
front end gets validated against this one.

## Sessions

One document per session. Complexity per `docs/policies.md`.

| # | Document | Complexity | Owns (disjoint) | Date |
|---|----------|------------|-----------------|------|
| 01 | techdesign-01-scaffold-sonnet.md | sonnet | `CMakeLists.txt`, new empty TUs/headers | 2026-07-21 |
| 02 | techdesign-02-runtime-core-opus.md | opus | `Eval.*`, `IrInterp.*`, `RuntimeValue.hpp`, `RuntimeCore.*` | 2026-07-22 |
| 03 | techdesign-03-checker-split-sonnet.md | sonnet | `Checker.*`, `Checker*.cpp`, `CheckerInternal.hpp` | 2026-07-22 |
| 04 | techdesign-04-rules-parser-split-sonnet.md | sonnet | `Rules.cpp`, `Parser.cpp` + their new TUs | 2026-07-23 |
| 05 | techdesign-05-llvmgen-split-fable.md | fable | `LlvmGen.cpp` + its new TUs | 2026-07-23 |
| 06 | techdesign-06-folder-reorg-sonnet.md | sonnet | ALL file moves + includes + CMake (solo, atomic) | 2026-07-24 |
| 07 | techdesign-07-ir-spec-conformance-opus.md | opus | `docs/ir-spec.md`, `tests/conformance/` | 2026-07-25 |
| 08 | techdesign-08-clone-ratchet-sonnet.md | sonnet | `tools/clonedet.py`, baseline, ctest hook | 2026-07-26 |

**Ordering constraints:**
- 01 lands first (it pre-stages every new file + CMake entry so sessions 02–05
  never touch `CMakeLists.txt` and stay conflict-free).
- 02–05 run in parallel; file ownership is disjoint.
- 06 runs **solo** after 02–05 have merged to master, as one atomic commit,
  with all agent branches synced first. Nothing else in that commit.
- 07 and 08 run after 06 (08's baseline must be generated post-dedup,
  post-reorg).

## Invariants (all sessions)

1. **X64 freeze is absolute.** `X64.hpp`, `X64.cpp`, `X64Gen.hpp`,
   `X64Gen.cpp` are never modified, moved, run, or tested — not even `git mv`
   in session 06. They stay at `src/` root permanently.
2. **No behavior changes.** Every session is code motion or additive tooling.
   Validation for every session includes a full `ctest` run
   (`regex_pathological_linear` host-timeout is a known flake; rerun stragglers
   individually). Session 05 additionally requires byte-identical `.ll`
   output; session 02 requires the divergence audit in its doc.
3. **Post-work per `docs/policies.md`**: stage, commit, pull origin master,
   push to origin master; move each design doc to `designs/complete/refactor_1/`
   when its session is fully implemented.
4. Sonnet-complexity sessions: if any step requires a decision not written in
   the doc, **STOP and escalate** — do not improvise.

## Fixed ending state of the whole track

- `src/` reorganized into `core/ frontend/ sema/ meta/ ir/ backend/ runtime/
  driver/` (X64 files excepted), with `tools/check_layering.sh` enforcing the
  include discipline in ctest.
- One shared `RuntimeCore` implements method lookup, object arithmetic, member
  access, and task-state transitions for both interpreters.
- `Checker`, `Rules`, `Parser`, `LlvmGen` each split into single-concern
  translation units; no file in `src/` (X64 excepted) exceeds ~2,500 lines.
- `docs/ir-spec.md` documents every `Ir.hpp` opcode; `tests/conformance/`
  runs the corpus against IrInterp, LLVM, and CGen legs under ctest.
- `tools/clonedet.py` + checked-in baseline fail ctest on new cross-file
  clones (X64 excluded from scanning).

## Explicitly out of scope (ticketed, not deferred)

Narrowing `Evaluator` to comptime-only, generating CGen's mini-runtime and
`NativeRuntime.cpp` from a single source of truth, and unifying the
`Lower.cpp`/`RuntimeNatives.cpp` native tables are **engine-consolidation
phase 2** — behavior-affecting work that does not belong in a
structure-only track. Ticket: `designs/requests/request-engine-consolidation-phase2.md`.
