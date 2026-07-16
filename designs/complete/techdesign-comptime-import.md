# Tech Design: Comptime Template Import — `import()` (LA-20)

**Status:** **LANDED** (2026-07-11) — C1–C4 all implemented per §13's commit
sequence (single-file core, trident `assets =` + `**` glob + hashing,
plan-mode moduleId-scoped resolution, `--assets` introspection + `--expand`
elision); see §12 for the acceptance corpus. Responds to
`designs/complete/request-comptime-template-import.md` (Atlantis Track 09, P2).
**Grounding:** the codebase as of `9da1df1` — metaprogramming Phases 1–4 landed
(`designs/complete/techdesign-metaprogramming.md` + phase3/phase4 docs), the
trident/leviathan toolchain split landed (`designs/complete/techdesign-toolchain.md`).
File:line references are against that tree.

**Reading order:** the request (what and why), then this doc (how). §numbers like §16.5
refer to `info.md`; "master §6" refers to `techdesign-metaprogramming.md`; "toolchain §3.3"
to `techdesign-toolchain.md`.

---

## 0. Contents

1. The ask, restated as a mechanism
2. Current-state audit — what the substrate already provides
3. Design at a glance
4. Surface: `std::import` — a prelude declaration, not new syntax
5. Root resolution: who owns the path (trident/leviathan split)
6. The comptime intrinsic (Evaluator changes)
7. Trident: `assets` manifest key, glob expansion, plan rows, hashing
8. Leviathan: plan reader, project plumbing, `--assets` introspection
9. Hermeticity, determinism, and the budget
10. Diagnostics catalog (I01–I05)
11. Engine/backend impact (none, by construction)
12. Testing plan
13. File-level change map and commit sequence (with timeline)
14. Deviations from the request (explicit, with rationale)
15. Risks and STOP conditions
16. Open calls for Leonard

---

## 1. The ask, restated as a mechanism

```
comptime string tpl = import("views/links/index.html");
```

At compile time, `import(path)` yields the named file's content as a comptime string.
The file is a **declared build input** — same determinism class as a `.lev` source: its
content hash participates in the build record, a content change is observed on rebuild,
and nothing else about comptime hermeticity moves (no network, no clock, no writes, no
reads outside the project tree; a missing or escaping path is a compile error).

The one-sentence mechanism: **`import` is a prelude function whose runtime body throws
and whose comptime evaluation is intercepted by the comptime driver** — the exact inverse
of the `sys*` gate (denied at comptime, allowed at runtime). Everything else in this
design is path policy (who resolves what, per the PM/compiler separation) and the build-
input record (trident's hash, leviathan's report).

## 2. Current-state audit

Everything LA-20 needs already exists except the intrinsic itself:

