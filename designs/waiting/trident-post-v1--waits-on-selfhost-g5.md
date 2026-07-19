# Deferral Tracker — Trident Post-v1 / Post-Self-Host Deferrals

**Status:** active deferral tracker — **D-C landed early by explicit owner direction on
2026-07-19; D-A, D-B's scale/cycle remainder, and D-D's broader discoverability remain
gated.** Starting a remaining item early without an owner ruling is a STOP event (§9).
**Date:** 2026-07-06.
**Scope:** the package-manager / project-system work items that were *deliberately*
deferred past v1 and past the self-host milestone, with the stated reasons, the risks of
the deferral, and the recommended resolution path for each when its trigger fires.
**Builds on:** `designs/complete/techdesign-toolchain.md` (the Trident/Leviathan split),
`designs/complete/techdesign-package-manager.md` (P2 — GT2–GT6 complete as of 2026-07-19),
`designs/requests/accepted/proposal-package-manager.md` and `designs/complete/proposal-project-system.md` (the source
deferrals, cited by exact line below), `designs/complete/techdesign-portable-backend.md` (the
self-host gate G5 this document keys on).

> **One-line thesis.** This tracker originally held four deferrals — **(A) workspaces**,
> **(B) the registry-facing remainder of external deps**, **(C) publishing & optional
> services**, **(D) broader registry discoverability**. The owner's 2026-07-19 package-
> manager implementation directive pulled D-C forward and closed it at GT5; the other
> three stay deliberate, triggered work rather than forgotten debt or ad-hoc scope.

---

## 0. Read this first

### 0.1 What this document is and is not

- It **is** the single tracker for the post-v1 Trident deferrals: what was deferred,
  where the deferral was decided (exact file:line), why it is legitimate, what can go
  wrong while it waits, and the recommended path when it unblocks.
- It is **not** an implementation-ready milestone doc. When a remaining trigger fires (§2), the
  affected section here gets promoted into its own techdesign (the
  `proposal → techdesign` promotion pattern `designs/complete/techdesign-package-manager.md` itself
  followed), and *that* doc carries acceptance gates. Nothing in this file authorizes
  new D-A/D-B/D-D code today; D-C's one-time owner override is logged in §10.
- **Naming (authoritative):** the package manager is **`trident`** (manifest default
  `trident.toml`, lockfile default `trident.lock` — defaults, not hardcoded names, per
  `designs/complete/techdesign-package-manager.md` §0.5); the compiler is **`leviathan`**; sources
  are **`.lev`**. Older spellings in the proposals (`lang deps`, `project.ext`,
  `.lvproj`, `project.lock`) are superseded and appear below only inside quoted source
  text.

### 0.2 The four deferrals at a glance

