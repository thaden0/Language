# Tracker: Metaprogramming deferrals — the consolidated tail

**Status:** tracker — re-grounded 2026-07-17 (see the dated section below; the 2026-07-06
baseline text is kept as the historical record and is superseded where the two conflict).
Promoted out of deferral 2026-07-17 (was `deferal-metaprogramming-tail.md`).
**No implementation rides this doc.**
**Canonical resolution design:** `docs/techdesign-metaprog-phase4.md` — that doc already
*designs* nearly everything listed here; this doc exists so the deferral set is visible
from `designs/` alongside the track index, cross-checked against the current tree, and so
the two items phase4 does **not** own (the named-arguments prerequisite and the residual
platform-predicate gap) have a recorded home.

**What this doc is not:** a re-derivation. Where phase4 §N designed an item, that section
is the answer and this doc only points at it. The value added here is (1) one table over
*every* metaprogramming deferral with its source-of-punt reference and a status against
today's tree, (2) the named-arguments dependency called out as a blocking language
prerequisite rather than a metaprogramming work item, (3) one genuinely untracked residual
(item Q below) surfaced, and (4) the drift/rot risks between phase4's grounding commit and
the current tree, recorded before they bite.

**Grounding:** claims below were checked against the current tree (master, 2026-07-06):
`src/Parser.cpp:1108-1112` (the `rewrites` Layer-D stub still errors "Phase 4" — Layer D
is unimplemented), `src/main.cpp:196-197` (`--expand` / `--ast-after-rules` still one
alias), `src/Ast.hpp:319` (`ruleRewrites` flag parked, unused), no `provenanceId` in
`Ast.hpp`. Phases 1–3 are feature-complete (`docs/techdesign-metaprog-phase3.md` header,
suite green at its third sync).

**Re-grounding 2026-07-17** (verified against master; supersedes the table/status text below
where they conflict):

- **Phase 4 SHIPPED.** info.md §0 records "Metaprogramming Phases 1–4 + procedural macros
  (F4)" landed. Verified in-tree: Layer-D `rewrites body of` parses for real
  (`Parser.cpp:1350-1359`, `:1901` — the "Phase 4" error stub this doc cites at `:1108-1112`
  is gone) → **item A LANDED**; M33 confluence/conflict detection exists (`Rules.cpp:2422`,
  the "rule X and rule Y conflict at \<anchor\>" shape) → **item B LANDED**; `reentrant`
  parses (`Parser.cpp:1364`) → **item C LANDED**; `--expand` is now the source-shaped
  re-emit with `--ast-after-rules` split off as its own mode (`main.cpp:247-251`) →
  **item E LANDED**. Beyond this doc's inventory, F4 procedural macros
  (`macro name(string) comptime` + `meta::parseExpr`/`parseStmts`, `Rules.cpp:39-49`) also landed.
