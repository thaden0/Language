# Design Challenge Difficulty Ratings

This rates implementation difficulty for the finished or implementation-ready technical
designs in `designs/`, `designs/complete/`, and `designs/atlantis/`.

Scale: 1 is a small, localized task a weaker model should handle reliably. 8 is a
large cross-cutting compiler/runtime/framework effort that needs strong planning,
source reading, and verification discipline. The buckets are meant to be roughly
even, but the rating favors honest difficulty over exact 12.5% distribution.

## Inclusion Rules

Included:

- `techdesign-*` files that describe an implementable track.
- Finished historical designs in `designs/complete/`, because they calibrate task
  difficulty even when already implemented.
- Deferral-resolution docs whose status is ready, ratified, or otherwise describes a
  concrete implementation path.
- Atlantis track docs `01` through `09`.

Excluded:

- `request-*.md` files without a finished tech design.
- Direction-finding proposals that are superseded by a tech design
  (`proposal-metaprogramming.md`, `proposal-package-manager.md`,
  `proposal-web-framework.md`) or have no finished tech design
  (`proposal-wasm-frontend.md`).
- Overview/anchor docs that duplicate child tracks (`techdesign-00-overview.md`,
  `atlantis/techdesign-00-overview.md`).
- Trackers/registers that explicitly schedule no implementation
  (`techdesign-http-and-streams-maturity.md`, `techdesign-metaprogramming-tail.md`,
  `deferal-performance-optimizations.md`, `waiting/trident-post-v1--waits-on-selfhost-g5.md`,
  `techdesign-track03-type-surface.md`, `suggested-features.md`).
- Exploratory designs with remaining open questions and no settled implementation
  target (`system-binds.md`).

## Ratings

