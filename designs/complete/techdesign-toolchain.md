# Tech Design — Toolchain Identity & the Trident / Leviathan Split

**Status:** design set, ready for implementation. **Date:** 2026-07-06. **Priority: HIGH.**
**Updated 2026-07-06 (owner redirect, mid-implementation):** manifest format is **TOML**,
default filename **`trident.toml`** (fixed, Cargo-style — not name-parameterized). This
replaces the original `<projectname>.lvproj` / `project{}`-literal plan below; see §9 for
the full record. Everything else in this design (the trident/leviathan split, the §3.3
build-plan contract, the milestone structure) is unchanged.
**Owner directive:** the package manager and the compiler **MUST NOT be the same
application.** Today they are (the compiler owns the project file); undoing that is the
spine of this design. **Establishes:** compiler binary `leviathan` (source ext `.lev`),
package manager binary `trident`, project manifest `trident.toml` (TOML).
**Supersedes naming in:** `designs/complete/proposal-project-system.md` and
`designs/proposal-package-manager.md` (both predate these names — they say `lang`,
`project.mf`, `.ext`, and a Go-style "subcommands of `lang`" single tool). Their
*mechanics* (MVS, lockfile, integrity, the whole-program gather) remain the reference
for Phase 2; only the names, the manifest format, and the one-tool model are overridden here.

---

## 0. Read this first

### 0.1 The mission

Split the toolchain into two separate applications, following established norms
(cargo/rustc, MSBuild/csc, SPM/swiftc):

- **`trident`** — the package manager / build driver. Owns the **project file**, builds
  the **dependency graph**, resolves and fetches deps, and hands a *resolved compilation
  unit* to the compiler. This is the front door users run.
- **`leviathan`** — a **pure compiler**. It compiles the source set it is handed. It does
  **not** parse the project manifest, does **not** resolve dependencies, does **not** know
  the registry exists.

The boundary between them is a **frozen build-plan contract** (§3.3). Once frozen, a
change to it is a STOP event (§0.3).

> **Why this is high priority.** The current architecture violates the owner's intended
> toolchain model: `src/Project.cpp` (manifest parse + gather + dependency graph +
> phantom-dep check) is compiled *into the compiler* and driven by `lang --project`. That
> conflation is the thing to remove. No new manifest/dependency logic may ever be added to
> the compiler again.

### 0.2 The tracks

Phase 0 is a single atomic prep step (one implementer, lands first). Phase 1 then splits
into **two parallel tracks with disjoint file ownership**, coordinated only through the
frozen contract of §3.3.

| Track | Role | Owns (may edit) |
|---|---|---|
| **Phase 0 (prep)** | Rename + extension + skeleton. Lands first, atomically. | `CMakeLists.txt` (target/name), all `lang`→`leviathan` and `.ext`→`.lev` references in `src/`, docs, tests; creates the empty `tools/trident/` target. |
| **Track A — compiler extraction** | Strip project/dependency ownership out of `leviathan`; give it the build-plan input. | `src/main.cpp`, `src/Project.cpp`, `src/Project.hpp`, Track-A region of `CMakeLists.txt`, the compiler-side corpus tests. |
| **Track B — trident tool** | Build `trident`: manifest ownership, plan generation, compiler invocation. | `tools/trident/**` (all new), Track-B region of `CMakeLists.txt`, `tests/trident/**` (new). |

**Interlock:** Track B codes against the §3.3 contract while Track A implements the
`leviathan` side of it. The contract is frozen at the start of Phase 1 (§3.3). The
manifest-parser code that moves from `src/Project.cpp` → `tools/trident/` is a **one-time
cut** owned by Track B, taken from Track A's post-Phase-0 tree (Track A deletes; Track B
re-homes). Sequence that cut explicitly (§7 hurdle H-1) so it is not edited on both sides
at once.

**If only one agent is available:** interleave Phase 0 → A-M1 → B-M1 → A-M2 → B-M2 → GT1.

### 0.3 STOP protocol (model escalation) — applies to every milestone

If, mid-implementation, the design turns out wrong and an **architectural** choice is
needed — in particular anything that would (a) put manifest/dependency logic back into the
compiler, (b) change the §3.3 build-plan contract, or (c) merge the two binaries — a
Sonnet-class agent must **STOP. Do not improvise a design change.** Log findings in §9
(Implementation log), commit WIP to the working branch, and escalate to a Fable-class
model for design correction. The separation in §0.1 is not negotiable at the
implementation layer.

### 0.4 Frozen / do-not-touch

- The `leviathan` compilation pipeline itself (Lexer → … → backends) is **out of scope**.
  This design changes only *how inputs reach the pipeline*, never the pipeline.
- The whole-program **gather** semantics (§12 of the project-system proposal: N sources →
  one buffer, offset map, `uses` graph, phantom-dep enforcement) are preserved
  byte-for-byte. They stay compiler-side; only their *driver* changes.
- The portable-backend work (`designs/complete/techdesign-portable-backend{,-2}.md`, gates G1–G5) is
  independent and must stay green throughout. Do not touch `runtime/**`, `src/LlvmGen.cpp`,
  `src/X64Gen.cpp`, or the backend milestones.

