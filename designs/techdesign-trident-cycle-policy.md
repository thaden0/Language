# Tech Design — Trident Cycle Policy (Acyclic External Dependency Graph)

**Status:** design, ready for implementation. **Date:** 2026-07-18. **Priority: MEDIUM —
small and self-contained, but must land before the self-host freeze window opens (~2026-11;
`designs/deferal-trident-post-v1.md` §8 freezes all trident scope during the bootstrap).**
**Records and implements an owner ruling.** This doc is the promotion of the cycle-policy
decision tracked at `designs/deferal-trident-post-v1.md` §4.3.3 / P-B3 / STOP (j): the
ruling has been made (2026-07-18, §0.2 below) and is recorded in that tracker's §10; this
design specifies the implementation. Until this doc's gate is green, the ruling is *made
but not enforced* — there is no interim state where partial enforcement ships.
**Builds on:** `designs/complete/techdesign-package-manager.md` (P2 — GT2–GT6 all landed; §10
log authoritative), `designs/complete/proposal-project-system.md` §4.5 + §8 Q4 (the
upheld text), `designs/deferal-trident-post-v1.md` (the tracker; its §4.3.3 sketched both
candidate resolutions — this doc supersedes its *recommendation* while honoring its
*protocol*). **Everything in this design lives inside `tools/trident/` + its tests.**

---

## 0. Read this first

### 0.1 The one-line change

`trident`'s Minimal Version Selection currently *tolerates* require-cycles in the external
(VCS) dependency graph — a cycle terminates the worklist naturally and produces a valid
build list. After this design, **a require-cycle is a hard error naming the full chain**,
enforced on every resolution path (MVS runs and lock-verbatim reads), so that no trident
code path ever accepts a cyclic external dependency graph.

### 0.2 The ruling (owner, 2026-07-18 — recorded in the tracker's §10)

`designs/complete/proposal-project-system.md` §4.5 and §8 Q4 demand an **acyclic external
package graph, hard error on cycle**. The implemented MVS (P2.1a) is deliberately
cycle-tolerant, and the deferral tracker's §4.3.3 recommended keeping tolerance (surfacing
cycles informationally in `why`/`audit`). The owner ruled the other way: **uphold the
literal §4.5 text.** Rationale, recorded for posterity:

1. **Design identity.** The project's fingerprint is strict-and-loud with the fix named in
   the error: phantom deps are impossible, an edited-but-unlocked manifest is a hard error
   (never a silent re-resolve), moved tags and tampered stores are loud errors, vendor
   mode refuses to fall through to the network. A tool loud about everything except
   cycles would be less predictable than a tool with one rule.
2. **The one-way door.** Strict now costs zero — no third-party ecosystem exists, so the
   error can break no one. Relaxing later (if evidence ever demands tolerance) is
   non-breaking and instant; *adding* the error after cycles exist in published module
   sets would be a compatibility break. Tolerance is the direction that gets expensive.
3. **The DAG invariant is a gift to every future consumer** — D-A workspace build
   ordering, visualization, audit tooling, any future caching layer — none of it ever
   needs to be cycle-aware.
4. **The Go-precedent argument was rejected knowingly.** Go tolerates module require
   cycles; this project has deliberately out-stricted Go at every comparable fork, and
   does so again here. MVS does not *need* acyclicity to converge — this is a policy
   error, not a correctness fix, and is chosen as policy.

Accepted trade, also recorded: a future user whose *transitive* deps form a cycle gets an
error they didn't cause. Mitigations: the error names the complete chain (actionable), the
rule predates the ecosystem (cycles should never form), and relaxation stays free.

**Superseded by this ruling:** the tracker §4.3.3 recommendation (cycle-tolerant +
`trident why` surfacing + `audit` warning). None of that ships — under a hard error, a
successful resolution can never contain a cycle for `why`/`audit` to surface.

### 0.3 STOP protocol (model escalation)

Inherits `designs/complete/techdesign-package-manager.md` §0.3 triggers (a)–(e) verbatim — in
particular **(e): MVS stays the only selection algorithm.** This design does not modify
selection; it validates selection's *output* (§3.1). Additionally, a Sonnet-class agent
must STOP, log in §8, and escalate to a Fable-class model before:

- moving the check *into* the selection walk (mid-walk detection that can perturb or
  short-circuit selection — the check runs post-convergence only, §3.1);
