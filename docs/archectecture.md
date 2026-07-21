# Leviathan Compiler — `src/` Architecture

> Scope: this document describes the C++ host compiler that lives in `src/` — the
> program that reads Leviathan source (`.lev`) and lexes, parses, resolves,
> expands metaprogramming, type-checks, lowers, and finally interprets or emits
> code for it. It does **not** cover the `.lev` prelude, the `trident` package
> manager, or the language libraries; those live outside `src/`.
>
> `src/` is organized into **eight layer directories** (~38,800 lines):
> `core/` (Source/Diagnostic/Token/Ast/Symbols/LexicalStack), `frontend/`
> (Lexer/Parser), `sema/` (Resolver/Checker), `meta/` (Rules/AstPrinter),
> `ir/` (Ir/Lower/Ownership/MemVerify), `backend/` (IrInterp/LlvmGen/CGen/
> NativeRuntime), `runtime/` (RuntimeValue/RuntimeCore/RuntimeNatives/
> RuntimeLoop/Eval), and `driver/` (main/Project/BuildPlan/PreludeEmbedded).
> The layering below is physical, enforced by `tools/check_layering.sh` (the
> `layering` ctest) over directory-qualified includes. The frozen X64 files
> (`X64.{hpp,cpp}`, `X64Gen.{hpp,cpp}`) stay at the `src/` root, untouched.
> Function/line counts are given per file; the function counts are approximate
> (they are derived by a brace-and-signature scan and include methods and
> non-trivial local lambdas).

---

## 1. The big picture: one pipeline, several tails

Leviathan is a whole-program compiler with **one front end** feeding **four
interchangeable back ends**. Everything is driven from `main.cpp`. The nominal
flow for a single compilation is:

```
 source text
    │
    ▼
 ┌────────┐   tokens    ┌────────┐   AST     ┌───────────┐
 │ Lexer  │ ──────────► │ Parser │ ────────► │ Resolver  │  (pass 1: names,
 └────────┘             └────────┘           │           │   scopes, shapes,
   Token.cpp             Ast.hpp             └───────────┘   prelude gather)
                                                   │
                                                   ▼
                                             ┌───────────┐   expands macros,
                                             │ RuleEngine│   rules, attributes,
                                             │  (Rules)  │   comptime folds
                                             └───────────┘   (uses Evaluator)
                                                   │
                                                   ▼
                                             ┌───────────┐  (pass 2: re-resolve
                                             │ Resolver  │   the rewritten tree)
                                             └───────────┘
                                                   │
                                                   ▼
                                             ┌───────────┐   type inference,
                                             │  Checker  │   diagnostics,
                                             └───────────┘   generic monomorph.
                                                   │
                          ┌────────────────────────┴────────────────────┐
                          ▼                                              ▼
                    ┌───────────┐  IrModule                        ┌───────────┐
                    │  Lowerer  │ ──────────┐                      │ Evaluator │
                    │  (Lower)  │           │                      │  (Eval)   │
                    └───────────┘           │                      └───────────┘
                          │                 │                     AST tree-walker,
        ┌─────────────┬───┴───┬─────────────┤                     also the comptime
        ▼             ▼       ▼             ▼                     engine for Rules
   ┌─────────┐  ┌─────────┐ ┌──────┐  ┌──────────┐
   │IrInterp │  │ LlvmGen │ │ CGen │  │  X64Gen  │  (frozen native backend)
   └─────────┘  └─────────┘ └──────┘  └──────────┘
    IR interp    LLVM IR     C++ src   direct machine code
    (--run)      → clang     → g++     (do not modify)
```

Two distinct execution strategies coexist:

* **The IR path** — `Lowerer` turns the checked AST into a compact linear
  `IrModule` (see `Ir.hpp`). That module is then either interpreted
  (`IrInterp`) or handed to a code generator (`LlvmGen`, `CGen`, `X64Gen`).