---

## 1. Context: what exists (verified 2026-07-06)

The compiler currently does the package manager's job. Ground truth:

- **Binary is `lang`.** `CMakeLists.txt:2` `project(lang CXX)`; `CMakeLists.txt:53`
  `add_executable(lang src/main.cpp)`; every test target shells `$<TARGET_FILE:lang>`
  (CMakeLists.txt:87–159). Error strings say "next to the `'lang'` executable"
  (`src/main.cpp:437`, `:442`, comments `:104–127`).
- **Source extension is `.ext`.** 152 `.ext` files. One hardcoded extension assumption:
  `src/Project.cpp:429` sniffs `entry` for a `.ext` suffix to decide File-vs-Function
  entry mode.
- **The compiler owns the project file.** `src/main.cpp` parses `--project <manifest>`
  (`:184`), calls `loadProject()` (`src/Project.cpp:331`), which:
  - `parseManifest()` (`Project.cpp:145`) — parses the `project { name=…; sources=[…];
    deps=[dep{…}] }` block (a literal in the language's own syntax; keyword check at
    `Project.cpp:151`);
  - `gatherSources()`/`expandSources()` (`Project.cpp:272`,`:211`) — reads/globs every
    listed source into one combined buffer with an offset map;
  - `loadDepsRec()` (`Project.cpp:289`) — recursively loads **local-path** deps, each a
    directory holding its own `project.mf` (`Project.cpp:300`,`:307`);
  - builds the module-dependency adjacency (`LoadedProject::moduleDeps`,
    `Project.hpp:89`) and applies `as` aliasing (`Project.cpp:391`).
  - The compiler then runs `validateEntry()` + `checkPhantomDeps()` (`main.cpp:292–293`)
    and gathers everything into the single-file pipeline.
- **Manifest filename is `project.mf`** (`Project.cpp:300`); every corpus project has one
  (`tests/corpus/project/*/project.mf`). The `deps` field is parsed but restricted to
  local paths — **no resolver, no lockfile, no registry, no integrity, no fetch exists.**
- **There is no package manager binary at all.** This is the violation.

`src/Project.cpp` is compiled into the shared `langfront` library (`CMakeLists.txt:21`),
so the *entire compiler* carries the manifest parser. That is exactly backwards.

---

## 2. Roadmap and timeline (authoritative)

| Phase | Scope | Gate ("done" = …) | Target |
|---|---|---|---|
| **P0** | Identity: `lang`→`leviathan`, `.ext`→`.lev` default, `trident` target skeleton, manifest `project.mf`→`trident.toml`. Mechanical, atomic. | **GT0:** full build + entire existing test suite green under the new names; `leviathan foo.lev` runs; `trident.toml` manifests load. | 2026-07-09 |
| **P1** | **The split.** Track A extracts project/dep ownership out of `leviathan` and adds the build-plan input; Track B builds `trident` (manifest owner + plan generator + compiler driver). | **GT1:** `trident` is the front door — `trident build` / `trident run` / `trident check` compile the corpus; `leviathan` no longer parses any manifest (`--project` gone or a deprecated shim, §5.4); grep proves no manifest/dep code remains in `langfront`. | 2026-07-23 |
| **P2** | **Dependencies in `trident`** — local-path deps first, then MVS + lockfile + registry + integrity, per `designs/proposal-package-manager.md` (now living entirely in `trident`). | **GT2:** local-path deps resolve through `trident` (corpus `dep_alias`/`phantom_dep` green). **GT3:** MVS + `<name>.lvlock` + content-addressed store + checksum DB; `deps` may be non-empty from a registry. | GT2 2026-08-20; GT3 2026-10-15 |

P2 is scoped by the existing package-manager proposal and gets its own milestone doc
(`designs/techdesign-package-manager.md`, the promoted proposal) when scheduled. This design
carries P2 only as far as the roadmap and the seams P1 must leave for it (§6).

---

## 3. Target architecture

### 3.1 The two applications

```
   user                    trident  (package manager / build driver)          leviathan (compiler)
   ────                    ──────────────────────────────────────────         ────────────────────
   trident build   ─────▶  1. discover  trident.toml  in project root
                           2. parse manifest (TOML; hand-rolled reader, §3.2)
                           3. resolve dependency graph  ── P2: MVS + lockfile + fetch + verify
                           4. materialize every source on disk (root + deps)
                           5. compute module adjacency (who-may-use-whom)
                           6. emit a resolved BUILD PLAN  ───────────────▶  7. read plan (NOT the manifest)
                                                                            8. gather N sources → one buffer
                                                                            9. enforce phantom-dep via the
                                                                               adjacency the plan carried
                                                                           10. compile → executable/object/IR
                           11. surface leviathan's diagnostics/exit code ◀──
```

`trident` decides *what* is compiled and *who may use whom*. `leviathan` decides *how* it
compiles. Neither reaches into the other's job.

### 3.2 What moves, what stays

Split `src/Project.cpp` along the responsibility line:

