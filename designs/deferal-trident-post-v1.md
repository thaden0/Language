# Deferral Tracker — Trident Post-v1 / Post-Self-Host Deferrals

**Status:** deferral tracker + resolution designs — **NOT ready for implementation.**
Every item in this document is gated on an explicit trigger milestone (§2); starting any
of them early without an owner ruling is a STOP event (§9). **Date:** 2026-07-06.
**Scope:** the package-manager / project-system work items that were *deliberately*
deferred past v1 and past the self-host milestone, with the stated reasons, the risks of
the deferral, and the recommended resolution path for each when its trigger fires.
**Builds on:** `docs/techdesign-toolchain.md` (the Trident/Leviathan split),
`docs/techdesign-package-manager.md` (P2 — GT2/GT3 landed 2026-07-06; GT4 next),
`docs/proposal-package-manager.md` and `docs/proposal-project-system.md` (the source
deferrals, cited by exact line below), `docs/techdesign-portable-backend.md` (the
self-host gate G5 this document keys on).

> **One-line thesis.** Four deferrals — **(A) workspaces**, **(B) the registry-facing
> remainder of external deps**, **(C) publishing & optional services**, **(D) registry
> discoverability** — are *correctly* deferred: none blocks a build from a complete
> `trident.lock` + git access, none blocks the compiler's bootstrap, and each gets
> materially cheaper or better-informed after self-host (2027-01-15). This document
> exists so the deferrals stay *deliberate* (with owners, triggers, and resolution
> designs) instead of becoming forgotten debt or, worse, being improvised early by an
> implementation agent.

---

## 0. Read this first

### 0.1 What this document is and is not

- It **is** the single tracker for the post-v1 Trident deferrals: what was deferred,
  where the deferral was decided (exact file:line), why it is legitimate, what can go
  wrong while it waits, and the recommended path when it unblocks.
- It is **not** an implementation-ready milestone doc. When a trigger fires (§2), the
  affected section here gets promoted into its own techdesign (the
  `proposal → techdesign` promotion pattern `docs/techdesign-package-manager.md` itself
  followed), and *that* doc carries acceptance gates. Nothing in this file authorizes
  writing code today.
- **Naming (authoritative):** the package manager is **`trident`** (manifest default
  `trident.toml`, lockfile default `trident.lock` — defaults, not hardcoded names, per
  `docs/techdesign-package-manager.md` §0.5); the compiler is **`leviathan`**; sources
  are **`.lev`**. Older spellings in the proposals (`lang deps`, `project.ext`,
  `.lvproj`, `project.lock`) are superseded and appear below only inside quoted source
  text.

### 0.2 The four deferrals at a glance

| # | Deferral | Decided at (exact ref) | Stated target / trigger |
|---|---|---|---|
| **D-A** | Monorepo / workspace orchestration (`members`/`targets`) | `docs/proposal-package-manager.md:434` — "**Monorepo/workspace orchestration** (v1) \| Deferred; the manifest leaves room (a future `members`/`targets` field) without committing now (mirrors Doc 3 §8 Q6)." Mirror: `docs/proposal-project-system.md:554–555` (§8 Q6: "out of scope for v1; the manifest schema leaves room (a `[targets]`-style extension)"). | No fixed date. Trigger: the first real multi-target consumer — earliest realistic one is this repo itself once the compiler is rewritten in Leviathan (post-G5, ~2027-Q1/Q2). |
| **D-B** | External deps / registry lockfile — the registry-facing remainder | `docs/proposal-project-system.md:548–550` (§8 Q4): "**`deps` / external packages:** deferred (§3.6). When it lands: named specifiers + a lockfile + optional registry, **never** raw URLs (Deno's proven mistake, §3.2). Acyclic package graph, hard error on cycle (§4.5)." Rationale at §3.6 (`:179–184`). | The *mechanism* half (VCS-path specifiers + `trident.lock` + MVS) already landed at **GT3, 2026-07-06**. The remainder — named (index-resolved) specifiers, ecosystem-scale lock guarantees, the cycle-policy ruling — rides D-D / P2.4, post-self-host. |
| **D-C** | Publishing & optional services (P2.3: `publish`/`yank` + caching proxy + thin index) | `docs/techdesign-package-manager.md:194` (§2 roadmap, P2.3 row): gate GT5, target "**2027-02-15 (post self-host; explicitly deferred, non-blocking)**." Detail at §6 (`:502–508`), seams at §9 (`:570–579`). | **2027-02-15**, explicitly after self-host (G5, 2027-01-15). Prerequisite: **P2.2 (GT4, checksum DB / vendor / audit, target 2026-11-30) is the next actual step and is NOT deferred** — see §2.2. |
| **D-D** | Registry discoverability (the searchable thin index) | `docs/proposal-package-manager.md:620–622` (§11 open Q2): "Do we ever need the searchable name index, or do VCS paths suffice forever? **Defer (Phase 4)**; Deno's reversal says *some* discoverability helps, but don't over-build early (§3.1)." Also §8.6 Phase 4 (`:503–505`): "added only when the ecosystem is big enough to want it." | Phase 4 / GT5 window at the earliest (post-self-host), and even then **measured** — built only when ecosystem signals justify it (§6.3). |