| Substrate fact | Where | What it gives us |
|---|---|---|
| Comptime fold: `comptime` var/expr/if evaluated on the hermetic oracle, result reified to a literal, `defineGlobal` makes it visible to later comptime sites | `Rules.cpp` `foldExpr`/`reify`, `Eval.cpp` `evalComptime`/`defineGlobal` | `comptime string tpl = import(...)` folds to a `StringLit`; a later `comptime` site (the template parser) reads `tpl` by name — zero new folding machinery |
| String reification escapes `"` `\` `\n` `\t` `\r` into a quoted literal owned by `reifiedText_` (deque, stable addresses) | `Rules.cpp:485-498`, `RuleEngine::own` | arbitrary text-file content survives reification today; no reifier change |
| The hermeticity gate is a **dynamic flag** (`comptime_ && hermetic_`), checked at the native choke point, with a carve-out already precedented (stdout/stderr writes) | `Eval.cpp:400-409` | `import` gates the same way — allowed iff `comptime_`; the deny-set for `sys*` is untouched |
| Step budget, sticky exhaustion, `--comptime-budget` | `Eval.cpp:326-341` | a runaway import loop is already a compile error |
| Prelude functions with language bodies resolve and lower like user code on all five engines | `Resolver.cpp` prelude segments | the runtime-throw behavior costs zero backend work (§11) |
| trident→leviathan build plan: frozen text contract, additive rows (`src`, `edge`), dedicated reader/writer pair | `src/BuildPlan.{hpp,cpp}`, `tools/trident/plan.{hpp,cpp}` | `asset` rows are one more row kind on each side |
| trident already has SHA-256 (dependency-free, streaming) and source-glob expansion | `tools/trident/hash.{hpp,cpp}`, `resolve.cpp` | asset hashing and `assets = [...]` glob expansion reuse both |
| Per-file → module attribution (`ProjectFile.moduleId`, `fileOf`-by-offset) | `Project.hpp:28-37`, `Rules.hpp:79` | a dep's `import` resolves against the dep's own assets, mirroring phantom-dep discipline |
| Project-of-one: a bare single file synthesizes its own one-entry file map | `main.cpp:342-348` | single-file builds get a natural root (the source file's directory) with no manifest |

**Consequence:** zero lexer changes, zero parser changes, zero AST changes, zero
Checker/Lower/backend changes. The work is: one Evaluator intrinsic, one prelude
declaration, plan/manifest plumbing in trident, plan-reader plumbing in leviathan, and
diagnostics/tests.

## 3. Design at a glance

```
 trident.toml                    trident build/plan               leviathan --plan
 ┌──────────────────┐   ┌─────────────────────────────┐   ┌──────────────────────────────┐
 │ assets = [        │   │ expand globs (per module),  │   │ BuildPlan gains asset rows;  │
 │   "views/**",     │──▶│ sort, hash (sha256),        │──▶│ LoadedProject carries a      │
 │   "schema/*.sql"] │   │ emit asset{} plan rows      │   │ moduleId-keyed asset table   │
 └──────────────────┘   └─────────────────────────────┘   └──────────────┬───────────────┘
                                                                          │
                                       ┌──────────────────────────────────▼───────────────┐
 single file (no plan):                │ RULE STAGE (existing): comptime fold walk        │
 root = dirname(source),               │   eval hits call resolved to std::import         │
 any contained file importable        │   └─ comptime_? intercept: validate path, look   │
 (project-of-one, §8)                  │      up / read file, cache, record, return       │
                                       │      Value{String}  → reify → StringLit          │
                                       │   runtime? the prelude body throws (loud, §4)    │
                                       └──────────────────────────────────────────────────┘