| Concern | New home | Rationale |
|---|---|---|
| **TOML manifest grammar & parse** (`parseManifest`, `ProjectManifest`, `Dependency`) | **trident** (`tools/trident/manifest.*`) | Package-manager job: it owns the project-file format. A small hand-rolled TOML reader (no external dependency — trident stays dependency-free like the rest of the toolchain). |
| **Dependency resolution** (`loadDepsRec`, dep-dir discovery, `as` aliasing; P2: MVS/lock/fetch/verify) | **trident** (`tools/trident/resolve.*`) | Package-manager job by definition. An `as` alias is materialized as a real file under the plan's build dir (the plan carries only on-disk paths, §3.3 rule 1) — "a dependency is just more source" now literally includes the synthesized alias too. |
| Which files + each file's `moduleId`/`origin` + the module adjacency | **trident** computes → carried in the plan | PM decides inputs; compiler is told. |
| **Whole-program gather** (`gatherSources`/`expandSources` reading a *given* path list into the combined buffer + offset map) | **leviathan** (`src/Project.cpp`, slimmed) | Compilation input assembly; needs the source text. |
| `computeFileImports`, `buildUsesGraph`, `validateEntry`, `checkPhantomDeps`, `renderProjectDiagnostics`, `ProjectFile`, `LoadedProject`, `EntryMode` | **leviathan** (`src/Project.cpp`) | Operate on the *resolved* set + adjacency; semantic, compiler-side. Enforcement stays here; the *policy* (adjacency) now arrives via the plan (cf. rustc enforces `--extern`, cargo decides them). |

After the cut, `leviathan` contains **zero** knowledge of the `trident.toml` format,
`Dependency`, registries, lockfiles, or MVS. Verify with grep at GT1 (§8).

### 3.3 The build-plan contract (FROZEN at start of P1)

The plan is `trident`'s machine-generated output and `leviathan`'s input. It is **not**
the human manifest — it is fully resolved (all paths absolute and on disk, all deps
already materialized as ordinary sources tagged by module). It lives in the build dir
(default `build/plan.lvplan`, override `--plan <path>`), is regenerated every build, and
is never hand-authored.

`leviathan` reads it with a small **dedicated reader** (not the manifest parser — that
lives in trident). Trident owns writing it. Frozen fields:

```
plan {
    out    = "<output path>";              // where leviathan writes the artifact
    mode   = "build-native" | "build" | "emit-llvm" | "run" | "check" | … ;  // maps to today's --* modes
    target = "<triple>";                   // "" = host
    optLevel = 0 | 2;

    entry {
        kind   = "script" | "file" | "function";   // explicit — NO extension sniffing
        target = "main.lev" | "App::main" | "";     // file path or function name
    }

    // Every resolved source, in deterministic order. moduleId "" = root project.
    src { path = "<abs>"; moduleId = "";      origin = ""; }
    src { path = "<abs>"; moduleId = "lib";   origin = "github.com/x/lib"; }
    …

    // Module adjacency: `from` may `uses` any namespace declared by `to`. Drives
    // phantom-dep enforcement (leviathan enforces; trident decided).
    edge { from = "";    to = "lib"; }
    …

    // LA-20 (additive, post-freeze): declared comptime import() build inputs,
    // resolved + hashed by trident from the manifest's `assets = [...]`.
    // `rel` is the exact string import() matches against; leviathan trusts
    // `hash` completely (rule 1 below) and never hashes anything itself. A
    // manifest with no `assets` key emits no `asset` rows — an older
    // leviathan reading such a plan sees no new grammar.
    asset { rel = "views/index.html"; path = "<abs>"; moduleId = "";
            hash = "sha256:<hex>"; }
    …
}
```

Contract rules (breaking any is a STOP event, §0.3):

1. `leviathan` trusts the plan: every `path` exists and is readable; trident guarantees
   it (deps already fetched/verified). `leviathan` does no fetching, no path resolution
   beyond opening the files.
2. `entry.kind` is **explicit**. This deletes the `.ext` suffix sniff at
   `Project.cpp:429` outright — trident tells the compiler the entry mode; the compiler
   never guesses from a filename.
3. `edge`/`moduleId` reproduce today's `LoadedProject::moduleDeps` semantics exactly, so
   `checkPhantomDeps` is unchanged apart from where its adjacency comes from.
4. The plan is the **only** channel. No side files, env vars, or manifest re-reads on the
   compiler side.

Serialization is an implementation choice for Track A's reader / Track B's writer — they
do not even share a struct definition, only this frozen text contract. The plan's own
grammar (`plan { ... }`, `src { ... }`, `edge { ... }`) is a small block-structured format
leviathan tokenizes with the language's real Lexer (levsyntax); trident's writer just
prints matching text directly (no lexer needed to emit it). Only the **fields above are
frozen.**

### 3.4 Binary discovery

`trident` locates `leviathan` in this order: `--leviathan <path>` flag → `$LEVIATHAN`
env → sibling of the `trident` executable (via `/proc/self/exe`, the same `exeDir()`
trick already in `main.cpp:108`) → `PATH`. This mirrors how cargo finds rustc and keeps a
relocated install self-consistent. On failure, a clear diagnostic naming all four probes.

---

## 4. Phase 0 — Identity (GT0)

Single atomic change, lands before any Phase-1 work. No behavior changes beyond names.