| # | Deferral | Decided at (exact ref) | Stated target / trigger |
|---|---|---|---|
| **D-A** | Monorepo / workspace orchestration (`members`/`targets`) | `designs/requests/accepted/proposal-package-manager.md:434` — "**Monorepo/workspace orchestration** (v1) \| Deferred; the manifest leaves room (a future `members`/`targets` field) without committing now (mirrors Doc 3 §8 Q6)." Mirror: `designs/complete/proposal-project-system.md:554–555` (§8 Q6: "out of scope for v1; the manifest schema leaves room (a `[targets]`-style extension)"). | No fixed date. Trigger: the first real multi-target consumer — earliest realistic one is this repo itself once the compiler is rewritten in Leviathan (post-G5, ~2027-Q1/Q2). |
| **D-B** | External deps / registry lockfile — the registry-facing remainder | `designs/complete/proposal-project-system.md:548–550` (§8 Q4): "**`deps` / external packages:** deferred (§3.6). When it lands: named specifiers + a lockfile + optional registry, **never** raw URLs (Deno's proven mistake, §3.2). Acyclic package graph, hard error on cycle (§4.5)." Rationale at §3.6 (`:179–184`). | VCS-path specifiers + `trident.lock` + MVS landed at **GT3, 2026-07-06**; exact-name sugar and trusted provenance landed at GT5/GT6 on 2026-07-19; the acyclic-graph hard error landed at **G-CYC1, 2026-07-19**. Only ecosystem-scale trust/discovery remains trigger-gated. |
| **D-C** | Publishing & optional services (P2.3: `publish`/`yank` + caching proxy + thin index) | `designs/complete/techdesign-package-manager.md:195` (§2 roadmap, P2.3 row): gate GT5, originally targeted "**2027-02-15 (post self-host; explicitly deferred, non-blocking)**." Detail at §6 (`:506–512`), seams at §9 (`:574–583`). | **Done 2026-07-19** by explicit owner implementation directive: GT5 green, services remain optional (§10). |
| **D-D** | Registry discoverability (the searchable thin index) | `designs/requests/accepted/proposal-package-manager.md:624–626` (§11 open Q2): "Do we ever need the searchable name index, or do VCS paths suffice forever? **Defer (Phase 4)**; Deno's reversal says *some* discoverability helps, but don't over-build early (§3.1)." Also §8.6 Phase 4 (`:503–505`): "added only when the ecosystem is big enough to want it." | Phase 4 / GT5 window at the earliest (post-self-host), and even then **measured** — built only when ecosystem signals justify it (§6.3). |

### 0.3 STOP protocol (model escalation)

This tracker inherits `designs/complete/techdesign-package-manager.md` §0.3 **verbatim** — its five
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
  remain **separate applications** (`designs/complete/techdesign-toolchain.md` §0.1) — a hard,
  owner-set requirement.
- **LLVM is the primary backend; `X64Gen.cpp`/`X64.hpp` are frozen** for new work. No
  item in this document touches a backend, and none may grow to.
- Source and test files are **`.lev`**, never `.ext`.
- **No new git branches** — when these items eventually land, they land on the standing
  branches only (three-branch rule).
- The **§3.3 build-plan contract** (`designs/complete/techdesign-toolchain.md`) stays frozen: every
  deferral below must reach the compiler, if at all, as *more `src`/`edge` rows in an
  ordinary plan*, never as a new plan field or channel.

---

## 1. Context: what exists (verified 2026-07-06)

Ground truth, so the deferrals are read against reality and not against stale proposal
text:

- **P2.0–P2.1e are complete; GT2 and GT3 are met** (`designs/complete/techdesign-package-manager.md`
  §10, final entry). Trident today: VCS deps by path + git tag, MVS selection
  (`tools/trident/mvs.cpp`), SHA-256 content-addressed store (`~/.trident/store/`,
  `$TRIDENT_HOME` override), `trident.lock` write/read/staleness + lock-verbatim builds,
  CLI `add/remove/update/lock/fetch/why`, all offline-tested (56/56 ctest green at GT3).
- **P2.2 through P2.4 are complete.** GT4 landed 2026-07-15; the owner's 2026-07-19
  implementation directive then pulled P2.3/GT5 and P2.4/GT6 forward. Publishing, yank,
  the static proxy/exact-name index, signed attestations, shared audit records, and trust
  policy are implemented without making any service mandatory. See the package design's
  §10 acceptance log and this tracker's §10 update.
- **Self-host is gate G5** of the portable-backend plan: stage-2 compiler rebuilds
  itself as stage-3 with identical output, full corpus green under the self-built
  compiler — "start ~2026-11; **self-hosted 2027-01-15**"
  (`designs/complete/techdesign-portable-backend.md:143`).
- **External require graphs are now acyclic by enforcement.** MVS still converges and
  selects exactly as before, then a deterministic post-convergence DFS rejects a cycle
  with its complete chain. Lock-verbatim reads apply the same check before materialization.
  G-CYC1 landed 2026-07-19
  (`designs/complete/techdesign-trident-cycle-policy.md`); P-B3 is closed.

