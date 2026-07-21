# Refactor 1 — Session 01: Build Scaffold (sonnet)

> Goal: pre-create every new translation unit and header that sessions 02–05
> will fill, and register them in `CMakeLists.txt`, in ONE commit — so the
> parallel sessions never touch the build file and cannot conflict on it.
> Date: 2026-07-21. Depends on: nothing. Blocks: 02, 03, 04, 05.

## Files owned by this session

- `CMakeLists.txt` (edit)
- All new files listed below (create)

## Steps

1. Create the following files, each containing only a comment header naming
   its session and the include(s) shown:

   | New file | Contents (initial) | Filled by |
   |---|---|---|
   | `src/RuntimeCore.hpp` | header guard + `#include "RuntimeValue.hpp"` + `#include "Symbols.hpp"` | 02 |
   | `src/RuntimeCore.cpp` | `#include "RuntimeCore.hpp"` | 02 |
   | `src/CheckerInternal.hpp` | header guard + `#include "Checker.hpp"` | 03 |
   | `src/CheckerInfer.cpp` | `#include "CheckerInternal.hpp"` | 03 |
   | `src/CheckerFlow.cpp` | `#include "CheckerInternal.hpp"` | 03 |
   | `src/CheckerDispatch.cpp` | `#include "CheckerInternal.hpp"` | 03 |
   | `src/CheckerGenerics.cpp` | `#include "CheckerInternal.hpp"` | 03 |
   | `src/CheckerReify.cpp` | `#include "CheckerInternal.hpp"` | 03 |
   | `src/RulesInternal.hpp` | header guard + `#include "Rules.hpp"` | 04 |
   | `src/RulesExpand.cpp` | `#include "RulesInternal.hpp"` | 04 |
   | `src/RulesClone.cpp` | `#include "RulesInternal.hpp"` | 04 |
   | `src/ParserInternal.hpp` | header guard + `#include "Parser.hpp"` | 04 |
   | `src/ParserExpr.cpp` | `#include "ParserInternal.hpp"` | 04 |
   | `src/ParserMeta.cpp` | `#include "ParserInternal.hpp"` | 04 |
   | `src/LlvmGenInternal.hpp` | header guard + `#include "LlvmGen.hpp"` | 05 |
   | `src/LlvmGenOps.cpp` | `#include "LlvmGenInternal.hpp"` | 05 |
   | `src/LlvmGenGlue.cpp` | `#include "LlvmGenInternal.hpp"` | 05 |

2. `CMakeLists.txt`: add every new `.cpp` above to the same source list that
   holds its parent file today — the `langfront` list that currently carries
   `src/Checker.cpp`, `src/Rules.cpp`, `src/Parser.cpp`, `src/Eval.cpp`,
   `src/IrInterp.cpp` (`RuntimeCore.cpp`, `Checker*.cpp`, `Rules*.cpp`,
   `Parser*.cpp` go there). `LlvmGenOps.cpp` and `LlvmGenGlue.cpp` go next to
   the existing `target_sources(langfront PRIVATE src/LlvmGen.cpp)` line,
   inside the same conditional block.
3. Do **not** touch `src/X64*`, and do not add any other content to the new
   files.

## Validation

- Full clean build succeeds.
- `ctest -j4` green (rerun any straggler individually; the
  `regex_pathological_linear` host-timeout is a known flake).

## Ending state (fixed)

All 17 files above exist, are empty-but-compiling, and are built by CMake.
Sessions 02–05 can proceed in parallel without editing `CMakeLists.txt`.

## STOP-and-escalate

If a listed file already exists, if the CMake source lists are not where
described, or if the build fails for any reason other than a typo in this
session's own edits: STOP and escalate. Do not restructure CMake.
