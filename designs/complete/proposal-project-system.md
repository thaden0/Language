# Proposal: The Project / File System (Phase 0)

> **⚠ Naming & architecture superseded (2026-07-06) by `designs/complete/techdesign-toolchain.md`.**
> The compiler is now **`leviathan`** (not `lang`), the source extension is **`.lev`**
> (not `.ext`), and the manifest is **`<projectname>.lvproj`** (not `project.mf`).
> Critically: the project file and dependency graph are owned by the **`trident`** package
> manager — a **separate application** — not by the compiler. Read the mechanics here (the
> whole-program gather, the `uses` graph, the `file→imports` map still hold); read
> `techdesign-toolchain.md` for the current names and the trident↔leviathan split.


**Status:** design proposal. This is **Phase 0** — the hard prerequisite (P-1..P-4) that
`designs/proposal-metaprogramming.md` and `designs/proposal-web-framework.md` both block on.
**Location note:** in `docs/` alongside `reference.md`, `proposal-metaprogramming.md`, and
`proposal-web-framework.md`.

> **One-line thesis.** The language is already whole-program and already gathers *two* sources
> (the embedded prelude + the one user file) into one scope. The project system is the
> **generalization of that existing gather from 2 sources to N** — plus one genuinely new thing:
> a **retained `file → imports` map** so namespace-scoped rules (Doc 1) know which files opted
> into which rules. Everything else is plumbing over machinery that already exists. The manifest
> should be the language's own literal syntax (not a bolted-on TOML/XML dependency), keeping the
> system dependency-free and consistent with "one rule across all scopes."

This document is grounded in the code as it stands today (`src/main.cpp`, `src/Resolver.cpp`) and
in the design commitments of `info.md` §12 (namespaces are **declaration-based, not
directory-based**; imports are **by name, not by path**) and §17 (**whole-program AOT**). It is
opinionated: where the cross-language record suggests a path the language's ideology forbids (or
vice versa), it says which wins and why.

---

## 0. Table of contents
1. Where the language is today (the single-file driver, and the gather that already exists)
2. What a project/file system must provide here (mapped to P-1..P-4)
3. Cross-language survey — what to steal, what to avoid
4. What fits *this* language's ideology (the decided design)
5. Implementation proposal — manifest schema, loader, resolver, data structures, driver, errors
6. Worked examples — manifest, a multi-file layout, `uses` + namespaced rules resolving across it
7. How it unblocks metaprogramming Phase 0 (and the web framework)
8. Improvements / open questions / refined P-1..P-4
9. Phasing / delivery plan
10. Risks and mitigations
11. Headline recommendations

---

## 1. Where the language is today

Two facts from the source decide the whole shape of this proposal.

**(1) The driver is single-file.** `src/main.cpp` reads exactly one path:

```cpp
SourceFile file;
if (!readFile(path, file)) { ... }          // one file, one SourceFile
Lexer lexer(file, sink);                      // one token stream
Parser parser(std::move(tokens), file, sink);
Program program = parser.parseProgram();      // one Program
Resolver resolver(file, sink);
resolver.run(program);                        // resolve, check, lower, run — all over one Program
```

There is no notion of a set of files, no manifest, no import graph. `project.ext` in the repo root
is a **demo program** (a feature tour), *not* a manifest — a naming collision this proposal
resolves (§5.1).

**(2) The multi-source gather already exists — for the prelude.** `Resolver::run` gathers *two*
programs into one global scope (`src/Resolver.cpp`):

```cpp
preludeProgram_ = parsePrelude();               // a SECOND source, embedded as kPrelude
gatherInto(preludeProgram_.items, sema_.global);// gather prelude names into global
gatherInto(program.items,        sema_.global); // gather user names into the SAME global
...
processImports(program.items, sema_.global);    // resolve `uses` AFTER the gather
```

And `uses` is a **flat symbol copy** into a scope, resolved *after* the gather, with nearer
declarations winning:

```cpp
// processImports: for `uses NS;` — copy all of NS's names into the importing scope
if (ns && ns->scope)
    for (auto& [name, syms] : ns->scope->names)
        for (Symbol* sym : syms)
            scope->names[name].push_back(sym);   // <-- no record of WHICH file imported NS
```

**Two conclusions that drive the design:**

- The **gather step is not new** — it already merges prelude + user into one whole-program scope.
  Multi-file is "call `gatherInto` N times," not a new subsystem. This is the single most important
  grounding fact: the language's whole-program model (§17) means *file boundaries already dissolve
  at gather time; namespace boundaries already persist* (§12) — exactly what a project loader needs.
- The **`file → imports` provenance is thrown away today.** `processImports` copies symbols and
  forgets which file's `uses` brought them. That discard is harmless now (single file) but is the
  one thing namespace-scoped rules (Doc 1 §5) cannot live without. **P-4 is the only genuinely new
  data structure in this whole proposal.**