**P0-1 — binary rename `lang` → `leviathan`.**
- `CMakeLists.txt`: `project(leviathan CXX)` (`:2`); `add_executable(leviathan
  src/main.cpp)` (`:53`); `target_link_libraries(leviathan …)` (`:54`); every
  `$<TARGET_FILE:lang>` / `COMMAND lang` in the test targets (`:87–159`) → `leviathan`.
  Keep the front library name `langfront` **as-is for now** (internal; renaming it is
  churn with no user-facing value — optional cleanup, not gating). Note in the log if
  renamed to `levfront`.
- `src/main.cpp`: user-facing strings that say `lang` → `leviathan` (`:437`, `:442`,
  comments `:104–127`, usage banner `:234–238`). `argv[0]` is already dynamic.
- Sweep `docs/`, `designs/`, `tests/*.sh`, `runtime/` for the literal `lang` invocation
  in commands/examples (e.g. `src/LlvmGen.hpp:18`); update to `leviathan`. Prose
  references to "the lang executable" → "leviathan".

**P0-2 — default source extension `.ext` → `.lev`.**
- `leviathan foo.lev` already works (single-file reader takes any path) — the change is
  making `.lev` the documented default and teaching the two spots that *reason* about
  extensions:
  - `src/Project.cpp:429` entry sniff → accept `.lev` **and** `.ext` (transitional).
    (This code is deleted entirely in Track A when `entry.kind` becomes explicit, §3.3 —
    in P0 just widen it so mixed trees work.)
  - `expandSources()` (`Project.cpp:211`) directory/glob discovery: confirm it does not
    filter to `.ext`; if a bare-directory source is ever expanded, discover `*.lev` and
    `*.ext` both. (Explicit `sources = [...]` lists are unaffected — they name files.)
- **Do NOT rename** existing demo/example/corpus `.ext` files (owner directive). The
  compiler keeps accepting `.ext` indefinitely. Only *main system files and new files*
  are `.lev`: rename the root demo `project.ext` → `demo.lev` (it is the CMake-wired
  canonical file, i.e. a system file, so it moves), and write all *new* fixtures/system
  files as `.lev`.

**P0-3 — manifest format `project.mf` (project{} literal) → `trident.toml` (TOML).**
- **Updated 2026-07-06 (owner redirect, §9):** the manifest is **TOML**, not the
  language's own `project{}` literal, and the filename is the **fixed** `trident.toml`
  (Cargo-style — not name-parameterized, unlike the original `<projectname>.lvproj` plan).
  `trident` (P1) discovers exactly `trident.toml` in the project root; there is no
  ambiguity to resolve since the name is fixed.
- Every existing manifest — the root demo, `examples/curl/`, and every
  `tests/corpus/project/*/project.mf` (root and dependency subdirectories) — is converted
  to `trident.toml` as part of this same change (owner decision: one manifest format, not
  a legacy fallback). `project.mf` is deleted everywhere; `leviathan --project` never
  existed for TOML and is removed outright in Track A (§5.4(a) — the compiler never gains
  a TOML-shaped door to begin with).
- No dedicated TOML dependency: trident carries a small hand-rolled reader for the
  minimal subset a manifest needs (top-level string/string-array/bool fields, plus
  repeated `[[dep]]` array-of-tables) — see §5.2 B-M1.

**P0-4 — `trident` target skeleton.**
- `CMakeLists.txt`: `add_executable(trident tools/trident/main.cpp)`; `tools/trident/
  main.cpp` a stub that prints usage and exits 2. No logic yet — this reserves the target
  and the directory so Phase-1 tracks have disjoint homes.

**Acceptance (GT0):**
```
cmake --build build            # builds `leviathan` + `trident` (stub) + tests
ctest --test-dir build         # entire existing suite green under new names
build/leviathan --run demo.lev                                 # single-file .lev runs
build/trident check .  --leviathan build/leviathan             # trident.toml resolves,
                                                                # drives leviathan (GT1
                                                                # once B-M1/B-M2 land)
```

---

## 5. Phase 1 — The split (GT1)

### 5.1 Track A — compiler extraction (owns `src/main.cpp`, `src/Project.{cpp,hpp}`, Track-A CMake region)

**A-M1 — add the build-plan reader; teach `main.cpp` to compile from a plan.**
- New `src/BuildPlan.{hpp,cpp}` (Track A owns): the §3.3 struct + a small reader. No
  manifest concepts.
- `main.cpp`: add `--plan <file>`. When given, populate a `LoadedProject`-shaped value
  *from the plan* (resolved paths, moduleId/origin, adjacency, explicit entry) and run the
  **existing** gather + validateEntry + checkPhantomDeps + pipeline unchanged. Reuse the
  current gather by feeding it the plan's path list — do not re-implement gather.
- Delete the `Project.cpp:429` extension sniff; entry mode comes from `entry.kind`.
- Acceptance: hand-write a `plan.lvplan` for two corpus projects (one File-entry, one
  Function-entry, one phantom-dep case) and confirm `leviathan --plan …` produces
  byte-identical diagnostics/output to today's `--project` on the same sources.

