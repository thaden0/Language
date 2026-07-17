# Struct equality implementation — staged work packets

Implements `techdesign-struct-equality.md` (in this directory). Read that first;
these packets are the *how*, it is the *what/why*. Milestones M1–M3; M4 stays
dormant (spec-only, no code — do not implement it).

## Packet order & model assignments

| # | Packet | Model | Milestone | Depends on |
|---|---|---|---|---|
| 01 | `01-corpus-scaffold.sonnet.md` | **Sonnet** | M1 | — |
| 02 | `02-eq-synthesis-pass.fable.md` | **Fable** | M1 | 01 |
| 03 | `03-checker-gate.opus.md` | **Opus** | M1 | 02 |
| 04 | `04-exterminate-fallbacks.opus.md` | **Opus** | M1 | 02, 03 |
| 05 | `05-canon-float-natives.opus.md` | **Opus** | M2 | 04 |
| 06 | `06-float-nan-constant.opus.md` | **Opus** | M2 | 05 |
| 07 | `07-canonical-match.opus.md` | **Opus** | M3 | 05, 06 |
| 08 | `08-docs-sweep.sonnet.md` | **Sonnet** | close | 01–07 |

Fable is used exactly once: packet 02 (the synthesis pass) is the single point
of truth for the whole design — recursive comparability classification,
generated-source splicing, and two-pass-resolver idempotency interact there.
Everything else is either sensitive-but-mapped surgery (Opus) or mechanical
(Sonnet). **Execute strictly in order. One packet = one commit.** Do not start
packet N+1 until packet N's acceptance section is green.

## Architecture map (verified 2026-07-16, current master @ fa3b577)

Pipeline (`src/main.cpp:426-510`): Resolver pass 1 (`run` → `desugarEnums`
first, `Resolver.cpp:6049`) → RuleEngine iff `program.hasMeta` → Resolver
pass 2 over the SAME `Program` iff rules changed it (`main.cpp:466-472`) →
Checker → engine.

Five engines. Four are actively maintained and must stay byte-identical:

| Engine | Entry | Struct `==` today (the code to exterminate) |
|---|---|---|
| Oracle | `src/Eval.cpp` | `combine` fallback → `keyEquals`, `Eval.cpp:1073-1082` |
| IR interp | `src/IrInterp.cpp` | `objectArith` fallback → `keyEquals`, `IrInterp.cpp:728-735` |
| emit-C++ | `src/CGen.cpp` | emitted `opm` fallback → emitted `keyEq`, `CGen.cpp:1560-1567` (keyEq body emitted at `CGen.cpp:231`) |
| LLVM | `src/LlvmGen.cpp` + `runtime/lv_runtime.c` | `lvrt_opm` fallback → `lvrt_keyeq`, `lv_runtime.c:1326-1337` / `lv_runtime.c:2075` |
| X64/ELF | `src/X64Gen.cpp` | **FROZEN** — never received #77, never extended. Do not touch. |

Shared value model: `src/RuntimeValue.hpp` — `VKind` enum (line 36), floats
are a real `double f` (line 43). `keyEquals` (line 242) is the Map-key
comparator used by oracle+IR; it STAYS (Map keys need it) — only the struct
`==` operator fallbacks die. Its float leg (`a.f == b.f`, line 249) and the
missing-`LV_FLOAT` bit-compare in `lvrt_keyeq` (`lv_runtime.c:2090`) get the
canonical fix in packet 05.

The generated-decl channel (the §5.5 mechanism) is proven in-tree:
`Resolver::desugarEnums` (`Resolver.cpp:5944-6046`) generates source text
including `bool (==)(N o) => _ord == o._ord;` (line 6000), parses it with the
real Lexer/Parser into `program.synthFiles` (deque = stable string_view
backing, `Ast.hpp:502-508`), and splices the decls in. Operator methods are
ordinary slots: parser captures `(==)` selector text (`Parser.cpp:1163-1174`),
`slotOf` names the slot `"=="` (`Resolver.cpp:5620`), every engine's
`findMethod(cls, "==")` then dispatches it (`Eval.cpp:1069`,
`IrInterp.cpp:724`, CGen's opm method table, `lvrt_opm` method lookup).

Checker seam: `typeOfBinary` (`Checker.cpp:1882-2073`). Value-struct `==`
with no `(==)` currently falls to `return unknown()` at `Checker.cpp:2066` —
no comparability check exists. Reference-class identity `==` is the branch at
`Checker.cpp:2060-2065` and MUST survive unchanged.

## House rules (binding on every packet)

- **Engine-differential at every step.** After each packet: build, then run
  the four lanes on the equality cluster and the full pre-existing suite.
  Byte-identical output across oracle/IR/emit-C++/LLVM or you STOP.
- **STOP-and-escalate** on any cross-engine divergence you did not introduce
  and cannot explain. Do not paper over it, do not pin around it.
- **Corpus pins land in the same commit as the code they pin.**
- **The working tree has unrelated in-flight const-system edits**
  (`src/Parser.cpp`, `src/Parser.hpp`, `tests/test_checker.cpp`,
  `designs/*const*`, `tests/corpus/const_section.*`). Never revert or reflow
  them. When you touch `tests/test_checker.cpp`, append your tests; commit
  only the hunks belonging to this effort (`git add -p` discipline).
- The frozen X64/ELF backend gets **zero** edits. The equality cluster lives
  under `composition/` which has no ELF lane, so it never sees these tests.
- No prelude-`.lev` canon, ever (design decision 3): canon is engine-level,
  integer/branchless form only.

## Build & verify commands

```sh
cmake --build build -j                       # incremental build
ctest --test-dir build -j8                   # full suite
ctest --test-dir build -R composition       # the equality cluster's 4 lanes
build/leviathan --run  <file.lev>            # oracle
build/leviathan --ir   <file.lev>            # IR interp
bash tests/run_native.sh      build/leviathan <file-or-dir>   # emit-C++
bash tests/run_native_llvm.sh build/leviathan <file-or-dir> runtime/lv_runtime.c  # LLVM (check CMake for exact runtime srcs: LANG_LVRT_SOURCES)
```

Checker-diagnostic tests use `tests/test_checker.cpp`'s `ERROR_HAS(src, msg)`
macro (line 49); build target `checkertests`.

## Definition of done (whole effort — packet 08 verifies)

All four engines byte-identical on every green pin; no pre-existing golden
moves; design doc status header flipped and the doc moved to
`designs/complete/`; `info.md` §9 + `docs/reference.md` sentences landed.
(#77's tracker entry and footgun row were already swept in the d68f1e8 merge —
packet 08 verifies rather than re-does.)
