# Tech Design — Trident Package Manager (Phase 2: Dependencies)

**Status:** complete — GT2–GT6 landed; archived 2026-07-19. **Date:** 2026-07-06.
**Priority: HIGH (post-GT1).**
**Promotes:** `designs/proposal-package-manager.md` into an implementation-ready milestone
doc, exactly as `designs/complete/techdesign-toolchain.md` §2 said it would ("P2 … gets its own
milestone doc, `designs/complete/techdesign-package-manager.md`, the promoted proposal, when
scheduled"). The proposal's *mechanics* — MVS, lockfile-as-hash-manifest, immutability,
checksum DB, zero install-time code, content-addressed store — are the reference and are
carried forward unchanged; only names, the manifest format (TOML, not the `project{}`
literal), and the one-tool framing were already overridden by `techdesign-toolchain.md`
and stay overridden here.
**Builds on:** `designs/complete/techdesign-toolchain.md` (the Trident/Leviathan split, GT0–GT1,
landed 2026-07-06). This design is **Phase 2 (P2)** of that roadmap — it delivers **GT3**
and the milestones beyond it. **Everything in this design lives inside `tools/trident/`.**
Per the toolchain design's §6 seam, **`leviathan` needs no change for dependencies**: the
frozen build-plan contract (`techdesign-toolchain.md` §3.3) already carries `moduleId`,
`origin`, and `edge` rows, so a resolved dependency is *just more `src`/`edge` rows in the
plan trident already writes*. That non-negotiable is the spine of this design (§0.3).

---

## 0. Read this first

### 0.1 The mission

`techdesign-toolchain.md` split the toolchain and made `trident` the front door, but
`trident`'s dependency support is **local-path only** (a dep's `path` is a directory on
disk, resolved by `loadDepsRec` in `tools/trident/resolve.cpp`). This design adds **real
dependencies**: fetched by VCS path at a version, selected by **Minimal Version Selection
(MVS)**, pinned by a **lockfile**, verified by a **checksum database**, and cached in a
**global content-addressed store** — with **zero install-time code execution** and **no
mandatory central registry**. The one-line thesis is unchanged from the proposal:

> **A dependency is not a new kind of thing — it is more source fed to the same gather.**
> `trident` *fetches, verifies, and edits the manifest*; `leviathan` *builds*. The package
> manager's whole new job is: get the right source for each declared dependency, prove it
> hasn't changed, and hand it to the plan writer as more `src`/`edge` rows.

The model is **Go-modules-shaped and hardened** (proposal §2 survey): import by VCS path
(ownership implied, no name-squatting) → MVS selection (deterministic, unique, no SAT
solver) → lockfile-as-hash-manifest → immutable + yank-never-delete versions backed by a
checksum DB → zero install-time code → content-addressed source store, with an *optional*
caching proxy (left-pad immunity) and *optional* thin index (discoverability) that never
become a mandatory single point of failure.

### 0.2 One track, sequential milestones

**P2 is a single-track effort — one implementer, one branch, in order.** It is not split
into parallel tracks: the work is tightly coupled around one resolver and one data flow
(manifest → MVS → fetch → store → lock → plan), the pieces share state and land in a strict
dependency order, and serializing it costs less than the coordination a split would add.
Do the milestones in the sequence below; each builds on the last.

| # | Milestone | Creates / edits |
|---|---|---|
| **P2.0** | Manifest dep-schema for VCS deps + define the internal `ModuleProvider` seam + refactor local-path resolution behind a `LocalProvider`. | `tools/trident/manifest.{hpp,cpp}` (dep schema), `tools/trident/provider.hpp` (new), `tools/trident/resolve.{hpp,cpp}` (refactor). |
| **P2.1a** | SemVer + MVS resolver, tested offline against a fake provider. | `tools/trident/semver.{hpp,cpp}` (new), `tools/trident/mvs.{hpp,cpp}` (new). |
| **P2.1b** | SHA-256 + content-addressed store. | `tools/trident/hash.{hpp,cpp}` (new), `tools/trident/store.{hpp,cpp}` (new). |
| **P2.1c** | Git fetch — the `GitProvider` implementing the seam. | `tools/trident/vcs.{hpp,cpp}` (new), `tools/trident/fetch.{hpp,cpp}` (new). |
| **P2.1d** | Lockfile read/write + staleness check. | `tools/trident/lock.{hpp,cpp}` (new). |
| **P2.1e** | Integration: wire the build list into the existing resolve→plan path; add the new CLI subcommands. | `tools/trident/resolve.{hpp,cpp}`, `tools/trident/main.cpp` (or a new `commands.{hpp,cpp}`). |
| **P2.2 (landed 2026-07-15)** | Integrity & hermeticity (GT4): checksum DB, `trident vendor`, `trident audit`. | `tools/trident/checksum.{hpp,cpp}` (new), `tools/trident/resolve.{hpp,cpp}` (`VendorProvider` + verification), `tools/trident/commands.{hpp,cpp}` (`cmdAudit`/`cmdVendor`), `tools/trident/main.cpp` (`--vendor`), §6. |
| **P2.3–P2.4** | Publishing, provenance (GT5–GT6). | Later files, §6. |

The existing local-path resolution in `resolve.cpp` (`loadDepsRec`, `gatherSources`,
`scanExportedNamespaces`) is the **reference implementation** of the provider for local deps
— do not delete it; P2.1c adds a *git* provider beside it, and P2.1e selects the provider per
dep kind. The `ModuleProvider` interface (§3.3) exists for one reason: to let the MVS
resolver be unit-tested offline against a fake in-memory provider before any git/network code
exists — it is an *internal* seam, not a cross-team contract.

### 0.3 STOP protocol (model escalation) — applies to every milestone

Inherits `techdesign-toolchain.md` §0.3 verbatim and adds P2's own architectural
invariants. If, mid-implementation, the design turns out wrong and an **architectural**
choice is needed — in particular anything that would:

- **(a)** put dependency-resolution, fetch, lockfile, or registry logic **into `leviathan`**
  (the compiler stays pure — `techdesign-toolchain.md` §0.1); or
- **(b)** change the **§3.3 build-plan contract** (`techdesign-toolchain.md` §3.3) — P2 may
  only *add more `src`/`edge` rows*, never new plan fields or a new channel; or
- **(c)** introduce **install-time / lifecycle code execution** (a `postinstall`-equivalent
  — the single largest supply-chain attack surface, proposal §2.5/§10); or
- **(d)** add a **mandatory central blob registry** (contradicts dependency-free — VCS-path
  + optional proxy/index only, proposal §3.1); or
- **(e)** replace **MVS** with a range/SAT resolver (proposal §2.2/§3.2)

— a Sonnet-class agent must **STOP. Do not improvise a design change.** Log findings in §10
(Implementation log), commit WIP to the working branch, and escalate to a Fable-class model
for design correction. These five are not negotiable at the implementation layer.

### 0.4 Frozen / do-not-touch

- **`leviathan` is entirely out of scope.** The GT1 grep proofs
  (`grep -rnE 'parseManifest|Dependency|trident\.toml|project\.mf' src/` → nothing) must
  stay green. If you find yourself editing anything under `src/`, STOP (§0.3a).
- The **§3.3-of-toolchain build-plan contract** is frozen. A dependency reaches the compiler
  only as additional `src { … }` and `edge { … }` rows — the same rows local-path deps
  already produce today. No new plan field is introduced by P2.
- The **portable-backend** work (`designs/complete/techdesign-portable-backend{,-2}.md`, gates G1–G5)
  and the **metaprogramming** tracks are independent and must stay green. Do not touch
  `runtime/**`, `src/LlvmGen.cpp`, `src/X64Gen.cpp`, or any backend/metaprog milestone.
- The **local-path dep path** that landed at GT1 (`resolve.cpp` `loadDepsRec` and the
  `dep_alias` / `dep_app` / `phantom_dep` corpus fixtures) must keep working byte-for-byte.
  P2 *extends* resolution; it does not rewrite the local path.

### 0.5 Naming decisions

**`trident.toml` is the *default* manifest filename, not a static one.** It is what
`discoverManifestPath()` looks for when trident is pointed at a *directory* — but trident can
already be pointed at an explicitly-named manifest *file* (the positional arg in
`main.cpp:resolveManifestArg` uses a path naming a file directly, as-is; only the
directory/cwd case falls back to the `trident.toml` default). So "the manifest is
`trident.toml`" means "that is the default a bare `trident build .` discovers," never "the
name is hardcoded." (The parent `techdesign-toolchain.md` and the toolchain-naming record
overstated this as a "fixed, non-parameterized" name; that framing is the *default*, and this
design follows the looser, correct reading.)

**The lockfile is the manifest's companion — default `trident.lock`, derived from the
manifest basename.** Whatever manifest trident resolves (default `trident.toml`, or an
explicitly-named `foo.toml`), the lockfile sits beside it with the same basename and a
`.lock` extension (`trident.toml → trident.lock`, `foo.toml → foo.lock`). This dissolves the
apparent tension with the parent doc's `<name>.lvlock`: both are "a lock named after / beside
the manifest," and neither is static. Extension `.lock` (Cargo's `Cargo.toml`↔`Cargo.lock`
pairing) is the recommendation; the parent wrote `.lvlock` (the `.lv*` family, cf.
`.lvplan`) — a one-symbol cosmetic in `lock.hpp`, left to owner preference and not otherwise
load-bearing.

