# Tech Design — Tooling Renames: Harpoon→Sonar, Sonar→Moby (unified)

**Complexity:** sonnet — pure tooling/framework/demo work (folders, `trident.toml`
manifests, import aliases, docs). No Leviathan compiler/source (`src/**`,
`runtime/**`) is touched, so this is covered by the framework/demo/test tier of
the complexity rules in [policies.md](../docs/policies.md).

## 1. Goal

Two renames plus a directory merge, declared by the user:

1. **Harpoon → Sonar.** The unit-test framework (currently `harpoon/`, package
   `harpoon`) is renamed to **Sonar**, which fits its role as a *detection*
   device.
2. **Sonar → Moby**, unified. The TUI framework currently exists as **two**
   directories — `sonar/` (v0.1.0) and `sonar_v2/` (v0.2.0), both declaring
   package `name = "sonar"`. These are merged into a **single** package
   **`moby/`** built from the most up-to-date code (v2), and the old `sonar/`
   is retired.

The name `sonar` is being **vacated** by the TUI and **claimed** by the test
framework. This creates a hard ordering constraint (see §4).

## 2. Motivation

- **Name clarity.** "Sonar" (detection) describes a test framework better than a
  TUI toolkit; "Moby" frees the name cleanly.
- **Eliminate duplicated maintenance.** `sonar/` and `sonar_v2/` share their
  entire core layout (`ansi_renderer`, `app`, `attributes`, `component`,
  `container`, `cursor`, `damage`, `errors`, …) and carry literal file
  duplicates that are edited in lockstep today. Evidence:
  `designs/sonar_v2/sonar-bugs.md:249` — *"Applied to `sonar/src/mixins.lev` AND
  the `sonar_v2/src/mixins.lev` copy."* Collapsing to one package removes this
  double-edit tax.

## 3. Current state (verified against the tree)

### 3.1 Directory inventory
| Dir | Package name | Version | `*.lev` files | Last commit | Consumer |
|-----|------|------|------|------|------|
| `harpoon/` | `harpoon` | 0.1.0 | — | 2026-07-19 | none as a dep (standalone; own `tests/runtests.sh`) |
| `sonar/` (v1) | `sonar` | 0.1.0 | 47 | 2026-07-19 | `examples/recon` only |
| `sonar_v2/` (v2) | `sonar` | 0.2.0 | 64 | 2026-07-22 (active) | `examples/helm` (flagship IDE) + all helm tests |

v2 is a **superset** of v1 (same core + an added `dom/` subsystem of ~17 files),
so unifying onto v2 is realistic rather than a divergent-rewrite reconciliation.

### 3.2 Consumers and wiring

**TUI consumers** declare a path dep and alias it `as = "Sonar"`; code refers to
the alias, not the package name:

```toml
# examples/recon/trident.toml            # examples/helm/trident.toml
[[dep]]                                   [[dep]]
path    = "../../sonar"                   path    = "../../sonar_v2"
as      = "Sonar"                         as      = "Sonar"
version = "0.1.0"                         version = "0.2.0"
```

Alias reference surface (`Sonar::` in source):
- `examples/recon`: **45** refs across **13** files.
- `examples/helm`: **6** refs across **6** files.

Per-test path deps that also need repointing:
- helm tests → `../../../../sonar_v2`: `search, shell, spike, status, terminal,
  command, build, filetree, config, langloop, panels, editor` (12 test manifests).
- recon tests → `../../../../sonar`: `sources, textarea, e2e` (3 test manifests).

**Harpoon consumers.** No `[[dep]]` anywhere points at `harpoon` — it is a
standalone framework exercised by `harpoon/tests/runtests.sh`. The only external
coupling is a **runtime output-format** dependency in Helm (see §6).

## 4. The ordering hazard (critical)

Because `harpoon → sonar` collides with the folder + package name the TUI
currently occupies, the steps are **not commutative**. Correct order:

1. **First** merge the TUI into `moby/` and delete both `sonar/` and
   `sonar_v2/`. This frees the folder path `sonar/` and the package name `sonar`.
2. **Then** rename `harpoon/ → sonar/` and package `harpoon → sonar`.

Reversing this produces two things named `sonar` simultaneously. The
implementation must gate step 2 on step 1 being complete and the tree clean.

## 5. Design

### Part A — Merge the TUI into `moby/` (canonical = v2)

1. **Rename `sonar_v2/ → moby/`** (this is the code that survives).
2. **`moby/trident.toml`:** `name = "sonar"` → `name = "moby"`; keep
   `version = "0.2.0"`.
3. **Delete `sonar/`** (v1). Before deleting, diff v1's `src/` against v2's to
   confirm no v1-only file carries behavior recon needs that v2 dropped (spot:
   `mixins.lev` and any file present in `sonar/src` but absent in `sonar_v2/src`).
4. **Retarget helm** (`examples/helm/trident.toml` + the 12 helm test manifests):
   `path = "../../sonar_v2"` → `"../../moby"` (adjust `../../../../` depth
   likewise). Update `version` stays `0.2.0`.
5. **Alias.** Change `as = "Sonar"` → `as = "Moby"` and rewrite helm's 6
   `Sonar::` refs → `Moby::` (**maximum**; see §8 for the minimum that keeps the
   alias as `Sonar`).
6. **recon** (`examples/recon`): repoint its dep from `../../sonar` (v0.1.0) to
   `../../moby` (v0.2.0) as a **best effort** — recon is a throwaway demo the
   language was tested against, not a maintained deliverable. Since v2 is a
   superset, expect it to build. Run recon's tests:
   - green → keep recon as a Moby demo (rewrite its 45 `Sonar::` → `Moby::` if
     doing the maximum alias rename).
   - broken by a v1-only API v2 removed → **archive/retire recon** rather than
     invest in porting. Do not let recon block the rename.

### Part B — Rename Harpoon → Sonar (test framework)

*Runs only after Part A has vacated the `sonar` name.*

1. **Rename `harpoon/ → sonar/`**.
2. **`sonar/trident.toml`:** `name = "harpoon"` → `name = "sonar"`.
3. **Internal references.** Rename symbols/text inside the framework as
   appropriate (`harpoon/README.md`, `src/runner.lev`, `src/registry.lev`,
   `src/discover.lev`, `src/assert.lev`, test manifests, `tests/runtests.sh`).
   The runner's user-visible report prefix (see §6) is part of the rename.

## 6. Helm ↔ test-runner format coupling (do not miss)

Helm parses the test runner's **stdout** to render results. `helm/src/build/harpoon.lev`:

```
// H11 — the harpoon test-output parser (§7.2, G-H4).
// ... Helm's Test command runs a harpoon test project through `trident run`
// and parses harpoon's **stdout** report here. The exact format is
// `harpoon/src/runner.lev`'s `runAll` output:
//     harpoon: 3 tests, 1 classes
```

If Part B changes the runner's `harpoon:` prefix to `sonar:`, three helm files
must change **in lockstep** or Helm's test panel silently stops parsing results:
- `examples/helm/src/build/harpoon.lev` (the parser; also rename the file →
  `sonar.lev` and fix its manifest entries in `tests/build/` and `tests/shell/`),
- `examples/helm/src/panels/testresults.lev`,
- `examples/helm/src/proc/proc.lev`.

Decision knob: either (a) rename the runner prefix and update all three helm
sites, or (b) keep the runner's literal output prefix stable for compatibility
and only rename the framework's identity. **Recommended: (a)** — a rename that
leaves `sonar:` printing `harpoon:` is a half-rename. This is the one spot in
the whole task that touches a *behavioral* contract, not just a name.

## 7. Docs & reference updates