- adding any tolerance escape hatch (a `--allow-require-cycles` flag, a manifest field, an
  env var) — the ruling has no escape hatch by design;
- extending the policy to Local-kind deps or namespace-level cycles (§6 non-goals — both
  are out of this ruling's scope);
- touching content-hash canonicalization or `(vcs-path, major)` identity encoding
  (tracker STOP (k), restated).

### 0.4 Frozen / do-not-touch (standing rules, restated)

- **`leviathan` (`src/`) untouched.** The separation grep
  (`grep -rnE 'parseManifest|Dependency|trident\.toml|project\.mf' src/`) must show
  exactly its two documented benign matches (tracker §10, 2026-07-17 entry) — no more.
- **The build-plan contract is untouched** — this feature errors *before* plan writing;
  it adds no plan rows, fields, or channels.
- **MVS selection semantics untouched** (§0.3). `kMaxSteps`, the worklist, the
  max-of-mins rule, ModuleId bucketing: all byte-identical.
- **No new branches; no fixture network access** (H-7 of the P2 design stands).

---

## 1. Context: what exists (verified 2026-07-18)

- **`selectVersions()`** (`tools/trident/mvs.cpp:40–96`): worklist BFS; converges because
  a module's selected version only increases (comment at `:45–48`); flattens
  `std::map<ModuleId, BuildListEntry>` into a ModuleId-sorted vector (`:92–95`). Cycles
  currently terminate silently. Each `BuildListEntry` caches its module's own direct
  requires in `requires_` (`:87`) — **the edge set the new check walks is already
  recorded; no new data is collected.**
- **`resolveVcsDeps()`** (`tools/trident/resolve.cpp:438`): seeds root requires
  (Vcs-kind only, dev-filtered per mode, `:442–449`), then either (a) the **lock-verbatim
  path** (`:488–511`) — reconstructs `BuildListEntry`s straight from `trident.lock`,
  including `requires_` edges via `parseRequireEdge`, with **zero MVS involvement** — or
  (b) a fresh `selectVersions()` run (`:520`). Materialization + checksum verification
  follow (`:537` on). Vendor mode requires the lock path.
- **A trident-written lock is produced only by a successful resolution** (`add`/`remove`/
  `update`/`lock`/`fetch` re-run MVS and rewrite the lock; P2.1e log). Consequence: once
  `selectVersions()` rejects cycles, **every legitimately-written lock is acyclic by
  construction**, and a cycle reaching the lock-verbatim path can only mean a hand-edited
  or corrupted lock.
- **Transitive `dev` edges are walked.** `expandRequires` (`mvs.cpp:29–36`) enqueues
  *every* dep a fetched manifest declares (only the *root's* dev deps are mode-filtered,
  `resolve.cpp:445`) — pre-existing, logged behavior. Cycle detection runs on the graph
  exactly as walked; if dev-edge expansion ever changes, the check follows automatically
  because it reads `requires_` off the build list rather than re-walking manifests.
- **Tests:** `tests/test_trident_mvs.cpp` (`mvstests`, 39 checks) drives MVS against an
  in-memory `FakeProvider`; `tests/run_trident_vcs_app.sh` (ctest `trident_vcs_app`)
  drives the offline end-to-end lifecycle on a scratch copy;
  `tests/run_trident_vendor.sh` owns checksum-tamper coverage. CMake Track-B region
  (`CMakeLists.txt` `# --- Track B`) already compiles `mvs.cpp` into both `trident` and
  `mvstests`.

---

## 2. Semantics: what exactly is a cycle

The graph under judgment: **nodes** are the resolved build list's `ModuleId`s
(`(vcs-path, bucketed major)` — majors 0 and 1 share one identity; each major ≥ 2 is its
own node, P2 §3.3 rule 5). **Edges** are each entry's recorded `requires_`. The root
project is not a node (nothing can require it — the manifest has no self-path).

| Shape | Verdict | Why |
|---|---|---|
| `A → B → A` (same majors) | **Hard error** | The §4.5 case. |
| `A → A` (self-require, same identity bucket — includes `A@0.x ↔ A@1.x`) | **Hard error** | One-node cycle; majors 0/1 are one identity. |
| `A@v2 → A(v1)` one-way (migration shim: v2 depends on its own v1) | **Legal** | Two distinct nodes, one edge — a DAG. This deliberately preserves the useful major-migration pattern. |
| `A@v1 → A@v2` **and** `A@v2 → A@v1` | **Hard error** | Two distinct nodes with mutual edges is a cycle like any other. |
| Edge to a ModuleId absent from the build list (possible only via a hand-edited lock) | **Not a cycle** | Treated as a leaf — dangling edges cannot fabricate a cycle; the lock's other guards (structural check, pinned hashes) own that class of tamper. |
| Cycle through a transitive `dev` edge | **Hard error** | The graph as actually walked (§1) is the graph judged. |

**Determinism:** the same build list must always name the same chain. The check iterates
entries in their already-sorted order and reports the first cycle closed by a
deterministic DFS — the reported chain is a test-assertable string, not advisory prose.

---

## 3. Implementation design

### 3.1 One shared check, two call sites

New function in `tools/trident/mvs.{hpp,cpp}` (beside the graph type it judges):

```cpp
// Returns "" when the entries' require graph is acyclic; otherwise the first
// cycle as a rendered chain "path@1.2.0 -> path@1.0.0 -> path@1.2.0"
// (deterministic for a given build list: entries are walked in their sorted
// order, edges in recorded order; the chain closes on its repeated head).
// Edges whose target ModuleId has no entry are leaves, never cycles.
std::string findRequireCycle(const std::vector<BuildListEntry>& entries);
```

Iterative three-color DFS over an index map `ModuleId → entry`; chain nodes render as
`mod.path + "@" + formatSemVer(selected)` (the true selected version distinguishes majors
naturally; do not reach into `lock.cpp`'s `serializeModuleId` — no new include edge).

**Call site 1 — `selectVersions()` (`mvs.cpp`), post-convergence.** After the worklist
finishes and the map flattens (`mvs.cpp:92–95`), before `result.ok = true`:

```
require cycle detected: <chain> — the external dependency graph must be acyclic;
break the cycle by removing one of these modules' requires on the other
```

Post-convergence placement is load-bearing (STOP §0.3): selection is *complete and
untouched* when the check runs, so the chain shows final selected versions and the check
cannot perturb the walk. This one site covers every MVS consumer: `add`/`remove`/
`update`/`lock`/`fetch`/`why`, and any `build`/`run`/`check`/`plan` whose lock is absent
or stale.

**Call site 2 — `resolveVcsDeps()` (`resolve.cpp`), lock-verbatim path.** After the lock
reconstructs `selected` (`resolve.cpp:488–511`), **before any materialization** (never
fetch for a rejected graph), and only when `usedLock` is true (the fresh-MVS branch is
already covered inside `selectVersions`):

```
trident.lock declares a require cycle (<chain>) — a trident-written lock is always
acyclic, so this file was hand-edited or corrupted — run `trident lock`
```

By §1's construction argument this fires only on tampered locks — but running it keeps
the invariant unconditional ("**no** code path accepts a cyclic graph"), covers vendor
mode for free (vendor requires the lock path), and costs O(nodes + edges) on a list that
is small by definition.

### 3.2 What deliberately does not change

- **`trident why` / `trident audit`:** nothing added. A resolution containing a cycle now
  fails before either could run; the error itself is the explanation `why` would have
  given. (`audit` inherits the lock-path check through the shared resolution it already
  reuses.)
- **`kMaxSteps`** (`mvs.cpp:49`) stays as the anti-pathological-provider backstop — it
  guards the *walk*; the new check guards the *result*. Different failure classes.
- **Local-kind deps** (`loadDepsRec` and its `visited` dedup) are untouched — see §6.

---

## 4. Testing strategy

**`mvstests` (`tests/test_trident_mvs.cpp`) — the FakeProvider makes real cyclic MVS runs
trivial; every §2 row gets a test:**

1. `test_direct_cycle_is_a_hard_error` — A requires B, B requires A: `!res.ok`; `err`
   contains `require cycle detected` and the exact deterministic chain (byte-asserted —
   this is also the determinism proof); existing suite's diamond/chain tests stay green
   untouched (DAG regression).
2. `test_self_require_is_a_hard_error` — A@1.x requires A@1.y (and the 0.x/1.x bucket
   variant).
3. `test_one_way_cross_major_is_legal` — A@v2 requires A(v1), no back edge: `res.ok`,
   both nodes in the list (the migration-shim pattern must keep working).
4. `test_mutual_cross_major_is_a_hard_error` — A@v1 ↔ A@v2.
5. `test_dangling_edge_is_a_leaf_not_a_cycle` — direct `findRequireCycle()` unit call on
   a hand-built list whose edge targets a missing ModuleId: returns `""`, no crash.
6. `test_cycle_through_dev_edge_is_a_hard_error` — the §1 as-walked rule, pinned.

**Lock-path coverage — extend `tests/run_trident_vcs_app.sh`** (it already owns the
offline lifecycle on a scratch copy; the lock exists mid-script): after the green
build/why steps, hand-edit the scratch `trident.lock` to give its one module a
self-referencing `requires = ["github.com/x/json@…"]`, then assert `trident build` fails,
stderr contains `require cycle` **and** the fix text `` run `trident lock` ``, and that a
subsequent `trident lock` + rebuild goes green again (the named fix actually fixes).
No new bare-repo fixture: FakeProvider covers multi-module cycles at the MVS layer; the
lock check shares one code path regardless of cycle length, so the self-cycle suffices
there. (Implementer's discretion: a separate `run_trident_lock_cycle.sh` + ctest target
if the append reads noisy — same assertions either way.)

---

## 5. Acceptance gate (G-CYC1)

```
cmake --build build                          # clean under -Wall -Wextra -Wpedantic
./build/mvstests                             # 39 prior checks + the §4 additions, 0 failures
ctest --test-dir build -R "trident"          # manifest_errors, vcs_app (incl. new tamper step), vendor — green
grep -rnE 'parseManifest|Dependency|trident\.toml|project\.mf' src/
                                             # exactly the two documented benign matches, nothing new
ctest --test-dir build                       # full suite green (196 targets as of 2026-07-17)
```

Landing protocol (standing workflow): log the landing in §8 here *and* append a
completion note to the tracker's §10 (closing its P-B3 row); this doc then moves to
`designs/complete/` with references fixed; update agent memory; commit and sync master.
The *tracker itself does not move* — its X-1 rule holds it in `designs/` until all four
D-items land or cancel.

---

## 6. Non-goals (scope fence — each is a STOP §0.3 item if reached for)

- **Local-kind deps.** §4.5's own text scopes the ruling to the *external* package graph.
  `loadDepsRec`'s `visited`-set behavior for local sibling deps is unchanged; if D-A
  (workspaces) later builds a member graph, *that* design decides member-cycle policy —
  with this ruling as precedent but not as mandate.
- **Namespace-level cycles.** The language's namespace layer is whole-program and
  cycle-immune by construction (Doc 3 §4.5's first bullet); nothing here touches the
  compiler side of that line — or any other (§0.4).
- **Escape hatches, warnings-only modes, config knobs.** The ruling is a hard error,
  period. If real-world evidence (post-ecosystem) ever justifies tolerance, that is a new
  owner ruling that *relaxes* — the cheap direction — not a flag smuggled in now.
- **`why`/`audit` cycle surfacing** — superseded (§0.2).

---

## 7. Suspected hurdles

- **H-1 (chain determinism).** DFS start order must be the entries' sorted order and edge
  order the recorded order, or the byte-asserted test flakes across STL implementations.
  Both orders are already deterministic upstream (map-sorted flatten; manifest-order
  `requires_`) — just don't introduce an unordered container in the check.
- **H-2 (placement in `resolveVcsDeps`).** The lock-path check must sit after the
  `usedLock` reconstruction and before the materialize loop — materializing (which can
  hit the network on a cold store) for a graph about to be rejected would violate the
  "reject before fetch" cleanliness this design promises.
- **H-3 (message stability).** Both messages become load-bearing the moment the tests
  byte-assert them; treat their wording as frozen-after-landing (same discipline as the
  existing `— run \`trident lock\`` family).
- **H-4 (don't over-read the ruling).** The temptation will be to "finish the job" with
  audit warnings or why annotations for *near*-cycles. There is no such feature. Ship
  the error, only the error.

---

## 8. Implementation log (append-only)

*(empty — filled by the implementing session; do not start before reading §0.3/§0.4 and
the tracker's §9.)*