### 0.3 STOP protocol (model escalation)

This tracker inherits `docs/techdesign-package-manager.md` §0.3 **verbatim** — its five
non-negotiable triggers (a)–(e) (no dep logic in `leviathan`; no build-plan contract
change; no install-time code execution; no mandatory central registry; MVS is the only
selector) apply to any future work item promoted out of this document. §9 adds the
deferral-specific STOP conditions (starting early; making an optional service
load-bearing; a workspace design that reaches the compiler). If, when a trigger fires,
the resolution path written here turns out wrong, a Sonnet-class agent must **STOP, log
findings in §10, and escalate to a Fable-class model** — do not improvise a replacement
design.

### 0.4 Frozen / do-not-touch (standing project rules, restated)

- **`leviathan` (everything under `src/`) is out of scope for every item here.** The GT1
  grep proofs (`grep -rnE 'parseManifest|Dependency|trident\.toml|project\.mf' src/` →
  nothing) must stay green through every future deferral landing. Trident and Leviathan
  remain **separate applications** (`docs/techdesign-toolchain.md` §0.1) — a hard,
  owner-set requirement.
- **LLVM is the primary backend; `X64Gen.cpp`/`X64.hpp` are frozen** for new work. No
  item in this document touches a backend, and none may grow to.
- Source and test files are **`.lev`**, never `.ext`.
- **No new git branches** — when these items eventually land, they land on the standing
  branches only (three-branch rule).
- The **§3.3 build-plan contract** (`docs/techdesign-toolchain.md`) stays frozen: every
  deferral below must reach the compiler, if at all, as *more `src`/`edge` rows in an
  ordinary plan*, never as a new plan field or channel.

---

## 1. Context: what exists (verified 2026-07-06)

Ground truth, so the deferrals are read against reality and not against stale proposal
text:

- **P2.0–P2.1e are complete; GT2 and GT3 are met** (`docs/techdesign-package-manager.md`
  §10, final entry). Trident today: VCS deps by path + git tag, MVS selection
  (`tools/trident/mvs.cpp`), SHA-256 content-addressed store (`~/.trident/store/`,
  `$TRIDENT_HOME` override), `trident.lock` write/read/staleness + lock-verbatim builds,
  CLI `add/remove/update/lock/fetch/why`, all offline-tested (56/56 ctest green at GT3).
- **P2.2 (GT4 — checksum DB, `trident vendor`, `trident audit`) is the next actual
  step, not started** (target 2026-11-30). It is **pre-self-host and load-bearing**, and
  is therefore *not* one of this document's deferrals — but D-B and D-C both depend on
  it (§2.2).
- **Self-host is gate G5** of the portable-backend plan: stage-2 compiler rebuilds
  itself as stage-3 with identical output, full corpus green under the self-built
  compiler — "start ~2026-11; **self-hosted 2027-01-15**"
  (`docs/techdesign-portable-backend.md:143`).
- **The implemented MVS is cycle-tolerant.** `selectVersions()`
  (`tools/trident/mvs.cpp:40–96`) is a worklist BFS that converges because a module's
  selected version only ever increases; a require-graph cycle terminates naturally and
  produces a valid build list. There is **no acyclicity hard-error** — which Doc 3 §4.5
  (`docs/proposal-project-system.md:278–279`) said the package graph would have. This
  conflict is real and is resolved in D-B (§4.3, problem P-B3).

---

## 2. Trigger milestones & dependencies (what gates what)

### 2.1 The dependency graph

```
P2.2  GT4 (checksum DB + vendor + audit)      target 2026-11-30   ← NOT deferred; next actual step
  │
  ├────────────► D-C  P2.3 publish/yank/proxy/index (GT5)   2027-02-15
  │                     │
  │                     └────► D-D  thin index (discoverability)   measured, GT5 window at earliest
  │                              │
  │                              └────► D-B remainder: named specifiers (`trident add json`)
  │
Self-host G5 (portable backend P5)            2027-01-15
  │
  ├────────────► gates D-C's start (P2.3 is "post self-host; explicitly deferred, non-blocking")
  └────────────► creates D-A's first real consumer (this repo becomes multi-package)
```

