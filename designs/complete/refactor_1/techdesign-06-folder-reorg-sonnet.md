# Refactor 1 — Session 06: Folder Reorg + Layering Enforcement (sonnet)

> Goal: make the logical layering of `docs/archectecture.md` physical, then
> make it enforceable. ONE atomic commit containing only the moves, include
> updates, CMake path updates, and the layering check. Runs SOLO after
> sessions 02–05 have merged; all agent branches must be synced to master
> before starting. Date: 2026-07-24. Depends on: 02, 03, 04, 05.

## Files owned by this session

Every file under `src/` EXCEPT `src/X64.hpp`, `src/X64.cpp`,
`src/X64Gen.hpp`, `src/X64Gen.cpp`; plus `CMakeLists.txt` and new
`tools/check_layering.sh`.

## Target layout (fixed — this is the decision, not a proposal)

| Directory | Files (post-split names included) |
|---|---|
| `src/core/` | `Source.hpp`, `Diagnostic.{hpp,cpp}`, `Token.{hpp,cpp}`, `Ast.hpp`, `Symbols.hpp`, `LexicalStack.hpp` |
| `src/frontend/` | `Lexer.{hpp,cpp}`, `Parser.{hpp,cpp}`, `ParserInternal.hpp`, `ParserExpr.cpp`, `ParserMeta.cpp` |
| `src/sema/` | `Resolver.{hpp,cpp}`, `Checker.{hpp,cpp}`, `CheckerInternal.hpp`, `CheckerInfer.cpp`, `CheckerFlow.cpp`, `CheckerDispatch.cpp`, `CheckerGenerics.cpp`, `CheckerReify.cpp` |
| `src/meta/` | `Rules.{hpp,cpp}`, `RulesInternal.hpp`, `RulesExpand.cpp`, `RulesClone.cpp`, `AstPrinter.{hpp,cpp}` |
| `src/ir/` | `Ir.hpp`, `Lower.{hpp,cpp}`, `Ownership.{hpp,cpp}`, `MemVerify.{hpp,cpp}` |
| `src/backend/` | `IrInterp.{hpp,cpp}`, `LlvmGen.{hpp,cpp}`, `LlvmGenInternal.hpp`, `LlvmGenOps.cpp`, `LlvmGenGlue.cpp`, `CGen.{hpp,cpp}`, `NativeRuntime.cpp` |
| `src/runtime/` | `RuntimeValue.hpp`, `RuntimeCore.{hpp,cpp}`, `RuntimeNatives.cpp`, `RuntimeLoop.{hpp,cpp}`, `Eval.{hpp,cpp}` |
| `src/driver/` | `main.cpp`, `Project.{hpp,cpp}`, `BuildPlan.{hpp,cpp}`, `PreludeEmbedded.hpp` |
| `src/` (root, UNTOUCHED) | `X64.hpp`, `X64.cpp`, `X64Gen.hpp`, `X64Gen.cpp` — frozen; never `git mv`'d, never edited. Their CMake entries keep their current paths. |

## Steps

1. Verify preconditions: sessions 02–05 are on origin/master; local worktree
   clean; `git fetch` + fast-forward. If any precondition fails: STOP.
2. `git mv` every file per the table (X64 files excepted).
3. Include style (fixed decision): all `#include "..."` directives among
   moved files become directory-qualified relative to `src/`
   (e.g. `#include "sema/Checker.hpp"`, `#include "core/Ast.hpp"`), and
   `src/` is added to the target include path in CMake
   (`target_include_directories(... PRIVATE src)`). The frozen X64 files keep
   compiling because includes they use (`Ir.hpp` etc.) — X64 files may NOT be
   edited, so `src/ir` and `src/core` must ALSO be on the include path so
   their existing `#include "Ir.hpp"`-style directives still resolve. Add
   exactly the include dirs needed for X64 to build unmodified:
   `src/ir`, `src/core`, `src/runtime` (adjust to the actual set the build
   reports, adding directories only — never editing X64 sources).
4. Update `CMakeLists.txt` source paths to the new locations (X64 entries
   unchanged).
5. Add `tools/check_layering.sh` and wire it as a ctest test named
   `layering`. It greps `#include` lines and FAILS on:
   - anything under `src/core/` including from any other `src/` directory
   - `src/frontend/` including from `sema/ meta/ ir/ backend/ runtime/ driver/`
   - `src/ir/` including from `sema/ meta/ backend/ driver/` (`core/` allowed)
   - `src/backend/` including from `sema/ meta/ frontend/ driver/`
     (`ir/ core/ runtime/` allowed)
   - `src/runtime/` including from `sema/ frontend/ backend/ driver/`
     (`core/ meta/`? NO — `runtime/` may include only `core/` and `ir/`;
     `Eval` needs `Ast`/`Symbols` which are `core/`)
   - X64 files are exempt from the checker (skip them by path).
   If the existing code violates a rule above, the rule is NOT weakened and
   the code is NOT changed: STOP and escalate with the exact include edge, so
   the owner can either bless an exemption line or schedule a fix.
6. One commit: moves + includes + CMake + checker. Nothing else. Push per
   policies.

## Validation

- Full clean build; `ctest -j4` green including the new `layering` test
  (flake policy per overview).
- `git log --follow` spot-check on 3 moved files confirms history is
  preserved.
- `git diff --stat` contains zero lines for `X64*` files.

## Ending state (fixed)

Layout exactly as the table; includes directory-qualified; `layering` ctest
enforcing the dependency map of `docs/archectecture.md` §4; X64 files
untouched at `src/` root. `docs/archectecture.md` §"flat directory" paragraph
updated to describe the new layout (documentation edit, permitted).

## STOP-and-escalate

Escalate on: any precondition failure; any layering violation found in step
5; any X64 build breakage that cannot be fixed by adding an include
directory; any merge conflict (per policies, sonnet does not resolve code
conflicts).