* **The AST-evaluator path** — `Evaluator` walks the AST directly. Its primary
  job is **compile-time evaluation** for the metaprogramming stage (constant
  folding, macro argument evaluation, `import()`), but it is a full interpreter
  in its own right.

Both interpreters share the same runtime value model (`RuntimeValue.hpp`), the
same native builtins (`RuntimeNatives.cpp`), and the same event loop
(`RuntimeLoop.cpp`), which is what keeps `--run`, the comptime evaluator, and
the compiled backends behaviourally identical.

---

## 2. File groups

### 2.1 Shared data model (the "vocabulary" every stage speaks)

These headers define the types passed between stages. They are almost pure
declarations — the whole compiler `#include`s them.

| File | LOC | Role |
|------|----:|------|
| `Source.hpp` | 29 | `SourceSpan` (offset+length into a file), `SourceFile`, and `lineColAt()` — the coordinate system for every diagnostic. |
| `Token.hpp` / `Token.cpp` | 164 / 244 | `TokenKind` enum, `Token` struct, keyword table, and literal decoders (`parseIntLiteral`, `parseFloatLiteral`, `decodeStringLiteral`, escape handling). ~15 functions. |
| `Ast.hpp` | 596 | The **AST**: `Expr`/`Stmt` (tagged unions via `ExprKind`/`StmtKind`), `TypeRef`, `Param`, `Member`, `Program`, plus rule/macro nodes (`RuleMatch`, `RuleAction`, `AttrUse`, `AstPayload`). Every other file revolves around these node types. Almost no logic. |
| `Symbols.hpp` | 393 | The **semantic model**: `Symbol` (classes, functions, namespaces, vars), `Scope` (lexical name resolution + imports), `Slot`/`Shape` (a class's laid-out fields incl. columnar layout helpers), and `Sema` (the arena that owns all symbols and scopes, with interning). ~13 inline helpers. |
| `LexicalStack.hpp` | 63 | `LexicalStack`/`LexicalFrame` — a small scope stack used during resolution/checking to answer nearest-wins `bind` lookups and namespace-in-scope queries. |
| `Diagnostic.hpp` / `Diagnostic.cpp` | 40 / 59 | `Severity`, `Diagnostic`, and `DiagnosticSink` — the `error/warning/note` collector every stage writes into, plus terminal rendering. ~5 functions. |

### 2.2 Front end — text to AST

| File | LOC | ≈fns | Structure |
|------|----:|-----:|-----------|
| `Lexer.hpp` / `Lexer.cpp` | 64 / 273 | 31 | `Lexer` class. `tokenize()` / `tokenizeRange()` drive a hand-written scanner: `skipTrivia`, `lexNumber`, `lexIdentifier`, `lexString`, `lexRawString`, `lexTripleString`, `lexQuasi` (for metaprogramming quasi-quotes). Produces `std::vector<Token>`. |
| `Parser.hpp` / `Parser.cpp` | 138 / 2351 | ~257 | `Parser` class — a **Pratt / recursive-descent** parser. Expression side is precedence-climbing (`parseExpr(minBP)`, `parseUnary`, `parsePostfix`, `parsePrimary`). Statement/decl side has one method per construct (`parseClass`, `parseEnum`, `parseNamespace`, `parseBind`, `parseUses`/`parseUse`, `parseRule`, `parseMacroDecl`, `parseAttributeDecl`). Substantial machinery for the splice/fork/fragment metaprogramming grammar (`parseForSpliceStmt`, `parseForkSpliceStmt`, `parse*Fragment`) and for disambiguating lambdas vs. parenthesised types (`typedParamAhead`, `looksLikeLambda`, `skipTypeLA`). `synchronize()` gives error recovery. Emits a `Program`. |

### 2.3 Semantic analysis — names, types, diagnostics

| File | LOC | ≈fns | Structure |
|------|----:|-----:|-----------|
| `Resolver.hpp` / `Resolver.cpp` | 115 / 1623 | ~59 | `Resolver` class — runs **twice** (before and after the rule stage). Gathers declarations into scopes (`gatherInto`, `gatherClass`), wires imports (`processImports`, `importOne` for `uses`, `useOne` for selective `use`), registers factory `bind`s, resolves every `TypeRef` to a `Symbol`, and computes class **shapes** (`buildShape`, `mergeSlot`, `slotOf` — this is where inheritance/diamond layout is resolved). Also owns `parsePrelude()` and the desugarings that run before checking: `desugarEnums`, `synthesizeStructEquality`, `synthesizeFloatNaN`. |
| `Checker.hpp` / `Checker.cpp` | 527 / 5336 | ~301 | The **type checker** — by far the largest single stage. `Checker` defines its own `Type` lattice (`TKind`) separate from the AST's `TypeRef`. `run()` walks every function; `typeOf*` (`typeOfMember`, `typeOfCall`, `typeOfBinary`, …) is the inference core. Also hosts: flow-sensitive narrowing (`analyzeCond`, `Fact`, `unionMinus`, `invalidatePath`), override/dispatch resolution (`buildOverrideIndex`, `resolveDispatch`, `dispatchesDynamically`), **generic monomorphization** (`markSpecializationSites`, `materializeSpecializations`, `specializeValueStruct`), method-reference eta-expansion (`tryResolveMethodRef`), and expression **reification** for the `expr`/ORM feature (`reifyNode`, `makeExprNode`, `buildBindsExpr`). This is the semantic authority the backends trust. |

### 2.4 Metaprogramming — rules, macros, attributes, comptime

| File | LOC | ≈fns | Structure |
|------|----:|-----:|-----------|
| `Rules.hpp` / `Rules.cpp` | 414 / 2864 | ~153 | `RuleEngine` — the whole metaprogramming stage, running between the two Resolver passes. Two phases: **(1)** comptime folding + attribute resolution (`foldExpr`, `foldParamDefaults`, `processAttrs`, `resolveAttr`, `evalAttrArgs`, `reify`), and **(2)** declaration-level `rule`/`macro` expansion (`collectRules`, `orderRules`, `matchAndExpandRule`, `runReentrantFixpoint`). Carries a hygienic AST cloner (`cloneStmt`/`cloneExpr`/`cloneType`, `collectTemplateLocals` for renames), splice-site indexing (`indexSpliceSites`, named anchors), and `meta::` reflection value construction (`buildMetaValue`). Delegates all actual evaluation to `Evaluator`. Anything it rewrites is re-resolved and re-checked, so generated code is validated exactly like hand-written code. |
| `AstPrinter.hpp` / `AstPrinter.cpp` | 39 / 932 | ~37 | `printProgram()` — renders an AST back to source or debug text. Powers `--ast`, `--ast-after-rules`, and `--expand` (showing macro output). `ExpandProvenance` tracks which spans came from expansion. A file-static side channel carries the "elidable literal" hint for source-shaped printing. |

### 2.5 IR and the mid-level

| File | LOC | ≈fns | Structure |
|------|----:|-----:|-----------|
| `Ir.hpp` | 167 | ~1 | The **intermediate representation**: `Op` (the opcode enum), `Inst` (op + up to 4 integer operands), `IrFunction`, `IrModule`, plus `computeFnGlobalRefs` (global-init dependency ordering). A flat, register-numbered, linear IR — the common currency of all four back ends. |
| `Lower.hpp` / `Lower.cpp` | 211 / 2347 | ~98 | `Lowerer` — translates the checked AST into an `IrModule`. `lowerStmt`/`lowerExpr`/`lowerCall`/`lowerLambda`/`lowerAssign` are the core walk. Owns register allocation (`emit`, `addConst`, `findLocal`, `thisReg`), field-slot resolution (`fieldSlotOf`, `packedSlot`), closure capture, and the structured-cleanup machinery for `using`/loop exits (`ExitChain`, `emitCloseCall`, `lowerUsingCleanupGroups`). Handles the columnar (`--no-columnar`) Array-of-struct layout decision and fresh-struct/`VFree` ownership hints. |

### 2.6 Back ends (from the IR)

| File | LOC | ≈fns | Structure |
|------|----:|-----:|-----------|
| `IrInterp.hpp` / `IrInterp.cpp` | 106 / 1011 | ~69 | `IrInterp` — the **IR interpreter**, the default `--run` executor. `run()`/`call()` execute `IrModule` instructions over `RuntimeValue`s. Implements object/member/index access (`getMember`, `setMember`, `getIndex`, `indexStore`), dynamic dispatch (`classOfValue`, `findMethodByName`), arithmetic (`objectArith`), exceptions (`raise`/`raiseClass`), and the task/async machinery (`taskRunProgram`, `taskLoopStep`, `taskPollPromise`) integrating with `RuntimeLoop`. |
| `LlvmGen.hpp` / `LlvmGen.cpp` | 38 / 3686 | ~99 | `LlvmGen::emitIr()` — emits textual **LLVM IR** from the `IrModule`; `main.cpp` then invokes `clang` to produce a native binary. This is the **primary AOT backend** for the portable/self-hosting track. Tiny header, huge implementation (one op-lowering routine per IR op, plus string/closure/exception runtime glue that binds to `NativeRuntime`). |
| `CGen.hpp` / `CGen.cpp` | 50 / 1720 | ~36 | `CGen::generate()` — emits **C++ source** embedding a scalar mini-runtime whose `arithPrim`/`valueToString` match the interpreters bit-for-bit. `main.cpp` compiles it with `g++`. Generates dispatch tables (`genDispatchers`, `genCallM`, `genClosureDispatch`), field-layout helpers, and one function body per IR function. A simpler, dependency-light alternative to the LLVM path. |
| `X64Gen.hpp` / `X64Gen.cpp` | 192 / 4249 | ~123 | **FROZEN — do not modify, test, or extend.** A direct x86-64 machine-code back end: one `gen*` method per IR op and per native builtin (`genSysWrite`, `genTcpConnect`, `genStrConcat`, `genCallClosure`, ARC helpers `genRetain`/`genRelease`, …). Emits bytes through `X64.hpp`'s assembler. Retained for historical reference only; the portable pivot made the LLVM path primary and froze this backend. |
| `X64.hpp` / `X64.cpp` | 181 / 57 | ~5 | The low-level assembler/emitter (`Asm`) used only by `X64Gen`: REX/ModRM encoding, one method per instruction (`movRR`, `alu`, SSE `addsd`/`cvtsi2sd`, `rep` string ops), and the static-executable container writer. Part of the frozen native backend. |
| `NativeRuntime.cpp` | 128 | ~14 | The tiny **C ABI runtime linked into LLVM-compiled binaries** (`kind`-tagged values, string builders). Its semantics replicate the shared scalar `arithPrim`/`valueToString`, keeping compiled output identical to the interpreters. |

### 2.7 Runtime support (shared by both interpreters)

| File | LOC | ≈fns | Structure |
|------|----:|-----:|-----------|
| `RuntimeValue.hpp` | 503 | ~22 | The **runtime value model**: `Value` (a `VKind` tag + payload: int/float/bool/string/`Object`/`Closure`/`Array`/`Map`/`Char`/`Block`/`Ast`), `Object`, `Closure`, `BlockData`. Inline constructors (`varr`, `vblock`, `vast`), `copyValue`, UTF-8 encode/decode (`utf8Encode`, `utf8DecodeAt` — with overlong/surrogate rejection), `valueToString`, and equality/default helpers. Shared by `Evaluator` and `IrInterp`. |
| `Eval.hpp` / `Eval.cpp` | 297 / 2345 | ~123 | `Evaluator` — the **AST tree-walking interpreter and comptime engine**. `eval`/`exec`/`evalCall`/`evalBinary`/`evalAssign` walk the AST; `evalComptime`/`finishComptime`/`callComptimeMacro`/`comptimeImport` are the entry points `RuleEngine` calls during metaprogramming. Also a full runtime: object construction (`initFields`, `callClosure`), method resolution (`findMethod`, `findAccessor`), pattern matching (`matchesValue`), exceptions, and the async/task state machine (`saveTaskState`, `taskRunProgram`, `taskPollPromise`) over `RuntimeLoop`. |
| `RuntimeNatives.cpp` | 2084 | ~122 | The **native builtin library** backing both interpreters (dispatched via each interpreter's `taskNative`). Free functions implementing `sys*` I/O and OS calls: file/stat/dir, sockets + DNS, TLS (OpenSSL behind helpers), terminal raw-mode + winsize, signals-as-streams (`signalfd`), timers, RNG, string ops, and OS-thread value copying. Includes `RuntimeValue.hpp` + `RuntimeLoop.hpp`; the single place OS surface is reached. |
| `RuntimeLoop.hpp` / `RuntimeLoop.cpp` | 83 / 165 | ~16 | `RuntimeLoop` (singleton) — the **event loop**: timers (`addTimer`/`cancelTimer`), fd read/write watches (`addWatch`/`addWriteWatch`), TLS want-read/want-write bookkeeping, and `nextBatch()` which the interpreters pump. `reset()` gives each program run a fresh loop. |
| `Ownership.hpp` / `Ownership.cpp` | 49 / 351 | ~22 | `analyzeOwnership()` — an interprocedural escape/ownership analysis over the `IrModule`, producing `OwnershipInfo` (alloc sites, escape summaries) with per-native annotations. Feeds free/retain decisions and the `--ownership` report. |
| `MemVerify.hpp` / `MemVerify.cpp` | 115 / 46 | ~1 | `MemVerifier` — an allocation tracker used under a debug/verify mode (`onAlloc`/`sweep`/`report`) to detect leaks/double-frees when the IR interpreter runs with `memVerify` on. |

### 2.8 Project loading, build plans, and the driver

| File | LOC | ≈fns | Structure |
|------|----:|-----:|-----------|
| `Project.hpp` / `Project.cpp` | 241 / 899 | ~65 | Multi-file **project model** and the analysis reports the CLI exposes: `loadProjectFromPlan`, `ProjectFile`/`LoadedProject`, the import graph (`FileImports`, `UsesGraph`), namespace inventory (`NamespaceInfo`), `why`-a-symbol-is-visible tracing (`WhyResult`), and namespace-layout linting (`lintNamespaceLayout`). Each has a `render*` for text output. Keeps project/dependency logic *out* of the compiler core (compiler ↔ `trident` separation). |
| `BuildPlan.hpp` / `BuildPlan.cpp` | 84 / 189 | ~2 | `readBuildPlan()` — a small dedicated grammar (reusing the Lexer/Token) over the machine-generated build plan that `trident` emits (`PlanSource`, `PlanEdge`, `PlanAsset`, entry mode). Deliberately *not* the `trident.toml` manifest parser. |
| `PreludeEmbedded.hpp` | 15 | — | Declares the ordered table of embedded prelude segments (`PreludeSegment`). The actual bytes are build-generated (`build/generated/PreludeEmbedded.cpp`) from the `prelude/*.lev` files, used as a fallback when no prelude directory is found on disk. |
| `main.cpp` | 905 | ~15 | The **driver/CLI**. `main()` parses flags, finds the prelude directory (`--prelude` → `LV_PRELUDE_DIR` → next-to-binary → source tree → embedded), then constructs and sequences every stage: `Lexer` → `Parser` → `Resolver` → `RuleEngine` → second `Resolver` → `Checker` → then dispatches to `Evaluator` (`--run` comptime), `Lowerer` + `IrInterp` (`--run`), or a codegen backend. Also owns `--build` (locating a `clang`/`g++`/cross linker driver via `probeLinkerDriver`/`probeCrossLinkerDriver`) and the many debug/report hatches (`--ast`, `--expand`, `--rules`, `--ownership`, `--dump-shapes`, …). |

---

## 3. How a compilation actually runs (control flow through `main.cpp`)

1. **Read + lex.** `main.cpp` loads the entry file (or a build plan via
   `BuildPlan`/`Project` for multi-file builds) into a `SourceFile`, then runs
   `Lexer::tokenize()` → `std::vector<Token>`.
2. **Parse.** `Parser::parseProgram()` → a `Program` (vector of top-level
   `Stmt`s). The prelude is parsed the same way via `Resolver::parsePrelude()`.
3. **Resolve (pass 1).** `Resolver::run()` gathers symbols, wires `uses`/`use`
   imports, resolves types, and builds class shapes. Enum/struct-equality/NaN
   desugarings happen here.
4. **Metaprogram.** `RuleEngine::run()` folds comptime expressions (calling
   `Evaluator::evalComptime`), resolves attributes, and expands `rule`/`macro`
   declarations to a fixpoint, cloning AST hygienically.
5. **Resolve (pass 2).** A fresh `Resolver` re-resolves the rewritten tree so
   generated code has real symbols.
6. **Check.** `Checker::run()` infers and validates types, resolves dispatch,
   monomorphizes generics, and records diagnostics into the `DiagnosticSink`.
   If any errors exist, `main.cpp` renders them and stops.
7. **Execute or emit** — one of:
   * `--run`: `Lowerer::lower()` → `IrModule`, then `IrInterp::run()`
     (optionally under `Ownership`/`MemVerify`). The tree-walking `Evaluator`
     is also available for comptime and simpler runs.
   * Codegen: `Lowerer` → `IrModule` → `LlvmGen::emitIr()` (then `clang`),
     `CGen::generate()` (then `g++`), or the frozen `X64Gen::emit()`.

Throughout, every stage shares the `Sema` arena (symbols/scopes), the
`DiagnosticSink`, and — for anything that *runs* code — the
`RuntimeValue`/`RuntimeNatives`/`RuntimeLoop` trio, which is what guarantees the
interpreter and the compiled backends behave identically.

---

## 4. Dependency map (who includes whom, conceptually)

* **Everyone** depends on `Ast.hpp`, `Symbols.hpp`, `Source.hpp`,
  `Diagnostic.hpp`, `Token.hpp`.
* **Front end:** `Lexer` → `Token`; `Parser` → `Token` + `Ast`.
* **Semantic:** `Resolver`, `Checker` → `Ast` + `Symbols` (+ `LexicalStack`).
* **Metaprog:** `Rules` → `Resolver`'s `Sema`, `Evaluator`, `AstPrinter`,
  `RuntimeValue`.
* **Mid/back end:** `Lower` → `Ast` + `Symbols` + `Ir`; each backend
  (`IrInterp`, `LlvmGen`, `CGen`, `X64Gen`) → `Ir`.
* **Runtime:** `Eval` + `IrInterp` → `RuntimeValue` + `RuntimeNatives` +
  `RuntimeLoop`; `LlvmGen`/`CGen` output links `NativeRuntime`.
* **Driver:** `main` → *all of the above* + `Project`/`BuildPlan`.

---

### Notes on the counts

- Line counts are exact (`wc -l`). Function counts are **approximate** — they
  come from a signature scan and fold in methods and non-trivial local lambdas,
  so a few code-generator files (which use many small lambdas) read high. Use
  them as relative-size signals, not exact tallies. Total ≈ 38,800 lines across
  50 files.
- The `X64.*` / `X64Gen.*` native backend is **frozen**: it is documented here
  for completeness but must not be modified, run, or tested.