Other names this design settles (all defaults/overridable, all trident-local, none reaching
the compiler): content-addressed store at **`~/.trident/store/`** (override `$TRIDENT_HOME`);
optional caching proxy via **`$TRIDENT_PROXY`** (Go's `GOPROXY` analog, off by default);
module identity keyed on **`(vcs-path, major)`**, serialized `path` for major ≤ 1 and
`path@v2`, `path@v3`, … for major ≥ 2 (settles proposal open-Q3).

---

## 1. Context: what exists (verified 2026-07-06)

`trident` today is a manifest-owner + plan-writer + leviathan-driver with **local-path deps
only**. Ground truth in `tools/trident/`:

- **Manifest** (`manifest.{hpp,cpp}`): a hand-rolled TOML reader for the subset a manifest
  needs — top-level `name`/`entry`/`sources`/`version`/`out`, plus repeated `[[dep]]`
  array-of-tables. The dep schema is already `struct Dependency { path; version; as_; dev; }`
  (`manifest.hpp:33`) — `version` and `dev` are **parsed but not yet consumed** (`dev` is
  read at `manifest.cpp:153`; nothing filters on it; `version` is ignored by resolution).
  These fields are the forward-looking hooks P2 fills in.
- **Resolution** (`resolve.{hpp,cpp}`): `resolveProject()` parses the manifest, then
  `loadDepsRec()` (`resolve.cpp:104`) **recursively loads local-path deps** — each dep's
  `path` is a local directory, `discoverManifestPath()` finds its `trident.toml`, its
  sources gather in via `gatherSources()`, and the dep-graph edge is recorded in
  `moduleDeps[parentModule]` (`resolve.cpp:113`). `as` aliasing is materialized as a real
  on-disk `namespace <as> { uses <exported>; }` file (`resolve.cpp:202–221`), because the
  plan format carries only on-disk paths (§3.3 rule 1 of the toolchain doc). A cycle/dup
  guard (`visited`, seeded with the root, `resolve.cpp:167–168`) already exists.
- **Plan** (`plan.{hpp,cpp}`): `writeBuildPlan()` serializes the `ResolvedProject`
  (`sources` with `moduleId`/`origin`, `moduleDeps` → `edge` rows, classified `entry`) into
  the frozen `.lvplan` text. **This is exactly where P2's fetched dep sources arrive** — as
  more `ResolvedSource` rows in `rp.sources` and more entries in `rp.moduleDeps`. No change
  to the plan *format*.
- **Driver / CLI** (`main.cpp`): subcommands `build` / `run` / `check` / `emit-llvm` /
  `plan`, plus `--version`. `findLeviathan()` (`discover.hpp:17`) already implements the
  §3.4 binary-discovery ladder. There is **no** `add`/`remove`/`update`/`lock`/`fetch`/
  `vendor`/`why`/`publish`/`yank`/`audit` — P2 adds them.
- **No network, no lockfile, no store, no hashing.** `grep -rniE 'sha256|http|git|clone'
  tools/trident/` → nothing. Every dep resolves off the local filesystem. This is the
  surface P2 builds out.

Two consequences drive the design:

1. **The plan is already dependency-shaped.** Because `ResolvedSource` carries
   `moduleId`/`origin` and `moduleDeps` carries the edges, a git dependency is
   *representationally identical* to a local one once fetched. P2's integration point is
   therefore narrow: produce the same `rp.sources`/`rp.moduleDeps` from a *fetched* module
   instead of a *local directory*.
2. **`version`/`dev` are dead fields waiting for P2.** The manifest already declares them;
   P2 gives them meaning (MVS selection; `dev`-dep exclusion from production builds).

---

## 2. Roadmap and timeline (authoritative)

Continues `techdesign-toolchain.md` §2's gate numbering. GT2 ("local-path deps resolve
through trident") was **already met at GT1** — the toolchain implementation log recorded
that §5's milestone text (re-homing `loadDepsRec`/aliasing into trident during P1) was taken
as authoritative over §2's summary table, so local-path deps work end-to-end today. P2
ratifies GT2 as done and delivers GT3 onward.

| Milestone | Scope | Gate ("done" = …) | Target |
|---|---|---|---|
| **P2.0** | Ratify GT2; extend the manifest dep schema for **VCS deps** (distinguish a VCS `path` + git-tag `version` from a local path); freeze the §3.3 `ModuleProvider` contract. Mechanical + contract. | **GT2 (ratified):** local-path corpus (`dep_alias`, `dep_app`, `phantom_dep`) green unchanged; manifest parses a `[[dep]]` with a `github.com/...` path + `version = "1.2.0"` without error; provider contract committed to `resolve.hpp`. | 2026-07-16 |
| **P2.1** | **Git deps + MVS + lockfile + content store.** In order: SemVer+MVS (P2.1a) → SHA-256+store (P2.1b) → git fetch (P2.1c) → lockfile (P2.1d) → integration + CLI (P2.1e). | **GT3:** a project whose `[[dep]]` names a real VCS path + version resolves via MVS, fetches into `~/.trident/store/<sha256>/`, writes `trident.lock` (selected versions + content hashes), and builds — the fetched module's namespaces compile in. `trident add/lock/fetch/why` work. **`leviathan` unchanged** (GT1 grep still green). | **2026-10-15** |
| **P2.2 (landed 2026-07-15)** | **Integrity & hermeticity:** checksum DB verify/record (tamper-evidence), `trident vendor` (hermetic/offline builds), `trident audit` (hash + policy check). | **GT4 (met 2026-07-15):** every fetch is verified against the committed `trident.lock` **and** the checksum DB; a moved tag / swapped content is a loud error; `trident vendor` produces a network-free build; `trident audit` passes on the corpus. | ~~2026-11-30~~ 2026-07-15 |
| **P2.3** | **Publishing & availability (optional services):** `trident publish` (validate + git tag + optional index register), `trident yank`, the optional caching proxy (`$TRIDENT_PROXY`), the optional thin name→path index. | **GT5:** `publish` tags a repo and records the version's hash in the checksum DB; `yank` blocks *new* MVS selection without breaking existing locks; a build succeeds against a proxy with upstream unreachable (left-pad immunity). **All three services stay optional** — a build from a complete `trident.lock` + git access needs none of them. | 2027-02-15 (post self-host; explicitly deferred, non-blocking) |
| **P2.4** | **Provenance layers (opt-in):** Sigstore-style build attestations; cargo-vet-style shared audit records; `trident audit` policy requiring trusted audits. | **GT6:** a consumer can require and verify provenance/audit records for its dependency set. | As-needed (no fixed date; ecosystem-scale-gated) |

**Sequencing note vs. self-host (2027-01, portable-pivot memory).** GT3 and GT4 land before
self-host and are the load-bearing ones (deterministic builds + integrity). Publishing,
proxy, and index (GT5) are deliberately scheduled *after* self-host and kept **optional** so
they never block the compiler's own bootstrap — honoring the proposal's "phase the network
away, build the registry last" ethos (proposal §8.6, headline rec 6).

---

## 3. Target architecture

### 3.1 The resolution pipeline (all inside `trident`)

```
   trident build / add / lock
        │
        ▼
   1. parse trident.toml         (manifest.cpp — unchanged; version/dev now consumed)
   2. build the require graph     (mvs.cpp: root requires + each dep's requires)
        │   each dep's requires read via ── ModuleProvider.manifestOf() ──┐
        │                                                                  │
        ▼                                                                  ▼
   3. MVS: selected = max(min)    (mvs.cpp — deterministic, unique)   [local | git] provider
        │                                                             (resolve.cpp | fetch.cpp)
        ▼
   4. materialize each selection  ── ModuleProvider.materialize() ──▶  content-addressed store
        │                                                              ~/.trident/store/<sha256>/
        ▼                                                                  │
   5. verify vs trident.lock + checksum DB   (lock.cpp + checksum.cpp) ◀───┘
        │
        ▼
   6. write trident.lock          (lock.cpp — selected versions + content hashes + edges)
        │
        ▼
   7. feed the build list into the EXISTING resolve→plan path           (resolve.cpp glue)
        │   each BuildListEntry → more rp.sources rows (moduleId/origin) + rp.moduleDeps edges
        ▼
   8. writeBuildPlan()            (plan.cpp — UNCHANGED format) ──▶ leviathan --plan  (UNCHANGED)
```

Steps 2–6 are **new** and entirely trident-internal. Step 7 is the narrow integration seam.
Step 8 and everything downstream (the compiler) is **untouched** — the payoff of the
toolchain split.

### 3.2 What moves, what stays

| Concern | Home | Milestone | Notes |
|---|---|---|---|
| Manifest dep-schema extension (VCS `path` vs local; `version` = git tag) | `manifest.*` (dep region) | P2.0 | Additive: `Dependency` gains a `kind` (local\|vcs) discriminator; existing fields keep meaning. |
| SemVer parse/compare (`Version`, `major` in identity) | `tools/trident/semver.*` (new) | P2.1a | ~150 lines; no ranges — SemVer *semantics* only (major = breaking), selection is MVS. |
| MVS resolver (require graph → build list) | `tools/trident/mvs.*` (new) | P2.1a | "max of the minimums"; a few hundred lines, no backtracking (proposal §2.2, §8.3). |
| SHA-256 (dependency-free) | `tools/trident/hash.*` (new) | P2.1b | Hand-rolled — no crypto library (matches the anti-dependency stance; no hash code exists in-tree today). |
| Content-addressed store | `tools/trident/store.*` (new) | P2.1b | `~/.trident/store/<sha256>/`; fetch-once, hard-link/copy into builds (proposal §6.3). |
| `git` invocation (clone/checkout tag, list tags) | `tools/trident/vcs.*` (new) | P2.1c | fork/execv the system `git`, like `runLeviathan` forks the compiler — **not a linked library** (§7 H-4). |
| Fetch a `module@version` → sources | `tools/trident/fetch.*` (new) | P2.1c | git provider; optional proxy backend; implements the §3.3 `ModuleProvider`. |
| Lockfile read/write (`trident.lock`) | `tools/trident/lock.*` (new) | P2.1d | TOML (reuses the manifest reader's cursor style); selected + hash + `requires` edges. |
| Checksum DB (verify/record) | `tools/trident/checksum.*` (new) | P2.2 | Append-only signed log client; baseline is a local signed file (proposal §4.5, §8.1). |
| Local-path resolution (`loadDepsRec`, aliasing) | `resolve.*` (kept) | — | The **reference `ModuleProvider` for local deps**; do not delete. P2.1e selects provider per dep. |
| Plan writing, entry classification, gather | `plan.*`, `resolve.*` (kept) | — | Unchanged format; fetched sources arrive as more rows. |

After P2.1, `trident` gains the whole package-manager surface and **`leviathan` still knows
nothing** of versions, tags, stores, locks, or MVS — verify with the GT1 grep at every gate.

### 3.3 The internal `ModuleProvider` seam

A small abstract interface that decouples the MVS resolver from *where source comes from* —
so MVS (P2.1a) can be written and unit-tested against a fake in-memory provider before the
git/network code (P2.1c) exists, and so the local-path and git paths are interchangeable.
The resolver calls it; the local-path code (P2.0) and the git provider (P2.1c) implement it.
It lives in a small `tools/trident/provider.hpp` (created in P2.0). It is an *internal*
design seam, not a cross-team contract — but its shape should be settled in P2.0 and changed
only deliberately, since every later milestone codes against it.

```cpp
struct ModuleId  { std::string path; int major; };        // (vcs path, major). major<=1 => key "path"; major>=2 => "path@v<major>"
struct Version   { int major, minor, patch; };            // from a git tag "vMAJOR.MINOR.PATCH"
struct Require   { ModuleId mod; Version min; std::string as_; bool dev; };  // one declared dependency edge

struct BuildListEntry {
    ModuleId    mod;
    Version     selected;         // MVS result
    std::string contentHash;      // "sha256:…"  — what trident.lock records and every later fetch verifies
    std::string storeDir;         // absolute path in the content-addressed store (materialized)
    std::vector<Require> requires;// this module's own direct requires (a cached copy of the edges)
};

// The seam. The MVS resolver depends ONLY on this abstract interface, never on git/fs directly.
struct ModuleProvider {
    virtual ~ModuleProvider() = default;
    // Read module@version's OWN manifest (to get its `requires`) without gathering its full
    // source. git provider: fetch just the tag's trident.toml (or from the proxy).
    virtual bool manifestOf(const ModuleId&, const Version&, ProjectManifest& out, std::string& err) = 0;
    // Materialize module@version's declared sources into the content-addressed store.
    // Returns the store dir and the content hash. Idempotent: a present hash skips the fetch.
    virtual bool materialize(const ModuleId&, const Version&, std::string& storeDir,
                             std::string& contentHash, std::string& err) = 0;
    // Enumerate published versions (tags) — for `update` / latest queries. May be empty for local.
    virtual bool versions(const ModuleId&, std::vector<Version>& out, std::string& err) = 0;
};
```

**Rules the seam must uphold:**

1. **The resolver never does I/O directly.** MVS reaches source *only* through a
   `ModuleProvider`. This is what makes MVS unit-testable offline against a fake in-memory
   provider, and what keeps the local-path and git paths interchangeable.
2. **`contentHash` is authoritative and content-defined.** It is `sha256` over the module's
   declared sources in a canonical order (fixed once in P2.1b and never changed thereafter).
   `trident.lock` records it; every later `materialize` must reproduce it, else a loud
   integrity error.
3. **MVS is the only selection algorithm.** `selected = max over requirers of (min version)`.
   No ranges, no SAT, no "newest available." Introducing any is a STOP event (§0.3e).
4. **The build list feeds the existing path as more rows.** A `BuildListEntry` becomes
   `ResolvedSource` rows (`moduleId` = the entry's store-scoped module id, `origin` = the
   VCS path) and `moduleDeps` edges — exactly the shape `loadDepsRec` already produces for
   local deps. **No new plan field, no new leviathan channel** (§0.3b).
5. **Two majors are two modules.** `ModuleId` includes `major`; `json@1.x` and `json@2.x`
   have distinct ids, coexist in one build list, and bind to distinct namespaces via `as`
   (proposal §3.5, §5.3). MVS runs per-`ModuleId`.

Serialization of `trident.lock` and the store layout are implementation choices *behind* this
interface; the resolver depends only on the interface, not on how any provider fulfills it.

### 3.4 The lockfile (`trident.lock`)

Generated, committed, TOML (reusing the manifest reader). It is a **hash manifest**: because
MVS makes *selection* deterministic from the manifests, the lock's job is **integrity pinning
+ a cached copy of the resolved graph** for offline/fast builds — not recording a solver's
choice (proposal §2.3, §3.3).

```toml
version = 1

[[module]]
path     = "github.com/thaden0/json"
selected = "1.2.0"
hash     = "sha256:9f3c1a…"

[[module]]
path     = "github.com/acme/http"
selected = "0.4.1"
hash     = "sha256:be21d0…"
requires = ["github.com/thaden0/json@1.1.0"]   # http needs 1.1.0; root needs 1.2.0 → MVS picks 1.2.0
```

`trident build`/`run`/`check` use the lock verbatim when it is present and **consistent with
`trident.toml`**; a mismatch (you edited `[[dep]]` but didn't re-lock) is a **loud error**
naming the fix (`trident lock`), never a silent re-resolve (proposal §5.2).

### 3.5 Module identity, versions, and the `/v2` non-problem

- **Identity = VCS path + major** (§0.5). Version = git tag `vMAJOR.MINOR.PATCH`.
- Because imports are **by name** (`uses Json;`) and the `as` binding lives **once in the
  manifest**, a major bump is a **one-line manifest edit** — no codebase-wide import-path
  grep (Go's worst wart, dissolved; proposal §2.7, §3.5). Two majors coexist as two `as`
  aliases (`as = "JsonV1"` / `as = "JsonV2"`), each a distinct `ModuleId`, each its own
  materialized alias file via the existing `resolve.cpp` aliasing machinery.

### 3.6 Store & fetch

- **Global content-addressed store** at `~/.trident/store/<sha256>/` (override
  `$TRIDENT_HOME`): each `module@version` fetched once, keyed by content hash, referenced
  (hard-linked, or copied where hard-links are unavailable) into builds. Integrity by
  construction — the hash *is* the identity (proposal §2.6, §6.3).
- **Fetch backends** (both behind the §3.3 provider): (a) **git** — `vcs.cpp` forks the
  system `git` to clone/checkout the tag and extract the declared `sources`; (b) **proxy**
  (P2.3, optional) — plain HTTPS fetch of a module tarball from `$TRIDENT_PROXY`, no git
  needed. **Fetching runs zero dependency code** — no install hooks exist (§0.3c, proposal
  §10).
- **Vendoring** (`trident vendor`, P2.2) copies the exact resolved sources into `./vendor/`
  for hermetic/airgapped/reproducible builds (Go's `go mod vendor`); a vendored build reads
  `./vendor` and never touches the network.

---

## 4. P2.0 — Manifest schema + the provider seam (GT2 ratified)

Small prep milestone; lands before any P2.1 work. No behavior change for existing local-path
fixtures.

**P2.0-1 — extend the dep schema for VCS deps.** `Dependency` (`manifest.hpp:33`) gains a
discriminator so a VCS dependency (`path` = a VCS path like `github.com/thaden0/json`,
`version` = a git tag) is distinguishable from a local-directory dep. Recommended: infer
`kind` (don't add manifest ceremony) — a `path` that resolves to an existing local directory
is `local`; otherwise it is `vcs` and `version` is required. Record the inference rule in a
comment; a future explicit `kind = "local"|"vcs"` field is a compatible add if inference ever
proves ambiguous. `version`/`dev` keep their existing fields; both now become load-bearing
(MVS reads `version`; production builds filter `dev`, §5).

**P2.0-2 — define the `ModuleProvider` seam.** Create `tools/trident/provider.hpp` (§3.3)
and refactor the *existing* local-path resolution behind a `LocalProvider` that implements it
— a pure refactor of `loadDepsRec`/`gatherSources`/`scanExportedNamespaces` with **no
behavior change** (the local corpus stays byte-identical). This proves the seam is real
before the git provider (P2.1c) is written against it.

**Acceptance (GT2 ratified):**
```
cmake --build build                       # trident builds; provider.hpp compiles
ctest --test-dir build -R corpus_project  # dep_alias / dep_app / phantom_dep green, unchanged
ctest --test-dir build -R trident         # manifest_errors green
# a VCS-shaped dep parses (does not yet resolve — that's P2.1):
build/trident plan tests/trident/vcs_parse   # [[dep]] path="github.com/x/y" version="1.0.0" parses OK
```

Only after this lands, proceed to P2.1a → P2.1b → P2.1c → P2.1d → P2.1e in order (§5).

---

## 5. P2.1 — Git deps + MVS + lockfile + store (GT3)

The heavy milestone. One implementer, five sub-steps in strict order — each builds on the
last. Do not start a sub-step until the previous one's acceptance passes.

### 5.1 P2.1a — SemVer + MVS (against a fake provider)

- `semver.{hpp,cpp}`: parse `vMAJOR.MINOR.PATCH`, compare, extract `major` for identity. No
  ranges.
- `mvs.{hpp,cpp}`: BFS the require graph — root's `requires` + each module's `requires` read
  via `ModuleProvider.manifestOf()` — and for each `ModuleId`, `selected = max(min over all
  requirers)`. Output the deterministic, unique `std::vector<BuildListEntry>` (proposal §8.3).
  Diamonds resolve to the higher compatible minor (§5.3 of proposal); a required-but-yanked
  version (P2.2) errors naming the chain (`trident why`).
- **Acceptance:** unit tests in `tests/trident/resolve/**` drive MVS against an **in-memory
  fake provider** (no I/O — this is why the seam exists) covering: single dep, transitive
  chain, diamond (same major → higher min wins), two majors (distinct ids coexist), and an
  unsatisfiable/missing version (clean error). Deterministic: same input → byte-identical
  build list.

### 5.2 P2.1b — SHA-256 + content-addressed store

- `hash.{hpp,cpp}`: a hand-rolled SHA-256 (no crypto dependency; there is no hash code in the
  tree today — verified). Test against published NIST vectors.
- `store.{hpp,cpp}`: `~/.trident/store/<sha256>/` (override `$TRIDENT_HOME`); given a set of
  source files, compute the canonical content hash (**fix the canonicalization here and never
  change it**: sorted relative path, then file bytes, into one SHA-256), materialize under the
  hash, and hard-link/copy into a build's source set. Idempotent: a present hash skips work.
- **Acceptance:** `hash` matches NIST vectors; storing the same content twice is a no-op;
  two different orderings of the same files produce the *same* hash (canonicalization holds).

### 5.3 P2.1c — git fetch (the `GitProvider`)

- `vcs.{hpp,cpp}`: fork/execv the system `git` (like `main.cpp:runLeviathan` forks the
  compiler — no libgit) to `clone --depth 1 --branch <tag>` into a temp dir and to
  `ls-remote --tags` for `versions()`. Clear diagnostics if `git` is absent (H-4).
- `fetch.{hpp,cpp}`: the `GitProvider` implementing §3.3 — `manifestOf()` fetches just the
  tag's `trident.toml`; `materialize()` fetches the tag, extracts the declared `sources`,
  hashes them into the store (P2.1b), returns `(storeDir, contentHash)`.
- **Acceptance:** against a **local bare git repo fixture** (a `tests/trident/store/` git
  repo with tagged versions — no external network in CI), `GitProvider.versions()` lists the
  tags, `manifestOf()` reads a tagged manifest, and `materialize()` populates the store with
  the expected hash. Re-fetch is a cache hit. MVS (P2.1a) now runs end-to-end over the
  `GitProvider` instead of the fake.

### 5.4 P2.1d — the lockfile

- `lock.{hpp,cpp}`: write `trident.lock` (§3.4) from the build list; read it back; and a
  **consistency check** against the manifest (edited-but-unlocked → loud error naming
  `trident lock`). Reuse the manifest reader's cursor style; do not add a TOML library.
- **Acceptance:** round-trip (write → read → identical build list); a stale lock (manifest
  dep bumped, lock not regenerated) is rejected with the fix message; a fresh `trident lock`
  regenerates deterministically.

### 5.5 P2.1e — integration + CLI

Wire the build list into the existing resolve→plan path (`resolve.cpp` glue) and add the CLI.

- In `resolveProject()`: after parsing the root manifest, if any dep is `kind = vcs`, run MVS
  with a provider that dispatches per dep kind (`LocalProvider` for local, `GitProvider` for
  vcs), then translate each `BuildListEntry` into `rp.sources` rows (`moduleId` from the
  entry, `origin` = VCS path) and `rp.moduleDeps` edges — **the same shape `loadDepsRec`
  already emits**. `as` aliasing reuses the existing materialization (`resolve.cpp:202–221`)
  verbatim. `dev` deps are excluded when the mode is a production build (`build`/`emit-llvm`),
  included for `check`/tests.
- New subcommands in `main.cpp` (or a new `commands.{hpp,cpp}` if `main.cpp` grows past
  ~300 lines): `trident add <path>[@version] [--as N] [--dev]`, `trident remove <path>`,
  `trident update [<path>]`, `trident lock`, `trident fetch`, `trident why <path|name>`.
  Each edits the manifest and/or (re)writes `trident.lock` and populates the store; none
  invokes leviathan except the existing build/run/check/emit-llvm.
- **Acceptance (GT3):**
  ```
  # a fixture project whose trident.toml has a [[dep]] with a VCS path + version,
  # resolved against the local bare-repo fixture (TRIDENT_PROXY unset, offline):
  build/trident add github.com/x/json@1.2.0 --as Json   # edits manifest, MVS, writes trident.lock, fetches to store
  build/trident build tests/trident/vcs_app --leviathan build/leviathan
                                                        # → links; json's namespace compiled IN
  build/trident why  github.com/x/json                  # explains the selected version + who required it
  test -f tests/trident/vcs_app/trident.lock            # lock committed alongside the manifest
  # the payoff — leviathan is untouched:
  grep -rnE 'parseManifest|Dependency|trident\.toml|project\.mf' src/   # → still no matches
  grep -rniE 'sha256|mvs|content-address|git ' src/                    # → no matches (all in trident)
  ctest --test-dir build                                 # entire pre-existing suite still green
  ```

---

## 6. P2.2–P2.4 — integrity, publishing, provenance (GT4–GT6)

Single-track, sequenced after GT3. Each is independently useful; each keeps the network/
registry surface optional (proposal §8.6).

- **P2.2 — integrity & hermeticity (GT4, landed 2026-07-15, ahead of the 2026-11-30 target —
  see the §10 implementation log entry for the full account).** `checksum.{hpp,cpp}`: on first
  fetch, record `module@version → sha256` in the checksum DB (baseline: a local append-only
  hash-chained file — tamper-evident like the design's "signed" framing without needing an
  asymmetric trust root, a logged simplification; growable to a Merkle transparency log at
  scale, proposal §4.5/§8.1); on every later fetch, verify against the DB **and**
  `trident.lock`. `trident vendor` (network-free builds from `./vendor`). `trident audit`
  (verify hashes vs DB; policy hook left for P2.4). A moved tag or swapped content is a loud,
  DB-logged error.
- **P2.3 — publishing & availability (GT5, 2027-02-15, post-self-host, optional).**
  `trident publish [--tag vX.Y.Z]` (validate manifest + clean tree → git tag → record hash in
  the checksum DB → optional index register, first-wins immutable). `trident yank <path>@<v>`
  (blocks *new* MVS selection; existing locks keep resolving — yank-not-delete, proposal
  §2.4/§4.3). The optional **caching proxy** (`$TRIDENT_PROXY`, left-pad immunity) and the
  optional **thin index** (name→VCS-path search; never hosts code). **None mandatory** — a
  build from a complete `trident.lock` + git needs zero services.
- **P2.4 — provenance layers (GT6, as-needed).** Opt-in Sigstore-style build attestations and
  cargo-vet-style shared audit records; `trident audit` policy that *requires* trusted audits
  for a dependency set (proposal §4.4, §11).

---

## 7. Suspected hurdles (read before each milestone)

- **H-1 (the integration seam).** The build-list→`rp.sources`/`rp.moduleDeps` wiring (P2.1e)
  touches `resolve.cpp`, which both the local path and the new git path share. Keep the
  `LocalProvider` refactor (P2.0-2) behavior-preserving so the integration is purely additive
  — the git path adds rows; it does not rewrite how the local path produces them.
- **H-2 (MVS needs manifests to resolve, resolving needs fetch).** MVS reads each module's
  `requires`, which lives in that module's `trident.toml`, which must be fetched. That is why
  `ModuleProvider.manifestOf()` is separate from `materialize()` — fetch *just the manifest*
  to walk the graph, then `materialize()` only the finally-selected build list. Do not fetch
  full sources during graph walk.
- **H-3 (hand-rolled SHA-256 correctness).** A wrong hash silently corrupts integrity. Gate
  P2.1b on NIST test vectors before anything depends on it; fix the source-set
  canonicalization (§3.3 rule 2) once and never change it — a canonicalization change
  reshuffles every recorded hash (a STOP-worthy compat break if it ever ships).
- **H-4 (`git` as an external tool, not a dependency).** trident forks the system `git`
  (Go's model), it does **not** link libgit — so the "dependency-free" stance (no *linked*
  libraries, static binary) holds. Document `git` as a runtime prerequisite for VCS deps;
  local-path deps and proxy fetch need no git. Clear diagnostic if `git` is missing.
- **H-5 (don't leak version/registry logic into `leviathan`).** The tempting shortcut is to
  have the compiler "just read the lock" or "just check a version." That is §0.3a. The
  compiler only ever sees the plan's `src`/`edge` rows. STOP and escalate instead.
- **H-6 (two majors / identity encoding).** `ModuleId` carries `major`; MVS runs per-id; the
  store/lock key is `path` (major ≤ 1) or `path@vN` (major ≥ 2). Get this right at P2.1a or
  diamonds across majors collapse incorrectly (§3.5, proposal §5.3).
- **H-7 (network in CI).** No test may reach the real network. The git-fetch tests (P2.1c)
  run against a **local bare git repo fixture** under `tests/trident/store/`; the MVS tests
  (P2.1a) use an in-memory fake provider. Keep it that way — a flaky network test is worse
  than none.

---

## 8. Testing strategy & CMake discipline

- **Lanes.** *Resolver lane*: `tests/trident/resolve/**` — MVS over a fake provider
  (deterministic build lists), lockfile round-trip + staleness. *Store lane*:
  `tests/trident/store/**` — SHA-256 NIST vectors, store idempotency, `GitProvider` against a
  local bare-repo fixture. *Integration lane*: a `tests/trident/vcs_app` fixture resolved
  end-to-end (offline, against the bare-repo fixture) through `trident build`. All under
  `ctest`.
- **GT3 proof the split still holds** (run at every gate):
  ```
  grep -rnE 'parseManifest|Dependency|trident\.toml|project\.mf' src/    # → no matches
  grep -rniE 'sha256|mvs|lockfile|content-address|GitProvider'   src/    # → no matches (all in trident)
  ```
- **CMakeLists.txt discipline.** All P2 files extend the **existing Track-B region**
  (`# --- Track B: trident package manager …`, `CMakeLists.txt:68`) — append the new sources
  to `add_executable(trident …)` (`CMakeLists.txt:72`) as they are created. Leave
  `levsyntax`, Track A, and the portable-backend regions untouched. ("Track B" here is the
  toolchain doc's label for the trident target — it is not a second implementation track.)
- **Regression bar.** The full pre-existing suite (39 ctest targets at GT1) stays green at
  every gate; local-path corpus stays byte-identical.

---

## 9. Phase-2 seams left for later (P2.3/P2.4 must not be foreclosed)

- The checksum DB client (P2.2) is written behind a small interface so the baseline
  local-signed-file backend can grow into a Merkle transparency log (P2.4) without touching
  callers.
- The `ModuleProvider` (§3.3) already abstracts fetch, so the P2.3 proxy backend is *another
  provider*, not a resolver change — MVS never learns the proxy exists.
- `trident publish`/`yank` (P2.3) edit only the checksum DB + git tags + optional index; they
  add no compiler-visible state. The index, if ever built, maps name→VCS-path and never hosts
  or gates code (proposal §3.1, §7).

---

## 10. Implementation log (append-only)

**2026-07-06 — P2.0 landed (GT2 ratified).** Branch `trident-p2` off `master`. File:line
anchors in §1/§4 (`manifest.hpp:33`, `resolve.cpp:104`, `:113`, `:167–168`, `:202–221`,
`CMakeLists.txt:68`, `:72`) were re-verified against the tree before starting and all still
matched exactly.

- **P2.0-1 (dep schema).** Added `enum class DepKind { Local, Vcs }` and a `DepKind kind`
  field to `Dependency` (`manifest.hpp`). `kind` is **inferred, not parsed** — `parseManifest`
  has no filesystem access (it only sees manifest text, no base dir), so `kind` defaults to
  `Local` there and is set for real by a new `inferAndValidateDependencyKinds()` in
  `resolve.cpp`, called once per manifest level (root deps in `resolveProject`, each dep's own
  sub-deps inside `loadDepsRec`, right after that level's manifest parses) — the same place
  `base`/`depDir` become known. A dep whose `base + path` is an existing directory is `Local`;
  otherwise `Vcs`, which requires non-empty `version` (loud error naming the fix otherwise,
  verified against a hand-fixture: `dependency 'github.com/x/y' is not a local directory, so
  it is treated as a VCS dependency, which requires 'version' (e.g. version = "1.2.0")`).
  `loadDepsRec` now `continue`s past any `Vcs`-kind dep (not yet resolved — P2.1) instead of
  trying to open it as a local directory. Confirmed this does not regress `dep_app`, whose
  local `jsonlib` dep already carries a `version` field today: inference is purely
  path-existence-based, so it still infers `Local` regardless of the `version` field being set.
- **P2.0-2 (provider seam).** New `tools/trident/provider.hpp`: `ModuleId`, `Version`,
  `Require`, `BuildListEntry`, and the abstract `ModuleProvider` (`manifestOf`/`materialize`/
  `versions`), matching §3.3 verbatim with one naming fix (below). `LocalProvider` (declared in
  `resolve.hpp`, defined in `resolve.cpp`) implements the seam by wrapping the same
  `discoverManifestPath`/`readWholeFile`/`parseManifest` helpers `loadDepsRec` already uses;
  `materialize()` is a no-op passthrough (a local dep is already on disk — no store copy, no
  hash) and `versions()` returns empty (a local dep has exactly one "version": whatever is on
  disk). Per §0.2's framing, `LocalProvider` is **not yet wired into `resolveProject`'s live
  path** — P2.1e dispatches a provider per dep kind; P2.0's job was only to prove the interface
  is real and implementable, which it now is. No CMake change needed (a header-only addition;
  the existing `.cpp` list in the Track-B region is unchanged).
- **Minor, non-architectural naming fix (not a §0.3 STOP event).** §3.3's own `BuildListEntry`
  example spells a field `requires`, but the project's `CMAKE_CXX_STANDARD` is 20, where
  `requires` is a full keyword (not merely contextual) — `std::vector<Require> requires;`
  fails to compile. Renamed to `requires_`, matching the trailing-underscore convention this
  same schema already uses for `Dependency::as_`/`Require::as_`. Purely a field-name spelling
  fix inside a not-yet-instantiated struct; does not touch any of the five STOP triggers
  (a–e) and needed no escalation.
- **Acceptance, run verbatim:** `cmake --build build` (clean, no new warnings under
  `-Wall -Wextra -Wpedantic`); `ctest --test-dir build -R corpus_project` → `dep_alias`/
  `dep_app`/`phantom_dep` green, byte-identical; `ctest --test-dir build -R trident` →
  `trident_manifest_errors` (+ `check_demo_trident`) green; new fixture
  `tests/trident/vcs_parse/trident.toml` (a `[[dep]] path = "github.com/x/y" version =
  "1.0.0"` with no such local directory) resolves via `build/trident plan
  tests/trident/vcs_parse` with exit 0 and a valid plan (the vcs dep recognized, skipped, not
  erroring). `grep -rnE 'parseManifest|Dependency|trident\.toml|project\.mf' src/` → no
  matches (leviathan untouched). Full pre-existing `ctest` suite reconfirmed green.
- **Next:** proceed to P2.1a (SemVer + MVS against a fake in-memory provider) per §5.1 — do
  not start until this entry's acceptance is read back as green.
- **Branch correction (repo hygiene, not a design matter).** P2.0 was first landed on a new
  `trident-p2` branch, per this doc's own generic "branch off master" convention note — but
  the repo has a separate, harder standing rule (only `master`/`agent2`/`agent3` may ever
  exist). Corrected same-session: fast-forwarded the commit onto `agent2` (this worktree's
  branch), deleted `trident-p2`, pushed both `agent2` and `master` directly. All P2 work from
  here on lands straight on `agent2`, no per-track branch.

**2026-07-06 — P2.1a landed (SemVer + MVS against a fake provider).**

- **`semver.{hpp,cpp}`.** Parses a version string into the `Version` value (`provider.hpp`);
  accepts an optional leading `v`/`V` so both the manifest's bare spelling (`version =
  "1.2.0"`, the existing corpus convention) and a git tag's `v1.2.0` spelling parse to the
  same value. `formatSemVer` renders the bare manifest/lockfile spelling; `formatSemVerTag`
  renders the `v`-prefixed git-tag spelling P2.1c will need. `compareSemVer` is ordinary
  lexicographic (major, minor, patch) — no ranges, SemVer *semantics* only, exactly as
  scoped.
- **`mvs.{hpp,cpp}`.** `dependencyToRequire()` converts one Vcs-kind `Dependency` into a
  `Require` (`ModuleId{path, major}` + minimum `Version`) — `major` comes from the required
  version's own major number, which is what makes two majors of the same path two distinct
  `ModuleId`s (§3.5) without any `/v2`-style path convention. `selectVersions()` is a
  worklist BFS over `Require` edges: for each `ModuleId` first seen (or whose required
  minimum just increased), `provider.manifestOf()` fetches *just* that version's own manifest
  (never `materialize()` — H-2) and its own deps enqueue as further `Require`s. Output is a
  `std::map<ModuleId, BuildListEntry>` flattened to a vector in map order, which is exactly
  ModuleId's `(path, major)` order — deterministic regardless of graph-walk/root-require
  order, verified by a dedicated test. A 100000-step cap guards against a pathological/buggy
  provider that never stops returning higher versions (no real finite manifest graph
  approaches it); not exercised by any test, just a defensive backstop.
- **Scope decision, logged per §0.3's spirit (not a STOP — none of triggers a–e apply):** a
  manifest reached via `ModuleProvider::manifestOf()` cannot run the existing local-vs-vcs
  `kind` inference (P2.0) on its own declared deps — there is no local base directory to
  `stat()` against before `materialize()` fetches real source. `mvs.cpp` therefore treats
  *every* dep declared inside a manifest reached this way as a further Vcs requirement,
  unconditionally (`expandRequires`, internal to `mvs.cpp`). Only the **root** project's own
  manifest legitimately mixes Local and Vcs deps (today's `dep_alias`/`dep_app` fixtures);
  splitting that by `kind` is the P2.1e integration's job when it seeds MVS's root requires,
  not `mvs.cpp`'s. A git-fetched module declaring a genuinely local (monorepo-style)
  sub-path is out of scope for P2.1 — no existing or planned fixture needs it; flagged here
  in case P2.2+ ever does.
- **Test-location reconciliation (documentation ambiguity, not a STOP — mirrors
  `techdesign-toolchain.md` §9's own precedent for resolving this kind of thing by taking the
  more specific/operative reading).** §5.1's acceptance names `tests/trident/resolve/**`, but
  it also requires the MVS tests run against a fake **in-memory** provider with **no I/O** —
  which rules out a fixture-directory-of-files test (this project's only existing "trident
  own tests" shape, `tests/trident/manifest_errors/`). Landed instead as
  `tests/test_trident_mvs.cpp`, matching this repo's existing hand-rolled C++ unit-test
  convention (`tests/test_lexer.cpp` et al.) — a new `mvstests` ctest target recompiling just
  `manifest.cpp`/`semver.cpp`/`mvs.cpp` (trident has no shared library target to link against;
  `main.cpp`'s own `main()` would clash with the test binary's).
- **Acceptance, run verbatim:** `cmake --build build` clean; `./build/mvstests` → 36 checks,
  0 failures, covering SemVer parse/compare, single dep, a transitive chain, a diamond (same
  major, higher min wins), two majors coexisting as distinct `ModuleId`s, a missing/
  unsatisfiable version (clean error naming the module), and determinism (root-require
  insertion order does not change the sorted build list). Full ctest suite reconfirmed green;
  `grep -rnE 'parseManifest|Dependency|trident\.toml|project\.mf' src/` still empty.
- **Next:** P2.1b (SHA-256 + content-addressed store) per §5.2.

**2026-07-06 — P2.1b landed (SHA-256 + content-addressed store).**

- **`hash.{hpp,cpp}`.** A hand-rolled, streaming (init/update/final) SHA-256 (FIPS 180-4) —
  no crypto library, matching the toolchain's dependency-free stance (no hash code existed
  anywhere in the tree before this). Verified against the four standard NIST test vectors:
  empty string, `"abc"`, the 448-bit multi-block string, and `"a"` × 1,000,000 (exercises
  single-block, padding-in-block, padding-wraps-to-a-new-block, and many-block cases).
- **`store.{hpp,cpp}`.** `storeRoot()` = `$TRIDENT_HOME/store` if set, else
  `~/.trident/store`. Canonicalization (fixed here, per H-3, never to change): sort
  `StoreFile`s by `relPath`, then feed `[relPath, a single 0x00 separator, file bytes]` into
  one `Sha256` stream per file, in that order — verified order-independent (materializing the
  same two files in reversed order produces an identical hash and store directory).
  `materializeToStore()` is idempotent (a present `storeRoot()/<hash>/` short-circuits the
  copy) and crash-safe (copies into a `.tmp.<pid>` sibling directory, then `rename()`s into
  place, so a crash mid-copy can never leave a partial directory that a later run mistakes
  for a valid cache hit). The bare hex digest is both the store's directory name and the
  `contentHash` return value — the `"sha256:"` prefix seen in `trident.lock`'s prose (§3.4)
  is purely a lockfile-text convention layered on top by `lock.cpp` (P2.1d), not part of the
  hash itself.
- **Acceptance, run verbatim:** `cmake --build build` clean; `./build/storetests` → 13
  checks, 0 failures (4 NIST vectors + idempotent-materialize + order-independent hash +
  content-change-changes-hash, run against a `$TRIDENT_HOME` redirected to a throwaway
  `mkdtemp` directory — never the real `~/.trident`). One test-authoring slip caught and
  fixed in the same pass: the hardcoded expected digest for the million-`a` vector was
  missing its trailing hex digit (a transcription typo, confirmed against `hashlib.sha256`
  in Python); the hash implementation itself was correct on the first try for all four
  vectors. Full ctest suite reconfirmed green; both `src/` grep proofs
  (`parseManifest|Dependency|trident\.toml|project\.mf` and
  `sha256|mvs|content-address|GitProvider`) still empty.
- **Next:** P2.1c (git fetch — the `GitProvider`) per §5.3.

**2026-07-06 — P2.1c landed (git fetch — the `GitProvider`).**

- **`vcs.{hpp,cpp}`.** `findGit()` searches `$PATH` only (no libgit, matching `runLeviathan`'s
  fork/execv pattern). `gitListTags()` runs `git ls-remote --tags <remote>` and parses its
  `<sha>\trefs/tags/<name>[^{}]` lines, collapsing an annotated tag's dereferenced `^{}` line
  into the plain name. `gitCloneTag()` runs `git clone --depth 1 --branch <tag> <remote>
  <tmpdir>` into a fresh `mkdtemp()` directory (`-c advice.detachedHead=false` to quiet the
  expected detached-HEAD advice on every checkout). `remote` may be a URL or a plain local
  filesystem path — the latter is what the offline test fixture uses (H-7); local clones print
  a harmless `--depth is ignored in local clones` warning from git itself, not from trident.
- **`fetch.{hpp,cpp}` — `GitProvider`.** `remoteUrlFor()` passes a path through as-is if it is
  already a URL (`://`) or an absolute local path (`/…`, the fixture's case); otherwise it
  prepends `https://` (Go-modules convention) to a bare host/path like
  `github.com/thaden0/json`. `manifestOf()` and `materialize()` both shallow-clone the
  requested tag into a throwaway temp dir, parse its `trident.toml`, and delete the checkout
  when done; `materialize()` additionally expands the manifest's `sources` (a small
  glob-expansion helper mirroring `resolve.cpp`'s `expandSources`, deliberately duplicated —
  see below) and hands the resulting files to `store.cpp`. `versions()` lists tags and keeps
  only the ones matching the requested `ModuleId`'s major.
- **Documented efficiency gap vs. the design text (not a STOP — none of triggers a–e apply).**
  §5.3/H-2 ask `manifestOf()` to fetch "just the tag's `trident.toml`" to avoid pulling full
  sources during the MVS graph walk. P2.1c's `manifestOf()` instead does the same full
  shallow-clone `materialize()` does, reads the manifest, and discards the checkout — simpler,
  but it *does* transfer the module's full tree at manifest-read time, not just one file. A
  true partial fetch would need `git archive --remote` (frequently disabled by hosts, e.g.
  GitHub) or a sparse partial clone (`--filter=blob:none` + sparse-checkout — real complexity,
  and not guaranteed supported by every remote). Correctness is unaffected (no test or
  contract depends on bytes-transferred), and the offline fixture is tiny, so this is left as
  a documented follow-up optimization rather than solved now.
- **Duplication, not a shared helper (deliberate).** `fetch.cpp`'s `expandModuleSources()`
  mirrors `resolve.cpp`'s anonymous-namespace `expandSources()` (glob a manifest's `sources`
  list relative to a base dir) but omits the whole-project dedup parameter that doesn't apply
  to expanding one already-fetched module in isolation. ~20 duplicated lines beat adding a
  cross-file API between `resolve.cpp` and `fetch.cpp` for a helper this small.
- **New fixture: `tests/trident/store/fixture_json.git`** (H-7's literal path) — a local bare
  git repo (no `.git` wrapper, so it commits as ordinary tracked files, no submodule/gitlink
  behavior triggered) with two tags: `v1.0.0` (`Json::encode`/`Json::tag`, matching the
  existing local `dep_app`/`dep_alias` jsonlib fixtures' API) and `v1.1.0` (adds
  `Json::version()`, so the two tags produce different content hashes — used to verify
  materialize() distinguishes versions correctly).
- **Acceptance, run verbatim:** `cmake --build build` clean; `./build/fetchtests` → 22 checks,
  0 failures — `findGit()` non-empty; `gitListTags()` lists both fixture tags;
  `GitProvider::versions()`/`manifestOf()` read the fixture correctly (including a clean error
  for a requested-but-missing `9.9.9`); `materialize()` populates the store (content readable,
  matches the tagged source), a re-fetch of the same version is an idempotent cache hit
  (identical hash/dir), and a different tagged version hashes differently; MVS (P2.1a) runs
  end-to-end over the real `GitProvider` (not the P2.1a fake) and selects the requested
  version. All against `$TRIDENT_HOME` redirected to a throwaway `mkdtemp` dir. Full ctest
  suite reconfirmed green; both `src/` grep proofs still empty.
- **Next:** P2.1d (the lockfile) per §5.4.

**2026-07-06 — correctness fix to P2.1a's `ModuleId` construction, caught while designing
P2.1d's lock serialization (not a STOP — an implementation bug fix, not an architecture
change).** §0.5 states the identity rule as "major <= 1 => key `path`" — a 0.x package and its
1.x successor are the SAME module identity (only major 2+ needs a distinct id/alias). P2.1a's
original `dependencyToRequire()` set `ModuleId.major` to the *raw* parsed major (0 or 1 kept
distinct), which meant a diamond where one requirer wants `0.9.0` and another wants `1.2.0` of
the same path would incorrectly produce **two** coexisting `BuildListEntry`s instead of MVS
correctly selecting the higher (`1.2.0`) for **one** shared identity. Fixed by clamping the
`ModuleId`'s bucketing major to `max(major, 1)` while leaving `Require.min`/
`BuildListEntry.selected` carrying the true, un-normalized version for comparison/display.
Added `test_major_0_and_1_share_one_identity` to `mvstests` (now 39 checks). Full `mvstests`/
`fetchtests` reconfirmed green; no other test's expectations changed (all existing fixtures
used major >= 1 requests that were already >1 apart or already both exactly 1).

**2026-07-06 — fixture bug fix (bug.md #20, filed by a concurrent session merging Track 07):
`tests/trident/store/fixture_json.git`'s `refs/heads`/`refs/tags` were missing on a fresh
checkout.** Git does not track empty directories; the bare repo's `refs/heads`/`refs/tags`
were empty at authoring time (their contents already rolled into `packed-refs`), so they never
made it into a commit — a fresh clone of this repo was then missing `refs/` entirely, and this
git version refuses to recognize the path as a repository at all. Reproduced by deleting the
(locally still-present) empty dirs and confirming `git ls-remote` failed exactly as reported;
fixed with a `.gitkeep` placeholder in each so git tracks the now-nonempty directories; verified
via `git archive HEAD | tar -x` into a scratch dir (simulating a true fresh checkout) that
`git ls-remote --tags` now succeeds against the extracted fixture. `bug.md`'s entry #20 removed
per that file's own convention ("fixed bugs are not tracked there — see git history").

**2026-07-06 — P2.1d landed (the lockfile).**

- **`lock.{hpp,cpp}`.** A hand-rolled TOML-subset reader/writer mirroring `manifest.cpp`'s
  cursor style (deliberately a separate, small struct — this grammar needs a top-level integer
  `version` and `[[module]]`'s `requires` array, which `manifest.cpp`'s reader doesn't parse).
  `serializeModuleId`/`parseModuleId` implement §0.5's identity text (bare `path` for
  major <= 1, `path@vN` for major >= 2) — straightforward now that the P2.1a major-clamp fix
  (above) guarantees `ModuleId.major` is never actually 0. `lockfileFromBuildList` is a pure
  value transform (no I/O) so the staleness check can run without touching disk.
  `lockfileMatchesBuildList` compares module set + selected version + requires edges
  positionally (build lists come out of MVS sorted by `ModuleId`, and a freshly-written lock
  preserves that order) — deliberately does NOT compare the hash (verifying fetched bytes
  against a recorded hash is the checksum DB's job, P2.2, not this structural check). Every
  mismatch message ends `— run \`trident lock\`` (§3.4's "loud error naming the fix").
- **Acceptance, run verbatim:** `cmake --build build` clean; `./build/locktests` → 38 checks,
  0 failures — module-identity text round-trips both ways (major 1 bare, major 2 `@v2`);
  `lockfilePathFor` derives `trident.lock`/`foo.lock` correctly; a full write→read round-trip
  reproduces the original `Lockfile` field-for-field and the reread lock still matches the
  build list it came from; two independent `lockfileFromBuildList` + `writeLockfile` runs on
  the same build list produce byte-identical file content (determinism); a bumped-selected-
  version, a removed module, and an unchanged build list are each judged correctly (stale /
  stale / matches), with every stale message containing `trident lock`. Manually inspected the
  generated text against §3.4's example — matches format exactly (`version = 1`, blank-line-
  separated `[[module]]` blocks, `requires = [...]` only emitted when non-empty). Full ctest
  suite reconfirmed green; both `src/` grep proofs still empty.
- **Next:** P2.1e (integration + CLI) per §5.5 — the last P2.1 sub-step; GT3 lands with it.

**2026-07-06 — P2.1e landed (integration + CLI). GT3 met.** The last P2.1 sub-step.

- **`resolveProject` integration.** After the unchanged `loadDepsRec` local-path pass, the
  root manifest's Vcs-kind deps feed a new `resolveVcsDeps()` (below); each materialized
  `BuildListEntry` becomes more `gather` rows (`moduleId` = its store directory,
  `origin` = its VCS path, files recovered via a new `listFilesRecursive()` over the store
  dir — no `ModuleProvider` interface change needed, since `store.hpp`'s invariant is that a
  store dir holds *exactly* a module's declared sources) and a `moduleDeps` edge — the exact
  shape `loadDepsRec` already produces for local deps, so `plan.cpp`/leviathan need zero
  changes (§3.3 rule 4, the whole payoff of the split). The root may `uses` only what it
  *directly* declared (mirrors the existing local phantom-dep rule); each fetched module's
  own `moduleDeps` edges come from its cached `requires_`. **No dispatching provider is
  needed** (a deviation from the design's literal "a provider that dispatches per dep kind,"
  logged as a deliberate simplification, not a gap): the root's Vcs-kind deps are the *only*
  seed into MVS (Local-kind deps stay entirely on `loadDepsRec`), and P2.1a's mvs.cpp already
  treats every dep reached via `manifestOf()` as Vcs unconditionally — so every `ModuleId`
  MVS ever visits is Git-backed, and `GitProvider` is passed directly. `LocalProvider` (P2.0)
  remains a proven-but-never-invoked seam implementation.
- **`as` aliasing** now uses a new `depModuleFor()` (kind-uniform: a Local dep's own directory,
  or a Vcs dep's materialized store directory via a `ModuleId -> storeDir` map) in place of
  the old `canon(base + d.path)` — both aliasing loops changed, behavior for Local deps is
  identical (same `canon()` computation, just reached through the new helper).
- **Beyond-minimum: lock-verbatim reads (§3.4's "use the lock verbatim when present and
  consistent... never a silent re-resolve").** Re-reading H-2 and §3.4 while wiring this in
  surfaced a real gap in the P2.1a–d design-as-written: nothing ever *read* `trident.lock` —
  every build would silently re-run MVS (and thus `manifestOf()`'s git clones) from scratch,
  which is both slower than necessary and a "silent re-resolve" the design explicitly rules
  out. Implemented: `resolveVcsDeps()` gained a `lockPath` parameter; when non-empty (the
  `resolveProject` path — `build`/`run`/`check`/`plan`), it parses any existing lock and
  checks a lighter-than-full-MVS consistency condition (`lockSatisfiesRootRequires`: every
  *current* root require is present in the lock at a version ≥ its minimum — trusts the
  lock's own transitive edges rather than re-verifying them, since a real `trident lock` run
  produced them). If satisfied, the lock's selection is used as-is (its `requires` text edges
  parsed back via `parseRequireEdge`, the inverse of `lock.cpp`'s `serializeRequireEdge`) —
  **zero `manifestOf()` calls, zero network**, only an idempotent `materialize()` per entry
  (a no-op cache hit unless the store was cleared, matching ordinary package-manager UX: a
  lock pins *what* to fetch, it does not forbid fetching it). `add`/`remove`/`update`/`lock`/
  `fetch` (commands.cpp) pass `lockPath=""` instead, forcing a fresh MVS run every time, since
  recomputing the lock is their entire purpose. Not a §0.3 STOP (no plan-contract/registry/
  MVS-replacement trigger applies) — a correctness completion of already-decided design text,
  caught by careful re-reading, not an architecture change.
- **The manifest gained a writer.** `manifest.{hpp,cpp}`: `writeManifest()` rewrites the whole
  file from a `ProjectManifest` (trident's own hand-rolled format, no comments/formatting
  preserved — same trade-off `lock.cpp` already makes for `trident.lock`). `kind` is never
  serialized (always re-inferred at load, P2.0).
- **New `tools/trident/commands.{hpp,cpp}`** (main.cpp was going to exceed the design's own
  "~300 lines" split trigger): `cmdAdd`/`cmdRemove`/`cmdUpdate`/`cmdLock`/`cmdFetch`/`cmdWhy`.
  `add`'s `<path>[@version]` splits on the last `@`; an omitted version queries
  `GitProvider::versions()` for the major-≤1 identity bucket (§0.5) and picks the highest — a
  `path@v2`-only (major, no minor.patch) "latest of major 2" selector is a documented
  follow-on, not needed by any current fixture. Every command re-resolves fresh
  (`lockPath=""`) and rewrites `trident.lock` via a shared `regenerateLock()`. `resolveManifestArg`
  moved from `main.cpp`'s anonymous namespace into `discover.{hpp,cpp}` (exported) so
  `commands.cpp` doesn't duplicate it; likewise `inferAndValidateDependencyKinds` (previously
  P2.0-internal to `resolve.cpp`) is now exported via `resolve.hpp` for the same reason.
  `main.cpp`'s subcommand dispatch gained the six new subcommands ahead of the existing
  build/run/check/emit-llvm/plan block; none of the six ever invoke leviathan.
- **New offline fixture `tests/trident/vcs_app`** (a `trident.toml` with **no** `[[dep]]` yet —
  `trident add` introduces it during the test, matching the design's own acceptance flow) +
  `tests/run_trident_vcs_app.sh`, wired as ctest target `trident_vcs_app`. The redirect
  mechanism that makes a real-looking `github.com/x/json` path resolve to the offline bare
  fixture (`tests/trident/store/fixture_json.git`) without any code change or `$TRIDENT_PROXY`
  involvement: git's own `url."file://<bare-repo>".insteadOf = https://github.com/x/json`,
  loaded via a throwaway `$GIT_CONFIG_GLOBAL` the test script points at a scratch file —
  transparent to `vcs.cpp`, which just execs `git` and inherits the environment. The design's
  own §5.5 acceptance text names this exact scenario ("resolved against the local bare-repo
  fixture… `github.com/x/json@1.2.0`"); the fixture only has tags `v1.0.0`/`v1.1.0` (P2.1c), so
  the script uses `v1.1.0` — the version number itself is illustrative in the design, not load-
  bearing. Runs on a scratch **copy** of the fixture (this repo's established
  `work=$(mktemp -d)` convention, tests/run_project.sh et al.) since `add` mutates
  `trident.toml`/writes `trident.lock` — a repeated ctest run must not dirty the tracked
  source tree; the design's literal `test -f tests/trident/vcs_app/trident.lock` is checked
  against that scratch copy's corresponding path, an equivalent invariant.
- **`tests/trident/vcs_parse` (P2.0) is now superseded, not maintained.** Its whole purpose
  was proving a VCS-shaped dep parses "without yet resolving" (true only through P2.1d); now
  that resolution is real, `trident plan tests/trident/vcs_parse` genuinely *tries* to resolve
  `github.com/x/y` and fails offline (no redirect configured for it) — expected, and not a
  regression: this fixture was never wired into `ctest` (grep-confirmed at P2.0 time and still
  true), so no automated test is affected. Left as-is rather than deleted or redirected —
  its P2.0-era acceptance text in this doc is a dated historical record (this doc's own
  convention elsewhere: don't rewrite what was true when written); `vcs_app` is the real,
  current, ctest-wired VCS coverage.
- **Acceptance, run verbatim (GT3):** manual end-to-end smoke (`add` with an explicit version,
  `add` with an omitted version resolving to the highest tag, `build`, running the compiled
  binary, `why`, `fetch`, `update`, `remove`, `remove` again correctly erroring) all behaved
  as designed. One fixture-authoring mistake caught and fixed during manual smoke-testing (not
  a trident/compiler bug): the first hand-written smoke manifest used `entry = "main.lev"`
  (file/script mode) around a body that was a *function definition*
  (`void main() { … }`), so nothing at file-scope ever called it and the program produced no
  output; switching to `entry = "main"` (function mode, matching `dep_app`/`dep_alias`'s
  existing convention) fixed it immediately — logged so a future reader doesn't mistake this
  for a resolution bug. `ctest --test-dir build -R trident_vcs_app` → green (the actual,
  automated GT3 check: `add` → `build` → run the binary → diff `expected.txt` → `why`).
  `grep -rnE 'parseManifest|Dependency|trident\.toml|project\.mf' src/` → still no matches.
  `grep -rniE 'sha256|mvs|content-address|git ' src/` → 7 matches, all false positives on the
  substring `"digit "` inside pre-existing Lexer/CGen/X64Gen comments (`git ` is a substring
  of `digit `) — the design's own literal grep pattern has this imprecision; no actual
  git/version-control code exists outside `tools/trident/`. Full ctest suite: **56/56 green**
  (55 prior + `trident_vcs_app`).
- **P2 (GT2 + GT3) is now complete per this design's P2.1 scope.** Remaining roadmap items —
  P2.2 (checksum DB, `vendor`, `audit`, GT4, target 2026-11-30), P2.3 (`publish`/`yank`/proxy/
  index, GT5, deliberately post-self-host), P2.4 (provenance, GT6, as-needed) — are explicitly
  scheduled later per §2's roadmap and are out of scope for this pass.

**2026-07-15 — P2.2 landed (GT4 met, ahead of the 2026-11-30 target).**

- **`checksum.{hpp,cpp}` (the checksum DB, §4.5/§6).** A deliberate simplification from the
  design's "signed append-only file" language, logged rather than silently substituted: real
  signing (asymmetric keys, a trust root) is explicitly Sigstore/P2.4 territory (§6 P2.4),
  and a purely local file needs tamper-evidence, not authentication of a remote signer. The
  baseline here hash-chains each record to the previous one (Merkle-log style, using the
  existing hand-rolled SHA-256 from P2.1b) — `$TRIDENT_HOME/checksum.db`, one line per
  `module@version -> sha256`, each line's own chain hash covering the previous line's chain
  hash. Editing, reordering, or truncating any past entry breaks the chain, caught on the next
  load with a diagnostic naming the exact line. Growing this into a real Merkle transparency
  log (P2.4) only needs a new backend behind `checksumDbVerifyOrRecord()`; callers never see
  the difference — the seam §9 already called for is now real, not just planned.
  `checksumDbVerifyOrRecord(dbPath, mod, version, contentHash, err)` is the one entry point:
  no existing record records a new baseline (ok); a matching record verifies clean (ok, no
  write); a **different** recorded hash for the same `module@version` is the tag-moved/
  content-swapped attack from §4.6's table — a loud `err` naming both hashes, never a silent
  accept.
- **Wired into `resolveVcsDeps` (resolve.cpp), not a separate step.** Every materialized entry
  — whether freshly fetched via MVS or read off a verbatim lock (§3.4) — is now checked twice
  before it's trusted: against the checksum DB (`checksumDbVerifyOrRecord`, tamper-evident
  across time/machines) and, when the lock was used verbatim, against the lock's own pinned
  `hash` field (`LockedModule::hash`, populated since P2.1d but never actually compared until
  now — `lock.cpp`'s own comment on `lockfileMatchesBuildList` had flagged this exact gap as
  "the checksum DB's job, P2.2, not this structural check"). Either mismatch is a loud
  `VcsResolution::err`, surfacing through every caller (`build`/`run`/`check`/`add`/`lock`/
  `update`/`fetch`/`why`/the two new commands below) with no code path that silently accepts
  drifted content — this is GT4's "every fetch is verified against trident.lock **and** the
  checksum DB; a moved tag / swapped content is a loud error" in full.
- **`trident audit` (commands.cpp).** Requires an existing `trident.lock` (a loud error naming
  `trident lock` otherwise — there is nothing to audit against). Re-resolves against that lock,
  which reuses the exact verification `resolveVcsDeps` now always does — no separate audit-only
  logic to keep in sync. Prints one `OK  <path>@<version>  sha256:<hash>` line per module on
  success, or the same tamper diagnostic `build` would have produced, on failure (exit 1). The
  "policy hook for P2.4" the design names is deliberately left as just that — a hook, not a
  policy engine — since trusted-audit-set enforcement is explicitly P2.4 scope (§6).
- **`VendorProvider` + `trident vendor` + `--vendor` (the hermetic path, "GT4:
  `trident vendor` produces a network-free build").** `VendorProvider` (resolve.cpp, file-local
  — it needs no seam beyond `ModuleProvider`) reads a module's sources straight from
  `<manifest-dir>/vendor/<serializeModuleId>/` and hashes them in place
  (`store.hpp`'s existing `canonicalContentHash`, no copy into `$TRIDENT_HOME/store`) — its
  `manifestOf()`/`versions()` are intentionally unimplemented (return a loud internal-error
  `err`) because `resolveVcsDeps` now REQUIRES a valid, lock-satisfying resolution before ever
  constructing one: a vendored build has nothing else to pin versions to, so a missing/stale
  lock under `--vendor` is a loud error naming `trident lock` then `trident vendor`, not a
  silent fall-through to MVS/network. `trident vendor` (a normal, possibly-networked,
  fully-verified resolution) copies each selected module's store directory into
  `vendor/<path>[@vN]/`; `--vendor` (new flag, `main.cpp`, threaded through `resolveProject` →
  `resolveVcsDeps`) then builds from that directory with zero git invocations and zero
  `$TRIDENT_HOME` dependency — deliberately skips the checksum DB (vendor mode's whole point is
  no dependency on host state) but still cross-checks against the lock's pinned hash, which is
  the correct and sufficient guard for "does the vendored content match what was pinned."
- **New tests.** `checksumtests` (P2.2, mirrors `locktests`'s harness): first-fetch baseline
  recording, repeat-fetch clean verification (no spurious append), a moved-tag/swapped-content
  rejection, two `module@version`s coexisting independently, and two tamper modes — an edited
  content-hash field and a truncated log — both caught on load. `tests/run_trident_vendor.sh`
  (ctest `trident_vendor`, mirrors `run_trident_vcs_app.sh`'s offline-fixture convention):
  `trident audit` passing on a clean lock and failing loudly after hand-tampering
  `checksum.db`; `trident vendor` laying down `vendor/github.com/x/json/`; `trident build
  --vendor` succeeding with a **fake `git` shadowing the real one at the front of `PATH`**
  (`findOnPath()`, `vcs.cpp`, scans left-to-right) — proving `VendorProvider` never falls
  through to `GitProvider` — followed by a sanity check that the identical build WITHOUT
  `--vendor` fails with git blocked, so the network-free property is attributed to `--vendor`
  itself, not to trident silently ignoring VCS deps.
- **Acceptance (GT4):** `trident audit` on a clean project passes; hand-editing a recorded
  content hash in `checksum.db` is caught by both `audit` and any subsequent `build`/fetch;
  `trident vendor` + `trident build --vendor` complete with `git` unreachable and produce the
  identical program output as the networked build. Full ctest suite: **190/190 green**
  (188 prior + `checksumtests` + `trident_vendor`).
- **Deferred to P2.3/P2.4, per §6/§9, not attempted here:** real asymmetric signing of the
  checksum DB, a Merkle transparency log backend, `publish`/`yank`, the caching proxy, the thin
  index, and audit *policy* (trusted-audit-set enforcement). None of today's work forecloses
  any of them — §9's seam claims are now proven, not just asserted.

**2026-07-19 — P2.3 and P2.4 landed (GT5 and GT6 met; design complete).**

- **The deliberately deferred milestones were pulled forward without changing the compiler
  contract.** Every new implementation file remains under `tools/trident/`; `src/` and
  `runtime/` have no diff. `process.{hpp,cpp}` centralizes external-tool lookup and captured
  `fork`/`exec`, while `source_set.{hpp,cpp}` gives Git publication and proxy extraction one
  canonical, sorted manifest-source expansion. It rejects absolute paths, `..`, symlink escapes,
  directories, symlink final entries, and special files before hashing or publishing them.
- **`trident publish` and immutable package inspection (P2.3).** New
  `package.{hpp,cpp}` validates a package root, required name/version, a clean Git tree, and that
  every declared source is tracked; it derives the canonical source hash, VCS identity, HEAD
  commit, and `vX.Y.Z` tag. `vcs.{hpp,cpp}` now provides repository/tag helpers in addition to
  fetch. Publish creates (or idempotently confirms) the immutable tag, records the source hash in
  `checksum.db`, optionally first-wins-registers the manifest name in `$TRIDENT_INDEX`, and can
  emit a signed attestation in the same invocation. Retrying the identical publication is clean;
  a tag pointing at another commit, dirty/untracked source, or mismatched checksum is a loud
  error. A newly-created tag is rolled back if the mandatory checksum step fails; optional-service
  failures are explicitly reported as partial publication rather than hidden.
- **Yank-never-delete is part of the integrity log.** `checksum.db` gained hash-chained `yank`
  events alongside content records. `trident yank <path>@<version>` requires a recorded immutable
  version and is idempotent. Fresh MVS, omitted-version `add`, and update selection filter yanked
  versions; an existing internally consistent lock remains buildable and auditable. Content and
  its checksum are never removed.
- **The optional index and proxy are real, static services rather than trust roots.**
  `endpoint.{hpp,cpp}` supports directories, `file://`, and HTTP(S) endpoints (using `curl` only
  when a network endpoint is configured), with static GET/download and first-wins PUT semantics.
  `index.{hpp,cpp}` maps an exact friendly name to one immutable VCS path under `names/`; explicit
  paths always bypass it. `ProxyProvider` serves the existing `ModuleProvider` seam from
  `modules/<encoded-module-id>/@v/{list,vX.Y.Z.toml,vX.Y.Z.tar}`. Tar entries are validated before
  extraction and the materialized source set is rehashed, then the ordinary lock/checksum checks
  still run. With `$TRIDENT_PROXY` absent, Git remains the provider; with `$TRIDENT_INDEX` absent,
  explicit VCS paths remain the naming model. A complete lock plus a recursively verified warm
  store now builds without Git, proxy, or index access.
- **Completion exposed and closed three integrity gaps in the already-landed floor.** A present
  lock is no longer accepted merely because its root requirements look plausible: trident reruns
  MVS over the lock's cached edge graph and requires the exact selected module set, catching
  bumped, lowered, removed, and orphaned entries (including removal of the last VCS dependency).
  A stale or malformed lock now says to run `trident lock` rather than silently resolving around
  it. Existing store entries are recursively rehashed against the pinned content hash and reject
  injected, missing, changed, symlink, or special entries. Version discovery also respects the
  major-≤1 identity bucket across both 0.x and 1.x, and Git remote spelling is normalized for
  HTTPS, SSH/scp, `file://`, and local paths.
- **Signed provenance, shared reviews, and enforceable policy (P2.4).** New
  `provenance.{hpp,cpp}` signs a length-framed canonical statement over module path, version,
  canonical source hash, Git commit, identity, and optional artifact hash. `trident attest` and
  `publish --sign-key` use the system OpenSSL CLI only when this opt-in feature is requested.
  `trident audit-record` writes shareable review records pinned to the locked version and source
  hash. `trident audit --policy <file>` (or automatic `trident.audit.toml`) first performs the GT4
  lock/checksum verification, then can require an audit from a configured trusted auditor and a
  valid attestation from a configured identity/public key for every selected module. Relative
  audit files, attestation directories, and public keys resolve beside the policy file.
- **New automated coverage.** `indextests` covers exact lookup, first-wins registration,
  idempotence, missing names, and explicit-path bypass. `trident_proxy` constructs a static proxy
  from the bare Git fixture, blocks Git, resolves/builds through the proxy, then blocks every
  provider and proves the verified warm-store path. `trident_publish_policy` creates a clean
  package repository and RSA key, exercises publish + retry + friendly-name add, stale-lock
  rejection, trusted audit records, valid policy/signature enforcement, untrusted-auditor and
  tampered-signature rejection, yank, existing-lock audit, and fresh-selection rejection.
  Checksum/store/fetch tests gained yank, injected-store-content, identity-bucket, and remote-
  normalization cases; the vendor test now distinguishes a valid offline warm-store build from a
  deliberately cold Git-blocked failure.
- **Acceptance (GT5 + GT6):** the focused package-manager set (`mvstests`, `storetests`,
  `fetchtests`, `locktests`, `checksumtests`, `indextests`, `corpus_project`, manifest errors,
  VCS app, vendor, proxy, and publish/policy) is **12/12 green**. The full run passed 218 targets
  directly; its sole initial failure was the optional AArch64 QEMU runtime lane because QEMU was
  not given the host's installed cross sysroot (`/lib/ld-linux-aarch64.so.1` was therefore not
  found). Re-running that target with `LVRT_SYSROOT=/usr/aarch64-linux-gnu` passed **1/1**, so all
  **219/219 test targets are green**. A clean rebuild and `git diff --check` pass. The stronger
  compiler-boundary search for manifest/provider/MVS/checksum/lock/proxy/index implementation has
  no hits under `src/` or `runtime/`, and neither directory changed.
- **P2 is complete.** GT2 through GT6 are implemented and tested; this design is archived at
  `designs/complete/techdesign-package-manager.md`.
