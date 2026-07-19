# Tech Design — Composition Corpus & Quality Gates

**Status:** LANDED 2026-07-14 — M0 (harness) + a curated M1+M2 seed matrix (all four
clusters, not the full ~90-120-file combinatoric estimate; see the implementation log
below for scope and rationale). P-1/P-3 (stop-the-line marker, footguns.md) were already
active from the 2026-07-13 policy commit.
**Owner ask:** stop the widening gap between the rate consumer tracks (Atlantis, Sonar,
Harpoon) file compiler bugs and the rate they get fixed.

**Implementation log (2026-07-14).** Landed: `tests/corpus/composition/{fnvalues,
aggregates,names,generics}/{green,red}` (31 files: 1 red + 30 green — see below for why
red shrank from an initial 8), `tests/run_red_corpus.sh` (inverted-exit red-lane runner —
each red `.lev` carries a sibling `.engines` file naming which of run/ir/cpp/llvm it
currently reproduces on, since several bugs are backend-specific and checking an
already-correct engine would false-flag), and one delimited `add_test` block in
`CMakeLists.txt` (`composition_treewalk`/`_ir`/`_cpp` ungated, `composition_llvm`/`_red`
inside the `LLVM_FOUND` guard since the red runner needs `LANG_LVRT_SOURCES` for any
`llvm`-listed repro). All 5 `ctest -R composition` lanes pass. Scope: a verified, curated
slice rather than the full matrix — every file was individually run against a build
before being placed in green/ or red/; the §3 matrices' full cross-product is left for the
M3 steady-state growth this doc already describes.