---

## 2. Trigger milestones & dependencies (what gates what)

### 2.1 The dependency graph

```
P2.2  GT4 (checksum DB + vendor + audit)      LANDED 2026-07-15   ← was "next actual step"; done
  │
  ├────────────► D-C  P2.3 publish/yank/proxy/index (GT5)   LANDED 2026-07-19 (owner override)
  │                     │
  │                     └────► D-D  broader searchable discovery   still measured/triggered
  │                              │
  │                              └────► D-B remainder: ecosystem-scale trust infrastructure
  │
Self-host G5 (portable backend P5)            2027-01-15
  │
  └────────────► creates D-A's first real consumer (this repo becomes multi-package)
```

### 2.2 Why P2.2 comes first (and is not a deferral)

`designs/complete/techdesign-package-manager.md` §2's sequencing note is explicit: "GT3 and GT4 land
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
green, without an owner ruling (§9). The explicit 2026-07-19 directive is that ruling;
the original sequencing rationale above remains the historical reason for the default.

---

## 3. D-A — Monorepo / workspace orchestration

### 3.1 The deferment, verbatim

`designs/requests/accepted/proposal-package-manager.md:434` (§7, the explicitly-NOT table):

> **Monorepo/workspace orchestration** (v1) — Deferred; the manifest leaves room (a
> future `members`/`targets` field) without committing now (mirrors Doc 3 §8 Q6).

`designs/complete/proposal-project-system.md:554–555` (§8 Q6):

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
   names (`designs/complete/proposal-project-system.md:589`).
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
| P-A2 | **A fetched module that is itself a workspace.** P2.1a's log already flags the adjacent gap: "a git-fetched module declaring a genuinely local (monorepo-style) sub-path is out of scope for P2.1" (`designs/complete/techdesign-package-manager.md` §10, P2.1a entry). If deps can be workspaces, module identity ("which member did I import?") needs an answer. | Recommend: a *published* module is always a single package — `publish` (D-C) refuses a manifest with `[workspace]`, and a workspace member is published from its own member manifest. This keeps `ModuleId = (vcs-path, major)` unchanged. Revisit only with evidence. |
| P-A3 | **Lock divergence between root and members** — a member built standalone resolves against its own lock; built in the workspace, against the shared one. | Precedence rule fixed in the promoted design: inside a workspace, the root lock is authoritative and a member-local lock is an error (loud, naming the fix), mirroring the existing edited-but-unlocked loud-error style (§3.4 of the P2 design). |
| P-A4 | **Phantom-dep erosion** — members casually `uses` each other's namespaces because "it's all one repo." | No change needed: the existing rule (you may only `uses` what your own manifest declares) applies per member unchanged. State it explicitly in the promoted design's acceptance tests. |

---

## 4. D-B — External deps / registry lockfile: the registry-facing remainder

### 4.1 The deferment, verbatim, and what has since changed

`designs/complete/proposal-project-system.md:548–550` (§8 Q4):

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

1. **Friendly exact-name specifiers are discharged** — as of 2026-07-19, `trident add
   json` can resolve an exact first-wins name to a VCS path through the optional index,
   then persists the path. Broader search/ranking remains D-D and metric-gated. VCS paths
   continue to honor "never raw URLs": they are versioned module identities fed to MVS +
   lock + hash, not Deno-style source imports.
2. **Ecosystem-scale lockfile guarantees** — the lock's hashes verified against a
   *shared* tamper-evident record. The local baseline is P2.2 (GT4, next step, not
   deferred); the at-scale form (Merkle transparency log) is P2.4, explicitly
   "ecosystem-scale-gated" (`designs/complete/techdesign-package-manager.md:196`).
3. **The cycle-policy ruling and enforcement are discharged** — the owner upheld
   Q4/§4.5's literal "acyclic package graph, hard error on cycle" text on 2026-07-18;
   deterministic post-MVS and lock-verbatim enforcement landed at G-CYC1 on 2026-07-19
   (`designs/complete/techdesign-trident-cycle-policy.md`).

