# Deferral Resolution — `const` System Extensions

**Status: OQ2 + OQ3/OQ4 landed (2026-07-16); OQ1 designed, not yet implemented.**
**Date:** 2026-07-07 (design); 2026-07-16 (OQ2/M-doc landed — see §8).
**Resolves / tracks:** the four open questions logged at the tail of the landed
`const` feature — [`designs/complete/const.md` §9](designs/complete/const.md)
(lines 289–303) plus the §4 interface note (lines 181–184). `const` itself
**shipped** (KwConst in the keyword table [Token.cpp:110](src/Token.cpp#L110),
`isConst` on the decl/param nodes [Ast.hpp:96](src/Ast.hpp#L96) /
[Ast.hpp:290](src/Ast.hpp#L290), the write-window error at
[Checker.cpp:876](src/Checker.cpp#L876)) — so these are deferrals *from a landed
feature*, not from a proposal. They must not rot as untracked §9 footnotes.
**Depends on:** the flow-narrowing engine (`narrow_` / `invalidatePath`,
[Checker.cpp:319](src/Checker.cpp#L319)) for OQ1; the sectional access-modifier
parser ([Parser.cpp:1009-1018](src/Parser.cpp#L1009)) for OQ2. No runtime, IR,
or backend surface (see §5 — the `const`-is-front-end-only invariant is load
bearing).
**Owns (if built):** for OQ1/OQ2 only — a `Parser.cpp` decl/section region and a
`Checker.cpp` flow region; **zero** AST-enum, IR, Eval, Lower, or backend edits.
OQ3/OQ4 own nothing: they are decisions, documented here, not code.

---

## 1. Summary — the four deferrals

The `const` design closed with one rule ("`const` scopes a slot's write view to
its initialization window", const.md §1) and deliberately shed four extensions
into §9 to keep v1 to that single rule. Two are **resolvable** (mechanical,
localized, front-end-only); two are **deliberate design choices** (documented
here as tracked non-work, with the criteria under which they should be
revisited). A fifth §9 item — naming — is a *resolved decision* ("Keeping
`const`", const.md §9.5), not a deferral, and needs nothing further.

| OQ | Item | Source | Class | Resolution |
|----|------|--------|-------|------------|
| 1 | **Definite single assignment** for `const` locals declared without an initializer (`const int x; if (c) x = 1; else x = 2;`) | const.md §9.1 (289–293), §3.1 (125–130) | **Resolvable** — flow analysis | §2 |
| 2 | **Sectional `const:`** — a section label declaring a run of members/globals `const`, mirroring `public:`/`private:` | const.md §9.2 (294–295) | **Resolvable** — pure parser sugar | §3 |
| 3 | **Interface `const` requirements** — allowing `const` in an interface's required members | const.md §9.3 (296) + §4 (181–184) | **Deliberate choice** — get-view contracts are the tool | §4 |
| 4 | **Deep-freeze** — a transitive/gated `freeze` making a value deeply immutable | const.md §9.4 (297–299) | **Deliberate choice** — out of scope, likely forever | §4 |

The two resolvable items share a hard constraint that governs the whole doc:
**`const` is a pure front-end feature — after the checker it does not exist**
(const.md §7.5, "Eval / IR / Lower / all four backends — zero changes"). Any
proposed extension that would need a runtime bit, an IR op, or a backend change
is out of bounds *by definition* and is a STOP condition (§7). Both OQ1 and OQ2
respect this: they add checker/parser logic only.

---

## 2. OQ1 — Definite single assignment for `const` locals

### 2.1 The deferral, exactly

v1 makes `const int x;` (no initializer) a **compile error** (const.md §3.1,
enforced at [Checker.cpp:1445](src/Checker.cpp#L1445): `if (s->isConst &&
!s->init) → error`). The stated reason: a bare declaration auto-constructs
(info.md §3), and auto-constructing a const would freeze the default forever — a
provably-useless slot, so refusing it is correct *for the auto-construct reading*.

But there is a second, useful reading the error currently also forbids:

```
const int x;                 // no initializer — but NOT auto-constructed…
if (c) { x = 1; }
else   { x = 2; }            // …assigned exactly once on every path, then frozen
print(x);                    // read only after the window provably closed
```

const.md §9.1 names this precisely: *"the flow-narrowing engine could carry
it; deferred to keep v1 to one rule, one window."* The window today is
syntactic (the initializer). OQ1 makes the window **flow-sensitive**: a const
local's write view stays open until the binding is *definitely assigned on the
current path*, then closes.

### 2.2 Resolution — definite-assignment analysis riding the flow engine

The narrowing engine already models exactly the shape this needs. Reading the
current tree:

- Flow facts carry a `path`, a `thenType`, and an `elseType`, applied at branch
  entry ([Checker.cpp:312-315](src/Checker.cpp#L312)) and merged/dropped at
  branch join (`narrow_ = saved`, [Checker.cpp:478](src/Checker.cpp#L478)).
- Assignment **invalidates** a path's narrowing (`invalidatePath`,
  [Checker.cpp:319-323](src/Checker.cpp#L319)) — the same event OQ1 keys on.
- The env slot already records constness per binding
  ([Checker.cpp:1474](src/Checker.cpp#L1474):
  `env_.back()[s->name] = {declared, s->isConst}`).

Definite assignment (DA) is the dual of narrowing: instead of tracking *what
type* a path has, track *whether a slot has been written* on this path. Design:

1. **A per-scope "unassigned const" set.** When a `const` local is declared
   without an initializer, instead of erroring at
   [Checker.cpp:1445](src/Checker.cpp#L1445), record it as **write-open**:
   `constPending_.insert(path)`. Its type is the declared type (still required —
   `const x;` with neither type nor initializer stays an error; there is nothing
   to infer from). The auto-construct that §3.1 feared is **suppressed** for a
   write-open const: it is not default-constructed at the declaration; it holds
   no value until assigned. (This is the one real semantic addition — see
   problem #4.)
2. **First assignment on a path closes the write view** for that path: move the
   slot from `constPending_` to assigned. This assignment is *permitted* (it is
   the initialization the window exists for); every *subsequent* assignment on
   that path hits the existing const write-error at
   [Checker.cpp:876](src/Checker.cpp#L876) unchanged.
3. **Branch join is set intersection.** At an `if/else` join, a const is
   "definitely assigned after" iff it was assigned on **both** arms (reuse the
   `saved`/merge machinery at [Checker.cpp:478](src/Checker.cpp#L478); DA merges
   by intersecting the assigned sets, symmetric to how narrowing drops facts not
   present on both arms). An `if` with no `else` cannot definitely-assign
   (the fall-through arm assigns nothing).
4. **Read-before-assign is an error.** A read of a slot still in
   `constPending_` on the current path → `error("'x' is read before it is
   definitely assigned")`. This is the DA safety property and it is what makes
   suppressing auto-construct sound: no path can observe the un-constructed slot.
5. **Scope exit.** A const that reaches end of scope still write-open is
   permitted (it was simply never used — dead, but not wrong); no default
   freeze is forced.

The whole feature is: one `std::set<std::string> constPending_` beside `narrow_`,
touched at declaration, assignment, read, and branch-join — the four sites the
flow engine already visits. No new AST, no runtime, no IR (invariant §5 held).

### 2.3 Deliberately out of OQ1's scope

- **Loops.** A const assigned inside a `for`/`while` body is assigned *zero or
  more* times depending on the trip count — DA cannot prove single-assignment,
  and permitting it would reopen the window per iteration (a `var` in disguise).
  Rule: a const assignment lexically inside a loop body, targeting a const
  declared outside that body, stays an **error**. (Assigning a const declared
  *inside* the loop body is just the normal per-iteration fresh-binding case and
  is fine.) This keeps "one window" honest.
- **`try`/`catch`.** A const assigned in a `try` may have been assigned or not
  when the `catch` runs (the throw could precede the assignment) — the `catch`
  and post-`try` join treat the `try` arm as **not** definitely-assigning.
  Conservative, sound, and matches Java's DA treatment of `try`.
- **`match`.** Definitely-assigned iff assigned in every arm including the
  default; a non-exhaustive match without a catch-all does not definitely
  assign. Falls out of the same intersection rule as `if/else`.

---

## 3. OQ2 — Sectional `const:`

### 3.1 The deferral, exactly

const.md §9.2: *"access modifiers have a sectional form (§2 of info.md); a
`const` section for blocks of constants is plausible sugar. Deferred."* The
motivating shape:

```
class Config {
  const:
    string host = "localhost";     // each member in the section is const
    int    port = 8080;
    Array<string> flags = defaults();
  public:                          // section ends; normal members resume
    var int hits = 0;
}
```

### 3.2 Resolution — pure parser sugar, reusing the access-section mechanism

The parser already runs sequential section labels for access. Reading
[Parser.cpp:993-1018](src/Parser.cpp#L993): `parseClass` tracks a `section`
(`Access section = Access::Default;`), flips it on a `public:`/`private:` label
([Parser.cpp:1013](src/Parser.cpp#L1013)), and threads it into each member via
`parseClassMember(section)` ([Parser.cpp:1018](src/Parser.cpp#L1018)), where
`parseClassMemberInner(Access sectionAccess)` applies it
([Parser.cpp:907-910](src/Parser.cpp#L907)). OQ2 adds a **parallel, orthogonal**
section axis:

1. Track a second section flag beside `section`: `bool constSection = false;`.
2. Recognize `const :` at a member-list position (the same place `public:` is
   recognized) → set `constSection = true` until the next section label or class
   end. Because access sections and the const section are **orthogonal axes**
   (const.md §1's matrix: *slot* vs *view* are independent), a `const:` label
   sets constness *without* resetting access, and a `public:`/`private:` label
   sets access *without* clearing const — the two labels compose (see problem
   #5 for the interaction rule).
3. In `parseClassMemberInner`, `if (constSection) m->isConst = true;` beside the
   existing access application. That is the entire semantic effect — it sets the
   **same** `isConst` bit the per-member `const` modifier already sets
   ([Ast.hpp:290](src/Ast.hpp#L290)). No new bit, no new checker path: every
   downstream check (the write-window error at
   [Checker.cpp:876](src/Checker.cpp#L876), the ctor-window logic, the collision
   rule const.md §4) sees a normal const member and is untouched.

"One word never means two things" (info.md §11) is respected: `const:` is the
*same* `const`, in section position — exactly as `public:` is the same `public`.

### 3.3 Scope of OQ2

- **Class bodies** are the primary target (the §9.2 example). This is where the
  section machinery already exists and OQ2 is a ~20-line addition.
- **Namespace / file-level globals** (const.md shows `const Array<string> args =
  …` as a global) *could* take a `const:` section too, but the global decl path
  has **no** section mechanism today (it is a flat statement list, not the
  `parseClass` label loop). Extending it would be new machinery, not sugar over
  existing machinery, so it is **out of OQ2's initial scope** — flagged as a
  natural follow-on if the class-body form proves its worth. Keeping OQ2 to
  "sugar over the section loop that already exists" is what keeps it a one-day
  item rather than a parser project.

---

## 4. OQ3 & OQ4 — deliberate design choices (tracked non-work)

These two are **not** pending work. They are decisions the `const` design made
on purpose. Documented here so the choice is a tracked, reversible ruling with
explicit reconsideration criteria — not a silent gap someone re-opens by
accident.

### 4.1 OQ3 — Interface `const` requirements: **declined in favor of get-view contracts**

**The ruling (const.md §4, 181–184):** an interface may not require an
implementer's member to be `const`. Rationale, restated: `const` governs *when
the implementer's own backing slot may be written* — an **internal** property.
An interface's job is to constrain **surface** (what callers can do), and the
language already has the right tool for "callers get read-only access": require
a `get` view and no `set` view (const.md §6/§8; the bodyless-requirement
machinery at [Checker.cpp:822](src/Checker.cpp#L822)). Requiring `const` on the
*backing slot* would reach past the surface into the implementer's internals,
constraining how it stores state rather than what it exposes — the exact
const-contagion the design killed by making `const` "not a type" (const.md §2).

**Why this is right, not merely convenient:** it keeps the four-way separation
clean. An interface says "you must *offer* read-only access" (get-view); it does
**not** say "you must *implement* it with an immutable slot." An implementer is
free to back a get-only view with a `var` field it recomputes — and that is
legitimate. Coupling the requirement to `const` would forbid valid implementations.

**Reconsider if, and only if:** a concrete, recurring pattern emerges where a
get-view contract is provably insufficient — e.g. an interface that must
guarantee *value stability across calls* (the returned view never changes for
the object's lifetime), which a get-view alone cannot promise. If that lands,
the fix is **not** "allow `const` in requirements" (still contagious) but a
distinct **stability contract** on the view (a `get` marked as
snapshot-stable), designed as its own axis. Track that as a new request, not as
a reopening of OQ3.

### 4.2 OQ4 — Deep-freeze: **out of scope, likely permanently**

**The ruling (const.md §9.4):** no transitive/gated `freeze` making a value
deeply immutable. `const` is explicitly *not transitive* (const.md §2: `const
MyClass m` fixes the binding, not the object — C# `readonly` semantics).

**Why:** immutable *data* is already answered by the **value axis** — `struct`
value semantics and pure `Array`/`Map` (const.md §1 matrix; info.md §9, §11). A
`freeze` would be a **fourth mechanism duplicating the third**, and a gated
runtime freeze would reintroduce a runtime notion of immutability the whole
design avoided (const being front-end-only, §5). Two ways to make data immutable
is the "one word never means two things" violation in reverse — two mechanisms
for one concern.

**Reconsider if, and only if:** the value axis proves structurally unable to
express a real immutability need — e.g. a large object graph that must be shared
immutably across threads (info.md §14's concurrency story) where converting the
whole graph to value types is infeasible for performance. Even then the answer
is more likely "an immutable *reference* wrapper" designed against the
concurrency model, not a general deep-freeze. This is the closest thing in this
doc to a permanent "no"; treat any push to reopen it as a concurrency-design
question, not a `const` question.

---

## 5. Dependencies & the load-bearing invariant

- **OQ1** depends on the flow engine (`narrow_`, `invalidatePath`, the
  branch save/merge at [Checker.cpp:312-478](src/Checker.cpp#L312)). It adds a
  *sibling* analysis (definite assignment) to the same visitor, not a new
  engine. If the flow engine is mid-refactor, OQ1 waits for it to settle.
- **OQ2** depends only on the class-body section loop
  ([Parser.cpp:1009-1018](src/Parser.cpp#L1009)); no other subsystem.
- **OQ3/OQ4** depend on nothing — they are decisions.
- **The invariant (const.md §7.5):** `const` produces **no** runtime, IR, Eval,
  Lower, or backend artifact. OQ1 and OQ2 are designed to preserve this
  absolutely — both are checker/parser-only. The differential test (positive
  corpus identical under `--run`/`--ir`, const.md §8) is therefore the acceptance
  oracle for both: **if either extension changes any lowered output, the design
  is wrong** and must stop (§7). This is not merely a convention here — it is the
  property that lets `const` (and its extensions) be free of the const-contagion
  the design's whole thesis rejects.

---

## 6. Potential problems & recommended solutions

| # | Problem | Strategy |
|---|---------|----------|
| 1 | **OQ1 blurs "one window."** DA makes the write window flow-sensitive instead of syntactic; the feature's headline simplicity ("`const` = write view during init, never again") gets a footnote. | Frame DA as *the initialization window, precisely computed* — not a new rule. The window is still "init, then frozen"; DA only lets init span an `if/else` instead of a single expression. Document it as a §3.1 relaxation, keep the loop/try/match exclusions (§2.3) so the window can never *reopen*. If the exclusion list starts growing to chase cases, STOP — the simple rule is worth more than the coverage. |
| 2 | **OQ1 read-before-assign vs auto-construct.** Suppressing auto-construct for a write-open const (§2.2 step 1) is a genuine semantic change; a path that reads before assigning would observe an unconstructed slot. | The DA read-before-assign error (§2.2 step 4) makes that observation **unreachable** — it is a compile error, so no runtime path exists. This is exactly Java/C#'s definite-assignment safety net. The corpus must include every read-before-assign shape (straight-line, one-armed `if`, loop-guarded) as compile-reject cases. |
| 3 | **OQ1 interaction with narrowing on the same slot.** A `const string? h;` assigned in both arms, then narrowed by `if (h != None)` — DA and narrowing both key on the same path and the same `invalidatePath` event. | They compose without conflict *because* they key on the same event: assignment closes the DA window **and** (per const.md §4) is the last write, so narrowing never invalidates afterward. Order them: DA close first, then narrowing proceeds on a now-const path. Add a corpus case (const optional, assigned-both-arms, then narrowed) to pin it. |
| 4 | **OQ1 scope creep into full DA.** Once definite assignment exists, every "why not also…" (definite assignment for `var` before first read? unreachable-code? null-flow?) becomes tempting — a language-sized analysis hiding behind a small feature. | Hard scope fence: OQ1 is **only** "a const local declared without initializer, assigned exactly once on every straight-line/if-else path before first read." No loops, no `var`, no reachability. Anything beyond is a separate design with its own justification. This fence is a STOP condition (§7). |
| 5 | **OQ2 axis composition.** Access sections (`public:`) and a `const:` section are C++-style *sequential labels*; a naive implementation makes them mutually exclusive, so you cannot have a public **and** const member via sections. | Model them as **two orthogonal sticky flags**, not one section state (§3.2 step 2): `const:` sets the const flag and leaves access untouched; `public:` sets access and leaves the const flag untouched. A member declared after both labels is public+const. Provide an explicit "reset" only via an opposing need — and since there is no `var:`/`mutable:` label, a const section runs until class end or is simply not used; a member needing to be non-const inside a const section writes `var` explicitly on itself (per-member override wins, exactly as a per-member access modifier overrides a section). Document this override precedence in the reference. |
| 6 | **OQ2 tempting over-reach to globals.** The `const` global is the poster-child consumer (argv, prelude), so extending `const:` to file-level globals looks natural — but the global path has no section loop and would need new machinery. | Keep OQ2 to class bodies (§3.3). If globals want it, that is a *separate* small design ("section labels for the global decl list"), justified on its own. Do not smuggle a parser feature in under "sugar." |
| 7 | **OQ3 reopened as "just allow const in requirements."** A future contributor hits a value-stability need and reaches for the obvious-looking `const` requirement, reintroducing contagion. | This doc is the tracked ruling: the answer to value-stability is a **stability contract on the view**, not `const` on the slot (§4.1). Link this file from const.md §9.3 so the reasoning travels with the question. |
| 8 | **OQ4 reopened as a concurrency shortcut.** Deep-freeze looks like an easy answer to "share this graph across threads immutably." | §4.2 routes that to the concurrency design (immutable *reference* wrapper against info.md §14), not to `const`. Deep-freeze duplicating the value axis stays declined. |

---

## 7. Milestones, timeline & STOP conditions

These are **post-v1 polish, non-blocking.** `const` v1 shipped and covers the
95% case; nothing here gates other tracks. Sequencing is "build when a real
program hits the friction," not calendar-driven.

| M | Deliverable | Accept |
|---|-------------|--------|
| M-OQ2 | Sectional `const:` in class bodies: parser `constSection` flag + `isConst` application; positive corpus (const section members reject reassignment) + negative (per-member `var` override inside a const section stays mutable); reference §4.x note | build clean; `--run`/`--ir` diff **zero** vs a hand-written per-member `const` equivalent (proves pure sugar); const-section members hit the existing write-error |
| M-OQ1 | Definite single assignment: `constPending_` set; suppress the §3.1 no-init error for typed const locals; assign/read/join handling; loop/try/match exclusions; read-before-assign errors | build clean; positive corpus (`const int x; if/else assign; read`) runs identically to the initializer form; negative corpus (second assign, read-before-assign, loop assign, one-armed if) all compile-reject; **`--ir` unchanged** on all positive cases |
| M-doc | OQ3/OQ4 rulings folded back: const.md §9.3/§9.4 gain a one-line pointer to §4 here; this file records the decisions as tracked | reviewer confirms the reasoning + reconsideration criteria are captured; no code |

Rough effort if greenlit: **OQ2 ≈ 1 day** (pure parser sugar over existing
machinery), **OQ1 ≈ 2–3 days** (a real flow analysis with a careful
exclusion-list and a thorough compile-reject corpus). OQ3/OQ4 ≈ 0 (this doc).
Suggested order: **OQ2 first** (cheap, self-contained, immediately useful for
config-heavy classes), OQ1 when a program actually wants the `if/else`-init
pattern.

### STOP conditions

- **Either OQ1 or OQ2 changes any lowered output** (`--run`/`--ir` diff on the
  positive corpus) — the `const`-is-front-end-only invariant (§5) is violated;
  the extension is mis-designed, stop and re-derive rather than adding a runtime
  bit.
- **OQ1's exclusion list grows past {loops, try, match}** to chase more cases,
  or DA is asked to cover `var`, reachability, or null-flow — that is a separate,
  language-sized analysis; stop and escalate rather than expanding OQ1's fence
  (problem #4).
- **OQ2 is pushed to file-level globals** inside the same change — that needs new
  section machinery, not sugar; split it into its own design (problem #6).
- **Any temptation to allow `const` in interface requirements (OQ3)** or to add a
  deep-freeze (OQ4) — both reintroduce contagion / mechanism-duplication the
  design rejected; route to the stability-contract / concurrency designs
  respectively (§4), do not reopen here.
- **Any AST-enum, IR, Eval, Lower, or backend edit** appears necessary — none is,
  by construction; its necessity means the design took a wrong turn.

## 8. Implementation log

**2026-07-16 — M-OQ2 (sectional `const:`) + M-doc (OQ3/OQ4 rulings) landed.**
Pure front-end sugar, zero AST-enum/IR/Eval/Lower/backend edits (§5 invariant held).

- **Parser** (`src/Parser.cpp`): `parseClass`'s member loop gained a `bool constSection`
  sticky flag orthogonal to the existing `Access section`, set by a `const :` label
  recognized at the same member-list position as `public:`/`private:` (an access label
  no longer clears it and vice-versa — orthogonal axes, §3.2 step 2 / problem #5).
  `parseClassMember`/`parseClassMemberInner` gained a defaulted `bool sectionConst = false`
  parameter (the two splice/attr callers are unaffected); the inner seeds `isConst` from
  it and the modifier loop now accepts `var` as a per-member override that clears `isConst`
  (`var` at member position previously failed to parse — it now means "explicitly mutable,
  overriding a `const:` section"). Header signatures updated in `src/Parser.hpp`.
- **Differential acceptance (the §7 STOP oracle):** a class using `const:` lowers
  **byte-identically** under `--run` and `--ir` to the hand-written per-member `const`
  equivalent — verified (`diff` empty). Pure sugar confirmed; the invariant holds.
- **Tests:** positive corpus `tests/corpus/const_section.lev` (+`.expected`, auto-globbed
  by `corpus_treewalk`/`corpus_ir`) exercises the section, the `var` override, and
  `const:`-sticky-across-`public:`; five `test_checker.cpp` cases pin the compile-rejects
  (section member reassignment, sticky-across-access reassignment) and the clean cases
  (`var` override, bare `var` field, per-member `const` in a normal class). Full suite
  green (`checkertests` 304/0, top-level corpus + composition lanes 100%).
- **M-doc:** `const.md` §9.1–§9.4 now point here — §9.2 marked **Implemented**, §9.3/§9.4
  record the declined-ruling reconsideration criteria (§4.1/§4.2).

**Remaining: M-OQ1 (definite single assignment).** Not started. It is the substantive
milestone — a checker-only definite-assignment analysis (§2.2) with the {loop, try, match}
exclusion fence (§2.3, a STOP condition per problem #4) and a thorough read-before-assign /
double-assign compile-reject corpus. The lowering precondition is verified: a **non-const**
no-init local assigned on both `if/else` arms already lowers correctly on `--run`/`--ir`,
so OQ1 adds no backend surface (const is erased after the checker). Sequenced "when a
program actually wants the `if/else`-init pattern" (§7); non-blocking.