**A-M2 — remove manifest ownership from the compiler.**
- Excise `parseManifest`, `depList`, `ProjectManifest`, `Dependency`, `loadDepsRec`,
  `expandSources`' manifest-glob coupling, and the `as`-aliasing block from
  `src/Project.cpp` / `Project.hpp`. Hand this excised code to Track B (§0.2 one-time cut,
  H-1) — Track A deletes; Track B re-homes into `tools/trident/`.
- Keep in `Project.cpp`: the gather-from-path-list, `ProjectFile`/`LoadedProject`/
  `EntryMode`, `computeFileImports`, `buildUsesGraph`, `validateEntry`,
  `checkPhantomDeps`, `renderProjectDiagnostics`.
- `--project` in `main.cpp`: replace with the deprecated shim of §5.4 (or remove, per the
  decision there).
- Acceptance: `grep -nE 'parseManifest|project\.mf|Dependency|trident\.toml' src/` returns
  **nothing** (proves the compiler no longer knows the manifest). Full suite green via
  `--plan` fixtures / through `trident` once B-M2 lands.

### 5.2 Track B — the `trident` tool (owns `tools/trident/**`, `tests/trident/**`, Track-B CMake region)

**B-M1 — manifest + resolve + plan.** **Updated 2026-07-06 (owner redirect, §9):** the
manifest is TOML, not the `project{}` literal, so `tools/trident/manifest.{hpp,cpp}` is a
small **hand-rolled TOML reader** (top-level string/string-array/bool fields, repeated
`[[dep]]` array-of-tables — no external TOML dependency, matching the toolchain's existing
anti-dependency stance). `tools/trident/resolve.{hpp,cpp}` re-homes the excised dependency
logic (from A-M2): `loadDepsRec`-equivalent recursion, dep-dir discovery (H-4), and `as`
aliasing — an alias's synthesized `namespace X { uses Y; }` body is written to a real file
under the plan's build dir (not spliced in-memory as the compiler used to) since the plan
only carries on-disk paths (§3.3 rule 1). `tools/trident/plan.{hpp,cpp}` is the §3.3
writer, working from `resolve.*`'s output — it owns no manifest concepts and shares no C++
struct with the compiler's own `src/BuildPlan.hpp`, only the frozen text contract.
`trident` discovers the project root's `trident.toml`, resolves it (manifest + deps +
aliasing), and emits a plan whose `src` list carries every gathered file's `moduleId`/
`origin` (root sources at `moduleId ""`, each dep's at its own), `entry` classified
explicitly from the manifest's `entry` string (the old extension-sniff heuristic moves
here, verbatim, since something must still decide file-vs-function — leviathan just never
does it itself), and `edge` rows from the dependency graph.

Factor a slim **`levsyntax`** library (`src/Token.cpp`, `src/Lexer.cpp`,
`src/Diagnostic.cpp`, `Source.hpp`) that both `langfront` and `trident` link (§8 CMake).
With the manifest now TOML, `levsyntax` is no longer needed for manifest parsing — it is
still needed for (a) leviathan's build-plan reader (`src/BuildPlan.cpp`, Track A, tokenizes
`plan { ... }` with the real Lexer) and (b) trident's own `scanExportedNamespaces` (a
lexical, no-full-parse scan of a dependency's `.lev`/`.ext` sources for top-level
`namespace` declarations, feeding `as` aliasing). *(Fallback if factoring proves noisy:
link `langfront` and use only its lexer — record the choice in the log; the responsibility
separation is what matters, not the link graph.)*

**B-M2 — driver.** `trident build [--target <triple>] [--opt-level <0|2>]`, `trident run`,
`trident check`, `trident emit-llvm`, plus `trident plan` (resolve + write only, no
invocation — a scripting primitive `tests/run_project.sh` drives directly so it can run
several leviathan modes against the *same* resolved plan, the way it always drove several
modes against the same `--project`). Each of the four real subcommands: discover manifest
→ resolve (parse + deps + aliasing) → write plan → locate `leviathan` (§3.4) → invoke
`leviathan --plan …` with the mode mapped from the subcommand → relay diagnostics + exit
code (fork/execv, not `system()` — no shell quoting to get wrong). `trident --version`
prints both its own and `leviathan --version` (leviathan gained a matching `--version`
flag as part of this bullet).
- Acceptance (GT1), as actually landed: `tests/run_project.sh` itself now resolves every
  `tests/corpus/project/*` fixture through `trident plan` before driving `leviathan
  --plan` across the oracle/IR/ELF/imports/graph/concat checks — a superset of the
  originally-named semantic cases (phantom_dep, use_leak_err, uses_cycle, entry_file,
  dep_alias), since it covers all 25 fixtures uniformly with the same rigor as before.
  `trident build examples/curl` links and runs (verified: all 72
  `examples/curl/test/run-tests.sh` cases pass across `--run`/`--ir`/`--native`, driven
  through a `trident plan` once at the top of that script). `tests/trident/manifest_errors/*`
  (4 fixtures: missing sources, unterminated string, unknown field, an unrecognized
  `[[table]]`) + `tests/run_trident_manifest_errors.sh` cover H-2's other half — trident's
  *own* manifest-format diagnostics, independent of leviathan's semantic ones.