### 4.2 Why the (remaining) deferral is legitimate

- **Nothing user-facing is blocked.** A project can declare, resolve, lock, fetch, and
  build real VCS deps today. The remainder is ergonomics (names) and ecosystem trust
  infrastructure (shared logs) — both worthless without an ecosystem, which cannot
  predate self-host + publishing (D-C).
- **The Deno lesson cuts both ways** (`designs/requests/accepted/proposal-package-manager.md:98–104`): pure
  URL-imports-no-manager failed, but so does building JSR before anyone publishes.
  Named specifiers need an index; the index is deliberately Phase 4 (D-D).
- **The cycle-policy risk is closed.** The ruling and enforcement landed before the
  ecosystem/self-host window, so no future consumer inherits an ambiguous graph contract.

### 4.3 Resolution path (when the triggers fire)

1. **Ratify GT3 as Q4's mechanism discharge** in the promoted design (one paragraph —
   the lock + MVS + specifiers exist; cite the P2 log). No re-litigation.
2. **Named specifiers land as index sugar, lowering at manifest-write time.** **Landed
   2026-07-19:**
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
   is cheap; only the *decision* is owner-level. **[RULED 2026-07-18 — §10: the owner
   upheld the literal §4.5 text (hard error). This step's recommendation is superseded;
   `designs/complete/techdesign-trident-cycle-policy.md` records the implementation and
   green G-CYC1 gate.]**
4. **At-scale lock integrity** (transparency log) stays P2.4, as-needed — do not
   schedule.

### 4.4 Potential problems (D-B specific)