```

## 4. Surface: `std::import` — a prelude declaration, not new syntax

No new keyword, no grammar production, no contextual identifier. The prelude
(`kPreludeStd`, `Resolver.cpp`) gains one ordinary function:

```
namespace std {
    // Comptime file inclusion (LA-20). At compile time the comptime driver
    // intercepts this call and returns the declared build input's content.
    // At runtime there is nothing to intercept — a program that reaches this
    // body asked for a compile-time construct at runtime, and runtime
    // failures are loud (§12.6).
    string import(string path) {
        throw RuntimeException("import() is compile-time-only: call it from comptime context (LA-20)");
    }
}
```

Why this shape (each point is an existing rule, not a new one):

- **Resolution is ordinary.** `import` resolves through the implicit `uses std` like
  `sysWrite` or `env::args`; a user declaration named `import` shadows it (nearest-wins,
  §12.6's std-shadowing rule) and nothing breaks — the intercept keys on **symbol
  identity** (the prelude `Stmt*`, §6), never on the name, so a user's own `import`
  function is never hijacked, even at comptime.
- **The comptime/runtime split is the `sys*` gate inverted.** `sys*` natives: allowed at
  runtime, denied under `comptime_ && hermetic_`. `import`: intercepted under
  `comptime_`, and at runtime the body it already has simply runs. One mechanism, two
  directions, both dynamic — a helper function that wraps `import` works at comptime
  (`string loadTpl(string n) => import("views/" + n + ".html");` called from a comptime
  initializer) because the gate is "is this evaluation comptime," not "is this call
  lexically under a `comptime` marker."
- **Runtime misuse is loud, local, and catchable** — the same class as index-out-of-
  bounds: a real `RuntimeException` naming the fix, identical on all five engines,
  because the throw is language code every backend already compiles (§11).
- `Program.hasMeta` needs no new trigger: `import` only ever *does* something under a
  comptime root, and the `comptime` marker already sets `hasMeta`.

Paths are **plain relative paths with `/` separators on every platform**: no leading
`/`, no `\`, no empty path, no `.` or `..` segments (lexical check, before any
filesystem contact). This is deliberately stricter than "normalize then check" — there
is no legitimate reason for an escape-shaped path to appear in source, so the language
refuses to guess at intent (the same stance as the bare read of a collided member).

## 5. Root resolution: who owns the path

The request says "resolved against the project root (manifest-relative)". Since the
toolchain split, leviathan does not know the project root — it reads a resolved plan and
does **no path resolution beyond opening the files** (toolchain §3.3 contract rule 1),
and all project/dependency logic is trident's exclusively (the PM/compiler-separation
hard requirement). So root resolution splits by build shape:

**Plan builds (`leviathan --plan`): assets are declared, expanded, and hashed by
trident; leviathan looks them up by string equality.** The manifest declares asset
globs; trident expands them against the manifest's own directory (the root it already
knows), hashes each file, and emits `asset` rows carrying both the root-relative name
and the absolute path. At an `import("views/index.html")` call, leviathan finds the
importing file's `moduleId` (the existing `fileOf` span attribution) and looks up
`"views/index.html"` in that module's asset table — plain string equality, exactly how
`entry.kind = file` matching already works (toolchain §3.3). No filesystem walking, no
path arithmetic, no root variable in the compiler. Containment is enforced **by
construction**: only declared, already-resolved files are reachable, so `..`-escape
prevention on the plan path is a manifest-layer fact, not a compiler check (the lexical
path check in §4 still runs first, so the diagnostic for an escape-shaped path is the
same in both modes).

- **Per-module scoping** mirrors phantom-dep discipline: a dependency's source imports
  against the dependency's *own* declared assets, relative to *its* manifest — a library
  can ship templates without knowing anything about the consuming app's tree, and an app
  cannot reach into a dep's assets (or vice versa) by path. Same rule as `uses` and
  namespaces: reach is declared, never ambient.

**Single-file builds (`leviathan foo.lev`): root = the source file's directory,
contained files importable without declaration.** A bare file is a project of one
(§12.8) — there is no manifest to declare assets in, and the project-of-one already gets
its own namespaces without `uses` (master Phase-1 deviation, confirmed intended). The
same reasoning applies here: the file's directory is the evident root, the §4 lexical
rules confine imports to the tree below it, and leviathan does the one `readFile` itself
(it already reads the source file; this is the same trust level). Symlinks that point
outside the tree are out of scope v1 (documented; the threat model here is accident
prevention and build-input honesty, not sandboxing hostile source — source code already
runs at comptime).

This split keeps every path decision where the toolchain design put it: trident decides
*what is in the build*, leviathan *compiles what it was handed* — extended verbatim from
sources to assets.

## 6. The comptime intrinsic (Evaluator changes)

The interception point is `Evaluator::callFunction` — the same function that hosts the
`sys*` deny gate (`Eval.cpp:394-409`), before body execution:

```cpp
// Eval.hpp — alongside ComptimeOptions:
struct ImportedAsset {
    std::string rel;        // the path as written / declared ("views/index.html")
    std::string abs;        // what was actually read
    std::string moduleId;   // "" = root project / single file
    size_t bytes = 0;
};
struct ImportContext {
    // Plan builds: moduleId -> (rel -> abs). Single-file: empty map + rootDir set.
    const std::map<std::string, std::map<std::string, std::string>>* assets = nullptr;
    std::string rootDir;                       // single-file mode only
    const Stmt* importFn = nullptr;            // the prelude std::import decl (identity)
    // Which module the current comptime root's FILE belongs to — set by the
    // RuleEngine before each comptime evaluation (fileOf on the root's span).
    std::string currentModule;
};