---

## 2. What a project/file system must provide (mapped to P-1..P-4)

Doc 1 §9 listed four blockers. Restated concretely and tied to what §1 shows already exists:

| Need | What it means here | Prereq | New, or generalize existing? |
|---|---|---|---|
| **Project manifest** | file set + entry point + metadata (name, version, out) | **P-1** | New (small) — a declarative file the driver reads first |
| **Multi-file loader / gather** | read all manifest sources; lex+parse each; gather all into the one unit | **P-2** | **Generalize** the prelude+user gather (§1.2) from 2→N |
| **Cross-file namespace resolution + include graph + build order** | merge re-opened namespaces across files; build the `uses` edge graph; order it | **P-3** | **Generalize** the resolver's within-file namespace merge; add the edge graph |
| **Retained `file → imports` provenance** | per-file record of which namespaces it declares into and imports | **P-4** | **New** — the one load-bearing addition; rule scoping's substrate |

The rest of the doc details each, but the headline is already visible: **three of the four are
generalizations of code that exists; only P-4 is new.**

---

## 3. Cross-language survey (what to steal, what to avoid)

Full sourced survey summarized; §4 cites back to these.

### 3.1 The closest analog is C#/.NET (and its known tax)
The two locked design choices — **declaration-based namespaces** and **import-by-name** — put the
language in the **C# quadrant** (namespace decoupled from folder, `using` by name), *not* the
Go/Java/Odin "directory = package" tradition. So C# is the closest existing analog, and its pains
are the ones to design around:
- **Tooling nags on divergence.** C#'s analyzer rule **IDE0130** ("namespace does not match folder
  structure") and ReSharper constantly flag mismatches — a friction that exists *because* the model
  is decoupled. Lesson: if you decouple, either provide an *optional* folder≈namespace lint or
  accept that layout carries no provenance.
- **"Where does this name come from?"** By-name import means a bare `Foo` could come from any
  imported namespace; extension-method-style names are "hidden … unless the namespace is explicitly
  imported … no way to discover it." This discoverability tax is the price of by-name + decoupled.
- **Loved:** file-scoped namespaces (`namespace X;`) and `global using` / `ImplicitUsings` for
  zero-ceremony common imports. The language's implicit `uses std;` (`reference.md` §5.1 /
  `info.md` §12.6) is *already* an `ImplicitUsings`.

### 3.2 Manifest format — TOML is the low-ceremony gold standard; code-as-config sacrifices predictability
- **Cargo's `Cargo.toml`** is repeatedly cited as *the* reason Rust's project layer is admired:
  "clean and simple TOML … without the need to learn new custom DSLs or magical scripts," one
  integrated tool for build/test/publish/deps. **TOML = the sweet spot.**
- **XML (.csproj / `pom.xml`)** — verbose, high ceremony.
- **JSON (`package.json`)** — familiar but `exports` maps are "the most important file you're
  writing wrong."
- **Code-as-config (Zig `build.zig`)** — maximal flexibility, *minimal predictability*; "the build
  system is the language" is elegant but the opposite of low-ceremony clarity.
- **No manifest / URL imports (Deno)** — Deno's own retrospective calls it a mistake: URLs "lack
  semantic versioning," import maps "aren't composable," deps could "brick CI." Avoid.

### 3.3 Cycles — allowing them is regretted; forbidding them is praised (with an escape hatch)
- **Allow → regret:** Python and Node tolerate import cycles and get half-initialized modules with
  load-order-dependent `ImportError`s; C++ "handles" header cycles only via include guards (ODR
  hazards, build times).
- **Forbid → praised:** Go makes cycles a **hard compile error** (its toolchain compiles a known DAG
  leaves-up); the accepted mitigation is **interfaces** for decoupling. The one complaint is the
  *cost of the workaround*, not the rule — so ship a cheap decoupling primitive.
- **Crucial caveat for a whole-program compiler:** Go/Python/Node forbid-or-suffer cycles because
  they compile/​load *separately*. A **whole-program AOT** compiler gathers everything first and sees
  all names at once — so **namespace reference cycles are already safe here** (they're common:
  `Demo::run(); Other::run();` at top level today). This is a genuine advantage to advertise, not a
  problem to fix (§4.5).

### 3.4 One module concept, one format — the "two-of-everything" trap
- **JPMS** (Java packages *and* `module-info` modules) is "still ignored" a decade on — a redundant
  second modularity layer nobody wanted.
- **Node ESM vs CJS** — "the seam between them is where productivity goes to die"; the dual-package
  hazard breaks singletons.
- Lesson: **ship exactly one module concept and one module format.** The language already has one
  (the namespace); do not add a second (no separate "module" unit on top of namespaces).