**First pass (against this branch's own tip, before syncing master).** Every candidate
repro was re-run rather than assumed from the bug write-ups, and four things did not match
`known_bugs_1/2.md`'s "still open" claim:

1. **New bug, found here and FIXED 2026-07-14 (owner-authorized `src/` change):** a bare
   `class`-typed field with no initializer (`class Outer { Inner i; }`) did not
   auto-construct, contradicting info.md §3's guarantee ("every type has a nullary
   construction path"). Reading through such a field silently yielded an empty/wrong value
   on `--run`/`--ir`/LLVM (exit 0, no diagnostic) and **segfaulted on emit-C++** (exit 139).
   info.md:1614-1616's earlier fix was scoped explicitly to *value-struct*-typed fields
   auto-constructing recursively; reference-`class`-typed fields were left out of it. Not
   seen earlier in practice because every existing reference-typed field in `sonar/`/
   `packages/atlantis/` is assigned in its constructor before first read. **Fix** (one
   source of truth, engines share it so field defaults stay byte-identical):
   `src/Symbols.hpp` gains `bareFieldAutoConstructs()` / `onConstructionCycle()` — a bare
   constructable-class field auto-constructs (value struct or reference class), *unless* its
   type sits on a construction cycle (`class Node { Node next; }`, mutual `A{B b} B{A a}`),
   which has no finite default and keeps the void/None default as before; an optional
   back-edge (`Node? next`) is excluded from the cycle (defaults to `None`), which is the
   language's intended cycle-breaker. `src/Eval.cpp` (oracle `initFields`) and
   `src/Lower.cpp` (`synthesizeInit`, feeding IR/emit-C++/LLVM) both consult the shared
   predicate; a reference field lowers to the exact `NewObject`(+nullary-ctor)+`RawSet`
   sequence a `Field f = Field();` initializer already used, so its ARC ownership rides the
   proven path (verified leak-free: 10k owners construct+reclaim at flat `live-at-exit`).
   Regression floors: `aggregates/green/bare_class_field_autoconstruct.lev` and
   `bare_class_field_selfref_default.lev`. The three in-corpus workarounds (explicit field
   initializers added to dodge this) were reverted to bare fields in the same change.
2. Bugs #40 and #41 no longer reproduced against a freshly-rebuilt runtime, even before
   the master merge below — the false "still reproduces" reading during the very first
   check came from the build tree's checked-in `liblvrt.a`, stale from 2026-07-12 (the
   `lvrt` CMake target hadn't been rebuilt since); rebuilding it made both clean. This
   matched bug #41's own entry, which already noted "#40 and #49 ... no longer reproduce
   standalone" from its 2026-07-13 root-cause trace.
3. Bugs #47 and #52 also no longer reproduced on any engine.
4. Bug #43's fluent-chain repro (the second of its two repros) no longer reproduced; only
   the nested-call-argument repro (`Foo(1, Map())`) still did.

**Then, mid-task, `origin/master` was fetched and merged into this branch** and turned out
to carry a large concurrent fix wave (`bug.md #36/#39/#41/#43/#45/#47/#52/#51/#56/#58/#59/
#64/#65`, landed by another agent/the owner while this work was in progress) —
`known_bugs_1.md` dropped from ~17 open entries to just `#35` (deferred, P2, owner
ruling), and `known_bugs_2.md` still holds only `#54`. Re-running `composition_red` after
the merge (exactly the workflow this doc's red lane exists to catch) reported **5 of the
7 remaining red repros as UNEXPECTED PASS** — `#39`, `#45`, `#51`, `#56`, and the `#43`
nested-call repro all now match `.expected` on every listed engine. Each was promoted to
green/ in place (comment rewritten to "regression floor for bug #N, fixed <date>", `.lev`
content otherwise untouched, `.engines` deleted) per this doc's own fix
definition-of-done. A `#36` regression floor (`aggregates/green/
namespaced_class_is_narrow.lev`) was added fresh, since the fix commit's own message
("emit-C++'s `is` test falls back to in.sym for a namespaced class") identified the
missing trigger axis (namespacing) that this track's pre-merge reduction attempts had not
tried.

**Net result: cluster red-lane counts are aggregates=0, names=0, generics=1 (`#54` only,
confirmed still open — its own fix is tracked as a design request, `designs/requests/
request-generic-overload-monomorphization.md`, not yet implemented), fnvalues=0.** This is
the corpus working as intended, not a gap: a red-heavy seed matrix authored against one
moment's bug registry, run again after a few hours of concurrent fixing, correctly
flagged every fix and forced the promotion the design mandates rather than silently
drifting out of sync with reality.

---

## 0. The problem this solves

As of 2026-07-13, bug.md holds ~23 open bugs; 21 of them (#36–#56) were filed in the last
two days by the Atlantis and Sonar tracks. The fix rate is one or two per day. Almost none
of these are single-feature failures — the features' own landing corpora pass on four
engines. They are **pairwise interactions** (lambda × field, struct × Map, qualified name ×
IR lowering, generic × overload) that no feature track's corpus covers, discovered
downstream by application tracks under their own deadlines, worked around per the bug
workflow, and left in the queue. Three consequences:

1. The backlog compounds — nothing gates on an open bug.
2. Workarounds fossilize inside the flagship libraries (explicit `this.` on every handler,
   parallel-array indexes instead of `Map<K,Struct>`, hand-written `toJson`, ten rules
   instead of five) with no registry to un-workaround when the bug is fixed.
3. Each new track re-pays discovery: the same footgun bites twice because the knowledge
   lives in one closed track's results file.

Open bugs cluster into four seams, which is what makes a systematic corpus tractable:

| cluster | seam | open bugs |
|---|---|---|
| **A** | function values in non-trivial positions | #39 #42 #44 #51 #52 #53 #55 #56 |
| **B** | aggregate (struct/enum) layout inside containers, native backends | #36 #38 #40 #41 #49 |
| **C** | qualified-name resolution & lowering across namespace boundaries | #37 #45 #46 #48 |
| **D** | generics × overload resolution / inference | #43 #54 (was #34) |

## 1. The three policies (all active now)

**P-1 — P0 stop-the-line for silent state corruption.** bug.md gains marker **P0.3**:
silent state corruption on an actively-maintained engine (memory corruption, staleness
after unrelated activity, a silently dropped operation — failures that surface away from
the causing site). #41, #49, #53 are reclassified P0 under it. Semantics for agents (who
never touch `src/`): while a P0 is open, **do not architect new consumer-track code on the
affected construct** — design around it from the start and say so in the track doc; and
surface every open P0 in each new track's kickoff summary. Semantics for the owner: P0s
are the fix queue's head, ahead of feature asks.

**P-2 — the composition corpus (this doc's build plan, §2–§4).** A compiler-owned,
ctest-wired interaction test suite generated from the cluster matrix, with a red/green
lane discipline so open-bug repros live *in the corpus* rather than only in bug.md prose.

**P-3 — the footgun & workaround-debt registry.** `docs/footguns.md` (created with this
doc): one row per known footgun — construct, tracking bug, sanctioned workaround, and the
**debt sites** carrying that workaround. Every track reads it at kickoff (cheaper than
re-discovering #53 at segfault time) and appends a row for each new finding alongside the
bug.md entry. Fixing bug #N ends with grepping the registry for #N and un-workarounding
the listed debt sites — reverting a workaround for a now-fixed bug is not "reverting
validated work"; the registry row is what makes that distinction auditable.

**Retired 2026-07-19:** `docs/footguns.md` was merged into `known_bugs_1.md`/
`known_bugs_2.md` — each open bug's own entry now carries its Workaround and Debt sites
inline, so there is no separate registry file to read at kickoff or grep on fix. Track
kickoff now means reading the two `known_bugs_*.md` files directly.

## 2. Corpus design

**Location:** `tests/corpus/composition/{fnvalues,aggregates,names,generics}/` — one
subdirectory per cluster, ordinary `.lev` + `.expected` pairs under the existing
`tests/run_corpus.sh` discipline (oracle `--run`, IR `--ir`; emit-C++/LLVM legs via the
same pattern the existing native corpus tests use). No ELF lane ever — X64Gen is frozen
and no finding here may gate on it.

**Two lanes:**

- **green/** — must pass on every active engine; the regression floor. Pins the *working
  neighbors* of each broken cell (e.g. lambda-in-local direct-call, `Map<string,int>`
  field at 3+ entries) so a fix for the broken cell can't silently break the adjacent one.
- **red/** — one minimal repro per open bug, `.expected` pinning the **correct** behavior
  (so red tests currently fail). Run by a new `tests/run_red_corpus.sh` with inverted
  exit semantics: exits 0 while red tests still fail, prints the failing set as a status
  report, and **exits 1 when a red test passes** — a fixed bug demands promotion to
  green/ in the same commit. Each red file's header comment cites its bug number.

**Fix definition-of-done** (recorded here, enforced by the red runner): a bug.md #N fix
lands together with (a) its red→green promotion, (b) removal of its bug.md entry (existing
convention, which since 2026-07-19 also removes that entry's inline Workaround/Debt-sites
note — see the P-3 retirement above), and (c, pre-2026-07-19) the footguns.md debt sweep
for #N.

## 3. The matrices

Each cluster is a small cross-product; each cell is one tiny file. Cells already covered
by an open bug go to red/; the rest to green/. Estimated total ≈ 90–120 files.

**A — function values.** {lambda literal, named-fn reference `NS::fn`, method reference
`C::m`, bound `obj.method`} × storage {local, class field, `Array` element, `Map` value,
parameter, return value} × invocation {direct, dot-call on field, `arr[i]()`, invoked from
a different call frame than creation} × capture {none, local, `this` implicit, `this`
explicit}. Not exhaustive (≈ 400 raw cells) — take the ~40 cells nearest the failing ones
plus one representative per untested axis value.

**B — aggregates in containers.** element {scalar-only struct, struct+enum field,
struct+string field, enum, class} × container {`Array`, `Map` value, class-field `Map`,
class-field `Array`, nested} × stressor {3+ entries, read-back+compare, unrelated heap
activity after populate, method-return boundary} × the four active engines. This cluster
is *why* the corpus exists — every one of its bugs is invisible on oracle/IR.

**C — names.** namespace shape {nested-brace, qualified-decl (red: #37), same-file
reopening, cross-file reopening} × access {fully-qualified 1/2/3 hops, `uses`+bare,
sibling-relative} × position {call, type annotation, generic type argument, ctor,
assignment target}.

**D — generics × overloads.** overloaded free-fn called from a generic body per
instantiation (red: #54); ctor type-arg inference through nested call / fluent chain
(red: #43); lambda-vs-string applicability ordering (green: pins the #34 fix).

## 4. Milestones & timeline

- **M0 — harness** (2026-07-14): directory skeleton, `run_red_corpus.sh`, CMake
  `add_test` entries (`composition_{treewalk,ir}`, `composition_red`, native legs).
- **M1 — clusters A + B** (2026-07-14): all red repros for #38–#56 in scope, green
  neighbors pinned. **The consumer-track gate lifts here:** no *new* Atlantis/Sonar/
  Harpoon-implementation milestone starts before M1 lands (in-flight milestones finish;
  bug-fixing, docs, and design work are exempt).
- **M2 — clusters C + D** (2026-07-15): red repros for #37/#43/#45/#46/#48/#54,
  qualified-name green matrix.
- **M3 — steady state** (from M2 on): every new bug.md entry lands with its red-lane
  repro in the same commit; the matrices grow a row/column when a new axis value ships
  (new container type, new callable form).

**Ownership (disjoint-files rule):** this track owns `tests/corpus/composition/**`,
`tests/run_red_corpus.sh`, one delimited `add_test` block in `CMakeLists.txt`,
`docs/footguns.md` (retired 2026-07-19 — see the P-3 note above), and bug.md's
standings/marker text. It does not touch `src/`, `packages/`, or any other track's corpus.

## 5. Non-goals

- Not a test *framework* — plain corpus files; Harpoon (techdesign-unit-test-library.md,
  unimplemented) is not a dependency and this corpus must not wait for it.
- Not exhaustive combinatorics — cells are chosen by adjacency to real failures.
- Not an ELF/X64Gen surface, per the freeze.
- Not a bug-fixing track — repros and gates only; fixes remain the owner's queue,
  now ordered by the P0 rule.