class Evaluator {
    // ...
    void setImportContext(ImportContext ctx);            // engine wires it once + per-root module
    const std::vector<ImportedAsset>& importedAssets() const;
private:
    ImportContext importCtx_;
    std::map<std::string, std::string> importCache_;     // abs path -> content
    std::vector<ImportedAsset> importedAssets_;          // in first-read order
};
```

In `callFunction`, immediately after the existing `sys*` gate:

```cpp
if (comptime_ && importCtx_.importFn && fn == importCtx_.importFn) {
    return comptimeImport(args);   // the intrinsic below; never runs the body
}
```

`comptimeImport(args)`:

1. **Validate** `args[0]` is a string (I01) and passes the §4 lexical rules (I02).
2. **Resolve**: plan mode — look up `(currentModule, rel)` in the asset table; absent →
   I03 (naming the `assets` manifest key and, if the rel matches an asset of a
   *different* module, saying so — the phantom-asset case gets the phantom-dep-style
   message). Single-file mode — `abs = rootDir + "/" + rel`; unreadable → I04.
3. **Read once, cache**: `importCache_` is keyed on the absolute path — a template
   imported from two comptime sites is read once, and both sites observe identical
   content even if the file mutates mid-compile (determinism within one compilation).
   First read appends an `ImportedAsset` record.
4. **Return** `vstr(content)` — from here the existing machinery takes over: `foldExpr`
   reifies it, `defineGlobal` publishes it, later comptime code (the template parser)
   consumes it as an ordinary string.

Failures use `throwRuntime` with the I-numbered message text, surfacing through the
standard comptime-failure channel (`evalComptime` → `err` → `foldExpr`'s "comptime
evaluation failed: …" at the comptime root's span, with the offending path in the
message) — the same reporting shape hermeticity violations use today. The `Stmt*`
identity check means the intercept costs one pointer compare on the comptime path and
nothing at all at runtime (`comptime_` is false).

The RuleEngine's only changes: locate the `std::import` decl once (via
`namespaceScope("std")`, the same lookup `metaClassSymbol` uses), build the
`ImportContext` from what `main.cpp` hands it (§8), set `currentModule` before each
comptime root (it already knows `fileOf(root->span)` — one line beside the existing
`comptimeScope_` bookkeeping), and expose `eval_.importedAssets()` for `--assets`.

## 7. Trident: `assets` manifest key, glob expansion, plan rows, hashing

**Manifest** (`manifest.{hpp,cpp}`): one new top-level key, same shape as `sources`:

```toml
name    = "app"
sources = ["*.lev"]
assets  = ["views/**", "schema/*.sql", "openapi.json"]
```

- `ProjectManifest` gains `std::vector<std::string> assets;`. Absent key = empty = no
  assets (imports in plan builds then fail I03 with the "add assets = [...]" hint).
- Globs reuse the source-glob expansion in `resolve.cpp`, plus **`**` recursive
  matching** (new, trident-only: a directory-recursing matcher over the existing
  listing machinery — assets are trees like `views/`, where sources' flat `*.lev` was
  enough). Expansion is sorted (byte order) for deterministic plan output. A glob
  matching zero files is a warning, not an error (an empty `views/` during scaffolding
  shouldn't fail the build); a *literal* (glob-free) entry naming a missing file is an
  error at plan time.
- Dependencies declare their own `assets` in their own manifests; `resolveProject`
  gathers them per module exactly as it gathers dep sources, keyed by the same
  `moduleId`.

**Plan** (`plan.{hpp,cpp}` writer, `src/BuildPlan.{hpp,cpp}` reader): one new row kind,
after `src` rows, before `edge` rows:

```
asset { rel = "views/links/index.html"; path = "<abs>"; moduleId = "";
        hash = "sha256:ab12…"; }