### 3.5 Directory-coupled models (the road not taken) and what's clean about them
Go (dir=package), Java (enforced `com/foo/bar/`), Odin (dir=package), Nim (file=module) are all
praised for **one obvious rule with zero manifest wiring to declare a module** — the file/dir *is*
the boundary, provenance is in the path. The language deliberately rejected this (§12: "the
*unopinionated* model, deliberately chosen over the opinionated directory-as-namespace model") — so
the clean discoverability those languages get for free must be *bought back* with tooling (§4.4).

### 3.6 Dependency-free posture sidesteps the worst pain
`node_modules` bloat, Deno's duplicate-deps-without-semver, transitive dependency hell — the
loudest universal complaints — are all downstream of large third-party trees. The language's
**dependency-free** stance (`info.md`: pure x86-64/ELF backend, zero deps; stdlib written in the
language) dodges this class entirely for now. The manifest should therefore keep `deps` minimal
(empty in v1) and *not* over-engineer a package registry before there is anything to depend on.

**Net synthesis:** *TOML-grade declarative manifest + one integrated tool; keep declaration-based
by-name (C# quadrant) but buy back discoverability with tooling; one module concept; whole-program
resolution makes namespace cycles safe (advertise it) while the future package/rule graph forbids
cycles with interfaces as the escape hatch; stay dependency-free.*

---

## 4. What fits *this* language's ideology

The language's ethos (`info.md` §1): **one rule over many special cases, explicit over implicit,
honesty over hidden magic, simplicity, dependency-free.** Each decision below is justified against
it.

### 4.1 The manifest is the language's own literal syntax (not TOML/XML)
The research says "use TOML." The *ideology* says something sharper. Adding a TOML (or XML, or JSON)
parser means adding a **second data language and a parsing dependency** to a project whose entire
identity is *one language, zero dependencies, one rule across all scopes*. The language already has
a lexer, a parser, and rich literal syntax — value `struct`s, arrays, maps, ranges (`reference.md`
§1.4, §6.4.5). So:

> **Recommendation: the manifest is a small, declarative file written in the language's own literal
> syntax — a `struct` literal / key-value block parsed by the existing lexer+parser — not a bespoke
> or foreign config format.**

This is the maximal "one rule across all scopes" move: the manifest is *Language data*, read by the
*Language's* parser, needing *no* new dependency and *no* new grammar. It is **declarative, not
code** (a data literal, not a runnable program) — so it keeps TOML's predictability and explicitly
*avoids* the Zig "build-is-the-language" predictability loss (§3.2). It is the TOML recommendation,
realized in the language's own terms.

Concretely (schema in §5.1):
```
project {
    name    = "myapp";
    version = "0.1.0";
    entry   = "src/main.ext";
    sources = ["src/**/*.ext"];
    out     = "build/myapp";
    // deps  = [];   // future; empty today (dependency-free, §3.6)
}
```

This is not a runnable program (contrast the current demo `project.ext`, which *is*); it is a data
block the compiler evaluates as a manifest. (If a pure data-only file is ever preferred over
reusing the expression grammar, a minimal `key = value` reader is the fallback — but reusing the
language's own literals is the on-ideology choice.)

### 4.2 Namespaces stay decoupled from file paths (locked by §12)
`info.md` §12 already decided this and this proposal does **not** reopen it: *disk layout is
irrelevant; the `namespace` declaration determines the namespace; imports are by name.* So:
- **A file may open any namespace(s); a namespace may span any files.** `namespace App { }` in
  `a.ext` and `namespace App { }` in `b.ext` merge — the resolver already merges re-opened
  namespaces *within* a file (`gatherInto`), and P-3 extends that merge *across* files. This is the
  §12 "one tree, many named scopes" made multi-file.
- **Many namespaces per file / many files per namespace — both allowed**, no one-namespace-per-file
  rule (that would be Nim's model, which §12 rejected). The only *convention* (optional, unenforced)
  is folder≈namespace, offered as a lint, never a requirement (learning from C#'s IDE0130
  love/hate, §3.1).

### 4.3 `uses` across files is the *same* mechanism, now with provenance
`uses` already resolves against the whole-program scope (§1.2). Once the gather is multi-file (P-2),
`uses Web;` in any file resolves to the merged `Web` namespace with **zero new resolution logic** —
the flat-copy `processImports` already does it. The *only* change: **record the edge** (this file
imports this namespace) into the P-4 map as it copies. One mechanism serves both the existing import
semantics *and* rule scoping — a single-rule-serves-multiple-needs win (§1).

### 4.4 Buy back discoverability with tooling, not with directory rules
Because the language is by-name + decoupled (the C# quadrant), it inherits C#'s "where does this
name come from?" tax (§3.1). Rather than re-couple to directories (which §12 forbids), pay it back
with **compiler queries** — consistent with the language's existing `--ast`/`--resolve`/`--tokens`
introspection surface:
- `lang --why <name> [in <file>]` — print every namespace a name could resolve to for a given file,
  and which one wins (the provenance query by-name import removes). This directly answers the C#
  discoverability complaint the path-based languages get for free.
- `lang --namespaces` — list every namespace, the files that open it, and its members (the
  "symbol index" the research recommends).
- `lang --imports` — per file, the `uses` set and the rules thereby in scope (this is the P-4 map,
  surfaced; also feeds Doc 1's `--rules`).
- An **optional** `folder≈namespace` lint (off by default), so teams who want the Go/Java tidiness
  can opt in without it being imposed.

### 4.5 Cycles: whole-program resolution is cycle-immune (advertise it); the rule/package graph is acyclic
A nuanced, grounded position that differs from a blanket "forbid cycles":
- **Namespace reference cycles are allowed and safe** — because the language is whole-program (§17):
  all names are gathered before any resolution, so `A` referencing `B` referencing `A` is fine
  (it already happens). This is the advantage §3.3 identifies: the language *structurally* cannot
  have Python/Node's half-initialized-module class of bug. **State this as a feature.**
- **The *rule-dependency* graph must be acyclic** (Doc 1 §5.4 determinism): if rule R₁'s output is
  matched by R₂ and vice versa, expansion doesn't converge. A cycle there is a **hard error** naming
  the rule chain — and the **interface** is the decoupling primitive (Go's lesson, §3.3), which the
  language already has as a first-class, allocation-free contract (§8). Since rules are additive and
  don't re-trigger by default (Doc 1 §8), this cycle is rare by construction.
- **A future *package* (external dep) graph must be acyclic** — a hard error (Go's stance) if/when
  `deps` becomes non-empty. Not a v1 concern (§3.6).

### 4.6 One module concept — the namespace, full stop
Per §3.4, the language must **not** grow a second modularity layer (no "module" unit on top of
namespaces, no JPMS repeat). The namespace *is* the module; the project is *a set of files that
gather into namespaces*; the manifest names the file set. Three concepts, each doing one job, no
overlap — the §1 rule applied to modularity itself.

---

## 5. Implementation proposal

### 5.1 The manifest schema (P-1)
A declarative block in the language's literal syntax (§4.1), file **`project.ext`** at the project
root.

> **Naming migration.** The repo's current `project.ext` is a *demo program*. Rename it (e.g. move
> to `examples/tour.ext`) and reclaim `project.ext` for the manifest — or, if preferred, name the
> manifest `Project.ext` / `manifest.ext`. This proposal assumes `project.ext` = manifest and the
> tour moves to `examples/`.

| Field | Type | Required | Meaning |
|---|---|---|---|
| `name` | `string` | yes | project name (binary name default, diagnostics) |
| `entry` | `string` | yes | the entry file (or, later, an entry function) — where top-level execution begins |
| `sources` | `Array<string>` | yes | glob patterns → the file set to gather |
| `version` | `string` | no | semver; metadata today, meaningful when `deps` exist |
| `out` | `string` | no | output path for `--build`/`--emit-elf` (default `build/<name>`) |
| `deps` | `Array<...>` | no | external dependencies — **empty/omitted in v1** (§3.6) |

```
project {
    name    = "links";
    version = "0.1.0";
    entry   = "app.ext";
    sources = ["**/*.ext"];
    out     = "build/links";
}
```

Rules for `sources`: globs are resolved relative to the manifest directory; `**` = recursive;
the manifest file itself and `out` are implicitly excluded; a file matched by no pattern is **not**
compiled (and `--why`/diagnostics can point that out). A lone-file invocation (`lang file.ext`,
no manifest) is treated as **a project of one file** — single-file mode is preserved (§5.5), so
nothing that works today breaks.

### 5.2 The loader / gather (P-2) — generalizing the prelude gather
A new `ProjectLoader` sits in front of the existing pipeline:

```
loadProject(manifestPath) -> CompilationUnit:
    manifest = parseManifest(read(manifestPath))          // reuse Lexer+Parser on the literal block
    paths    = expandGlobs(manifest.sources, manifestDir) // ordered, de-duplicated, stable sort
    unit     = CompilationUnit{}
    for (i, path) in paths:                               // deterministic order = sorted paths
        text   = read(path)
        fileId = FileId(i)
        tokens = Lexer(SourceFile{path,text}).tokenize()
        ast    = Parser(tokens).parseProgram()
        unit.files.push(SourceUnit{fileId, path, ast})
    return unit
```

Then the resolver's gather runs once over prelude + **every** file's AST into the one global scope —
this is `gatherInto` called `1 + N` times instead of `1 + 1` (§1.2). File identity (`FileId`) is
carried on every declaration during gather (needed for P-4 and for Doc 1 rule scoping).

### 5.3 Cross-file resolution + include graph + build order (P-3)
- **Namespace merge across files:** unchanged in spirit — the resolver already merges re-opened
  namespaces into one `Scope`. The extension is that the re-openings now come from different
  `SourceUnit`s. Because the gather is whole-program, this is automatic once P-2 feeds all files in.
- **The include (`uses`) graph:** while resolving imports, record edges `file --uses--> namespace`
  and `namespace --declared-in--> file`. Nodes: namespaces (and files). This graph is *derived*, not
  authored — no `requires`/`exports` ceremony (avoiding JPMS, §3.4).
- **Build order:** for *resolution*, none is needed (whole-program gathers first — §4.5). Order
  matters only for (a) the **rule stage** (Doc 1: topological over namespace-rule deps, hard error on
  cycle) and (b) **deterministic diagnostics/expansion** (sort by `FileId`/path so output is stable
  run-to-run — the reproducibility the research and Doc 1 §7.2 demand).

### 5.4 The retained `file → imports` map (P-4) — the one new structure
The load-bearing addition. Produced during `processImports`, keyed by `FileId`:

```cpp
struct FileImports {
    FileId               file;
    std::string          path;
    std::vector<NsId>    declaresInto;   // namespaces this file opens
    std::vector<NsId>    importsExplicit;// namespaces via `uses` in this file
    // effective import set = declaresInto ∪ importsExplicit ∪ {std}   (the always-on prelude import)
};
struct CompilationUnit {
    std::vector<SourceUnit>              files;
    Program                             prelude;
    std::unordered_map<FileId,FileImports> fileImports;   // <-- P-4
    UsesGraph                            graph;
    // ... existing sema/global ...
};
```

`processImports` today copies symbols and discards provenance (§1.2). The change is one line of
bookkeeping per `uses`: **also** append the namespace to `fileImports[currentFile].importsExplicit`.
That is the entire cost of P-4 at gather time; the payoff is that Doc 1 §5 can compute, for any
declaration, "which rules may touch it" = rules from namespaces in the declaring file's effective
import set. **One mechanism (`uses`) now serves both import semantics and rule scoping** (§1).

### 5.5 Driver changes — `main.cpp` becomes project-aware
Minimal, backward-compatible:
```
lang                      # no arg: look for ./project.ext, build the project
lang <dir>                # dir containing project.ext
lang <manifest>.ext       # explicit manifest
lang <single>.ext         # a single source file with NO `project{}` block -> project-of-one (today's behavior)
```
Detection: parse the given file; if it contains a top-level `project { ... }` manifest block, treat
it as a manifest and load the project; otherwise treat it as a single-file project (wrap it in an
implicit one-file unit). All existing modes (`--run`, `--ir`, `--emit-elf`, `--ast`, …) operate on
the resulting `CompilationUnit` unchanged — they already consume "one gathered Program"; they now
consume "one gathered Program built from N files." The five-engine differential-testing discipline
(`reference.md` §7.1) extends to "a multi-file project produces identical output on all engines."

### 5.6 Error reporting
Consistent with the language's "loud, not silent; refuse to guess" stance (§1, §3.7):
- **Unknown namespace in `uses`** — already handled (`"unknown namespace 'X'"`); extend the message
  to suggest near-matches and name the file.
- **Ambiguous name across imports** — when two *imported* namespaces export the same name **and
  type** (a genuine collision, since different types disambiguate — §4.3 of `info.md`), that is the
  existing `distinct`/collision machinery: a bare use is a **compile error** ("refuse to guess"),
  fixed by `::` qualification. By-name import does **not** silently shadow across imports (avoiding
  Python/JS's silent-shadow footgun, §3); within-file, nearer declarations legitimately shadow
  imports (the existing rule).
- **A source matched by no `sources` glob** — a warning (the file won't compile; likely a manifest
  omission).
- **Rule/package cycle** (§4.5) — a hard error printing the cycle chain (`A → B → A`) and suggesting
  an interface seam.
- **Manifest errors** (missing `entry`, `entry` not in `sources`, bad glob) — reported against the
  manifest file with spans, like any other source.

---

## 6. Worked examples

### 6.1 A minimal multi-file project
```
links/
  project.ext                 # manifest
  app.ext                     # composition root + entry
  models/link.ext             # namespace App::Models
  data/links.ext              # namespace Data
  web/routing.ext             # namespace Web (framework rules live here)
```

**`project.ext`**
```
project {
    name    = "links";
    entry   = "app.ext";
    sources = ["**/*.ext"];
    out     = "build/links";
}
```

**`web/routing.ext`**
```
namespace Web {
    public attribute Route { string method; string path; }
    rule registerRoutes { match @Route(r) on method m in class C : IController
                          inject `this.router.add($r.method, $r.path, ...)` at bottom of C.constructor }
}
```

**`models/link.ext`**
```
namespace App::Models {
    struct Link { int id; string title; int votes; }
}
```

**`app.ext`**
```
uses Web;                                  // this file opts into Web's rules (P-4 records: app.ext -> {Web})
uses App::Models;
namespace App {
    class LinkController : Web::IController {
        @Route("GET", "/") View index() => ...;   // fires: app.ext imports Web
    }
}
App::main();                               // entry (top-level, as today)
```

### 6.2 How resolution proceeds across the files
1. **Load (P-2):** `expandGlobs(["**/*.ext"])` → `[app.ext, data/links.ext, models/link.ext,
   web/routing.ext]` (sorted, stable). Each lexed+parsed to a `SourceUnit` with a `FileId`.
2. **Gather (P-2/P-3):** prelude + all four files gathered into one global scope. `namespace Web`
   (from `web/routing.ext`), `namespace App` (from `app.ext`), `namespace App::Models` (from
   `models/link.ext`) all merge into the whole-program tree. File boundaries dissolve; namespace
   boundaries persist (§12).
3. **Imports + provenance (P-4):** `processImports` copies `Web`'s and `App::Models`' names into
   `app.ext`'s scope **and records** `fileImports[app.ext].importsExplicit = {Web, App::Models}`.
   `data/links.ext` and `models/link.ext` never `uses Web`, so their `fileImports` do **not** include
   `Web`.
4. **Rule scoping (Doc 1 §5, powered by P-4):** for each declaration, the active rule set = rules
   from namespaces in that declaration's file's effective import set. `LinkController.index` is in
   `app.ext`, whose set includes `Web` → `registerRoutes` fires. A `@Route` written in
   `data/links.ext` (which never imports `Web`) would **not** fire — dangling-attribute warning,
   code untouched. This is the §5.3-of-Doc-1 guarantee: *to know what magic can touch a file, read
   its `uses` list* — and P-4 is what makes that computable.
5. **Resolve → check → lower → engine:** the augmented whole-program tree flows through the existing
   pipeline unchanged.

### 6.3 Introspection
```
lang --imports              # app.ext -> uses {Web, App::Models, std}; rules in scope: Web::registerRoutes
lang --why View             # View resolves via Web (imported in app.ext); 1 candidate, no ambiguity
lang --namespaces           # Web (web/routing.ext), App (app.ext), App::Models (models/link.ext), ...
```

---

## 7. How it unblocks metaprogramming Phase 0 (and the framework)

Doc 1 §9 lists P-1..P-4 as **blockers** and P-5..P-8 as the "rules" milestone. This proposal
delivers P-1..P-4 and hands the rule layer exactly what it needs:

- **P-1/P-2** give the compiler a *set of files* — without which "a rule in namespace `Web` in
  `web/routing.ext`" and "a controller in `app.ext`" cannot coexist in one build at all.
- **P-3** merges those files' namespaces into the whole-program tree the matchers read (Doc 1 §8
  pass-1 resolve reads these facts).
- **P-4** is the substrate for Doc 1 §5 rule scoping — the *entire* namespace-binding hard
  requirement reduces to "look up the declaring file's import set in `fileImports`." Without P-4,
  namespaced rules are impossible; with it, they are a map lookup.

For the **web framework** (Doc 2), P-1..P-4 are equally load-bearing: a web app is inherently
multi-file (controllers, models, data, views, composition root) and the framework's ergonomics ride
the rule layer that P-4 unblocks. Doc 2's "Phase A" (routing/DI/streams on today's features) can
begin the moment P-1..P-2 exist (multi-file build), even before the rule layer lands.

The dependency chain, stated once: **P-1 → P-2 → P-3 → P-4 (this doc)  ⇒  Doc 1 P-5..P-8 (rules)
⇒  Doc 2 ergonomics.** This proposal is the root of that chain.

---

## 8. Improvements / open questions / refined P-1..P-4

Refinements to the P-list as originally stated in Doc 1 §9:

- **P-1 refined:** the manifest is **the language's own literal syntax** (`project { … }`), not TOML
  — a stronger, more on-ideology choice than "a manifest, format TBD" (§4.1). Resolve the
  `project.ext` naming collision by moving the demo to `examples/` (§5.1).
- **P-2 refined:** explicitly *generalize the existing prelude gather* rather than build a new
  loader from scratch (§5.2) — smaller and lower-risk than Doc 1 implied.
- **P-3 refined:** split into (a) cross-file namespace merge (nearly free — extends existing merge),
  (b) the derived `uses` graph, (c) build order that matters **only** for the rule stage and
  determinism, not for resolution (whole-program is cycle-immune, §4.5). Doc 1's "build order" is
  therefore lighter than it sounded.
- **P-4 refined:** it is *one field on the compilation unit* + *one line in `processImports`*
  (§5.4) — the smallest of the four to implement, though the most important semantically.

Open questions:
1. **Entry point: file or function?** Today top-level statements run (the demo ends with
   `Demo::run();`). Manifest `entry` = a file preserves that. A future `entry = App::main` (a
   function) is cleaner for larger apps — decide when top-level-statements-vs-explicit-main is
   settled.
2. **Glob semantics / ordering:** is sorted-path order enough for determinism, or does the manifest
   need explicit ordering for the rare order-sensitive case? (Recommendation: sorted paths; rules
   are order-independent by design, Doc 1 §5.4, so file order should never be semantically
   meaningful — flag it if it ever is.)
3. **Incremental builds:** whole-program AOT sacrifices separate compilation (`info.md` §17). Cache
   per-file parse results keyed by content hash to blunt this; design the cache when build times
   actually hurt, not before.
4. **`deps` / external packages:** deferred (§3.6). When it lands: named specifiers + a lockfile +
   optional registry, **never** raw URLs (Deno's proven mistake, §3.2). Acyclic package graph, hard
   error on cycle (§4.5).
5. **Manifest as data vs as program:** locked to *declarative data* (§4.1) to preserve
   predictability; revisit only if a real build-step-scripting need appears (then a gated,
   Zig-style escape hatch — not the default).
6. **Multi-target / workspaces** (multiple binaries/libs in one repo): out of scope for v1; the
   manifest schema leaves room (a `[targets]`-style extension) without committing now.

---

## 9. Phasing / delivery plan

- **Phase 0.1 — Multi-file build (P-1 + P-2).** Manifest parsing (`project { }` via the existing
  lexer/parser), glob expansion, N-file gather generalizing the prelude gather, project-aware
  driver with single-file back-compat. *Outcome:* the language builds real multi-file projects.
  This alone unblocks Doc 2 Phase A.
- **Phase 0.2 — Cross-file resolution + graph (P-3).** Ensure re-opened namespaces merge across
  files (mostly verification + tests over 0.1); build the derived `uses` graph; deterministic
  ordering. *Outcome:* `uses` works across files with stable, reproducible builds.
- **Phase 0.3 — Provenance map (P-4).** Add `fileImports` to the compilation unit; record the edge
  in `processImports`; expose `--imports`/`--why`/`--namespaces`. *Outcome:* rule scoping's
  substrate exists — Doc 1's rule layer is unblocked.
- **Phase 0.4 — Polish.** Error messages (ambiguity, no-match globs, cycles), the optional
  folder≈namespace lint, docs, and corpus tests that exercise a multi-file project across all five
  engines.

Each phase is independently testable against the shared corpus; the differential-testing invariant
extends to "the same multi-file project produces identical output on all five engines."

---

## 10. Risks and mitigations

| Risk | Mitigation |
|---|---|
| Multi-file gather subtly diverges from single-file semantics | It *is* the prelude gather generalized (§1.2); differential-test a multi-file corpus vs the equivalent single concatenated file on all five engines |
| Whole-program build times grow with file count | Content-hash parse cache (§8 Q3); whole-program is a deliberate `info.md` §17 trade — accept, then optimize |
| By-name + decoupled discoverability tax (C# lesson, §3.1) | `--why`/`--namespaces`/`--imports` queries (§4.4); optional folder≈namespace lint |
| Silent name shadowing across imports | Reuse the `distinct`/collision machinery — ambiguous same-type name is a hard error, not a silent pick (§5.6) |
| `project.ext` name collision with the demo | Rename the demo to `examples/tour.ext`; reclaim `project.ext` for the manifest (§5.1) |
| Over-engineering deps/registry before any deps exist | `deps` empty in v1; design the package graph only when needed (§3.6, §8 Q4) |
| Manifest-as-code creep (Zig trap) | Manifest is *declarative data*, not a program (§4.1); scripting is a future gated escape hatch only |
| Rule-dependency cycles (Doc 1) | Hard error with chain + interface-seam suggestion; rules additive & non-reentrant by default (§4.5) |

---

## 11. Headline recommendations (for the report)

1. **It's a generalization, not a new subsystem.** The language already gathers prelude + user into
   one whole-program scope; the project system is that gather from **2 sources to N**, plus one new
   structure (the `file → imports` map). Three of the four blockers (P-1..P-3) are generalizations
   of existing code; only **P-4 is genuinely new — and it's one field + one line** in
   `processImports`.
2. **The manifest is the language's own literal syntax** (`project { name; entry; sources; }`), read
   by the existing lexer/parser — **not** a bolted-on TOML/XML/JSON dependency. This is the "one
   rule across all scopes," dependency-free realization of TOML's low-ceremony win, and it avoids
   Zig's code-as-config predictability loss by staying *declarative data*.
3. **Keep namespaces decoupled from paths (locked by §12); buy back discoverability with tooling.**
   The language is in the C# quadrant (by-name + decoupled), so ship `--why <name>`, `--namespaces`,
   `--imports`, and an *optional* folder≈namespace lint to pay the known discoverability tax instead
   of re-coupling to directories.
4. **Whole-program resolution is cycle-immune — advertise it.** Unlike Go/Python/Node,
   namespace reference cycles are structurally safe here (all names gathered before resolution), so
   the language cannot have their half-initialized-module bug class. Cycles are forbidden only in
   the future **rule-dependency** and **package** graphs, with **interfaces** as the cheap decoupling
   seam (Go's lesson) — and the language already has interfaces as first-class contracts.
5. **One module concept — the namespace.** Do not grow a second modularity layer (no JPMS repeat, no
   ESM/CJS-style dual format). Namespace = module; project = a file set that gathers into namespaces;
   manifest = names the file set. Three concepts, no overlap.
6. **This is the root of the chain.** P-1 → P-2 → P-3 → P-4 (this doc) ⇒ Doc 1's rule layer
   (P-5..P-8) ⇒ Doc 2's framework ergonomics. Deliver it in four small phases (multi-file build →
   cross-file resolution → provenance map → polish); the differential-testing discipline extends to
   "a multi-file project agrees across all five engines." Doc 2's Phase A can start after Phase 0.1.

---

## 12. Implementation status (append-only)

> Names below follow the `techdesign-toolchain.md` supersession: the compiler is **`leviathan`**,
> sources are **`.lev`**, and the manifest is trident-owned TOML (`trident.toml`), not `project{}`.
> The *mechanics* this proposal describes are what landed.

- **P-1..P-4 landed (2026-07-06).** Multi-file build, the N-source whole-program gather (the
  prelude gather generalized), cross-file namespace merge, the derived `uses` include graph +
  deterministic build order, and the retained `file → imports` provenance map — commits
  `34ea5b7` (Phase 0.1), `2740616`/`c4261cb` (manifest globs + entry), `e1bd124` (P-4),
  `7556b70` (P-3). Manifest ownership moved out of the compiler into `trident` per
  `techdesign-toolchain.md`; leviathan consumes a resolved build plan (`--plan`), so P-1's
  manifest is trident's `trident.toml`. `--imports` (P-4) and `--graph` (P-3) introspection ship.
- **Phase 0.3 discoverability tooling landed (2026-07-08).** The two queries §4.4 / §6.3 named but
  the P-1..P-4 commits had not yet built: **`leviathan --namespaces`** (the symbol index — every
  namespace, the files that open it, and its members) and **`leviathan --why <name> [in <file>]`**
  (every namespace a bare name could resolve to; for a given file, which candidate is visible and
  whether the choice is ambiguous). Both derive from the same AST + offset map as `--imports` /
  `--graph` — no resolver run, no directory coupling — buying back the by-name + path-decoupled
  discoverability tax (§4.4) with a compiler query. Corpus-pinned in `tests/run_project.sh`
  (`expected.namespaces` / `expected.why` + a `why.query` sidecar).
- **Phase 0.4 polish — the optional folder≈namespace lint landed (2026-07-08).** The last
  Phase-0.4 item: **`leviathan --lint-namespaces`**, off by default (a normal build never runs it —
  §12 keeps namespaces path-decoupled). Opt-in, for teams who want the Go/Java folder-tidiness
  anyway: a root-project file that opens a namespace should sit in a directory whose path, relative
  to the project root, is a case-insensitive suffix of that namespace's `::`-segments (so
  `models/link.lev` opening `App::Models` matches — "models" ~ "Models"; a root file or a
  namespace-free composition root always passes; dependencies are never linted). It prints per-file
  `ok`/`WARN` and exits non-zero on any mismatch, so CI can enforce the convention without the
  language imposing it. Computed from the same AST + offset map as the §4.4 queries. Covered by a
  new subdirectory corpus fixture, `tests/corpus/project/layout_lint` (which also adds the first
  multi-*subdirectory* project to the corpus — exercised across oracle/IR/ELF/concat), pinned via
  `expected.lint`. This was the last *substantive* Phase-0.4 item; with it, **every deliverable the
  proposal calls out (P-1..P-4, the four introspection queries, the opt-in lint) is implemented.**
  The Phase-0.4 error-message refinements are in place or owned elsewhere: ambiguity across imports
  is the existing `distinct`/collision hard error; the file-naming half of §5.6's unknown-namespace
  bullet is already handled by `renderProjectDiagnostics` (it prints `path:line:col`); no-match-glob
  warnings and manifest-format errors are trident's (`techdesign-toolchain.md`). The *only* untaken
  §5.6 sub-item is the optional "did you mean …?" near-match *suggestion* appended to the (already
  working, already file-located) unknown-namespace message — a cosmetic hint that would restructure
  the Resolver's import loops for modest value, deliberately left out rather than destabilize a
  heavily-tested path.