### 2.2 Why P2.2 comes first (and is not a deferral)

`docs/techdesign-package-manager.md` §2's sequencing note is explicit: "GT3 and GT4 land
before self-host and are the load-bearing ones (deterministic builds + integrity).
Publishing, proxy, and index (GT5) are deliberately scheduled *after* self-host and kept
**optional** so they never block the compiler's own bootstrap." Two of this document's
deferrals lean on GT4 directly:

- **D-C** needs the checksum DB to exist before `publish` can "record the version's hash
  in the checksum DB" and before a *proxy* can be treated as an untrusted cache (the
  hash check is what makes proxy content trustworthy without trusting the proxy).
- **D-C's availability story before the proxy exists** is `trident vendor` (GT4):
  hermetic, network-free builds from `./vendor` are the left-pad mitigation that holds
  during the entire deferral window (§7, problem X-3).

**Rule:** no D-item may start before GT4 is green, and D-C may not start before G5 is
green, without an owner ruling (§9).

---

## 3. D-A — Monorepo / workspace orchestration

### 3.1 The deferment, verbatim

`docs/proposal-package-manager.md:434` (§7, the explicitly-NOT table):

> **Monorepo/workspace orchestration** (v1) — Deferred; the manifest leaves room (a
> future `members`/`targets` field) without committing now (mirrors Doc 3 §8 Q6).

`docs/proposal-project-system.md:554–555` (§8 Q6):

> **Multi-target / workspaces** (multiple binaries/libs in one repo): out of scope for
> v1; the manifest schema leaves room (a `[targets]`-style extension) without committing
> now.

No date was ever attached — this is a "when a real consumer exists" deferral, and both
source docs deliberately reserved *only a field name*, not a semantics.

### 3.2 Why the deferral is legitimate (detailed rationale)

1. **No consumer exists.** Every project in the tree today is single-target; the corpus
   fixtures (`dep_alias`, `dep_app`, `vcs_app`) are single-binary projects with deps.
   Designing workspace semantics against zero real usage is exactly the
   "over-engineering deps/registry before any deps exist" risk Doc 3's own risk table
   names (`docs/proposal-project-system.md:589`).
2. **Local-path deps already cover the intra-repo case.** A repo with several packages
   can already point `[[dep]] path = "../lib"` at a sibling — `loadDepsRec` resolves it,
   aliases bind it, phantom-dep rules police it. What's missing is only *orchestration*
   (build-all, shared lock, member discovery), which is convenience, not capability.
3. **Whole-program AOT changes the trade-offs.** One binary = one plan = one
   whole-program gather. A workspace is therefore *N independent plans*, not one
   mega-build — there is no shared incremental artifact graph to design (no crate-graph
   equivalent), which means most of Cargo's workspace complexity simply does not apply
   yet. Deciding this early would bake in complexity the architecture doesn't need.
4. **Self-host will produce the first honest requirements.** After G5 this repository
   itself becomes the flagship multi-package repo (the compiler written in Leviathan,
   trident, the stdlib, tests) — the best possible design input, available ~2027-01.
   Designing before it exists means guessing; designing after means transcribing.

### 3.3 Resolution path (when the trigger fires)

Recommended shape — to be promoted into its own techdesign at trigger time, not
implemented from this sketch:

1. **A `[workspace]` table in the root `trident.toml`** with `members = ["tools/a",
   "libs/b", …]` (the reserved `members` field, now in TOML per the toolchain
   overrides). Each member stays an **ordinary standalone project** with its own
   `trident.toml` — a member must build identically whether entered via the workspace or
   directly. No second project format (Doc 3 §3.4's "one module concept" rule applied to
   projects).
2. **`targets` (multiple outputs from one manifest)** is the smaller sibling: a
   `[[target]]` array (each with `name`/`entry`/`out`, sharing the manifest's `sources`
   and deps). Recommend shipping `members` first and `targets` only if a member with
   multiple binaries actually appears — they solve different problems and `members`
   subsumes the common case (one dir per binary).
3. **One shared `trident.lock` at the workspace root** for VCS deps: a single MVS run
   over the union of all members' requires (Go/Cargo convergent behavior), so two
   members cannot silently pin two different selections of the same module identity.
   Intra-workspace member→member edges stay Local-kind deps and never enter the lock.
4. **Orchestration only in trident:** `trident build [--member <name>]` iterates
   members in dependency order (the member graph is derivable from their Local-kind
   edges) and writes **one ordinary plan per member binary**. `leviathan` is invoked N
   times and never learns the word "workspace" — the §3.3 plan contract is untouched by
   construction.