### 5.3 Merge order
Phase 0 lands first (atomic). Then A-M1 and B-M1 proceed in parallel against the frozen
§3.3 contract. The A-M2↔B-M1 code cut is sequenced (H-1): Track A does not delete the
parser until Track B has taken it (or takes it from a shared WIP commit). GT1 requires
both tracks merged; `trident` becomes the documented front door and `--project` is retired
per §5.4. Branch merge implies push (project convention).

### 5.4 What happens to `leviathan --project`
Two options; **recommend (a)** for a clean break the owner is asking for:
- **(a) Remove it.** The compiler's only project entry is `--plan`. Corpus project tests
  go through `trident`. Cleanest; matches "these MUST NOT be the same application" —
  the compiler cannot even open a manifest.
- (b) Keep a deprecated shim that prints "use `trident`" and internally builds a
  single-module plan (no dep support). Lower test churn, but leaves a manifest-shaped door
  on the compiler. If chosen, it is temporary and deleted at GT2.

Decision recorded at implementation; default to (a) unless test-migration cost forces (b)
for one milestone.

**Decision: (a).** `--project` is removed from `main.cpp`; `--plan` is the compiler's only
project-shaped entry. `tests/run_project.sh`, `tests/run_plan.sh`, and
`examples/curl/test/run-tests.sh` all migrated cleanly (§9).

---

## 6. Phase 2 seams (leave these open in P1)

P1 must not foreclose the package-manager proposal. Ensure:
- The plan's `src` rows already carry `moduleId` + `origin`, and `edge` carries adjacency
  — so P2 dep resolution just produces more `src`/`edge` rows; **leviathan needs no change
  for deps.** (This is the payoff of the split: "a dependency is just more source,"
  proposal §1, now realized as more plan rows.)
- Lockfile lands as `<name>.lvlock` beside the manifest (trident-owned; compiler never
  sees it).
- MVS, content-addressed store, checksum DB, `trident add/publish/yank/audit`, zero
  install-time code execution — all per `designs/proposal-package-manager.md`, implemented
  **inside `trident`**. That proposal's "subcommands of `lang`" framing is replaced by
  "subcommands of `trident`"; its trust/security/MVS mechanics are unchanged.

---

## 7. Suspected hurdles (read before each milestone)

- **H-1 (the code cut).** Moving the manifest parser from `src/Project.cpp` (Track A
  deletes) to `tools/trident/` (Track B adds) must not be edited on both sides
  simultaneously. Sequence: Track A commits the excision to a shared WIP point; Track B
  cherry-picks/re-homes; then both proceed. Do not race it.
- **H-2 (diagnostics ownership).** Manifest *format* errors were the compiler's; they
  become trident's. Semantic errors (phantom-dep, entry-rule, use-scoping) stay the
  compiler's but now surface *through* `trident`. When migrating corpus tests, sort each
  `.expected` into the right tool. `renderProjectDiagnostics` stays compiler-side (it maps
  combined-buffer offsets back to per-file lines — a gather concern, not a manifest one).
- **H-3 (`levsyntax` factoring).** Pulling Lexer/Token/Diagnostic into a shared lib must
  not disturb `langfront`'s existing includes. If the include graph fights back, use the
  fallback in B-M1 and log it — do not block GT1 on the factoring. (Landed clean — no
  include-graph conflicts; see §9.) With the manifest now TOML, this lib's manifest-parsing
  rationale is gone, but it is still load-bearing for leviathan's build-plan reader and
  trident's dependency-namespace scan (§5.2 B-M1).
- **H-4 (dep-dir discovery).** `loadDepsRec` hardcoded `project.mf` (`Project.cpp:300`).
  Superseded by the TOML redirect (§9): trident's dep-dir discovery looks for the fixed
  `trident.toml` only — no legacy fallback, since every fixture (root, dep subdirectories,
  the curl example) was converted in the same change that introduced TOML.