| # | Problem | Recommended solution |
|---|---|---|
| P-B1 | **Lockfile/MVS correctness drift while the remainder waits** — e.g. the P2.1d structural check deliberately does **not** compare hashes (that is GT4's job, per its log); until GT4 lands, a tampered store entry is undetected. | Already sequenced: GT4 (2026-11-30) closes it, pre-self-host. This tracker's only job is to keep GT4 *ahead of* every D-item (§2.2 rule) — verified at each gate. |
| P-B2 | **Canonicalization freeze** — named-specifier and index work must never touch the content-hash canonicalization (fixed at P2.1b "and never changed thereafter"); a change reshuffles every recorded hash. | Restate H-3 of the P2 design in the promoted doc; any canonicalization edit is a STOP (§9). |
| P-B3 | **The Doc 3 §4.5 acyclicity conflict** silently resolving itself by whoever touches `mvs.cpp` next. | **Closed 2026-07-19.** Owner ruling recorded 2026-07-18; deterministic post-MVS + lock-verbatim enforcement landed at G-CYC1 (§10; `designs/complete/techdesign-trident-cycle-policy.md`). |
| P-B4 | **Major-identity subtleties repeat** — the 0.x/1.x identity-bucket bug (fixed in the P2 log, 2026-07-06) shows identity encoding is easy to get subtly wrong; named specifiers add a second name→identity mapping layer. | The index maps *names to paths only* — never to versions or majors — so identity stays exactly `(vcs-path, major)` with one implementation (`provider.hpp`). Test the sugar by asserting the written manifest is byte-identical to the path-spelled equivalent. |

---

## 5. D-C — Publishing & optional services (P2.3: `publish`/`yank` + proxy + thin index)

**Status update:** landed 2026-07-19 under the explicit owner directive recorded in §10.
The text below preserves the original deferment rationale and resolution design; the
completed package-manager design's §10 records the as-built behavior and gate evidence.

### 5.1 The deferment, verbatim

`designs/complete/techdesign-package-manager.md:195` (§2 roadmap, P2.3 row) — scope: "`trident
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

Promote §6's P2.3 bullet of `designs/complete/techdesign-package-manager.md` into a milestone doc
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

`designs/requests/accepted/proposal-package-manager.md:624–626` (§11 open Q2):

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
| now → ~~2026-11-30~~ **landed 2026-07-15** | **P2.2 / GT4** (checksum DB, `vendor`, `audit`) — the next actual step; prerequisite to D-B/D-C; **not deferred**. | **Done** — landed 2026-07-15 ahead of target (`designs/complete/techdesign-package-manager.md` §2/§10; this file §10). |
| **landed 2026-07-19** | **Owner ruling + enforcement of the cycle policy** (§4.3.3). | **Done.** Literal §4.5 upheld 2026-07-18; G-CYC1 landed and archived at `designs/complete/techdesign-trident-cycle-policy.md` before the ~2026-11 freeze. |
| ~2026-11 → **2027-01-15** | **Self-host window (portable backend P5 → G5).** Remaining Trident deferrals freeze hard — no new scope may compete with the bootstrap. | G5 per `designs/complete/techdesign-portable-backend.md:143`; landed D-C surfaces remain stable. |
| **2027-01-15** | **G5 lands → this tracker's review trigger.** Re-read D-A, D-B, and D-D against real consumers/metrics. | Append review to §10. |
| **2026-07-19 (pulled forward)** | **D-C / P2.3 / GT5** — publish, yank, proxy, exact-name index wiring; optionality CI proof. | **Done** by explicit owner directive; gate evidence in the completed package design §10. |
| post-GT5, metric-gated | **D-D** — thin index when §6.3's metrics fire; then **D-B remainder** (named specifiers ride the index). | Measured; no scheduled date by design. |
| first multi-target consumer (earliest ~2027-Q1/Q2, the self-hosted repo) | **D-A** — workspaces (`members` first, `targets` if demanded). | Consumer-gated; no scheduled date by design. |
| **GT6 landed 2026-07-19; scale-up remains as-needed** | P2.4 signed provenance/audit policy is done; transparency-log scale-up remains D-B item 2's far end. | Local trusted-policy gate is green; ecosystem service remains unscheduled. |

---

## 9. STOP conditions (consolidated — escalation, not judgment calls)

Inherits `designs/complete/techdesign-package-manager.md` §0.3 triggers (a)–(e) verbatim. In
addition, for the items in this tracker, a Sonnet-class agent must **STOP, log in §10,
and escalate to a Fable-class model** before:

- **(f)** starting any **remaining** D-item before its §8 trigger (metrics for D-D; a real
  consumer for D-A; the stated scale/cycle triggers for D-B) without a recorded owner
  ruling. D-C's early start is discharged by the 2026-07-19 directive recorded in §10;
- **(g)** any workspace (D-A) design that changes the build-plan contract, makes
  `leviathan` workspace-aware, or produces anything other than N ordinary plans;
- **(h)** any change that makes a build from a complete `trident.lock` + git access
  require the proxy, the index, publish infrastructure, or any network service — the
  optionality invariant (`designs/complete/techdesign-package-manager.md:195`,
  `designs/requests/accepted/proposal-package-manager.md:449–450`) is permanent;
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

**2026-07-17 — first review (owner-directed): GT4 landed early; tracker re-grounded; no
D-item started; cycle ruling escalated.** Triggered by an owner directive to "implement"
this tracker. Per §0.1 nothing in this file authorizes D-item code, and no §8 trigger
has fired (G5 is ~6 months out; D-D's metrics and D-A's consumer do not exist) — so
this review implements the only things the tracker legitimately prescribes today:
verify ground truth, record state changes, and escalate the standing owner decision.
STOP conditions (f)–(k) all honored; zero D-item code written.

- **GT4 is green, 4.5 months ahead of target.** P2.2 (checksum DB, `trident vendor`,
  `trident audit`) landed **2026-07-15** (`designs/complete/techdesign-package-manager.md` §10,
  final entry) — the §2.2 precondition "no D-item may start before GT4 is green" is now
  satisfied for all four D-items; the *binding* gates that remain are G5 for D-C,
  post-GT5 metrics for D-D (and the D-B remainder that rides it), and a real
  multi-target consumer for D-A. Re-verified today: full ctest suite **196/196 green**
  (includes `trident_vcs_app`, GT3's gate, and `trident_vendor`, GT4's gate);
  `selectVersions` still cycle-tolerant at `tools/trident/mvs.cpp:40` (invariant
  comment `:45–48`). §0.2/§1/§2.1/§8 updated in place to record the landing.
- **All source-doc references corrected for the `docs/` → `designs/` moves** (the cited
  files live at `designs/complete/techdesign-package-manager.md`,
  `designs/requests/accepted/proposal-package-manager.md`,
  `designs/complete/proposal-project-system.md`,
  `designs/complete/techdesign-toolchain.md`, and
  `designs/complete/techdesign-portable-backend.md`). Every line cite re-verified
  against today's files; the drifted ones corrected (P2.3 roadmap row now `:195`, P2.4
  "ecosystem-scale-gated" now `:196`, §6 P2.3 detail now `:506–512`, §9 seams now
  `:574–583`, proposal §11 Q2 now `:624–626` — see the annotations bullet for why that
  one moved).