5. **Trigger milestone:** post-G5, when the first real multi-target consumer exists.
   Earliest realistic candidate: the self-hosted compiler repo itself (~2027-Q1/Q2). Do
   not schedule a date before the consumer exists.

### 3.4 Potential problems (D-A specific)

| # | Problem | Recommended solution |
|---|---|---|
| P-A1 | **Workspace build-graph complexity creep** — member graphs invite shared caches, partial rebuilds, task pipelines, and eventually a build *system* inside trident. | Hard scope rule in the promoted design: a workspace is *N ordinary projects + one shared lock + a build order*. Anything smarter (artifact caching, task running) is out — the §7 exclusions table of the proposal (no task runner) already forbids the slippery slope. |
| P-A2 | **A fetched module that is itself a workspace.** P2.1a's log already flags the adjacent gap: "a git-fetched module declaring a genuinely local (monorepo-style) sub-path is out of scope for P2.1" (`docs/techdesign-package-manager.md` §10, P2.1a entry). If deps can be workspaces, module identity ("which member did I import?") needs an answer. | Recommend: a *published* module is always a single package — `publish` (D-C) refuses a manifest with `[workspace]`, and a workspace member is published from its own member manifest. This keeps `ModuleId = (vcs-path, major)` unchanged. Revisit only with evidence. |
| P-A3 | **Lock divergence between root and members** — a member built standalone resolves against its own lock; built in the workspace, against the shared one. | Precedence rule fixed in the promoted design: inside a workspace, the root lock is authoritative and a member-local lock is an error (loud, naming the fix), mirroring the existing edited-but-unlocked loud-error style (§3.4 of the P2 design). |
| P-A4 | **Phantom-dep erosion** — members casually `uses` each other's namespaces because "it's all one repo." | No change needed: the existing rule (you may only `uses` what your own manifest declares) applies per member unchanged. State it explicitly in the promoted design's acceptance tests. |

---

## 4. D-B — External deps / registry lockfile: the registry-facing remainder

### 4.1 The deferment, verbatim, and what has since changed

`docs/proposal-project-system.md:548–550` (§8 Q4):

> **`deps` / external packages:** deferred (§3.6). When it lands: named specifiers + a
> lockfile + optional registry, **never** raw URLs (Deno's proven mistake, §3.2).
> Acyclic package graph, hard error on cycle (§4.5).

The rationale, §3.6 (`:179–184`): the loudest universal pain (node_modules bloat,
transitive dependency hell) is downstream of large third-party trees, so "keep `deps`
minimal (empty in v1) and *not* over-engineer a package registry before there is
anything to depend on."

**Status correction (important):** this deferral is the *root* deferral of the whole
chain, and its **mechanism half has since been discharged** — P2.0–P2.1e (GT3,
2026-07-06) delivered specifiers (VCS path + version), the lockfile (`trident.lock`,
hash-manifest-shaped), and MVS, exactly along Q4's "named specifiers + a lockfile"
line. What legitimately **remains deferred** is the registry-facing remainder:

1. **Named specifiers in the friendly sense** — `trident add json` resolving a *name*
   to a VCS path. This is the optional thin index's job and rides D-D (post-self-host).
   Today's specifiers are VCS paths, which is by design (`docs/proposal-package-manager.md`
   §3.1) and already honors "never raw URLs": a VCS path is a versioned module identity
   fed to MVS + lock + hash, not a Deno-style raw URL import pasted into source.
2. **Ecosystem-scale lockfile guarantees** — the lock's hashes verified against a
   *shared* tamper-evident record. The local baseline is P2.2 (GT4, next step, not
   deferred); the at-scale form (Merkle transparency log) is P2.4, explicitly
   "ecosystem-scale-gated" (`docs/techdesign-package-manager.md:195`).
3. **The cycle-policy ruling** — Q4/§4.5 demand "acyclic package graph, hard error on
   cycle"; the implemented MVS is deliberately cycle-tolerant (§1). Unresolved conflict.

### 4.2 Why the (remaining) deferral is legitimate

- **Nothing user-facing is blocked.** A project can declare, resolve, lock, fetch, and
  build real VCS deps today. The remainder is ergonomics (names) and ecosystem trust
  infrastructure (shared logs) — both worthless without an ecosystem, which cannot
  predate self-host + publishing (D-C).
- **The Deno lesson cuts both ways** (`docs/proposal-package-manager.md:98–104`): pure
  URL-imports-no-manager failed, but so does building JSR before anyone publishes.
  Named specifiers need an index; the index is deliberately Phase 4 (D-D).