| Design | Difficulty | Why |
|---|---:|---|
| `designs/complete/exit-codes.md` | 1 | Tiny surface: process exit status and exception-to-exit mapping. Mostly driver/runtime plumbing plus shell-observable tests. |
| `designs/complete/techdesign-06-stdlib-math.md` | 1 | Mostly prelude/native math wrappers and numeric mask additions. The ELF split needs care, but the semantic surface is narrow. |
| `designs/terminal-winsize.md` | 1 | Small terminal/platform query API with a clear fallback path. Testing with a pty is the hardest part. |
| `designs/complete/argv.md` | 2 | Conceptually simple, but it touches CLI parsing and multiple execution engines, so regressions can hide in invocation details. |
| `designs/complete/const.md` | 2 | Front-end-only language rule with parser/checker changes and clear diagnostics. No runtime or backend work. |
| `designs/complete/terminal-raw-mode.md` | 2 | Local platform feature, but terminal restoration and pty tests raise the bar above a simple native wrapper. |
| `designs/deferal-transcendental-math.md` | 2 | Focused backend/runtime completion for math functions. The main risk is consistent coverage across native paths, not design ambiguity. |
| `designs/signals.md` | 2 | Small API, but signal delivery has platform and event-loop sharp edges. The design keeps handlers out of user space, which limits scope. |
| `designs/complete/techdesign-const-system-extensions.md` | 3 | Still front-end-only, but definite single assignment is flow-sensitive and must compose with existing narrowing/invalidation rules. (Landed 2026-07-18.) |
| `designs/techdesign-labeled-break-continue.md` | 3 | Requires parser, AST, checker, and lowering changes for nested control flow, but the feature is bounded and testable. |
| `designs/deferal-char-block-abi.md` | 3 | Short design, but ABI tags are contract-governed. Small implementation footprint with high compatibility sensitivity. |
| `designs/complete/techdesign-04-stdlib-strings.md` | 3 | Mostly prelude/library expansion plus a `toInt` migration. Broad surface, moderate implementation complexity. |
| `designs/complete/techdesign-utf8-chars-string-ops.md` | 3 | Unicode correctness and string API behavior make this harder than ordinary string helpers, but the ownership is narrow. (Landed 2026-07-19.) |
| `designs/techdesign-block-scoped-use.md` | 4 | Requires unifying per-block lexical scope machinery across resolver/checker/lowerer behavior. Medium-sized compiler architecture cleanup. |
| `designs/complete/imports.md` | 4 | Alias semantics, lexical scoping, resolver/checker/lowerer integration, and runtime alias blind spots make this more than parser sugar. |
| `designs/complete/techdesign-01-literals-operators.md` | 4 | Several language-surface features at once: literals, shifts, escapes, compound assignment. Broad parser/checker/backend testing. |
| `designs/complete/techdesign-02-control-flow.md` | 4 | `break`/`continue`, `do-while`, and `using` cross parser, checker, IR/lowering, runtime cleanup semantics, and diagnostics. |
| `designs/complete/techdesign-05-stdlib-collections.md` | 4 | Library-heavy, but key equality and `Set<T>` interact with generics, maps, inference, and existing collection contracts. |
| `designs/complete/techdesign-toolchain.md` | 4 | Mostly product/tooling identity and driver split work. Lots of file and CLI churn, but comparatively little deep type/runtime semantics. |
| `designs/complete/techdesign-07-iteration.md` | 5 | Generic interfaces, iterator protocol, lazy `Seq<T>`, and composition classes require type-system and prelude coordination. |
| `designs/complete/techdesign-09-web-foundations.md` | 5 | JSON, DateTime, encoding/digests, and HTTP hardening are mostly library/runtime work, but the combined surface is broad and framework-critical. |
| `designs/complete/proposal-project-system.md` | 5 | Detailed enough to implement: manifest parsing, project gather, cross-file resolution, include graph, and diagnostics. Not backend-deep, but touches the compilation model. |
| `designs/atlantis/techdesign-04-di-config.md` | 5 | DI uses existing `bind` machinery, but config parsing, options validation, command dispatch, and composition-root conventions create many integration points. |
| `designs/complete/techdesign-03-core-types.md` | 6 | Adds/finishes core value surfaces (`char`, `enum`, `Block`) with parser/checker/runtime/backend implications and ABI-sensitive gaps. |
| `designs/complete/techdesign-08-system-natives.md` | 6 | Broad platform floor: argv/env/exit, time/random, dirs, isatty, sockets, DNS, and spawn. Testing across engines and OS behavior dominates. |
| `designs/techdesign-package-manager.md` | 6 | Trident dependency management needs manifest semantics, Git deps, MVS, lockfiles, store layout, integrity, and publishing seams. |
| `designs/atlantis/techdesign-03-serialization.md` | 6 | Compile-time generation, JSON conversion, content negotiation, DTO conventions, and metaprogramming probes make this a serious framework/compiler boundary task. |
| `designs/atlantis/techdesign-09-views.md` | 6 | Template parser/runtime, escaping model, fragments, content negotiation, and boot diagnostics. It is framework-local, but security and UX details matter. |
| `designs/atlantis/techdesign-01-kernel.md` | 7 | Core web kernel: middleware fold, `Context`, server bootstrap, error mapping, static files, logging, ops endpoints, SSE, and async readiness. |
| `designs/atlantis/techdesign-02-routing-controllers.md` | 7 | Router/trie, attributes and compile-time rules, controller mounting, parameter binding, validation, auth defaults, and manual escape hatches. |
| `designs/atlantis/techdesign-07-mcp-openapi.md` | 7 | Combines MCP JSON-RPC, schema generation, OpenAPI 3.1, typed clients, and shared metadata emitters. Lots of protocol and codegen coordination. |
| `designs/complete/techdesign-metaprogramming.md` | 8 | Foundational compiler feature: syntax, AST, rule engine, comptime driver, hygiene, diagnostics, pipeline changes, and test strategy. High blast radius. |
| `designs/complete/techdesign-metaprog-phase3.md` | 8 | Deepens metaprogramming with semantic reflection, predicates, `$for`, macros, anchors, conditional `uses`, hygiene, and struct reification. |
| `designs/complete/techdesign-metaprog-phase4.md` | 8 | Adds body-replacing rewrites, conflict/confluence rules, reentrancy, provenance, expand-as-source, incremental caching, and pre-checking. |
| `designs/complete/techdesign-portable-backend.md` | 8 | LLVM parity and IR memory operations across objects, collections, closures, exceptions, system layer, event loop, async, sockets, and performance closeout. |
| `designs/complete/techdesign-portable-backend-2.md` | 8 | Runtime v2, ABI contract, archive/link driver, platform floor, event loop/net natives, cross targets, and CMake/CI integration. |
| `designs/atlantis/techdesign-05-mysql-driver.md` | 8 | Full MySQL wire protocol, handshake/auth, text and binary prepared statements, async loop plumbing, pooling, error mapping, and live fixture testing. |
| `designs/atlantis/techdesign-06-orm.md` | 8 | Stateful MI model, generated tracking members, unit of work, identity map, typed query builder, relations, migrations, and DTO/entity mapping. |
| `designs/atlantis/techdesign-08-auth-security.md` | 8 | Security-critical subsystem: sessions, JWT-shaped bearer tokens, PBKDF2/HMAC, constant-time discipline, CSRF, CORS, headers, rate limiting, lockout, and audit events. |

## Model Challenge Bands

| Difficulty | Suitable model/task profile |
|---:|---|
| 1 | Small localized coding task; can be handled by weaker models if tests are explicit. |
| 2 | Localized implementation with platform or engine wrinkles; needs basic repo navigation. |
| 3 | Compiler or runtime change with one nontrivial semantic rule; needs careful tests. |
| 4 | Medium cross-subsystem work; a model must read existing architecture before editing. |
| 5 | Broad feature track with several integration points; needs planning and staged verification. |
| 6 | Major subsystem implementation; strong models only, with good failure recovery. |
| 7 | Framework/compiler boundary or protocol-heavy work; needs high-context reasoning and careful acceptance tests. |
| 8 | Platform-scale or security/metaprogramming/backend work; requires top-tier models and tight human review. |