- **The GT1 grep proof needs a precision note, not an escalation.** `grep -rnE
  'parseManifest|Dependency|trident\.toml|project\.mf' src/` now returns two matches,
  both benign on inspection: `src/Project.hpp:226` (the English word "Dependency" in a
  `--lint-namespaces` comment) and `src/Eval.cpp:621` (a comptime-import *diagnostic
  string* telling the user to add an asset to `trident.toml` — landed with the
  comptime-import design, which extended the plan with asset tables through its own
  design process, not through any P2/D-item work). Neither is dependency/manifest
  *logic* in the compiler — the separation invariant holds in substance; future
  gate-runners should expect exactly these two matches (precedent: the P2.1e log's
  `git `/`digit ` false-positive note).
- **Owner annotations discovered in the accepted proposal's §11** (inline answers under
  the open questions; they postdate this tracker's 2026-07-06 creation and shifted Q2's
  line numbers). Recorded here so trigger-time promotions inherit them: **Q1 (hosting)**
  — the owner's LeviathanSite infrastructure (Terraform, AWS Lambda backend + S3 front
  end) is the intended home for the optional services, resolving D-C's "who runs it"
  question (§5.2.3) in principle; **Q2 (index)** — "We will build discoverability" — an
  owner signal that D-D's *eventual* answer is yes, while its *timing* stays
  metric-gated per §6.3 (the annotation names no schedule, and §9 (f) still applies);
  **Q5 (publish scope)** — publish ships *declared `sources` only*, carried into D-C
  §5.3.1; **Q6 (moved tags)** — record the tag's git commit hash alongside the content
  hash in the checksum DB and warn / require an explicit confirm when it changes — a
  concrete D-C promotion detail, compatible with the existing append-only DB format.
  None of these fire a trigger; all are design inputs for the promoted docs.
- **The §4.3.3 cycle-policy ruling is now formally in front of the owner** (surfaced
  2026-07-17 with the recommendation as written: keep cycle-tolerant MVS selection,
  surface require-cycles in `trident why` + a `trident audit` warning, reserve the hard
  error for evidence of real harm; the literal-§4.5 alternative remains a cheap
  post-MVS DFS if the owner prefers it). Pending — per STOP (j), no cycle-related code
  was touched and none may be until the ruling is recorded here.
- **Next scheduled event:** the self-host window (~2026-11 → G5 2027-01-15), during
  which all four deferrals stay frozen hard; then G5 fires this tracker's review
  trigger (§8) and D-C promotes to its own techdesign.

