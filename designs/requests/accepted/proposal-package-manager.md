# Proposal: Package Manager & Registry (working name: **`lang` deps**)

> **⚠ Naming & tool model superseded (2026-07-06) by `designs/complete/techdesign-toolchain.md`.**
> The package manager is a **separate binary named `trident`** — NOT "subcommands of
> `lang`". Every `lang add/build/publish/yank/audit` in this doc is now
> `trident add/build/publish/yank/audit`; the compiler is `leviathan`; the manifest is
> `<projectname>.lvproj`; sources are `.lev`. The MVS / lockfile / checksum-DB /
> zero-install-code / content-addressed-store **mechanics below are unchanged** and remain
> the reference for Phase 2 — only the names and the one-tool framing are overridden.


**Status:** design proposal. Builds directly on `designs/complete/proposal-project-system.md` (Doc 3 — the
`project.ext` manifest, the multi-file gather, the include graph, the `file → imports` map) and
references `designs/proposal-metaprogramming.md` (Doc 1) for the compile-time-rule security story.
**Location note:** in `docs/` alongside `reference.md` and the other three proposals.

> **One-line thesis.** For a **dependency-free, whole-program-AOT** language whose dependencies are
> *just source compiled into the one program*, the right package manager is **Go-modules-shaped and
> hardened**: import by **VCS path** (ownership implied, no name-squatting), **Minimal Version
> Selection** (deterministic, tiny, reproducible — no SAT solver), integrity via an **immutable,
> yank-but-never-delete** model backed by a **checksum database**, **zero install-time code
> execution**, and a **global content-addressed source store** the compiler pulls from. A dependency
> is not a new kind of thing — it is *more files fed to the Doc-3 gather step*. And because the
> language imports **by name, not by path**, it can fix Go's single most-hated wart (the `/v2`
> import-path pain) by binding package→namespace **once in the manifest** instead of in every source
> line.

This document is opinionated and grounded in the language as it stands (`info.md`, `reference.md`)
and in Doc 3's manifest. Where the cross-language record points somewhere the ideology forbids (or
vice versa), it says which wins.

---