- **Item M is UNBLOCKED — the §4 prerequisite was built.** Named arguments + default
  parameter values landed as a language feature (info.md §0/§2;
  `designs/complete/techdesign-named-arguments.md`, whose own §log updates this tracker by
  name). What remains of M is only the attribute-side inheritance (phase4 §9.5's "nothing
  attribute-specific to design here") — verify whether it rode the named-args landing;
  if not, it is now a trivial follow-on, **not** blocked.
- **Item Q is half-unblocked.** The Track 03 statics STOP resolved long ago — `enum` landed
  in full (closed int-carried value type, all four engines), so the `Platform::current`-as-enum
  shape is buildable. The predicate itself is still **absent** (verified: no
  `Platform::current`/target-const anywhere in `src/` or `docs/reference.md`), so Q's §8.2
  one-pager (predicate shape + target-not-host semantics per the portable pivot) is now
  **the tracker's one open design item**. New demand signal since 2026-07-06: the
  LA-15/LA-16-shaped asks filed from Sonar T11 and Atlantis Track 03 (field-type holes,
  name-string reification — `designs/requests/request-metaprog-attr-values.md`,
  `request-metaprog-splices.md`) mean the pull-based tail items (H/I and friends) now have
  real pullers; and the Harpoon compiler work (2026-07-15) already extended the rule engine
  adjacently (`$C.name`/`$m.name` reification, `$for`-bound-method selector splicing,
  type-position `$C` hygiene — reference.md §5.2/§6.9). Re-ground H–L against that before
  building any of them.
- **Demand gates that stand:** D (no `provenanceId` in `Ast.hpp` — v1 range-based provenance
  remains the accepted answer) and F (no measured compile-time regression logged). G/K/N/O/P
  unverified individually; K's `Symbol::homeNs` confirmed absent.
- **Path currency:** the `docs/techdesign-metaprog-*` citations below now live at
  `designs/complete/techdesign-metaprog-*` (the 2026-07-07 designs reorg); `docs/` holds only
  reference material.

---

## 0. Contents

1. Summary
2. The consolidated deferral table (items A–Q)
3. Per-item notes — why deferred, where the resolution lives
4. The named-arguments prerequisite (item M's blocker — a language feature, not a meta one)
5. Item Q — platform-conditional `uses`, the residual phase4 does not track
6. Potential problems
7. Recommended solutions and sequencing
8. What still needs new design (short answer: two things)

---

## 1. Summary

The metaprogramming arc (proposal → master design → Phases 1–3 built → Phase 4 designed)
deliberately punted a set of items at each stage. `docs/techdesign-metaprog-phase4.md` §1
already inventories sixteen of them (A–P: A–G the proposal's Phase 4 proper, H–P the
"deferred tail" of smaller in-phase punts) and §§2–9 there design each one — Phase 4 is
itself a deferral-tracking doc that resolved most deferrals *on paper*. Nothing in A–P
needs redesign.

Three classifications used below:

- **(a) designed & shippable** — the resolution is written in phase4 §N; building it is
  scheduling, not design. Some carry an explicit *demand gate* (build only when a stated
  trigger fires) — that gate is part of the design, not a gap.
- **(b) MUST-WAIT** — blocked on a prerequisite outside metaprogramming. Two items:
  attribute named arguments (M — waits on the *language* growing named args) and the
  platform-predicate half of platform-conditional `uses` (Q — waits on the Track 03
  statics/enum ruling).
- **(c) intentional v1 scope** — the shipped v1 behavior *is* the accepted answer
  (range-based provenance, D); an upgrade design exists but the v1 position is not a debt,
  it is a decision (master §14 dev. 8).

---

## 2. The consolidated deferral table

"Source of punt" is where the deferral was recorded (doc:line against the current tree);
"resolution" is where the design lives or what it waits on.

| # | Item | Source of punt | Class | Status | Resolution |
|---|---|---|---|---|---|
| A | Body-replacing rules (`rewrites body of m`, `replace`, `$body`) | proposal §4.4/§10.5; parser stub `Parser.cpp:1108-1112` | (a) | **LANDED** (verified 2026-07-17: `rewrites` parses, stub gone) | phase4 §2; M30–M32/M35 |
| B | Confluence / rule-conflict detection at a shared anchor | proposal §5.4 | (a) | **LANDED** (verified 2026-07-17: M33 conflict shape in `Rules.cpp`) | phase4 §3; M33 |
| C | `reentrant` — gated rule re-triggering | proposal §8/§13-Q3; master §15 | (a) | **LANDED** (verified 2026-07-17: `reentrant` parses) | phase4 §4; M34 |
| D | Exact per-clone provenance (`provenanceId`) | master §8.2 (`techdesign-metaprogramming.md:625-643`), §14 dev. 8 (`:900-902`); Phase-2 landed-note (`:818-825`) | (c) | v1 range-based is the accepted answer; upgrade designed, **demand-gated** | phase4 §5; trigger = a real ambiguous diagnostic |
| E | `--expand` as readable source (pretty-printer) | master §14 dev. 7 (`:897-899`); `main.cpp:196-197` still one alias | (a) | **LANDED** (verified 2026-07-17: `--expand`/`--ast-after-rules` demuxed) | phase4 §6 (round-trip acceptance) |
| F | Incremental per-file rule-output caching | proposal §3.6/§13.7; master §15 (`:915`) | (a) | designed, **demand-gated** | phase4 §7; trigger = measured compile-time regression |
| G | Pass-1 comptime-root pre-checking | master §7 ("good enough for v1") | (a) | designed, unbuilt; pure diagnostic polish | phase4 §8 |
| H | `meta.*` structured `Type` | P3 §3/§4 | (a) | designed, pull-based | phase4 §9.1; add on a real `where` driver |
| I | Attribute-value reflection (`meta::Attr`) | P3 §3, §5 landed-note | (a) | designed, pull-based | phase4 §9.1; values already exist in `attrValues_` |
| J | Statement-position `$for` | P3 §5 | (a) | designed, pull-based | phase4 §9.2; bounded — no `$if`/`$while` follows |
| K | Def-site "true home" (`Symbol::homeNs`) | P3 §10 landed-note | (a) | designed, pull-based; provenance-cosmetic only | phase4 §9.3 |
| L | Class-wide marker search | P3 §15 Q2 | (a) | designed, pull-based | phase4 §9.4 |
| M | Attribute named arguments | master §3.2 (`techdesign-metaprogramming.md:169-171`), §14 dev. 4 (`:886-889`) | **(b)→(a)** | **UNBLOCKED 2026-07-17** — named args landed (`designs/complete/techdesign-named-arguments.md`); attribute-side inheritance is the only remainder | phase4 §9.5; prerequisite discharged — see §4 note |
| N | Single-resolve fast path | master §14 dev. 1 (`:876-880`) | (a) | resolved-by-reference: folded into F, no standalone build | phase4 §9.6 |
| O | Macro single-splice auto-hoisting | P3 §15 (M22 is the honest v1) | (a) | designed-by-association: rides A's statement-lifting | phase4 §9.7 |
| P | Golden-`--expand` determinism fixture | P3 §14 landed-note | (a) | subsumed by E's round-trip test; standalone fallback only if E slips | phase4 §9.8 |
| Q | Platform-conditional `uses` — the **platform predicate** half | master §14 dev. 6 (`:893-896`), §16 decision 4 (`:928-929`); proposal §4.3 example (`proposal-metaprogramming.md:351-355`) | **(b)→(a)** | mechanism **landed** (P3 §9, M15 deleted); statics blocker **cleared 2026-07-17** (`enum` shipped) — the predicate itself is still **unbuilt** | §5 below; the §8.2 one-pager is the open design item |

Reading the table: fifteen of seventeen rows are already resolved on paper by phase4
(twelve buildable/pull-based, two demand-gated by explicit written triggers, one
intentional-v1 with a designed upgrade). The two (b) rows are the only ones that can
actually *stall* — both wait on non-metaprogramming prerequisites, and neither
prerequisite has an owning design today.

---

## 3. Per-item notes — why deferred, where the resolution lives

Kept deliberately short; phase4 §1's "why deferred originally" column is authoritative for
the original rationale, and phase4 §§2–9 are authoritative for the designs. Only the
tracker-relevant delta per item:

- **A–C (Layer D arc).** One interlocked arc — confluence detection (B) only *needs* to
  exist once two rewriters can target one body (A), and `reentrant` (C) is the gated
  escape from the one-direction pipeline. Phase4 §11 sequences them as commits 1–3. The
  parser stub confirms zero drift risk: nothing partially built, `rewrites` still a clean
  "Phase 4" error at `Parser.cpp:1112`.
- **D (provenance).** Classify as (c), not debt: master dev. 8 *chose* range-based
  3-origin attribution to keep provenance out of every `sink_.error` call site in
  Resolver/Checker. The render-time containment check (master §8.2) already names up to 3
  candidate origins. The upgrade (phase4 §5) has a written trigger: *a specific confusing
  error report*, not a schedule. See §6 problem 2 for how that trigger stays observable.
- **E (source-shaped `--expand`).** The strongest deferred item by acceptance value — the
  round-trip test (expand output compiles and runs byte-identical) turns `--expand` into a
  verifiable artifact. Note the file-extension correction in §6 problem 4.
- **F/N (caching / fast path).** One slot: N is explicitly not-planned, folded into F;
  F is demand-gated on a *measured* regression (the stage prints timing under verbosity —
  master §15). No action until the measurement exists.
- **G–L.** Individually shippable, none blocks another, all pull-based per phase4 §11
  item 8. No tracker action beyond existing.
- **O/P.** Resolved by association (O rides A's machinery; P is subsumed by E's round-trip
  test). They cannot rot independently — if A and E land, O's home and P's guarantee land
  with them; if A/E slip, O/P slip identically and this table says so.
- **M, Q.** The two blocked rows — §4 and §5.

---

## 4. The named-arguments prerequisite (item M's blocker)

> **2026-07-17: DISCHARGED.** The prerequisite this section commissions was designed and
> built — `designs/complete/techdesign-named-arguments.md` (named args + default parameter
> values + overload interaction, one story, exactly the §4 shape below). Item M is
> unblocked; only the attribute-side inheritance (phase4 §9.5) remains to verify/build.
> The section is kept as the record of the dependency chain.

This is the one place a metaprogramming deferral is really a **language-design dependency**,
and it deserves its own callout because the dependency chain is easy to lose:

- **The punt:** attributes take positional args only (`@Route("GET", "/users")`). Master
  `techdesign-metaprogramming.md:169-171`: *"Named arguments are deferred until the
  language itself has named arguments (deviation §14.4)."* Deviation 4 (`:886-889`) gives
  the reason: the proposal's "positional or named args" assumed a facility that exists
  nowhere in the language; inventing it for attributes first is a §1 special-case
  violation.
- **What must exist first** (none of it is metaprogramming work):
  1. Call-site named-argument syntax for ordinary calls (`server.listen(port = 8080,
     host = "0.0.0.0")` — the shape `designs/suggested-features.md:178-179` suggests).
  2. One unified resolution story with **default parameter values and overloads** —
     suggested-features `:181-182` is explicit that the three interact and must be
     specced together, not incrementally.
  3. Only then: attributes inherit named args *for free* by reusing that parse against the
     attribute's field list (phase4 §9.5: "nothing attribute-specific to design here").
- **Current owner: none.** Named arguments appear in `designs/suggested-features.md` §3.4
  (parameter ergonomics) but are **not** one of the nine tracks in
  `designs/complete/techdesign-00-overview.md`, and no `techdesign-*` doc specs them. Until a
  named-arguments (or "parameter ergonomics": named args + defaults + overload story)
  design exists, item M is unschedulable — and that is correct behavior, not a stall to
  work around. **Recommendation:** commission it as its own design doc when parameter
  ergonomics gets prioritized; it is a resolution-order/checker design more than a runtime
  feature, and it unblocks M as a trivial follow-on.

---

## 5. Item Q — platform-conditional `uses`, the residual phase4 does not track

> **2026-07-17: blocker cleared, work still open.** The Track 03 statics ruling landed and
> `enum` shipped in full, so the `Platform::current`-as-enum shape (and the statics-free
> comptime-target-const alternative) are both buildable. Verified the same day: no such
> predicate exists in the tree yet. The §8.2 one-pager — predicate shape + **target**-not-host
> sourcing per the portable pivot — is now this tracker's single open design item.

The one deferral in the cited sources that is **absent from phase4's A–P inventory**,
because its bigger half already landed and the leftover is small and easy to lose:

- **What was deferred:** master dev. 6 (`:893-896`) — `uses` inside `comptime if` was
  forbidden in v1 (M15); §16 decision 4 (`:928-929`) asked whether deferring past Phase 3
  was acceptable ("The web framework doesn't need it; self-hosting eventually might").
- **What landed:** Phase 3 answered by *building the mechanism* — P3 §9 (pipeline split
  A–G, engine-owned post-fold imports, item-level splice flatten), M15 deleted
  (`techdesign-metaprog-phase3.md:617`). `uses` inside `comptime if` is legal and tested.
- **What did NOT land and is untracked:** the proposal's motivating example
  (`proposal-metaprogramming.md:351-355`) conditions on **`Platform::current ==
  Platform::Linux`** — a comptime-visible platform/target constant. No such constant
  exists anywhere in the tree (no platform symbol in `src/`, none in `docs/reference.md`).
  So platform-conditional `uses` works *mechanically* but has nothing to condition on:
  the feature is a door with no key.
- **What the predicate waits on:** `Platform::current` as sketched is an enum with a
  static-side constant — exactly the mechanism Track 03 STOPped on (class static-side
  consts don't exist; handoff in repo-root `/this_bug.mg`, commit `affa8a4`). Chain:
  statics ruling → Track 03 `enum` → a `Platform` (or `Target`) comptime constant →
  item Q closes. Alternative shapes that dodge the statics dependency exist (a
  compiler-provided comptime string const, e.g. `comptime target.os == "linux"`), but
  choosing one is a small design decision, not a given — see §8.
- **The cross-compilation wrinkle (new since the deferral was recorded):** the portable
  pivot made LLVM the primary backend with real cross-target ambitions
  (`docs/techdesign-portable-backend.md`). The predicate must therefore reflect the
  **target** triple, not the host the compiler runs on — a distinction the proposal's 2026
  single-target framing never had to make. Whatever design closes Q must take the value
  from the target configuration, not `uname`.

---

## 6. Potential problems

1. **Phase4 rotting as an untracked footnote — including this tracker's own failure
   mode.** Phase4 is "design for review — no implementation yet" and grounded at commit
   `6c66c49`; its file:line references have **already drifted** (it cites the `rewrites`
   stub at `Parser.cpp:1041` — now `1108-1112`; `--expand` alias at `main.cpp:64` — now
   `196-197`). Harmless today (the *anchors* — function names, token text — still hold),
   but each landed track widens the gap. **Solution:** do not chase lines now (this doc
   records the current-tree equivalents above); on Phase-4 implementation start, the
   implementer re-grounds phase4 §11's file map against HEAD as step zero, exactly as the
   Track 03 convention requires reading predecessor implementation logs first.
2. **Provenance precision limits (D) — a demand gate nobody can trip.** The upgrade
   trigger is "a real diagnostic is shown ambiguous", but confused users don't file
   "3-origin ambiguity" reports; they file "weird error" reports that die in chat.
   **Solution:** route them through the existing `/bug.md` workflow — any report where a
   rule-injected-code diagnostic listed multiple candidate origins and the reporter picked
   wrong gets filed there tagged as a D-trigger. The gate stays demand-driven (the dev. 8
   decision stands); it just becomes *observable*.
3. **Platform-conditional `uses` timing (Q) vs the web track.** Track 09
   (`designs/complete/techdesign-09-web-foundations.md`) is prelude-level and contains no
   platform-conditional imports (checked — no reference in the doc), consistent with the
   master's "the web framework doesn't need it". So Q does **not** block the web track.
   The pressure comes from the *portable* track instead: multi-target LLVM makes
   target-conditional imports the natural way to split platform floors, and Q is blocked
   behind the Track 03 statics STOP. **Solution:** don't couple them — when the statics
   ruling lands, close Q via Track 03's enum; if the portable track needs it *sooner*,
   take the statics-free shape (compiler-provided comptime const) as a stopgap and note it
   in the Q design one-pager (§8). Either way the decision point is the statics ruling,
   already escalated in `/this_bug.mg`.
4. **Phase4's `.ext` references vs the `.lev` rule.** Phase4 predates the extension
   convention: its §6 acceptance says the pretty-printer emits "compilable `.ext` syntax"
   and its examples cite `app/users.ext`. The standing hard rule is **all new source/test
   files are `.lev`, never `.ext`** — even when a design doc says otherwise. **Solution:**
   recorded here as the binding correction (this tracker may not edit phase4): item E's
   implementation emits/round-trips `.lev`, and every phase4 §12 corpus file is created as
   `.lev`.
5. **Phase4's acceptance backend vs the X64Gen freeze.** Phase4 §12 specifies twins
   verified "oracle==IR==ELF" — written before the portable pivot froze X64Gen/ELF and
   made LLVM primary. Layer D is entirely pre-lowering AST work, so the backends are
   untouched by the *features*; the exposure is test-harness-only. **Solution:** new
   Phase-4 corpus asserts **oracle==IR==LLVM**; existing ELF assertions on the legacy
   corpus stay (finished X64Gen behavior is kept, not reverted), but no new work targets
   X64Gen even where phase4's text implies it.
6. **The named-args special-case temptation (M).** The longer M sits blocked, the more
   attractive an attributes-only named-arg syntax becomes ("it's just the attribute
   parser..."). That is precisely the §1 violation deviation 4 exists to prevent, and it
   would create a second, incompatible named-arg story when the language feature lands.
   **Solution:** M's classification here is (b)-blocked, full stop; the unblock path is
   §4's design doc, never an attribute-local parse.

---

## 7. Recommended solutions and sequencing

In priority order, cheapest-unblock first:

1. **Nothing in A–P needs design work — treat phase4 §11 as the schedule when Phase 4 is
   prioritized.** Commits 1–5 there (A, B, C, E, G) are buildable now; 6–7 (D, F) stay
   demand-gated behind their written triggers; the tail (H–L) stays pull-based. Apply the
   §6 corrections above (re-ground lines, `.lev` corpus, LLVM acceptance) at
   implementation start.
2. **Commission the named-arguments design** (§4) when parameter ergonomics is
   prioritized — one doc covering named args + defaults + overload resolution as a single
   story (suggested-features §3.4's own advice). This is the only path that unblocks M,
   and M then costs ~nothing.
3. **Close Q behind the Track 03 statics ruling** — when the ruling lands and `enum`
   ships, a one-pager picks the predicate shape (`Platform::current` enum vs a
   compiler-provided comptime target const), sourcing the value from the *target*
   configuration per the portable pivot. If the portable track needs conditional imports
   before then, the statics-free const is the sanctioned stopgap.
4. **Keep the D-trigger observable** via the `/bug.md` tagging convention (§6 problem 2) —
   zero engineering cost, preserves the demand gate's integrity.
5. **This tracker's own maintenance:** one-line status updates per row when an item lands
   or a blocker clears (same convention as `techdesign-00-overview.md`'s track table);
   when Phase 4 is fully landed, this doc moves to `designs/complete/` alongside it.

---

## 8. What still needs new design

Everything in the table except two rows points at phase4 as its finished resolution.
The complete list of genuinely undesigned work:

1. **Language-level named arguments** (prerequisite for M) — its own design doc, scoped
   as named args + default parameter values + overload interaction. Not a metaprogramming
   doc; attributes consume it for free afterward (phase4 §9.5).
2. **The Q predicate one-pager** — which comptime constant platform-conditional `uses`
   conditions on, target-not-host semantics, and its statics dependency (or the
   statics-free shape). Small; blocked on (or shaped by) the Track 03 statics ruling.

Everything else: `docs/techdesign-metaprog-phase4.md` is canonical, and the correct
response to "should we redesign X?" for any A–P item is *no — read phase4 §N*.

---

*Companions: `docs/techdesign-metaprog-phase4.md` (canonical resolutions, A–P),
`docs/techdesign-metaprogramming.md` (master design; §13 Phase-4 checklist, §14
deviations), `docs/techdesign-metaprog-phase3.md` (the landed substrate, §9 for Q's
mechanism), `designs/suggested-features.md` §3.4 (the named-args seed),
`/this_bug.mg` (the statics STOP that gates Q).*