- **The cycle ruling has no forcing function yet.** No real dep graph exists that could
  contain a cycle; deciding under G5 time pressure would be policy-by-accident.

### 4.3 Resolution path (when the triggers fire)

1. **Ratify GT3 as Q4's mechanism discharge** in the promoted design (one paragraph —
   the lock + MVS + specifiers exist; cite the P2 log). No re-litigation.
2. **Named specifiers land as index sugar, lowering at manifest-write time.**
   `trident add json` asks the (optional, D-D) index for `json → github.com/thaden0/json`,
   then writes the **VCS path** into `trident.toml` exactly as `trident add
   github.com/thaden0/json` would. The manifest and lock never contain index-dependent
   names — so a build never needs the index (optionality preserved, §0.4/§9), and a
   later index outage or name dispute cannot retarget an existing project.
3. **Settle the cycle policy with an owner ruling** (this amends proposal text, so it is
   not an implementation-layer call). Recommended ruling to bring to the owner: Doc 3
   §4.5 conflated two graphs. Go forbids *package import* cycles but tolerates *module
   version-graph* cycles, and MVS's convergence proof does not need acyclicity
   (`tools/trident/mvs.cpp:45–48`'s comment states the actual invariant: selected
   versions only increase). Leviathan's namespace layer is whole-program and
   cycle-immune (Doc 3 §4.5's own first bullet), so a module-graph cycle is harmless
   here for the same reason. **Recommend:** keep cycle-tolerant selection; surface any
   require-cycle in `trident why` (informational) and add an `audit` warning; reserve
   the hard error for the day evidence shows cycles causing real harm. If the owner
   instead upholds the literal §4.5 text, the implementation is a trivial post-MVS DFS
   over `BuildListEntry.requires_` edges erroring with the named chain — either ruling
   is cheap; only the *decision* is owner-level.
4. **At-scale lock integrity** (transparency log) stays P2.4, as-needed — do not
   schedule.

### 4.4 Potential problems (D-B specific)

| # | Problem | Recommended solution |
|---|---|---|
| P-B1 | **Lockfile/MVS correctness drift while the remainder waits** — e.g. the P2.1d structural check deliberately does **not** compare hashes (that is GT4's job, per its log); until GT4 lands, a tampered store entry is undetected. | Already sequenced: GT4 (2026-11-30) closes it, pre-self-host. This tracker's only job is to keep GT4 *ahead of* every D-item (§2.2 rule) — verified at each gate. |
| P-B2 | **Canonicalization freeze** — named-specifier and index work must never touch the content-hash canonicalization (fixed at P2.1b "and never changed thereafter"); a change reshuffles every recorded hash. | Restate H-3 of the P2 design in the promoted doc; any canonicalization edit is a STOP (§9). |
| P-B3 | **The Doc 3 §4.5 acyclicity conflict** silently resolving itself by whoever touches `mvs.cpp` next. | §4.3 step 3: an explicit owner ruling, recorded in §10 of this file, before any cycle-related code change. |
| P-B4 | **Major-identity subtleties repeat** — the 0.x/1.x identity-bucket bug (fixed in the P2 log, 2026-07-06) shows identity encoding is easy to get subtly wrong; named specifiers add a second name→identity mapping layer. | The index maps *names to paths only* — never to versions or majors — so identity stays exactly `(vcs-path, major)` with one implementation (`provider.hpp`). Test the sugar by asserting the written manifest is byte-identical to the path-spelled equivalent. |

---

## 5. D-C — Publishing & optional services (P2.3: `publish`/`yank` + proxy + thin index)

### 5.1 The deferment, verbatim

`docs/techdesign-package-manager.md:194` (§2 roadmap, P2.3 row) — scope: "`trident
publish` (validate + git tag + optional index register), `trident yank`, the optional
caching proxy (`$TRIDENT_PROXY`), the optional thin name→path index." Gate GT5:

> `publish` tags a repo and records the version's hash in the checksum DB; `yank`
> blocks *new* MVS selection without breaking existing locks; a build succeeds against
> a proxy with upstream unreachable (left-pad immunity). **All three services stay
> optional** — a build from a complete `trident.lock` + git access needs none of them.

Target: **2027-02-15 (post self-host; explicitly deferred, non-blocking).** The §2
sequencing note gives the reason in one line: GT5 is "deliberately scheduled *after*
self-host and kept **optional** so they never block the compiler's own bootstrap —
honoring the proposal's 'phase the network away, build the registry last' ethos
(proposal §8.6, headline rec 6)."

### 5.2 Why the deferral is legitimate (detailed rationale)

1. **Nothing consumes publishing yet.** Publishing is for *other people's* builds; until
   self-host + an initial stdlib/tool ecosystem exist, there are no other people. The
   consumer-side machinery (fetch/verify/lock) is what bootstrap needs, and it landed
   (GT3) or is next (GT4).
2. **The bootstrap must not depend on services.** G5's 3-stage bootstrap has to run from
   a clean checkout + git. If publish/proxy/index existed *before* G5, the temptation to
   lean on them (fetch the stage-0 stdlib "from the index") would create exactly the
   mandatory-service SPOF the whole design forbids (STOP trigger (d)). Deferring them
   past G5 makes the bootstrap's zero-service property true by construction.
3. **Services are the only part with hosting/operational cost** (who runs the proxy? who
   signs the checksum DB? — proposal §11 open Q1). Deferring converts an open
   operational question into one that can be answered with real usage data.
4. **The cross-language record endorses it.** The proposal's survey (§2.1, §2.4): npm's
   mutable central store produced left-pad; Go's registry-less core + optional proxy
   arrived *years* after go itself and lost nothing by waiting. Composer/Packagist shows
   tag-a-repo publishing needs near-zero infrastructure — which is why waiting is cheap.

### 5.3 Resolution path (when G5 lands; target 2027-02-15)

Promote §6's P2.3 bullet of `docs/techdesign-package-manager.md` into a milestone doc
with these load-bearing points carried forward:

1. **`trident publish [--tag vX.Y.Z]`** = validate manifest + clean tree → `git tag` →
   record `module@version → sha256` in the checksum DB (append-only; GT4's client) →
   *optionally* register name→path in the index (first-wins, immutable). Publishing is
   fundamentally **still just tagging a repo** — the services only witness it. `publish`
   must refuse a `[workspace]` root (P-A2).
2. **`trident yank <path>@<version>`** blocks *new* MVS selection only; every existing
   `trident.lock` keeps resolving (yank-never-delete, proposal §2.4). A newly-blocked
   selection errors naming the requiring chain (`trident why` output inline).
3. **The caching proxy** (`$TRIDENT_PROXY`) is implemented as **another
   `ModuleProvider`** — the §9 seam of the P2 design already guarantees "MVS never
   learns the proxy exists." Proxy content is *untrusted cache*: every byte is verified
   against `trident.lock` + the checksum DB (GT4), so proxy operators need no trust.
4. **The thin index** is scoped in D-D (§6) — P2.3 only wires the optional
   `publish`-time registration call.
5. **The optionality invariant becomes a permanent CI proof**, not prose: a ctest lane
   that builds the `vcs_app` fixture with `$TRIDENT_PROXY` unset, no index configured,
   and only the committed lock + the local bare-repo fixture reachable — green forever.
   (The checksum DB baseline is a local file per GT4, not a service, so it does not
   violate the zero-service build.)

### 5.4 Potential problems (D-C specific)

| # | Problem | Recommended solution |
|---|---|---|
| P-C1 | **Left-pad exposure during the deferral window** — until the proxy exists, a deleted/moved upstream repo can break *fresh* fetches (not locked+stored ones). | Accepted and mitigated, in order: (i) the content store caches every fetched version locally; (ii) GT4's `trident vendor` gives committed, hermetic source for anything that matters; (iii) the checksum DB detects (not prevents) moved tags — proposal §11 Q6's honest framing. The window's real exposure is "new machine, dead upstream, no vendor dir" — document it in GT4's user docs rather than accelerating the proxy. |
| P-C2 | **Optional services drifting mandatory** — the classic failure: tooling, docs, or CI start assuming the proxy/index exist, and the "optional" label rots. | The P-C-5 CI proof (§5.3.5) plus STOP trigger (d): any code path where a build *requires* proxy/index/publish infrastructure is an escalation, not a bugfix. |
| P-C3 | **Registry centralization risk** — a single community proxy + single checksum DB signer quietly becomes a de-facto central registry with gatekeeping power. | Architecture already resists it: identity is the VCS path (no name grant), the index never hosts or gates code (proposal §3.1), and everything is mirrorable (append-only signed files served statically). Add to the promoted design: publish self-hosting instructions for proxy + checksum DB on day one, and never add a service-side "approve/reject publish" step — `publish` gates are all local (validate + clean tree). |
| P-C4 | **Yank semantics vs. lock-verbatim builds** — GT3's lock-verbatim fast path skips MVS, so a yank must not (and does not) affect locked builds; but `update`/`add` re-runs must see yanks, which requires the yank record to be *fetchable* (checksum DB or index). | Put yank records in the checksum DB (GT4 infrastructure, local-file baseline, mirrorable), not the index — so yank visibility never depends on an optional service being reachable; an unreachable DB degrades to "yank not seen," which is fail-open selection, never a broken build. State this trade explicitly for owner sign-off. |
| P-C5 | **Trident/Leviathan separation under service pressure** — provenance/signing features love to creep toward "the compiler embeds the attestation." | The compiler never learns any of this exists. Grep proofs from GT1 run at GT5's gate verbatim; anything else is STOP trigger (a). |

---

## 6. D-D — Registry discoverability (the searchable thin index)

### 6.1 The deferment, verbatim

`docs/proposal-package-manager.md:620–622` (§11 open Q2):

> **Index necessity & timing.** Do we ever need the searchable name index, or do VCS
> paths suffice forever? Defer (Phase 4); Deno's reversal says *some* discoverability
> helps, but don't over-build early (§3.1).

Reinforced at §8.6 Phase 4 (`:503–505`): the "thin searchable index for
discoverability" is "added only when the ecosystem is big enough to want it — Deno's
lesson: don't over-build the registry early," and at §3.1 (`:192–194`): the index "maps
friendly names to VCS paths; it never hosts or gates code."

### 6.2 Why the deferral is legitimate

1. **Both halves of the Deno evidence are respected.** Deno shipped no
   discoverability and had to reverse into JSR (proposal §2.1) — so "never" is wrong.
   But JSR was built *after* an ecosystem demanded it — so "now" is also wrong. The
   only defensible timing is measured demand.
2. **Discoverability without an ecosystem is a squatting market.** A name index whose
   names point at a handful of modules invites pre-registration of every good name the
   moment it opens. Opening it *after* real modules exist (post-GT5 publishing) means
   most valuable names map to real, established paths on day one.
3. **VCS paths genuinely suffice for the bootstrap era.** Every consumer today is
   in-repo or first-party; `github.com/thaden0/json` is not a discoverability problem
   for the people who wrote it.

### 6.3 Resolution path — *measured* discoverability

1. **Define the trigger metrics now, cheaply** (this document is the record): revisit
   the index when, post-GT5, any of these holds — (a) ≥ ~50 distinct third-party
   modules appear in checksum-DB records, (b) recurring name-collision or
   "how do I find X" reports, (c) `trident add` UX feedback specifically requesting
   names. Until a metric fires, the answer to "where's the registry website?" is
   "VCS paths, on purpose" — with this section as the citation.
4. **When it fires, build the *thin* index and stop:** a signed, append-only,
   statically-servable name→VCS-path list (same serving model as the checksum DB);
   first-wins, immutable entries; consumed only by `trident add <name>` sugar (§4.3.2)
   and a `trident search <term>` that greps the list. **No** blob hosting, **no**
   publish gating, **no** download counts/stars/web app in v1 of the index — each of
   those is the over-build the deferral exists to prevent, and each is one-way
   (removing a registry feature is a community fight; not adding it is free).
3. **Name policy fixed before opening:** first-wins immutable; disputes handled only
   for legal/malware takedown (mirroring the hard-delete policy of proposal §2.4);
   because manifests store *paths* (§4.3.2), a name dispute can never retarget an
   existing project's builds — which drains most of the venom from name fights.

### 6.4 Potential problems (D-D specific)

| # | Problem | Recommended solution |
|---|---|---|
| P-D1 | **The index reintroduces the flat-namespace squatting the VCS-path design dodged** (proposal §2.5). | Contain, don't prevent: names are advisory sugar resolved once at `add` time; identity/integrity stay path+hash; first-wins immutable + takedown-only disputes; open the index only post-ecosystem (§6.2.2). |
| P-D2 | **Index becomes a SPOF via UX gravity** — if docs teach `trident add json` as *the* way, an index outage "breaks" onboarding even though builds are fine. | Docs always show the path form first, name form as sugar; `add <name>` failure message prints the path-form fallback. CI proof from §5.3.5 keeps builds index-free. |
| P-D3 | **Scope creep toward a full registry** (hosting, ratings, analytics). | The "never hosts or gates code" line is a STOP-level invariant (§9); anything beyond the signed name→path list needs a new owner-approved design. |

---

## 7. Cross-cutting potential problems (the deferral window itself)

Problems that exist *because* the items wait, independent of any one item:

| # | Problem | Recommended solution |
|---|---|---|
| X-1 | **Deferred ≠ remembered.** Four deferrals across three docs and a memory file are easy to lose; the failure mode is an agent re-discovering (and re-designing) one ad hoc in 2027. | This document is the single tracker; `designs/complete/techdesign-00-overview.md`-style convention applies (move to `designs/complete/` only when *all four* items have landed or been explicitly cancelled). Each trigger firing appends to §10. |
| X-2 | **Premature starts.** An eager implementation pass reads P2.3's spec in the P2 design and starts it "since it's written down." | §9's STOP condition (f): the P2.3/GT5 row's own "post self-host; explicitly deferred, non-blocking" text plus this tracker gate it. The P2 design's §2 note already says GT5 must not compete with bootstrap. |
| X-3 | **Availability during the window** (left-pad class). | Covered per-item at P-C1: store cache + GT4 `vendor` + checksum-DB detection carry the window; the proxy closes it at GT5. |
| X-4 | **Correctness debt compounding under the deferrals** — lock/MVS behaviors that D-items will build on (hash verification, cycle policy, identity encoding) shifting between now and 2027. | GT4 lands the verification half on schedule (pre-self-host); the cycle ruling is escalated once, now-ish, via §4.3.3 (it is cheap to decide and expensive to leave ambiguous); canonicalization and identity are frozen with STOP protection (P-B2, P-B4). |
| X-5 | **Trident/Leviathan separation erosion** — every one of these features (workspaces, names, publishing, provenance) has a tempting compiler-side shortcut somewhere. | The standing invariant: the compiler sees plans (`src`/`edge` rows) and nothing else. The GT1 grep proofs run at every future gate; every section above restates its own separation clause; violation is STOP trigger (a)/(b), never a local judgment call. |

---

## 8. Sequencing / timeline (authoritative for the deferrals)

| When | What | Status/gate |
|---|---|---|
| now → **2026-11-30** | **P2.2 / GT4** (checksum DB, `vendor`, `audit`) — the next actual step; prerequisite to D-B/D-C; **not deferred**. | Per `docs/techdesign-package-manager.md` §2; not started as of 2026-07-06. |
| now-ish (no code) | **Owner ruling on the cycle policy** (§4.3.3) — a decision, not an implementation. | Recorded in §10 when made. |
| ~2026-11 → **2027-01-15** | **Self-host window (portable backend P5 → G5).** All four deferrals frozen hard — no trident scope may compete with the bootstrap. | G5 per `docs/techdesign-portable-backend.md:143`. |
| **2027-01-15** | **G5 lands → this tracker's review trigger.** Re-read this doc; promote D-C to a milestone techdesign. | Append review to §10. |
| **2027-02-15** | **D-C / P2.3 / GT5** — publish, yank, proxy, index wiring; optionality CI proof. | Gate text at `docs/techdesign-package-manager.md:194`. |
| post-GT5, metric-gated | **D-D** — thin index when §6.3's metrics fire; then **D-B remainder** (named specifiers ride the index). | Measured; no scheduled date by design. |
| first multi-target consumer (earliest ~2027-Q1/Q2, the self-hosted repo) | **D-A** — workspaces (`members` first, `targets` if demanded). | Consumer-gated; no scheduled date by design. |
| as-needed (ecosystem-scale) | P2.4 provenance / transparency-log scale-up (D-B item 2's far end). | Explicitly unscheduled (GT6). |

---

## 9. STOP conditions (consolidated — escalation, not judgment calls)

Inherits `docs/techdesign-package-manager.md` §0.3 triggers (a)–(e) verbatim. In
addition, for the items in this tracker, a Sonnet-class agent must **STOP, log in §10,
and escalate to a Fable-class model** before:

- **(f)** starting **any** D-item before its §8 trigger (GT4 for all; G5 for D-C;
  metrics for D-D; a real consumer for D-A) without a recorded owner ruling;
- **(g)** any workspace (D-A) design that changes the build-plan contract, makes
  `leviathan` workspace-aware, or produces anything other than N ordinary plans;
- **(h)** any change that makes a build from a complete `trident.lock` + git access
  require the proxy, the index, publish infrastructure, or any network service — the
  optionality invariant (`docs/techdesign-package-manager.md:194`,
  `docs/proposal-package-manager.md:449–450`) is permanent;
- **(i)** an index that hosts code, gates publishing, or maps names to anything other
  than VCS paths;
- **(j)** resolving the Doc 3 §4.5 acyclicity conflict (§4.3.3) in code before the
  owner ruling lands;
- **(k)** touching the content-hash canonicalization or the `(vcs-path, major)`
  identity encoding in the course of any D-item.

---

## 10. Review log (append-only)

**2026-07-06 — tracker created.** All four deferrals grounded against their source
docs at the exact refs in §0.2; current-state verification in §1 (GT3 landed; GT4 next,
not started; MVS cycle-tolerance confirmed at `tools/trident/mvs.cpp:40–96`; self-host
G5 = 2027-01-15). Open action carried: the §4.3.3 cycle-policy owner ruling. No code
written; no existing file modified.