## 0. Table of contents
1. The framing: a dependency is just more source in the whole-program gather
2. Cross-language survey — what to steal, what to avoid
3. What fits *this* language's ideology (the decided design)
4. Submissions / publishing — the trust & security model
5. Dependencies — resolution (MVS), the lockfile, conflicts, dev vs runtime
6. Project-file interaction — how it reads/writes the Doc-3 manifest & join the include graph
7. What's IN this tool and what's explicitly NOT (and why)
8. Implementation proposal — registry architecture, CLI, resolver, store, security, phases
9. Worked flows — manifest with deps, lockfile, publish + install end to end
10. The supply-chain security posture (why deps can't attack you)
11. Improvements / open questions / recommendations

---

## 1. The framing: a dependency is just more source

Everything in this proposal follows from one observation about the language's architecture:

> The language is **whole-program AOT** (`info.md` §17) and has **no runtime loader**. There is no
> dynamic linking, no plugin system, no `require()` at runtime. Doc 3 showed that compilation is a
> **gather** of N source files (generalizing the prelude+user gather) into one whole-program unit.

Therefore **a dependency is not a new concept.** It is *more files* — fetched from somewhere else —
added to the same gather. The package manager's entire job is:

1. **Fetch** the right source for each declared dependency (the right module at the right version).
2. **Verify** it hasn't changed (integrity).
3. **Feed** it into the Doc-3 gather so its namespaces join the whole-program unit.

There is no `node_modules` runtime tree to satisfy, no dynamic resolution, no install step that runs
code. This is the deepest reason the **Go model fits and the npm model does not**: npm's machinery
exists to serve a *runtime* resolver walking a tree; the language has no such thing. The manager is a
**source fetcher + integrity checker + manifest editor**, and nothing more.

This also means the manager is *consistent with the language's two defining stances*:
- **Dependency-free:** the *tool* adds no mandatory central service, no blob-store SPOF, no daemon —
  it leans on VCS hosts that already exist (§3.1), and the produced binary stays statically-linked
  and libc-free exactly as today.
- **One rule across scopes:** a dependency's source flows through the *same* lexer → parser → gather
  → resolve → check → lower pipeline as your own code and the prelude. No parallel path.

---

## 2. Cross-language survey (what to steal, what to avoid)

Full sourced survey summarized; §3–§8 cite back. The convergent modern answer for *these* constraints
is **Go-modules-shaped**, so Go is treated as the closest analog and the others as sources of specific
lessons.

### 2.1 Registry: central blob store vs VCS-path (registry-less)
- **npm / crates.io / PyPI (central blob store):** strong discoverability, but you run and pay for a
  blob store, and it is a single point of failure. crates.io fixes availability with **immutability**
  ("a permanent archive… allowing deletion would go against this goal"); npm *didn't*, and got
  **left-pad** (2016): an author unpublished an 11-line package that was a transitive dep of
  Babel/Webpack, breaking builds ecosystem-wide until npm restored it from backup. npm then
  restricted unpublish (24h window, no dependents).
- **Composer / Packagist (VCS-indexed):** Packagist **indexes VCS repos, it is not a blob store** —
  "new versions are automatically fetched from the tags you create in your VCS." Publishing = tag a
  git repo. Near-zero hosting; ownership implied by repo URL.
- **Go modules (VCS-path, registry-less):** import by VCS URL (`github.com/user/mod`); **no central
  registry**; ownership is the path (so **no name-squatting**). A **module proxy**
  (`proxy.golang.org`, `GOPROXY`) caches source so it "can continue to serve source that is no longer
  available from the original locations" — left-pad immunity **without** a mandatory central store.
- **Deno's reversal — the counter-lesson:** Deno shipped *pure URL imports, no manager*, then built
  **JSR** because URL imports lacked dedup, versioning ergonomics, and discoverability. So
  *pure* registry-less is insufficient; a **thin index** for discoverability earns its keep — but JSR
  kept immutability/semver/provenance as first-class.

**Steal:** Go's VCS-path + proxy (no squatting, left-pad-proof, ~zero hosting). **Avoid:** a
mandatory central blob store (cost + SPOF) and pure-URL-with-no-index (Deno's proven gap).

### 2.2 Version selection: SemVer-ranges + SAT vs MVS vs pinned
- **Ranges + solving (npm `^`/`~`, Cargo, pip, Bundler, NuGet):** "stay close to latest" at the cost
  of **"why is my build different today?"** non-determinism and NP-complete resolver complexity
  (pip's backtracking resolver "can take a long time"; SAT solvers).
- **MVS — Minimal Version Selection (Go):** take the **union of everyone's minimum requirements and
  pick the newest of the minimums** — not the newest available. Russ Cox's rationale: it "does not
  solve general Boolean satisfiability," it "lies in the intersection of 2-SAT, Horn-SAT, and
  Dual-Horn-SAT," so it's polynomial-time with a **unique** answer, "only a few hundred lines," and
  gives **high-fidelity, reproducible builds** — deviating from the author's own versions "only to
  satisfy a requirement elsewhere." The trade: you **give up automatic freshness** for
  "reproducible, predictable, explainable." Upgrades are explicit.
- **NuGet corroboration:** even a mainstream range manager **defaults to "lowest applicable
  version,"** independently confirming the minimums-are-more-reproducible thesis.

**Steal:** **MVS.** It is the structural fit for whole-program AOT (the *exact same source* compiled
everywhere), removes the solver, and is on-ideology (simplicity, honesty — no build drift).

### 2.3 Lockfiles
A good lockfile carries **exact versions + integrity hashes (SHA-256) + the resolved graph** (npm's
`package-lock.json`, `Cargo.lock`, `uv.lock`, `Gemfile.lock`; `uv` producing one universal lock).
**The MVS twist (Go):** because MVS selection is deterministic from `go.mod`, the lock's real job
becomes **integrity pinning** (`go.sum`), not "recording a solver's arbitrary choice." **Steal:** a
lockfile that is essentially a **hash manifest** — version selection is already deterministic, so the
lock exists to pin *content*.

### 2.4 Immutability & yanking
The single clearest lesson: **published versions must never change and never disappear.** crates.io's
"permanent archive" + **yank-but-don't-delete** ("no new dependencies against a yanked version, but
existing builds keep working; a yank does not delete code") is the model; npm's mutable unpublish gave
the world left-pad. Caveat worth carrying: *yank alone doesn't prevent left-pad* (it doesn't check
dependents) — **immutability does**, and Go's checksum DB adds that **even the author can't move a
tag** without detection. **Steal:** immutable + permanent versions; yank blocks *new* selection only;
hard delete reserved for legal/malware takedown.

### 2.5 Publishing trust, provenance, and no-install-scripts
- **Flat namespace = squatting** (crates.io's standing complaint: good names grabbed, no scopes).
  **VCS-path identity sidesteps it** (URL = owner).
- **Provenance:** three live models — npm+**Sigstore** (source→build attestation in the Rekor
  transparency log, GA 2023), Go's **`sum.golang.org` checksum DB** (tamper-evident transparency
  log), and **cargo-vet** (recorded human audits). 
- **Install scripts are the #1 attack surface:** npm `postinstall` runs arbitrary code on every
  machine including CI with deploy keys — GitHub calls it "the single largest code-execution surface
  in the npm ecosystem," and **npm v12 disables it by default**. **Go, Cargo, and NuGet never adopted
  install-time code execution.** **Steal:** path-based identity + a checksum DB baseline + optional
  audit records, and **zero install-time code execution** (nearly free here — §10).

### 2.6 Store layout — content-addressed is the convergent answer
`node_modules`-per-project duplication is the anti-pattern; the modern answer is a **global
content-addressed store**: **pnpm** ("every file… is a hard link to the content-addressable store,"
2–3× faster, 50–70% less disk, and it *prevents phantom dependencies*), **uv** (SHA-256-keyed cache,
hard-linked, "never downloaded again across any project"), and Go's module cache. **Steal:** a global
content-addressed **source** store keyed on content hash; the compiler resolves imports *only* against
declared deps (phantom-dep prevention).

### 2.7 Go's one big wart — and why the language dodges it
Go's most-hated feature is **major-version handling**: the `/v2` import-path suffix means "users need
to grep through their codebase and change each import statement" on a major bump — "a terrible
developer experience." **Root cause: Go imports by PATH, so the major version is baked into every
import line.** The language imports **by NAME** (namespace, `info.md` §12) — so it can bind
package→namespace *once in the manifest* and leave source untouched across a major bump (§3.5). This
is a real, ideology-native improvement over the model it otherwise adopts.

**Net synthesis:** *VCS-path sourcing (no squatting, proxy for availability, thin optional index) →
MVS selection (deterministic, tiny) → lockfile-as-hash-manifest → immutable + yank-not-delete +
checksum DB → zero install-time code → global content-addressed source store → and fix Go's `/v2`
pain via by-name binding in the manifest.*

---

## 3. What fits *this* language's ideology

The ethos (`info.md` §1): **one rule over many special cases, explicit over implicit, honesty over
hidden magic, simplicity, dependency-free, gate the dangerous.** Each decision justified against it.

### 3.1 Registry-less by default: import by VCS path, with a thin optional proxy + index
A mandatory central blob registry contradicts *dependency-free* (a service to run, pay for, and
depend on — a SPOF). So:

> **Recommendation: a dependency is identified by its VCS path** (`github.com/thaden0/json`), with
> the version being a **git tag**. Publishing = **tag a repo** (Composer/Go model). Ownership is the
> path — **no flat namespace to squat.**

Two optional, non-mandatory services layer on without becoming a SPOF (the language still builds
fine with neither, straight from git):
- **A caching proxy** (`proxy`-style) — mirrors module source + metadata so a deleted/moved upstream
  can't break builds (left-pad immunity, §2.1). Optional; `GOPROXY`-style env/manifest override.
- **A thin index** (a searchable name→module-path list, *not* a blob store) — bought only when
  discoverability matters (Deno's lesson, §2.1). It maps friendly names to VCS paths; it never hosts
  or gates code.

This gives crates.io-grade availability guarantees with Composer/Go-grade near-zero hosting — the
dependency-free realization of a registry.

### 3.2 MVS selection — the simplicity/honesty fit
Range-solving is a SAT engine that makes builds drift ("why is it different today?"). MVS is a few
hundred lines, deterministic, unique-answer, reproducible — the exact §1 virtues (simplicity,
honesty, explicit-over-implicit: **upgrades are an explicit act, never a silent drift**). And it is
the structural fit for whole-program AOT: the *same source* compiles everywhere. **Locked to MVS.**
SemVer *semantics* are retained for compatibility declarations (major = breaking); selection picks
**minimums**, not maximums.

### 3.3 The lockfile is a hash manifest, in the language's own literal syntax
Consistent with Doc 3 (§4.1 of Doc 3: the manifest is the language's own literal syntax, not a
bolted-on TOML). The lockfile is a **generated** data file, `project.lock`, in the same literal
syntax — a `lock { … }` block of `module@version → content-hash`. Because MVS makes *selection*
deterministic, the lock's job is **integrity pinning + a cached record of the resolved graph** for
offline/fast builds — not recording a solver's choice (§2.3). One format across manifest and lock:
the §1 "one rule across all scopes" applied to the tool's own files.

### 3.4 Deps join the Doc-3 gather — no new subsystem
Per §1, a dependency's source is *more files*. Concretely (detailed in §6): the resolver produces a
set of `module@version` sources; the manager fetches each into the content-addressed store; the
Doc-3 loader adds them to the gather as additional `SourceUnit`s (tagged with their `FileId` and
their origin module). Their namespaces merge into the whole-program tree; a file `uses PkgNamespace;`
exactly as it `uses` a local namespace. **The include graph and the `file → imports` map (Doc 3
P-3/P-4) already model this** — a dependency file is just a file whose namespaces you can import.

### 3.5 Fix Go's `/v2` pain with by-name binding (the ideology-native improvement)
Because imports are **by name** (§2.7), the manifest binds a package to the local namespace(s) it
provides, and *that binding* carries the version. Source code says `uses Json;`; the manifest says
"`Json` = `github.com/thaden0/json@2.x`." A major bump is a **one-line manifest edit**, not a
codebase-wide grep. If two incompatible majors must coexist, the manifest binds them to **two local
aliases** (`uses JsonV1;` / `uses JsonV2;`) — clean because they are different modules with different
identity. This turns Go's worst wart into a non-issue using Doc 3's decoupled, by-name model.

### 3.6 One integrated tool, separate responsibilities (Cargo's loved integration)
Cargo's praise is its **one integrated tool**. So the manager is **subcommands of `lang`**
(`lang add`, `lang publish`, `lang build`) — not a separate binary — but with **cleanly separated
responsibilities**: *fetching/resolving* deps is distinct from *building* (the compiler). One CLI,
distinct verbs, no responsibility-blur (§7). This is the §1 rule: integrate the UX, don't conflate
the jobs.

### 3.7 The gate pattern extends to the supply chain (§10)
`info.md` §16's "gate the dangerous, guarantee the safe" applies directly: **fetching a dependency
runs zero of its code** (no install scripts), and even a dependency's *compile-time rules* (Doc 1)
are **namespace-scoped** (fire only if you `uses` them) and **hermetic** (no I/O at comptime, Doc 1
§7.2). So a dependency cannot attack you by being fetched *or* by being compiled — a materially
stronger posture than npm's. Detailed in §10.

---

## 4. Submissions / publishing — the trust & security model

### 4.1 How a package gets published: tag a repo, (optionally) register a name
Publishing has **no blob upload and no review gate** (Composer/Go model):
1. Write a library: a project whose `project.ext` declares a `package { }` block (name it provides,
   the namespaces it exposes, its version = the git tag to be created).
2. `lang publish` — validates the manifest, ensures a clean tree, creates the **git tag** `vX.Y.Z`,
   and (if a thin index is in use) submits the **name → VCS-path** mapping to the index once. The
   *code* stays in your repo / the proxy cache; the index stores only the mapping.
3. Consumers `lang add github.com/you/lib@1.2.0` (or `lang add lib` if the index resolves the name).

### 4.2 Naming, ownership, namespacing
- **Identity = VCS path.** Ownership is whoever controls the repo — **no flat-namespace squatting**
  (§2.5). No `@scope/` needed; the path *is* the scope.
- **The friendly name** (via the optional index) is a convenience alias, first-registration-wins, and
  **immutable once mapped** (a name always points at the same VCS path; transfers require both
  parties + a signed record).

### 4.3 Versioning & immutability
- **A version is a git tag; once published it is immutable and permanent** (crates.io's archive rule,
  §2.4). The checksum DB (§4.5) records the content hash so **even the author cannot move the tag**
  (Go's guarantee).
- **Yank, don't delete** (§2.4): `lang yank github.com/you/lib@1.2.0` marks a version so **MVS will
  not newly select it**, while existing locks keep resolving it (integrity preserved). Hard removal
  is reserved for legal/malware takedown and is loudly logged in the checksum DB.

### 4.4 Signing / provenance (layered, opt-in beyond the baseline)
- **Baseline (mandatory): the checksum database** (§4.5) — every published `module@version` has a
  recorded, transparency-logged content hash. This alone defeats tag-moving and proxy-tampering.
- **Opt-in: build provenance** (Sigstore-style, §2.5) — a publisher building in CI can attach a
  signed source→artifact attestation, letting consumers verify "this came from that repo + commit."
- **Opt-in: shared audit records** (cargo-vet-style) — teams record human audits of a
  `module@version`; `lang audit` can require that dependencies carry audits from a trusted set.

### 4.5 The checksum database (tamper-evident, small)
A **transparency-log-backed checksum DB** (Go's `sum.golang.org` model): an append-only, auditable
log of `module@version → SHA-256(content)`. On first fetch, the manager records the hash; on every
later fetch it verifies against the log and the lockfile. This makes the supply chain **tamper-evident
without trusting the proxy or the git host**. For a small language it can start as a simple signed
append-only file and grow into a full Merkle-tree transparency log if scale demands (§8, phased).

### 4.6 Defenses against the known attacks (summary)
| Attack | Defense here |
|---|---|
| Typosquatting | VCS-path identity (URL = owner); optional curated index; no flat blob namespace (§4.2) |
| Left-pad (disappearing dep) | Immutable versions + caching proxy + committed lock (§2.1, §2.4) |
| Tag-moving / tampering | Checksum DB — author can't move a tag undetected (§4.5) |
| Malicious `postinstall` | **No install-time code execution at all** (§10) |
| Malicious compile-time rule | Namespace-scoped + hermetic comptime (Doc 1 §7.2); can't do I/O (§10) |
| Dependency confusion | Explicit VCS paths; the index never shadows an explicit path (§4.2) |

---

## 5. Dependencies — resolution, lockfile, conflicts, dev vs runtime

### 5.1 Resolution: MVS, concretely
Given the root `project.ext` `deps` and each dependency's own `deps` (read from its published
`project.ext`), MVS:
1. Build the **module requirement graph** (root → deps → their deps …).
2. For each module, take the **maximum of the minimum versions** any node requires (the "newest of the
   minimums").
3. That set is the **build list** — deterministic, unique, no backtracking (§2.2).

Because it's whole-program, the build list is then fetched and gathered (§6). No SAT, no solver
timeouts, no "different today."

### 5.2 The lockfile (`project.lock`)
Generated, committed, in the language's literal syntax (§3.3):
```
lock {
    version = 1;
    module { path = "github.com/thaden0/json"; selected = "1.2.0";
             hash = "sha256:9f3c…"; }
    module { path = "github.com/acme/http";   selected = "0.4.1";
             hash = "sha256:be21…"; requires = ["github.com/thaden0/json@1.1.0"]; }
}
```
- `selected` = the MVS result; `hash` = the checksum-DB-verified content hash; `requires` = the edges
  (a cached copy of the graph so offline builds skip re-reading every dep's manifest).
- `lang build`/`lang run` use the lock verbatim if present and consistent with `project.ext`; a
  mismatch (you edited `deps` but didn't re-lock) is a **loud error** with the fix (`lang lock`),
  never a silent re-resolve.

### 5.3 Conflicts, diamonds, majors
- **Diamond, same major:** two paths requiring `json@1.1.0` and `json@1.2.0` → MVS picks `1.2.0`
  (compatible by SemVer). One copy in the build. No conflict.
- **Incompatible majors:** `json@1.x` and `json@2.x` are **different modules** (major is part of
  identity, §3.5) → both may coexist, bound to different local namespaces via the manifest. The
  whole-program tree holds both namespaces; `distinct`/`::` (info.md §4) already disambiguates any
  same-named types across them. No slicing, no diamond ambiguity.
- **Unsatisfiable:** a module requiring a *yanked* version that nothing else pulls → error naming the
  chain (`lang why` explains it).

### 5.4 Dev vs runtime (really: always-compiled vs tooling-only)
Whole-program AOT + no runtime loader means there is no "runtime dependency" in the npm sense — a dep
is *compiled in* or it isn't. So the axis is **always-compiled vs dev/test/tooling-only**:
```
deps = [
    dep { path = "github.com/thaden0/json"; version = "1.2.0"; as = "Json"; },
    dep { path = "github.com/acme/test";    version = "0.9.0"; as = "Test"; dev = true; },
];
```
`dev = true` deps are excluded from a `--build`/`--emit-elf` production build and from what
downstream consumers inherit (they're for *your* tests/tooling only). The default (`dev` absent) is
always-compiled. This is the only dep-kind distinction needed — one axis, one rule.

---

## 6. Project-file interaction (how it plugs into Doc 3)

### 6.1 Where deps are declared: the Doc-3 manifest's `deps` field
Doc 3 (§5.1) left `deps` as "future; empty in v1." This proposal fills it in — **the only manifest
change** is giving `deps` a schema:
```
project {
    name    = "myapp";
    entry   = "app.ext";
    sources = ["src/**/*.ext"];
    deps = [
        dep { path = "github.com/thaden0/json"; version = "1.2.0"; as = "Json"; },
        dep { path = "github.com/acme/http";    version = "0.4.1"; as = "Http"; },
    ];
}
```
`dep` fields: `path` (VCS identity), `version` (minimum, MVS), `as` (the local namespace binding —
§3.5, optional if the package's own namespace name is used verbatim), `dev` (§5.4).

### 6.2 How fetched deps join the include graph + namespace resolution
This reuses Doc 3's machinery end-to-end — **no new resolution concept**:
1. `lang build` reads `project.ext` → MVS → `project.lock` → fetches each `module@version` into the
   content-addressed store (§6.3), verifying hashes.
2. The **Doc-3 loader** treats each dependency's `sources` as additional files, tagged with a
   `FileId` **and their origin module id** (so diagnostics say "in dependency `json@1.2.0`").
3. **Gather (Doc 3 P-2/P-3):** dependency files gather into the *same* whole-program unit; their
   namespaces merge and become importable. The `as` binding aliases the package's exported namespace
   to the local name the manifest chose.
4. **`file → imports` map (Doc 3 P-4):** a dependency file's imports are recorded like any other — so
   Doc 1's namespace-scoped rules apply correctly *within* a dependency (a dep's rules fire only on
   the dep's files that `uses` them), and a dep's rules reach *your* code only if *you* `uses` its
   namespace. **The supply-chain scoping is the same mechanism as intra-project rule scoping.**
5. **Phantom-dep prevention:** the compiler resolves a `uses PkgNamespace;` **only** if the package
   is a declared `dep` (pnpm's strictness, §2.6) — importing an *indirect* dep you didn't declare is a
   compile error suggesting `lang add`.

### 6.3 Store: global content-addressed, vendoring opt-in
- **Global content-addressed store** (`~/.lang/store/<sha256>/…`, §2.6): each `module@version` fetched
  once, keyed by content hash, referenced (hard-linked) into builds. Massive dedup; integrity by
  construction (the hash *is* the identity). No per-project duplicated tree (there's no runtime loader
  to feed — §1).
- **Vendoring (`lang vendor`)** copies the exact resolved sources into `./vendor/` for
  hermetic/airgapped/reproducible builds — opt-in (Go's `go mod vendor`). A vendored build reads
  `./vendor` and never touches the network.

### 6.4 The whole picture
```
project.ext (deps)  --MVS-->  project.lock (selected + hashes)
        │                              │
        │ lang build/fetch             │ verify vs checksum DB
        ▼                              ▼
  content-addressed store  ──►  Doc-3 gather (your files + prelude + dep files)
        (fetch once)                   │
                                       ▼
                            whole-program unit → resolve → check → lower → engine
```

---

## 7. What's IN this tool and what's explicitly NOT

Scope kept tight, per §1 simplicity. The rule: **the manager fetches, verifies, and edits the
manifest; the compiler builds and runs; nothing conflates the two.**

**IN:**
- Resolve deps (MVS), write/verify the lockfile, fetch into the store, verify integrity.
- Edit the manifest (`add`/`remove`/`update`), explain the graph (`why`), vendor, audit.
- Publish (tag + optional index registration), yank.
- Integrity/provenance (checksum DB baseline; optional Sigstore/audit layers).

**NOT (with justification):**
| Excluded | Why |
|---|---|
| **A task/script runner** (npm `scripts`) | The compiler already owns build/run/test (`lang --build`, `lang --run`). Adding a script layer duplicates it and re-opens the `postinstall` attack surface (§2.5, §10). One rule: compiler builds, manager fetches. |
| **Install-time / lifecycle hooks** | The #1 supply-chain attack vector (§2.5); and pointless here — deps are source, nothing legitimate needs to run on fetch (§10). |
| **A runtime loader / plugin system** | Whole-program AOT has no runtime linking (§1). Deps are compiled in; "dynamic module loading" is out of scope by construction. |
| **A mandatory central blob registry** | Contradicts dependency-free (SPOF + hosting cost). VCS-path + optional proxy/index instead (§3.1). |
| **A heavyweight SAT/range resolver** | MVS is deterministic and ~hundreds of lines (§2.2, §3.2). |
| **A toolchain multiplexer** (rustup/pyenv-style) | The compiler is a single dependency-free static binary; a `toolchain = "1.4"` manifest pin (which compiler version) is enough — no version-switching daemon. |
| **Monorepo/workspace orchestration** (v1) | Deferred; the manifest leaves room (a future `members`/`targets` field) without committing now (mirrors Doc 3 §8 Q6). |

---

## 8. Implementation proposal

### 8.1 Registry architecture (registry-less core + optional services)
- **Core:** module = VCS path; version = git tag; the manager clones/fetches the tag's tree, extracts
  the declared `sources`, hashes the content.
- **Checksum DB (baseline):** append-only signed log of `module@version → sha256`. Start as a single
  signed file served statically; grow to a Merkle transparency log at scale.
- **Caching proxy (optional):** mirrors module source + metadata; env/manifest override; left-pad
  immunity.
- **Index (optional):** name → VCS-path search list; never hosts code.

None of the three optional services is required to build from a fully-specified `project.lock` +
git access — the dependency-free guarantee.

### 8.2 CLI surface (subcommands of `lang`, §3.6)
```
lang init                         # scaffold project.ext (Doc 3)
lang add <path>[@version] [--as N] [--dev]   # add dep -> resolve -> lock -> fetch
lang remove <path>                # drop a dep; re-lock
lang update [<path>]              # explicitly raise minimum(s) (MVS: opt-in freshness)
lang lock                         # recompute project.lock from project.ext
lang fetch                        # populate the store from the lock (no build)
lang vendor                       # copy resolved sources into ./vendor
lang why <path|name>             # explain why a dep/version is in the graph (ties to Doc 3 --why)
lang publish [--tag vX.Y.Z]      # validate + git tag + optional index register
lang yank <path>@<version>       # block new selection; keep existing builds working
lang audit                       # verify hashes vs checksum DB; check provenance/audit policy
lang build | run | emit-elf …    # existing compiler modes, now dependency-aware
```
Backward-compatible: a project with no `deps` behaves exactly as Doc 3 (single-file or multi-file, no
network).

### 8.3 The resolver (MVS) — data structures
```
struct ModuleId  { string path; Major major; }          // major is part of identity (§3.5)
struct Version   { int major, minor, patch; }
struct Require   { ModuleId mod; Version min; }
struct Resolved  { ModuleId mod; Version selected; Hash contentHash; Array<Require> requires; }
// MVS: load root Requires; BFS the graph reading each dep's project.ext; for each ModuleId,
// selected = max(min over all requirers); output Array<Resolved> = the build list.
```
A few hundred lines, no backtracking (§2.2). Deterministic → `project.lock` is a pure function of the
manifests.

### 8.4 The store & fetch
- `~/.lang/store/<sha256>/` holds extracted module source, content-addressed.
- Fetch: resolve VCS path + tag → download tree → extract declared `sources` → hash → compare to
  checksum DB + lock → store. Idempotent; a present hash skips the download (uv/Go cache behavior).

### 8.5 Driver integration
`lang build` (Doc 3's project-aware driver) gains a pre-step: if `deps` non-empty, ensure the lock is
current and the store is populated, then hand the **union of local `sources` + resolved dep sources**
to the Doc-3 loader as the file set. Everything downstream (gather → resolve → check → lower →
five-engine) is unchanged. The differential-testing discipline extends to "a project with
dependencies produces identical output on all five engines."

### 8.6 Phased plan
- **Phase 1 — Local path deps.** `deps` with `path` pointing at a **local directory** (no network);
  MVS trivial (one version); store = the directory; gather integration. Proves the "deps are just
  more source" pipeline (§1, §6) end to end with zero registry surface. Unblocks internal
  multi-package repos immediately.
- **Phase 2 — Git deps + lockfile + content store.** Fetch by VCS path + tag; `project.lock` with
  hashes; content-addressed store; `add`/`remove`/`lock`/`fetch`/`why`. MVS over the real graph.
- **Phase 3 — Integrity: checksum DB + `vendor` + `audit`.** Tamper-evidence; hermetic vendored
  builds; no-install-scripts posture formalized.
- **Phase 4 — Publish + optional proxy + optional index.** `publish`/`yank`; caching proxy for
  availability; thin searchable index for discoverability (added only when the ecosystem is big
  enough to want it — Deno's lesson: don't over-build the registry early).
- **Phase 5 — Provenance layers.** Sigstore-style attestations; cargo-vet-style shared audits;
  policy (`audit` requiring trusted audits).

Each phase is independently useful; Phase 1 needs only Doc 3, and the network/registry surface is
deferred as late as possible (§3.1, dependency-free).

---

## 9. Worked flows

### 9.1 A manifest with dependencies
```
project {
    name    = "links";
    entry   = "app.ext";
    sources = ["**/*.ext"];
    out     = "build/links";
    deps = [
        dep { path = "github.com/thaden0/json"; version = "1.2.0"; as = "Json"; },
        dep { path = "github.com/acme/http";    version = "0.4.1"; as = "Http"; },
        dep { path = "github.com/acme/test";    version = "0.9.0"; as = "Test"; dev = true; },
    ];
}
```
Source uses them by name (§3.5), version-agnostic:
```
uses Json; uses Http;
namespace App {
    string render(User u) => Json::encode(u);          // from the dep, compiled in
}
```

### 9.2 The generated lockfile (`project.lock`)
```
lock {
    version = 1;
    module { path = "github.com/thaden0/json"; selected = "1.2.0"; hash = "sha256:9f3c1a…"; }
    module { path = "github.com/acme/http";    selected = "0.4.1"; hash = "sha256:be21d0…";
             requires = ["github.com/thaden0/json@1.1.0"]; }
    // json required at 1.1.0 by http and 1.2.0 by root -> MVS selects 1.2.0
}
```

### 9.3 Install flow (consumer) — end to end
```
$ lang add github.com/thaden0/json@1.2.0 --as Json
  → edits project.ext deps
  → MVS over the graph (reads json's project.ext for its transitive deps)
  → writes project.lock (selected versions + hashes verified vs checksum DB)
  → fetches json@1.2.0 into ~/.lang/store/<sha256>/   (NO code from json runs)
$ lang build
  → lock current? yes. store populated? yes.
  → Doc-3 gather: your files + prelude + json's sources + http's sources → one whole-program unit
  → resolve → check → lower → emit
  → build/links   (statically linked, dependency-free binary; json is compiled IN)
```

### 9.4 Publish flow (author) — end to end
```
# in the json repo, project.ext declares: package { name="json"; provides=["Json"]; }
$ lang publish --tag v1.2.0
  → validate manifest + clean tree
  → git tag v1.2.0 && push
  → record json@1.2.0 → sha256(content) in the checksum DB (append-only, signed)
  → (if index in use) register name "json" → github.com/thaden0/json   [first-wins, immutable]
# consumers can now `lang add github.com/thaden0/json@1.2.0` (or `lang add json` via the index)
```

### 9.5 A major bump (the Go-`/v2` pain, dissolved — §3.5)
```
# json releases 2.0.0 (breaking). Consumer wants it:
$ lang update github.com/thaden0/json@2.0.0        # or edit deps: version = "2.0.0"
  → ONE manifest line changes; source `uses Json;` is UNTOUCHED
# need both majors during migration? bind two aliases:
deps = [ dep{ path=".../json"; version="1.2.0"; as="JsonV1"; },
         dep{ path=".../json"; version="2.0.0"; as="JsonV2"; } ];
  → `uses JsonV1;` / `uses JsonV2;` — no codebase-wide grep, no import-path suffix
```

---

## 10. The supply-chain security posture (why deps can't attack you)

Pulling §3.7, §4.6, and §7 together — a materially stronger story than the incumbents, and mostly
*free* given the language's architecture:

1. **Fetching runs zero dependency code.** No install/postinstall/lifecycle hooks exist (§7). A
   dependency is source; downloading it executes nothing. This closes npm's "single largest
   code-execution surface" by construction.
2. **Compiling runs only ordinary, whole-program-visible code.** A dependency's code runs when *your
   program* runs, exactly like every other translation unit — the compiler sees it; there is no
   privileged install hook hiding in the graph.
3. **Compile-time rules from deps are gated twice** (Doc 1): **namespace-scoped** (a dep's rule fires
   on your code only if you `uses` its namespace — §6.2) *and* **hermetic** (no file/socket/clock/RNG
   at comptime — Doc 1 §7.2, bounded step budget). So a malicious dep cannot exfiltrate or run I/O at
   *your compile time* either — the §16 gate pattern extended to the supply chain (§3.7).
4. **Integrity is tamper-evident** (§4.5): the checksum DB means the author can't move a tag and the
   proxy/host can't swap content, undetected. The committed lock pins exact hashes.
5. **Availability is guaranteed** (§2.1, §2.4): immutable versions + caching proxy + committed lock →
   no left-pad.
6. **Phantom deps are impossible** (§6.2): you can only `uses` what you declared; a compromised
   indirect dep can't silently become importable.

The one-line security summary: **a dependency can harm you only if you compile its code into your
program and run it — never merely by being added, fetched, or compiled — and even its compile-time
metaprogramming is namespace-scoped and I/O-free.**

---

## 11. Improvements / open questions / recommendations

### Open questions
1. **Checksum DB hosting.** Who runs the baseline log for the public ecosystem? Start self-hostable +
   a community instance; a full transparency-log (Merkle) is Phase 3+ only if scale demands (§8.1).

   "~/code/LeviathanSite" is a Terraform project, using AWS Lamdas on the backend, and S3 Bucket to 
   host the Front End.

2. **Index necessity & timing.** Do we ever need the searchable name index, or do VCS paths suffice
   forever? Defer (Phase 4); Deno's reversal says *some* discoverability helps, but don't over-build
   early (§3.1).

   We will build discoverability, as I said in #1, we have an AWS Framework ready for our end of the
   data.

3. **`major` in identity — encoding.** How is a package's major version represented in the store /
   checksum DB keys so two majors coexist cleanly (§3.5, §5.3)? Likely `path` + major tag; settle
   with Phase 2.

   That seems fine for now.

4. **Compiler version pinning.** A `toolchain = "1.4"` manifest field (which compiler built this) —
   enforce-or-warn on mismatch? Recommend warn in v1 (the compiler is one static binary; no
   multiplexer, §7).

   Warn

5. **Source subset published.** Does a package publish its whole repo or only declared `sources`?
   Recommend only declared `sources` (smaller surface, hashed exactly) — decide with `publish`
   (Phase 4).

   Sources, we do not want to host all packages and they dont want us to either.

6. **Reproducible fetch across VCS hosts.** Tag immutability depends on the host; the checksum DB is
   the backstop (§4.5) — but document that force-push-moved tags are *detected*, not *prevented*, at
   the host.

   Fair, it's a limit. We could potentially record a new versions with a git hash, and if that hash 
   changes warn the user, or make them press an extra confirm.

7. **Private deps / auth.** Private VCS paths need credentials; keep it out of the manifest (use the
   VCS host's own auth / env), never store secrets in `project.ext`. <- I dont know what your refering 
   to but its out of date. Source files are .lev

### Headline recommendations (for the report)
1. **Go-modules-shaped, hardened** is the fit for a dependency-free whole-program-AOT language:
   **import by VCS path** (ownership implied, no squatting) → **MVS** selection (deterministic, tiny,
   reproducible — no SAT solver) → **immutable + yank-never-delete** versions backed by a **checksum
   DB** → **zero install-time code execution** → **global content-addressed source store**, with an
   **optional caching proxy** (left-pad immunity) and **optional thin index** (discoverability) that
   never become a mandatory SPOF.
2. **A dependency is just more source in the Doc-3 gather.** No `node_modules`, no runtime loader, no
   new resolution concept — the manager *fetches + verifies + edits the manifest*; the Doc-3 loader
   feeds dep files into the same whole-program unit; the include graph and `file → imports` map (P-3/
   P-4) already model it. Deps are compiled *in*, keeping the produced binary statically-linked and
   dependency-free.
3. **Fix Go's worst wart with by-name binding.** Because the language imports by *name* (namespace),
   package→namespace binding lives **once in the manifest**, so a major bump is a one-line edit (not a
   codebase-wide import-path grep), and two majors coexist as two aliases. Doc 3's decoupled model
   turns Go's `/v2` pain into a non-issue.
4. **The manifest and lockfile are the language's own literal syntax** (Doc 3 consistency): `deps`
   fills Doc 3's reserved field; `project.lock` is a generated hash manifest (MVS makes selection
   deterministic, so the lock pins *integrity*, not a solver's choice).
5. **Tight scope, security by construction.** The tool fetches and verifies; the *compiler* builds and
   runs; no task runner, no install hooks, no runtime loader, no SAT resolver, no mandatory registry.
   Combined with Doc 1's namespace-scoped, hermetic compile-time rules, **a dependency cannot attack
   you by being added, fetched, or compiled — only by code you compile in and run** — the §16 gate
   pattern extended to the supply chain.
6. **Phase the network away.** Phase 1 is local-path deps (needs only Doc 3), proving the "deps are
   source" pipeline with zero registry surface; git deps, integrity, publish, and the optional
   proxy/index follow — the registry is built last and kept optional, honoring the dependency-free
   ethos.