- **Active docs to update:** `info.md`, `docs/reference.md`, `docs/policies.md`
  (already refers to Sonar/harpoon — re-verify after rename), the moved
  framework's `README.md`/`CHANGELOG.md`.
- **Design docs still in flight** (`designs/helm/techdesign-00-overview.md`
  says "A Full IDE on Sonar 2", `designs/sonar_v2/**`): update brand references
  to "Moby" where the doc is still governing active work.
- **Archived/complete designs** (`designs/complete/**`) are historical records —
  **do not** rewrite them; they describe the state at the time they landed.
- Add a gotcha entry to `docs/gotchas.md`: the three synchronized decoders-style
  lockstep note for the §6 runner-format coupling.

## 8. Minimum vs. maximum

- **Minimum (identity-only):** rename folders + `trident.toml` `name` fields +
  dep `path`s; **keep** consumer aliases as `as = "Sonar"`, so no `Sonar::`
  source churn (0 of the 45+6 refs change). Runner prefix left as-is. Fastest,
  but leaves code reading `Sonar::` against a package named `moby` — confusing.
- **Maximum (full rebrand, recommended):** everything in §5–§7, including alias
  `Sonar → Moby` and the 51 `Sonar::` source rewrites, the runner-prefix change,
  and doc sweep. Coherent end state; this is what "rename the project" means.

## 9. Execution order (implementer checklist)

1. Branch is your agent branch; work mechanically, commit in logical chunks.
2. **Part A**: `sonar_v2 → moby`; edit `moby/trident.toml` name; retarget helm
   (main + 12 test manifests) + alias; handle recon (repoint, test, keep-or-
   archive); **delete `sonar/`**. Run `moby/tests/runtests.sh` + helm tests +
   recon (if kept). All green before proceeding.
3. **Part B** (only now): `harpoon → sonar`; edit `sonar/trident.toml` name;
   rename runner prefix; update helm's 3 parser sites + rename
   `helm/src/build/harpoon.lev → sonar.lev` + its manifest refs. Run
   `sonar/tests/runtests.sh` + helm build/shell tests.
4. Docs sweep (§7). Update `docs/gotchas.md`.
5. Full validation (§10), then Post Work per policy (branch → merge master →
   test → push → merge master).

## 10. Testing / verification plan

Per [policies.md](../docs/policies.md) §Testing — these are all Leviathan-written
packages, so they are covered by **Sonar** (the renamed framework) and each
package's `runtests.sh`, not ctest. No compiler change means the ctest gate is
not the relevant one here, but run it once at the end to confirm no accidental
`src/**` fallout.

1. `moby/tests/runtests.sh` — the merged framework's own suite, green.
2. `examples/helm` full test suite green against `moby` (proves the TUI
   consumer wiring + alias rename landed).
3. `examples/recon` — green against `moby`, or explicitly archived with a note.
4. Framework-renamed: `sonar/tests/runtests.sh` (ex-harpoon) green.
5. Helm Test panel end-to-end: run a `sonar` (ex-harpoon) test project through
   Helm and confirm the results panel still parses the report (guards §6).
6. `grep -rIn "harpoon" --exclude-dir=build --exclude-dir=.git` and the same for
   stale `sonar_v2`/`../../sonar` paths return only intended residue (archived
   designs). Zero dangling dep paths.

## 11. Risks & open items

- **§6 format coupling** is the only behavioral risk; everything else is
  identity. Test #5 exists to catch it.
- **recon fate** is resolved as best-effort/archive — not a blocker.
- **Archived-design churn**: resist rewriting `designs/complete/**`; only active
  docs move.
- **Two-things-named-sonar window**: enforce the Part A → Part B ordering; never
  commit a state with both a TUI and a framework claiming `sonar`.

## 12. Out of scope

- No changes to the Leviathan compiler, runtime, or standard library.
- No API/behavior changes to either framework beyond the runner's report prefix.
- No new features; this is rename + merge only.