```

- `rel` uses `/` separators, relative to the owning module's manifest directory —
  the exact string `import()` matches against.
- `hash` is `sha256Hex` of the file content (`hash.{hpp,cpp}`, already in-tree),
  computed at plan emission. This is the "content hash participates in the build" line
  of the request: the plan is the build's input record, and the future incremental
  cache (metaprog phase4 §7) extends its key with these rows. leviathan **trusts** the
  hash (contract rule 1 — it does not re-hash; hashing stays out of the compiler
  entirely, preserving "no hash code anywhere else in the tree").
- `ResolvedProject` gains `std::vector<ResolvedAsset> assets;`
  (`{rel, path, moduleId, hash}`); `writeBuildPlan` prints the rows; the toolchain
  design's frozen-fields list is **extended, not broken** — old plans (no asset rows)
  read fine, and the row is additive on both sides. `techdesign-toolchain.md` §3.3 gets
  the row appended to its grammar block when this lands.

## 8. Leviathan: plan reader, project plumbing, `--assets` introspection

- `BuildPlan.hpp`: `struct PlanAsset { std::string rel, path, moduleId, hash; };` +
  `std::vector<PlanAsset> assets;` on `BuildPlan`; `readBuildPlan` parses the row
  (same cursor style as `src`/`edge`).
- `Project.hpp`: `LoadedProject` gains the moduleId-keyed asset table
  (`std::map<std::string, std::map<std::string, std::string>> assets;` — rel → abs per
  module) plus a parallel rel → hash map kept only for `--assets` display.
  `loadProjectFromPlan` fills it. No existence probing (trident guaranteed the files;
  rule 1).
- `main.cpp`: builds the `ImportContext` — plan mode: the table; single-file mode:
  `rootDir` = dirname of the source path (the same `find_last_of('/')` split the argv
  stash at `main.cpp:309-317` already does). Hands it to the RuleEngine alongside
  `ComptimeOptions`.
- **`--assets`** (new introspection mode, the `--imports`/`--rules` pattern): runs the
  pipeline through the rule stage, then prints one line per consumed asset —
  `views/links/index.html  4213 B  sha256:ab12…  (module "")` — from
  `engine->importedAssets()`, joining hashes from the plan table when present
  (single-file mode prints path + bytes; no hash, by design — hashing is trident's).
  Prints `no assets imported` when the program folds none. This is acceptance line 1's
  observability surface and the debugging window for I03.
- **`--expand`**: needs no code change for correctness (a folded import is an ordinary
  `StringLit` and already prints), but large template literals would drown the dump —
  so the source printer elides string literals over ~200 chars to
  `"…(4213 bytes, imported: views/links/index.html)…"` **in the provenance comment
  style, only when the literal's span matches an `ImportedAsset` record**; the
  round-trip fixture (phase4 §6) keeps a small-template case un-elided so
  `expand_roundtrip` stays honest, and elided programs carry the existing
  `@no-roundtrip` marker.

## 9. Hermeticity, determinism, and the budget

- **The deny-set does not move.** `import` never touches a `sys*` native — the intrinsic
  reads through the compiler's own file I/O (`readFile`, the function that loads
  sources). Comptime code still cannot open, stat, list, read fds, or reach the event
  loop; the request's framing holds: this widens *the build's declared inputs*, it does
  not open *runtime I/O* at compile time.
- **Determinism:** content is cached per absolute path for the whole rule stage (one
  read, stable view); asset expansion and plan rows are sorted; `importedAssets_` is
  first-read order (deterministic because the fold walk is deterministic). Same inputs →
  same fold → same binary; a content change changes the plan's hash row and the folded
  literal together.
- **Budget:** each `import` call costs its ordinary evaluation steps; the read itself is
  one step. A pathological import-in-a-loop burns the step budget like any comptime
  loop. **No file-size cap in v1** — a cap is policy without a demonstrated failure, and
  the step budget already bounds the loop shape; flagged as an open call (§16) with a
  soft-cap recommendation if binaries bloat in practice.
- **Content is bytes, and strings are byte-counted (§19.9)** — a UTF-8 template, a SQL
  file, and an `openapi.json` golden all round-trip exactly. Text files are the intended
  use; arbitrary binary *works* through the value path (std::string is NUL-safe, the
  reifier escapes the literal-breaking bytes) but is not the design center — a
  `Block`-returning `importBytes` is deferred until `Block` is the settled byte-buffer
  story (§15). The `--expand` elision (§8) keeps exotic content out of the dump.

## 10. Diagnostics catalog (I01–I05)

A new `I` (import) series rather than continuing `M` — these are build-input errors, not
metaprogramming-stage errors, and the request's SQL/fixture uses will outlive the views
driver. All surface through the comptime-failure channel at the comptime root's span,
message carrying the offending path (§6); all are errors.

| # | Trigger | Message shape |
|---|---|---|
| I01 | argument is not a string value | `import: path must be a string (got 'int')` |
| I02 | escape-shaped path: absolute, `\`, empty, `.`/`..` segment | `import: path must be a plain project-relative path ('../x' — no '..', no leading '/', '/' separators)` |
| I03 | plan build: rel not in the importing module's asset table | `import: 'views/x.html' is not a declared asset of this module — add it to assets = [...] in trident.toml` (+ `note: 'views/x.html' is an asset of module "lib" — assets do not cross module boundaries` when applicable) |
| I04 | single-file build: file missing/unreadable under the root | `import: cannot read 'views/x.html' (relative to <dir>)` |
| I05 | plan build: declared asset vanished between plan emission and read | `import: declared asset 'views/x.html' is unreadable — the build plan is stale (re-run trident)` |

The runtime throw (§4) is deliberately **not** I-numbered: it is a catchable
`RuntimeException`, not a compile diagnostic.

## 11. Engine/backend impact: none, by construction

The intrinsic runs entirely inside the rule stage, above the IR. By pass 2 every
successful `import` is a string literal indistinguishable from a hand-written one —
fixed cost, `--expand`-visible, identical on oracle/IR/C++/LLVM/ELF (P1 cost-identity,
inherited). The runtime path is a prelude function with an ordinary `throw` body, which
every engine already compiles — no `RuntimeNatives.cpp` entry, no backend stubs, no
Lower/Checker changes, no new opcodes. The zero-cost guard holds: a program with no
comptime surface never constructs the engine and never evaluates the intercept compare.

## 12. Testing plan

Corpus `tests/corpus/meta/` unless noted; fixture files (`.html`/`.sql`/`.txt`) live
beside the tests they feed.

1. **`import_fold`** — acceptance 1 core: import a fixture, assert the comptime string
   equals disk content byte-for-byte (length + probe substrings + a non-ASCII char);
   import the same file from two comptime sites (cache: identical values); feed the
   content to a comptime function (the parser pattern) and fold its result. oracle==IR;
   ELF via the meta-corpus manual check, like the Phase-3 additions.
2. **`import_twin`** — the P1 check: the imported program vs a hand-written twin with
   the literal pasted in; identical output on `--run`/`--ir`/`--emit-elf`.
3. **Project tests** (`tests/corpus/project/` + `run_plan.sh` templates, which already
   `@DIR@`-substitute): `asset_ok` — manifest `assets`, plan rows, import resolves,
   glob-`**` over a nested `views/` tree; `asset_module` — a dep with its own assets:
   dep code imports its template (fires), app code importing the dep's rel path fails
   I03 with the cross-module note.
4. **Negative metatests** (`test_meta.cpp` pattern), one per I01–I05 — I05 by deleting
   the file between plan emission and the compile call in the test harness — plus: the
   runtime-throw path (`string s = import("x");` in plain runtime code → catchable
   `RuntimeException` on `--run` and `--ir`); user-declared `import` function shadowing
   std's, called at comptime, still runs the user's (symbol-identity guard).
5. **`--assets` golden** — path, byte count, hash presence in plan mode / absence in
   single-file mode; deterministic across two runs.
6. **`--expand`**: small-template import stays in the `expand_roundtrip` set (folded
   literal compiles back, byte-identical output); a large-template case pins the elision
   comment and carries `@no-roundtrip`.
7. **Recompile-observes-change** (acceptance 1, tail): a harness case that compiles,
   rewrites the fixture, recompiles, asserts the new content in the output — trivially
   true today (full rebuild) but pinned so the future incremental cache (phase4 §7)
   cannot regress it silently.
8. **Zero-cost guard** unchanged: the legacy corpus runs with no engine construction.

Acceptance 3 (the Track 09 swap-in: one demo template compiled via import renders
byte-identically to the runtime engine) is **downstream, deliberately** — it exercises
the views engine, not this feature, and lands with the Atlantis track's upgrade
(techdesign-09 §1.3) once this ships.

## 13. File-level change map and commit sequence

| File | Change |
|---|---|
| prelude (`Resolver.cpp`, `kPreludeStd`) | `std::import` declaration with the throwing body (§4) |
| `src/Eval.hpp/.cpp` | `ImportContext`/`ImportedAsset`, `setImportContext`, the `callFunction` intercept + `comptimeImport` (I01/I02/I04/I05 + plan lookup I03), read cache, record |
| `src/Rules.hpp/.cpp` | locate `std::import` symbol; wire context + `currentModule` per comptime root; expose `importedAssets()` |
| `src/BuildPlan.hpp/.cpp` | `PlanAsset` + `asset { }` row parsing |
| `src/Project.hpp/.cpp` | asset table on `LoadedProject`; filled by `loadProjectFromPlan` |
| `src/main.cpp` | build `ImportContext` (plan table / single-file rootDir); `--assets` mode |
| `src/AstPrinter.cpp` (source printer) | large-imported-literal elision in `--expand` (§8) |
| `tools/trident/manifest.{hpp,cpp}` | `assets` key |
| `tools/trident/resolve.{hpp,cpp}` | per-module asset gathering; `**` recursive glob |
| `tools/trident/plan.{hpp,cpp}` | `ResolvedAsset`; emit `asset` rows (sorted, hashed) |
| `docs/reference.md` | §6.9 comptime: `import()`, path rules, the runtime throw; CLI table `--assets` |
| `designs/complete/techdesign-toolchain.md` | §3.3 grammar block: append the `asset` row (additive contract extension, noted in place) |
| `info.md` §16.5 | one paragraph: the sanctioned build-input read |

**Commit sequence** (each independently green, pushed as it lands):

1. **C1 — single-file core** (leviathan only): prelude decl, intrinsic with
   rootDir-mode resolution, I01/I02/I04, cache/record, `import_fold` + `import_twin` +
   runtime-throw + shadowing tests. *The whole Atlantis demo loop works from this commit
   via `leviathan app.lev`.* (~1 day)
2. **C2 — trident assets**: manifest key, `**` glob, per-module gathering, hashed
   `asset` plan rows, plan-writer tests. (~1 day)
3. **C3 — plan-mode resolution**: BuildPlan/Project plumbing, moduleId-keyed lookup,
   I03/I05, `asset_ok` + `asset_module` project tests. (~0.5 day)
4. **C4 — introspection + docs**: `--assets`, `--expand` elision, reference.md/info.md/
   toolchain-doc updates, recompile-observes-change harness case. (~0.5 day)

Total ≈ 3 working days; C1 is independently shippable and unblocks Track 09
experimentation immediately.

## 14. Deviations from the request (explicit, with rationale)

1. **"Resolved against the project root" becomes two modes** (declared assets in plan
   builds; dirname-root in single-file builds) — the request predates reckoning with
   the toolchain split, under which leviathan *has* no project root (§5). The declared-
   asset form is strictly stronger than a root check (reach is declared, per module),
   and the single-file form preserves the request's exact semantics where no manifest
   exists.
2. **The content hash lives in trident's plan rows, not in leviathan** — "its content
   hash participates in the build" is satisfied at the layer that owns build identity
   (and where the future cache keys, phase4 §7); the compiler stays hash-free. The
   request did not specify a layer; the PM/compiler separation does.
3. **`import` is a std function, not syntax** — the request's spelling `import(...)`
   is preserved verbatim; making it a prelude symbol rather than a keyword follows the
   contextual-keyword trajectory one step further (not even contextual — just a name)
   and keeps user code that already uses the identifier legal.
4. **No `importBytes`/binary surface in v1** — text-shaped inputs (templates, SQL,
   JSON goldens) are the entire demand set named by the request; bytes wait for `Block`
   (§9, §15).

## 15. Risks and STOP conditions

| Risk | Exposure | Mitigation |
|---|---|---|
| Huge imported literals bloat `reifiedText_`, IR string tables, and emitted binaries | memory/binary size on template-heavy apps | content cached once per path; `--assets` makes totals visible; size cap is a one-line follow-up if measured (§16) |
| Raw bytes in a reified literal break an engine's string handling | corpus failure on exotic content | `import_fold` includes non-ASCII; if a genuinely literal-breaking byte class surfaces, extend the reifier's escape set for it — **STOP and escalate if any engine needs more than an escape-table addition** (that would mean literals aren't byte-clean, a bigger fact than this feature) |
| Plan contract drift: an older leviathan reading a newer plan with `asset` rows | toolchain skew | same-repo lockstep today; the reader change lands in C3 *before* trident emits rows by default only when `assets` is declared — a manifest without assets emits no rows, so mixed-version windows see yesterday's grammar |
| Dep-module asset integrity: are a dep's assets inside its store contentHash? | stale dep templates under future caching | verify during C2 whether `store.cpp`'s module walk includes non-`.lev` files; if not, widen the walk to declared assets — **STOP if that changes existing lockfile hashes** (a hash-breaking change needs an owner ruling on lock migration) |
| Symlink escape in single-file mode | reads outside the tree on a hand-crafted layout | documented out of scope v1 (§5); comptime already runs project source, so this is not a privilege boundary |
| mid-compile file mutation (editor save race) | confusing one-off output | per-compile read cache gives a consistent view; the plan hash row makes the mismatch visible next build |

## 16. Open calls for Leonard

| Call | Position taken here (recommendation) |
|---|---|
| **File-size cap?** | No cap v1; step budget bounds the loop shape. If template-heavy apps measurably bloat, add a soft cap (default ~16 MiB, flag override) as a follow-up — a measured trigger, not a guess. |
| **Single-file mode: undeclared imports OK?** | Yes — project-of-one already relaxes declaration (own namespaces without `uses`); requiring a manifest for a one-file script kills the scratch/demo loop the mode exists for. |
| **`.` segments in paths** (`./views/x`)? | Reject (I02). One spelling per path; `.` adds nothing and invites `..` by association. |
| **Asset hashes in the lockfile now?** | No — the lockfile pins *dependency* integrity; root-project assets change freely by design. Dep assets ride the module contentHash (verified in C2, see §15). |
| **Name: `import` vs `embed`?** | Keep `import` — the request, the views design, and Atlantis docs all already say it; "embed" describes the mechanism, "import" the declared-build-input semantics we're claiming. |

---

*Responds to `designs/complete/request-comptime-template-import.md`. Companions:
`designs/complete/techdesign-metaprogramming.md` (+ phase3/phase4),
`designs/complete/techdesign-toolchain.md`, `designs/atlantis/techdesign-09-views.md`
§1.3 (the consuming upgrade — downstream, not part of this landing). Implemented
per §13's commit sequence; acceptance corpus green (§12).*