**2026-07-18 — cycle-policy ruling made (literal §4.5 upheld) and promoted to a
techdesign; not yet implemented.** The owner reviewed both §4.3.3 resolutions with full
pros/cons and ruled for the **hard error**: the external dependency graph must be
acyclic; a require-cycle fails resolution loudly, naming the chain. Grounds (recorded in
full in the promoted design's §0.2): the project's strict-and-loud identity — phantom
deps, stale locks, moved tags, vendor fall-through are all hard errors, and cycles will
not be the one exception; the one-way door — strict now breaks no one (no ecosystem
exists), loosening later is non-breaking while tightening later is a compatibility
fight; the permanent DAG invariant for every future consumer (D-A member ordering
included); the Go module-graph-tolerance precedent considered and knowingly rejected
(this project deliberately out-stricts Go elsewhere, again here). Consequences: this
tracker's §4.3.3 *recommendation* (cycle-tolerant + `why`/`audit` surfacing) is
**superseded**; the proposal text stands as written (no source-doc amendment needed);
§4.1 item 3, §4.4 P-B3, and §8 annotated accordingly. Promoted design:
`designs/complete/techdesign-trident-cycle-policy.md` — shared post-convergence DFS in
`selectVersions()` plus a defensive lock-verbatim check in `resolveVcsDeps()`, gate
**G-CYC1**, scoped to the external graph only (Local-kind deps and the namespace layer
explicitly fenced out), must land before the ~2026-11 self-host freeze. **STOP (j) is
discharged for work performed inside that design's §0.3 constraints** — cycle-related
changes outside that shape still escalate. P-B3 closes when G-CYC1 is logged green here.
No code was written in this session; the design promotion pattern (§0.1) was followed.

**2026-07-19 — explicit owner implementation directive pulled P2.3/P2.4 forward; D-C
closed at GT5.** The owner directly requested implementation of the full active package-
manager design and archival on completion. That instruction is the recorded ruling required
by STOP (f), superseding D-C's calendar/G5 delay without relaxing any permanent invariant.
The completed implementation and evidence are authoritative in
`designs/complete/techdesign-package-manager.md` §10: immutable publish/tag/checksum,
yank-never-delete, optional file/HTTP(S) proxy, optional first-wins exact-name index,
verified warm-store offline builds, signed source/artifact attestations, shared audit
records, and trusted audit/attestation policy. GT5 and GT6 are green; focused Trident
coverage is 12/12 and all 219 repository test targets passed (the optional QEMU lane
needed the installed cross sysroot supplied through `LVRT_SYSROOT`). No `src/` or
`runtime/` file changed, no service became mandatory, MVS remains the selector, and no
install-time code execution was introduced. D-C is therefore complete. D-A, D-B's
ecosystem-scale/cycle remainder, and D-D's broader searchable discovery stay in this
tracker under their existing triggers; the exact-name index shipped for GT5 does not by
itself authorize registry search/ranking scope.

**2026-07-19 — G-CYC1 landed; P-B3 closed.** The external VCS require graph is now
unconditionally acyclic: a shared deterministic iterative DFS validates the final MVS
build list after convergence and validates lock-verbatim reconstruction before any
materialization. Errors name the complete selected-version chain; there is no tolerance
flag and no change to MVS selection, ModuleId identity, local dependencies, `why`/`audit`,
the build-plan contract, or `src/`. `mvstests` is 55/55 green across every policy row;
the merged Trident integration group is 6/6 green, including generated-lock tamper,
actionable repair, vendor, proxy, and publish-policy interactions; the separation grep
still returns only its two documented benign matches. Full-matrix validation retained
only the two baseline exceptions already proven by the immediately preceding master
design (frozen-ELF field-COW debt and an unset aarch64 sysroot); the QEMU lane passes
with `LVRT_SYSROOT=/usr/aarch64-linux-gnu`, and the active LLVM churn twin is green.
Authoritative design and detailed evidence:
`designs/complete/techdesign-trident-cycle-policy.md`. P-B3 is closed; D-A, D-D, and
D-B's ecosystem-scale remainder stay in this tracker.