- **H-5 (don't re-add manifest logic to the compiler).** The tempting shortcut under test
  pressure is to let `leviathan` peek at the manifest "just for entry mode" or "just for
  one dep." That is the exact violation being removed. STOP and escalate (§0.3) instead.

---

## 8. Testing strategy & CMake discipline

- **Lanes.** Compiler lane: `leviathan --plan` fixtures (`plan_contract_conformance`,
  `tests/run_plan.sh`) + all existing non-project corpus (unchanged). Project lane:
  `tests/corpus/project/*` resolved through `trident plan` inside `tests/run_project.sh`
  itself (semantic cases; see the B-M2 bullet's "as actually landed" note for why this is
  one script, not a separate parallel tree). Trident lane: `tests/trident/manifest_errors/*`
  + `tests/run_trident_manifest_errors.sh` — trident's own manifest-format diagnostics
  (H-2), independent of leviathan. All three under `ctest`.
- **GT1 proof that the split is real:**
  ```
  grep -rnE 'parseManifest|Dependency|trident\.toml|project\.mf' src/    # → no matches
  grep -rn 'leviathan' tools/trident/ | grep -v exeDir                   # trident only *invokes* leviathan, never links its pipeline
  ```
- **CMakeLists.txt discipline** (two clearly-marked regions, as with the backend tracks):
  ```
  # --- levsyntax: lexer/token/diagnostic shared by compiler + trident (techdesign-toolchain.md §5.2) ---
  # --- Track A: compiler build-plan input (techdesign-toolchain.md §5.1) ---
  # --- Track B: trident package manager (techdesign-toolchain.md §5.2) ---
  ```
  Each track edits only its region; `add_executable(trident …)` and `levsyntax` are Track
  B / shared. Keep the portable-backend regions untouched.
- Regression bar: the full pre-existing suite must stay green at every gate (byte-identical
  oracle output where it was byte-identical before).

---

## 9. Implementation log (append-only)

**2026-07-06 — Phase 0 + Phase 1 (both tracks) landed in one solo pass, per §0.2's
interleave order (Phase 0 → A-M1 → B-M1 → A-M2 → B-M2 → GT1). Full suite green
(39 ctest targets) at every gate. Notable decisions and deviations below.**

- **Mid-implementation owner redirect: TOML, not `.lvproj`.** Partway through A-M2 (after
  Phase 0 and A-M1 had already landed against the original `<projectname>.lvproj` /
  `project{}`-literal design), the owner redirected: manifest format is TOML, default
  filename the fixed `trident.toml`. This is reflected throughout the design above (marked
  "Updated 2026-07-06"); it is not a Sonnet-improvised deviation — the owner asked for it
  directly, so per this session's instructions it was applied to the design itself rather
  than treated as a STOP-and-escalate event. Two follow-up questions were asked and
  answered before implementing: (1) convert every existing `project.mf`/`project{}`
  fixture to TOML now (chosen) vs. dual-format support; (2) hand-rolled minimal TOML
  reader, no external dependency (chosen) vs. vendoring a library. All 25
  `tests/corpus/project/*/project.mf` files (root + nested dep dirs) plus
  `examples/curl/project.mf` were mechanically converted via a one-off script
  (not checked in) and the originals `git rm`'d. `leviathan --project` was never made to
  understand TOML — it was removed in the same A-M2 pass per §5.4(a), so the compiler
  never gained a TOML-shaped door at all.
- **`levsyntax` (H-3) landed clean** — no include-graph conflicts splitting
  Diagnostic.cpp/Token.cpp/Lexer.cpp out of `langfront` into a shared static lib that
  `langfront` links `PUBLIC`. No fallback needed. Its role shifted with the TOML redirect:
  no longer used for manifest parsing (trident's TOML reader is a from-scratch
  character-cursor parser, no Lexer involvement), but still load-bearing for (a)
  `src/BuildPlan.cpp`'s plan-text reader and (b) trident's `scanExportedNamespaces` (`as`
  aliasing needs to lexically scan a dependency's `.lev`/`.ext` sources for top-level
  `namespace` declarations).
- **`entryName` decoupling (A-M1, ahead of A-M2's need).** `validateEntry`'s diagnostic
  text originally read `proj.manifest.entry` directly. Since `ProjectManifest` is deleted
  from the compiler at A-M2, `LoadedProject` gained a plain `std::string entryName` field
  (set by both `loadProject` transitionally and `loadProjectFromPlan`) so `validateEntry`
  never had to change shape across the two milestones — done proactively in A-M1 once the
  problem was visible, not left as an A-M2 scramble.
- **Trident's alias materialization directory follows `--plan`, not a hardcoded `./build`.**
  Caught before it shipped: `resolveProject`'s build/alias dir was originally hardcoded to
  `"build"` relative to cwd regardless of an explicit `--plan <path>` override, which would
  have littered the real CMake build directory with `trident-alias-*.lev` files whenever a
  test script passed its own tmpdir via `--plan`. Fixed so the alias/build dir is derived
  from `--plan`'s own directory when given, `./build` only as the no-flag default.
- **B-M2 roadmap ambiguity, resolved in favor of the more specific text.** §2's roadmap
  table frames "local-path deps resolve through trident" as a P2/GT2 item, while §5.1's
  A-M2 bullet and §5.2's B-M1/B-M2 bullets already describe re-homing `loadDepsRec` and
  `as` aliasing into trident *during P1*, and B-M2's own acceptance explicitly names
  `dep_alias`/`phantom_dep` as cases to migrate. Treated the milestone-level text (§5) as
  authoritative over the summary roadmap table (§2) for *when* dependency resolution
  becomes real — local-path deps (the existing Phase-1 scope, not P2's MVS/lockfile/
  registry) work end-to-end through trident as of B-M2/GT1. This is a documentation
  ambiguity, not one of the three STOP triggers (§0.3), so it was reconciled rather than
  escalated.
- **`tests/run_project.sh` migrated in place, not duplicated into `tests/trident/**`.**
  Rather than hand-picking the 5 named semantic fixtures into a new parallel test tree, the
  existing script was changed to resolve every `tests/corpus/project/*` fixture through
  `trident plan` before driving `leviathan --plan` across all its existing checks (oracle/
  IR/ELF/imports/graph/concat-equivalence). This covers the 5 named cases plus the other
  20 fixtures uniformly, with zero loss of existing rigor, and is a strictly smaller diff
  than forking the test machinery. A new `trident plan` subcommand (resolve + write, no
  `leviathan` invocation) was added to support this — needed because the script drives
  several `leviathan` modes against one resolved manifest, the same way it always drove
  several modes against one `--project` path.
- **`tests/trident/manifest_errors/**` added** (H-2's other half): 4 fixtures exercising
  trident's own format diagnostics (missing `sources`, unterminated string, unknown field,
  unrecognized `[[table]]`) + `tests/run_trident_manifest_errors.sh`, wired into `ctest` as
  `trident_manifest_errors`.
- **Doc-sweep scope (P0-1).** Swept and updated: `CMakeLists.txt`, `src/main.cpp`,
  `src/BuildPlan.{hpp,cpp}`, `src/CGen.cpp` (one generated-code comment string),
  `src/LlvmGen.hpp` (a comment; `.cpp` left untouched, see below), `runtime/build-triple.sh`
  (one comment), `tests/*.sh`, `fuzz/*.py`, `fuzz/README.md`, `examples/curl/test/
  run-tests.sh`, and the two *living* reference docs `docs/reference.md` + `info.md`.
  Deliberately **left untouched** (historical/frozen, not the living CLI reference this
  bullet is about): `designs/complete/proposal-project-system.md` and `designs/proposal-package-manager.md`
  (already carry an owner-authored superseding banner — this predates this implementation
  pass), `designs/complete/techdesign-portable-backend{,-2}.md`, `designs/complete/techdesign-metaprogramming.md`,
  `designs/complete/techdesign-metaprog-phase4.md`, `the Track-A handoff (removed 2026-07-07)`,
  `designs/proposal-metaprogramming.md`, every dated "verified against this tree" note under
  `designs/` and `designs/complete/`, and `examples/curl/curl-design.md` — these are other
  tracks' authored specs or dated historical investigation notes; rewriting their `lang`
  mentions would be revisionist (they record what was literally true on the date written).
  `src/LlvmGen.cpp` and `src/X64Gen.cpp` were left completely untouched, including one
  cosmetic `"lang"` string each — both are named explicitly in §0.4's do-not-touch list,
  which reads as a stronger, file-level freeze than the directory-level docs/designs/tests
  sweep instruction; the two named files are one-line, zero-behavior-impact strings either
  way, so nothing was lost by leaving them.
- **GT1 verified:** `grep -rnE 'parseManifest|Dependency|trident\.toml|project\.mf' src/`
  → no matches. `grep -rn 'leviathan' tools/trident/ | grep -v exeDir` → only strings/
  comments/CLI-arg plumbing, no pipeline symbols linked. Full ctest suite green (39/39);
  `examples/curl/test/run-tests.sh` green (72/72 across `--run`/`--ir`/`--native`), driven
  through trident, not wired into `ctest` (matches its pre-existing status — a manual
  example test, not part of the automated suite).

**2026-07-10 — completion verification on current master + reference sync; design moved to
`designs/complete/`.** Re-verified the whole P0+P1 scope (the toolchain split proper; P2 is
tracked in `designs/techdesign-package-manager.md` per §2) still holds on the current merged
tree, after the intervening bug.md/Resolver/Project.cpp churn that landed since the 2026-07-06
pass:

- **Full build green** — `cmake --build build` builds `levsyntax`, `langfront`, `leviathan`,
  and `trident` (plus all test targets) clean.
- **Full suite green — 102/102 ctest** (the suite has grown from 39 targets since the original
  pass). Trident/project lanes all pass: `mvstests`, `storetests`, `fetchtests`, `locktests`,
  `check_demo_trident`, `corpus_project`, `plan_contract_conformance`, `trident_manifest_errors`,
  `trident_vcs_app`.
- **GT0 re-verified:** `leviathan --version` / `trident --version` report `0.1 (techdesign-
  toolchain.md Phase 1)`; a bare `.lev` file runs via `leviathan --run`; `trident check
  tests/corpus/project/geometry --leviathan build/leviathan` resolves `trident.toml` and drives
  the compiler.
- **GT1 re-verified:** `grep -rnE 'parseManifest|Dependency|trident\.toml|project\.mf' src/`
  → no code matches (one prose hit in a `Project.hpp` doc comment only); `leviathan` no longer
  accepts `--project` (usage banner: `(<source-file> | --plan <build-plan>)`), only `--plan`;
  `trident` links only `levsyntax`, never `langfront`.
- **Reference sync (the one substantive gap found):** `docs/reference.md` §8 and its intro still
  described the *old* `leviathan --project project.mf` / `project{}`-literal model — that section
  of the 2026-07-06 doc-sweep had not survived into master (intervening merges to `reference.md`).
  Rewrote §8.0 (the two-tool split + build-plan boundary + trident subcommands), §8.1
  (`trident.toml` TOML manifest), and §8.2 (TOML `[[dep]]`, `dev`, VCS-vs-local, phantom-dep
  ownership); fixed the compiler-CLI block's stale `--project` line to `--plan`; updated the
  reference/`info.md` headers from "split now underway" to the landed split pointing at
  `designs/complete/`. No compiler/trident code changes were needed — the split itself was already
  correct and green; only the living reference doc had drifted.

The toolchain design (P0 + P1 / GT0 + GT1) is **complete**.
