# Tech Design: Metaprogramming Phase 4 — Layer D rewriters, polish, and the deferred tail

**Status:** design for review — no implementation yet. Companion to
`designs/complete/techdesign-metaprog-phase3.md` (Phases 1–3 built and merged) and
`designs/complete/techdesign-metaprogramming.md` (the master design + phase checklist).

**Scope:** every piece the accepted proposal and the Phase 1–3 tech designs deliberately
deferred, gathered into one place so nothing rots as an untracked footnote. Two tiers:

- **Layer D + polish** (the proposal's named "Phase 4"): body-replacing rules
  (`rewrites`/`replace`/`$body`), rule-conflict/confluence detection, the `reentrant`
  gate, exact per-clone provenance, a source-shaped `--expand`, incremental per-file rule
  caching, and pass-1 comptime-root pre-checking.
- **The deferred tail** (smaller in-phase deferrals that never needed their own phase but
  should not be lost): `meta.*` structured `Type`, attribute-value reflection, statement-
  position `$for`, the def-site "true home" (`Symbol::homeNs`) refinement, class-wide
  marker search, attribute named arguments, the single-resolve fast path, macro
  single-splice auto-hoisting, and a golden-`--expand` determinism fixture.

**Reading order:** assumes the master design and the Phase 3 doc; §numbers like §5.4 refer
to the master design (`techdesign-metaprogramming.md`) unless prefixed `P3`
(`techdesign-metaprog-phase3.md`).

**Grounding commit:** the codebase as of `6c66c49` (Phase 3 feature-complete: §2–§11 all
landed, suite green 26/26). File:line references below are against that tree.

---

## 0. Contents

1. Inventory — what's deferred, from where, and why it was deferred
2. Body-replacing rules — `rewrites` / `replace` / `$body` (the headline)
3. Confluence — rule-conflict detection at a shared anchor
4. `reentrant` — gated rule re-triggering on rule-generated code
5. Provenance — exact per-clone IDs (`provenanceId`)
6. `--expand` as source — the pretty-printer
7. Incremental per-file rule-output caching
8. Pass-1 comptime-root pre-checking
9. The deferred tail (nine smaller items)
10. Diagnostics catalog (M30–M35)
11. File-level change map and sequencing
12. Testing plan
13. Open calls for Leonard

---

## 1. Inventory — what's deferred, from where, and why

Every row is something a shipped design doc explicitly punted. "Source" is where the punt
is recorded; "why deferred" is the original stated reason (not a new judgment).

| # | Item | Source | Why deferred originally |
|---|---|---|---|
| A | Body-replacing rules (`rewrites body of m`, `$body`, `replace`) | proposal §4.4/§10.5; master §14 dev.; parser stub `Parser.cpp:1041` | "deliberately last and deliberately marked"; the additive Layers A–C cover the framework drivers, only wrap-a-body transforms (memoize/@Timed/trace) need it |
| B | Confluence / rule-conflict detection at a shared anchor | proposal §5.4 | additive injections already compose in rule order; conflict detection only *needs* to exist once two **rewrites** can target one body — i.e. once (A) lands |
| C | `reentrant` — rules triggering on rule-generated code | proposal §8, §13-Q3; master §15 | "default no"; no real case yet; a fixpoint with a budget backstop, wanted only on demand |
| D | Exact per-clone provenance (`provenanceId`) | master §8.2, dev. 8; `Ast.hpp` has no such field | range-based 3-origin attribution is good enough until the ambiguity actually bites a diagnostic |
| E | `--expand` as readable source (not an AST dump) | master §7 dev., proposal §3.7; `--ast-after-rules` already aliases it (`main.cpp:64`) | information content shipped first (Phase 2); form is polish |
| F | Incremental per-file rule-output caching | proposal §3.6/§13.7; master §15 | whole-program re-resolve is the accepted §17 trade for v1; caching is the designed answer when compile time regresses |
| G | Pass-1 comptime-root pre-checking (earlier type errors) | master §7 (`good enough for v1`) | comptime errors surface at eval time; pre-checking is a diagnostic-quality nicety |
| H | `meta.*` structured `Type` (vs canonical strings) | P3 §3, §4 | strings cover the realistic `where` predicates; a structured `Type` is a forever-API growth, add by demand |
| I | Attribute-value reflection (`meta::Attr {name, fields}`) | P3 §3, §5 landed-note | the minimal `meta.*` surface reflects attribute *names* only; the ORM `$for` example was adapted to field-names rather than grow the surface pre-demand |
| J | Statement-position `$for` (repeat whole statements) | P3 §5 | array form covers the known drivers; statement repetition invites the template-as-program smell |
| K | Def-site "true home" qualification (`Symbol::homeNs`) | P3 §10 landed-note | found unimplementable from Scope data alone; the gap is provenance-cosmetic, not correctness |
| L | Class-wide marker search (vs subject-body-only) | P3 §15 Q2 | subject-only keeps reach tied to the match; widen only if a real case needs it |
| M | Attribute named arguments | master dev. 4 | positional-only until the *language* grows named args; inventing them for attributes first is a §1 special-case |
| N | Single-resolve fast path for syntactic matchers | master dev. 1 | two staging modes double the state space; the slot stays open, Phase-4 caching subsumes it |
| O | Macro single-splice auto-hoisting | P3 §15 | M22 (error) is the honest v1; auto-hoisting via statement-lifting is Layer-D machinery |
| P | Golden-`--expand` determinism fixture | P3 §14 landed-note | the property is exercised indirectly (`rule_marker` value-diff); a shape-regression fixture is worth adding only if that failure mode appears |

Items **A–G** are the proposal's Phase 4. **H–P** are the deferred tail (§9). The
sequencing (§11) lands A→B→C as one arc (they interlock), then D–G and the tail as
independent, individually-shippable commits.

---

## 2. Body-replacing rules — `rewrites` / `replace` / `$body`

The one genuinely new *capability* in Phase 4: a rule that **replaces** a matched method's
body instead of adding beside it. Everything else in this doc is detection, polish, or a
small surface growth. This is the "marked bazooka room" (proposal §4.4) — gated behind an
explicit `rewrites` marker so it is greppable and its `--expand` diff is the loudest.

### 2.1 Grammar (extends the master §3.3 rule grammar)

```
RuleDecl    ::= 'rule' Identifier RewritesClause? '{' MatchClause ActionClause+ '}'
RewritesClause ::= 'rewrites' 'body' 'of' Identifier        // the bound method whose body is replaced
ActionClause   ::= InjectClause | ReplaceClause
ReplaceClause  ::= 'replace' QuasiLiteral                    // only legal in a `rewrites` rule
```

The proposal (§10.5) writes the clause as `rule timed rewrites body of m { … }`. The
current parser stub (`Parser.cpp:1041`) consumes only a **bare** `rewrites` token before
`{` and errors "Layer D … Phase 4". Phase 4 extends it to the full `rewrites body of
<bind>` clause, sets `ruleRewrites = true` (the flag already exists on `Stmt`,
`Ast.hpp:293`-area), and records the target bind.

- The `body of <bind>` names which matched declaration's body is replaced — normally the
  subject (`on method m` → `rewrites body of m`), but naming it explicitly keeps the door
  open for `rewrites body of` an *encloser*-bound method without a grammar change.
- A `replace` action is legal **only** inside a `rewrites` rule; a `replace` in an
  ordinary rule, or an `inject` in a `rewrites` rule, is **M30** (loud, at the action).
  One rule is either additive (`inject`) or a rewriter (`replace`), never both — this is
  what makes the `--expand` diff legible and the confluence check (§3) tractable.
- `rewrites` stays a **contextual** identifier (like `rule`/`macro`/`attribute`), matching
  master decision-3. Recognition: `rewrites` only after a rule name and only followed by
  `body` — no new reserved word, no existing program breaks.

### 2.2 The `$body` hole — the original body as a splice

`$body` is a new well-known hole (family with P3 §6's `$_params`/`$_args`): it reifies to
the **original** matched method's body, so replacement is composition, not obliteration.
The AST already distinguishes the two body shapes a method can have (`Ast.hpp:271`,
`memberBody` is a single Stmt — a `Block` for `{ … }` bodies, or the wrapped expression
statement for `=> expr` arrow bodies).

`$body` is **context-sensitive**, and this is the crux of the design:

- **Statement position** (`$body;` as a statement inside the replace template): splices the
  original body's statements verbatim in place. This is the general, always-valid form.
  The original's own locals are **not** alpha-renamed — the original body moves as one
  authored unit, and the *template's* new locals are the ones gensym'd (P3 §7.1 hygiene,
  unchanged), so the two can't collide.
- **Expression position** (`var __r = $body;` — the proposal's §10.5 example): valid **only
  when the original body is a single value expression** — an arrow body (`=> expr`), or a
  block that is exactly `{ return expr; }`. Then `$body` = that expr. Any other block body
  in expression position is **M31** ("`$body` used as an expression, but `m`'s body is a
  statement block; capture the result in statement position, or the method needs an arrow
  body"). This keeps v1 honest rather than silently wrapping.

  *Why not auto-wrap in an IIFE?* A block-valued `$body` in expression position could lower
  to `(() => { …original… })()`. Rejected for v1: it changes the cost profile (an
  allocation + indirect call the author didn't write), which violates the P1
  cost-identity guarantee that makes the whole feature safe to reach for. The author who
  truly wants that writes the lambda explicitly. Revisit if the block-value case proves
  common (§13).

The `@Timed` driver (proposal §10.5) works under this rule for arrow-bodied and
single-return methods — the common shape for the methods people annotate — and gives a
precise, actionable error for the rest.

### 2.3 Where replacement happens in the pipeline

Body replacement rides the **existing** Layer B rule machinery (`runRules`/`expand` in
`Rules.cpp`), a sixth anchor kind alongside the five P3 `AnchorKind`s:

- New `AnchorKind::BodyReplace` (`Ast.hpp:217` enum). `parseRuleAction` produces it for a
  `replace` clause; `expand()` gets one new branch.
- The branch: resolve the target method via the rule's bindings (the `rewrites body of
  <bind>` target); require it callable with a body (else M27-family, reusing the P3
  wording); clone the `replace` template with `$body` bound to a **clone of the original
  `memberBody`** (so the template can splice it once — and, per §3, exactly once); run the
  clone-time hygiene + def-site qualification + macro-expansion passes it already runs for
  every other anchor (P3 §7×§9 5c); then **overwrite** `subject->memberBody` with the
  result (normalized to a Block via the existing `normalizeMemberBody`,
  `Rules.cpp:1268`-area).
- `$body` referenced **zero** times in a `replace` template is **M32** (a rewriter that
  drops the original body silently is exactly the obliteration the design forbids — if you
  really mean "discard the body", that is a different, louder future verb, not `replace`).
  Referenced **more than once** is also M32-adjacent: like macro args (P3 M22), splicing a
  statement body twice would duplicate its side effects; a `rewrites` rule may reference
  `$body` **at most once**. Checked statically at rule-collection time (the P3
  `countHoleRefs*` walkers, `Rules.cpp:479`-area, already count hole occurrences — reused
  verbatim).

### 2.4 Interaction with additive rules on the same method

A method can be both `@Timed` (a rewriter wraps its body) and, say, `@Log` (an additive
`top of body` rule prepends a line). Ordering is the confluence question (§3): the design
is **all additive `inject`s apply first, in rule order, then the single permitted
`replace` wraps the resulting body**. Rationale: `$body` should capture "the method as the
programmer wrote it plus any additive prologue/epilogue", which is what a memoize/trace
wrapper wants to wrap. Two `replace`s on one body is a hard conflict (§3) — there is no
order that composes two independent whole-body replacements.

---

## 3. Confluence — rule-conflict detection at a shared anchor

Proposal §5.4 requires: two rules affecting the same anchor either compose order-
independently, **or** the compiler reports a conflict. Phase 1–3 shipped the composing
cases (additive injections stack in rule order; the marker-anchor accumulator
`markerInsertCount_` preserves order, `Rules.hpp:199`-area). Phase 4 adds the **detection**
that becomes necessary the moment (A) makes whole-body replacement possible.

Three conflict classes, all reported as **M33** (with the two rule names + the anchor):

1. **Two `replace`s targeting one method body.** No composition exists — flat conflict.
   Detected in `expand()` by a per-method "already replaced by rule X" mark (a
   `std::map<const Stmt*, std::string_view>` keyed on the subject `memberBody`'s owning
   `Stmt`); the second replacer trips it.
2. **A `replace` and an additive body-anchor rule whose relative order changes the
   result.** Resolved by the §2.4 rule (additive-then-replace is *defined*), so this is
   **not** a conflict — documented as the deliberate order, not left to chance. Recorded
   here so the "why isn't this M33" question has an answer in the doc.
3. **Two `member of` injections of the same name **and** the same type** — already caught
   today (`injectMember`, `Rules.cpp:1761`, emits an existing error). Phase 4 folds that
   message into the unified M33 family for a consistent "rule X and rule Y conflict at
   <anchor>" shape and adds the reuse of the §4.3 `distinct` escape (two same-name+type
   members coexist if one is marked `distinct`, the existing collision machinery).

Confluence detection runs **after** all rules have matched but is naturally interleaved
with `expand()` (the marks accumulate as each firing applies). Determinism is already
guaranteed by `orderRules()` (`Rules.cpp`, `stable_sort` on source offset) — the conflict
report names rules in that stable order so the diagnostic is reproducible.

---

## 4. `reentrant` — gated rule re-triggering

Default, today and after Phase 4: a rule **never** matches code another rule injected (the
one-direction pipeline, P3 §9's standing stance; the master design's no-reentry position
everywhere). `reentrant` is the gated opt-in for the rare case that genuinely wants a
fixpoint — a rule that emits an attribute a *second* rule consumes.

- **Grammar:** `rule NAME reentrant { … }` — a second contextual marker on the rule
  header, parallel to `rewrites`. (`rewrites` and `reentrant` are independent; a rule may
  carry both, though that combination is exotic.)
- **Semantics:** after the normal `runRules()` pass, if **any** `reentrant` rule exists and
  the tree changed, re-run rule matching (`indexDecls` + `runRules`) on the post-expansion
  tree. Repeat until a fixpoint (no rule fires) **or** a re-trigger budget is hit — the
  same step-budget philosophy as comptime (`ComptimeOptions`, `Rules.hpp`), a new
  `reentrantRounds` cap (default small, e.g. 8) surfaced by `--comptime-budget`'s sibling
  or a dedicated flag.
- **Budget exhaustion is M34** ("`reentrant` rule expansion did not converge in N rounds —
  a rule is re-triggering itself; raise the bound or break the cycle"). Loud, not silent
  truncation.
- **Only `reentrant` rules re-trigger.** A non-`reentrant` rule still sees the tree exactly
  once, even in a program that also contains a `reentrant` rule — the gate is per-rule, so
  the safe majority keeps its guarantee (the §16 "protect the safe majority" stance).

Non-goal for v1: cross-`reentrant`-rule ordering guarantees beyond the existing stable
source-offset order re-applied each round. If two `reentrant` rules ping-pong, that's a
non-convergence (M34), not a feature.

---

## 5. Provenance — exact per-clone IDs

Today: range-based 3-origin attribution (master §8.2). Each injected node keeps its
**template span** (rule-site), and `ExpansionRecord` (`Rules.hpp:14`) carries the use-site
origin + template span + file index. `--expand` prints injections under a provenance banner
from those records; pass-2 diagnostics on injected code already point at the template
source because the clone preserves spans. This is *sufficient* until one node's three
plausible origins (use site, rule site, def site) genuinely can't be told apart in a
diagnostic.

Phase 4, **only if that ambiguity bites in practice** (master dev. 8 makes this explicitly
demand-driven):

- Add `uint32_t provenanceId = 0;` to `Stmt` and `Expr` (`Ast.hpp`).
- Stamp it during `cloneStmt`/`cloneExpr` (`Rules.cpp`) with a per-firing id from a counter
  on the engine; map id → `ExpansionRecord` in a side table.
- Resolver/Checker diagnostic sites consult the map to print "…in code injected by rule
  `Web::registerRoutes` at app/users.ext:8" instead of the range-based approximation.

Cost the deferral avoided: touching every `sink_.error` call site in Resolver/Checker to
thread provenance. The design keeps that cost unpaid until a real diagnostic is shown to be
ambiguous — the trigger is a *specific* confusing error report, not a schedule.

---

## 6. `--expand` as source — the pretty-printer

Today `--expand` (and its alias `--ast-after-rules`, `main.cpp:64`) prints the
`AstPrinter` dump — structurally complete, provenance-bannered, but shaped like an AST, not
like source you could paste back. Proposal §3.7 makes "expansion inspection" table stakes;
Phase 2 satisfied it by *information content*, Phase 4 by *form*.

Design:

- A source-shaped pretty-printer — a second rendering mode on `AstPrinter` (or a sibling
  `SourcePrinter`) that emits compilable `.ext` syntax: real `class`/`method`/`=>`/`{}`
  forms, not `Class …`/`Method …` labels.
- `--expand` switches to the source-shaped output; **`--ast-after-rules` keeps the AST
  dump** (the alias splits from `--expand` and becomes the way to get the old form — it is
  already a distinct spelling in `main.cpp`, so this is a one-line demux, not a new flag).
- Provenance survives as **comments**: `// from rule Web::registerRoutes @ users.ext:8`
  above each injected block (the proposal §10.1 shows exactly this shape), sourced from the
  same `ExpansionRecord`s.
- **Acceptance (the strong one):** the source-shaped `--expand` output, saved as a `.ext`
  file and compiled, produces byte-identical program output to the original — "`--expand`
  output compiles and runs identically to the hand-written equivalent" (proposal §12).
  This is a *round-trip* test, mechanically checkable, and is the real reason to do the
  pretty-printer: it turns `--expand` from a debugging aid into a verifiable artifact.

Determinism is a hard requirement here (a pretty-printer whose output reorders run-to-run
is useless as a diff target) — see §9's golden-`--expand` fixture (item P), which this
section's round-trip test subsumes and strengthens.

---

## 7. Incremental per-file rule-output caching

The accepted v1 trade (master §17): the rule stage re-resolves the whole program after any
expansion. On a large meta-heavy project this is a measurable compile-time cost. Proposal
§3.6/§13.7 designs the answer; Phase 4 builds it *when the cost is measured, not guessed*
(master §15 mitigation: "the stage prints timing under existing verbosity").

Design:

- **Cache key per file:** `hash(file source) ⊕ hash(imported-rule-set)`. The first term
  catches edits to the file; the second catches edits to any rule the file is *subject to*
  (a rule change must invalidate every file that rule can fire on). The imported-rule-set is
  already computable — it's the rules whose namespace is in the file's `effective` set
  (`FileImports`, `Project.hpp`), the same scoping `tryMatch` uses (`Rules.cpp:771`-area).
- **Cached value:** the post-expansion AST for that file's items (or a serialized form),
  plus its expansion records.
- **Invalidation:** whole-program still, but only *un*-cached files re-run the rule stage;
  the resolve/check pass-2 still sees the whole gathered tree (correctness unchanged — this
  caches the *expansion*, not the resolution). This is the "incremental beachhead" the
  proposal §12 notes the project system already sets up (every file imports the project's
  namespaces).
- **Non-goal:** cross-invocation on-disk caching (a build-daemon concern) — v1 of this is
  in-process, for the multi-file project build within one `lang` invocation. On-disk is a
  further step behind a real daemon.

This item is **the** reason `provenanceId` (§5) and the `reentrant` fixpoint (§4) are
sequenced *before* it: a cache of expansion output must key on, and correctly invalidate
around, both. Caching is last in Phase 4 for that reason.

---

## 8. Pass-1 comptime-root pre-checking

Master §7 accepts: comptime evaluation errors surface at *eval* time (a type error inside a
`comptime` expression is reported when the oracle runs it, not by a prior static check).
"Good enough for v1; targeted pass-1 checking of comptime roots is Phase 4 polish."

Design: before the oracle evaluates a comptime root (a `comptime` var init, `comptime if`
condition, or folded `comptime expr`), run the **existing** resolver/checker over just that
subtree so ordinary type errors surface with ordinary type-error messages, *before* the
"comptime evaluation failed: …" wrapper. Purely a diagnostic-quality improvement — no
behavior change, no new capability. Sequenced late and independently; skippable without
blocking anything.

---

## 9. The deferred tail

Nine smaller deferrals. Each is self-contained, individually shippable, and none blocks
another. Grouped by the surface they touch.

### 9.1 `meta.*` surface growth (items H, I)

- **Structured `Type` (H).** Today `meta::{Field,Param,Method}` expose types as canonical
  **strings** (`meta::Field.type : string`, prelude in `Resolver.cpp:684`-area). A
  structured `meta::Type { name; Array<meta::Type> generics; bool isValue; … }` would let a
  `where` predicate ask `f.type.isValue` or walk generic arguments. Deferred because every
  field is a forever-API (proposal §3.8); the string form covers the realistic predicates
  (`f.type == "int"`, `m.returnType != "void"`). **Add by demand**, additively — a new
  prelude class + a `buildMetaValue` (`Rules.cpp:677`) branch that builds it; no existing
  `where` clause breaks (string fields stay).
- **Attribute-value reflection (I).** Today `meta::{Field,Method}.attrs` is
  `Array<string>` — attribute **names** only. The proposal's ORM example (§10.2) wanted
  `f.attr(Column).name` — reading an attribute's evaluated *arguments* through reflection.
  Phase 3 adapted `rule_orm` to field-names to avoid growing the surface pre-demand. The
  growth, when wanted: a `meta::Attr { name : string; fields : Array<meta::AttrField> }`
  (a name + its evaluated field values), and `meta::Field.attrObjects : Array<meta::Attr>`
  beside the existing name array. The evaluated values already exist —
  `attrValues_` (`Rules.hpp:73`) holds every attribute's field→value map; `buildMetaValue`
  would surface them as reflectable objects instead of dropping to names. Additive.

### 9.2 Statement-position `$for` (item J)

Today `$for` splices **array-literal elements** (P3 §5, `ExprKind::ForSplice`,
`cloneArrayElements` in `Rules.cpp`). Statement-position `$for` would repeat whole
**statements** in a `ctor`/`body` template:

```
inject `$for f in C.fields : this.validate($f.name);` at bottom of C.constructor
```

Design: a `StmtKind::ForSplice` (paralleling the expr kind), parsed inside statement
fragments (`parseStmtsFragment`, `Parser.cpp:1147`) when a statement begins with `$for
<id> in <expr> :`, and expanded in `cloneStmt`'s statement-list walk exactly as
`cloneArrayElements` expands the expr form (iterator via `materializeBindings`, M21 on
non-array, per-item extended bindings). Deferred because the proposal warned that statement
repetition invites "template-as-program" (a full imperative sublanguage inside quasiquotes);
the array form covers every known driver. **Add on demand**, and keep it bounded — no
`$if`/`$while` follows it (that line is where the smell starts).

### 9.3 Def-site "true home" qualification (item K)

P3 §10 landed the common case (a free name resolving in the rule's own namespace qualifies
to `NS::name`). The unshippable refinement: if the rule's namespace only **re-exported** a
name via its *own* `uses`, qualify to the name's true home namespace, not the rule's. Found
unimplementable from `Scope` data alone — `uses`-copying makes a borrowed name and a
home-declared name identical (both a `Symbol*` in the namespace's `names` map). The fix, if
ever wanted: a `homeNs` field on `Symbol` (`Symbols.hpp:49`-area), stamped at declaration
time by the Resolver's gather (the one place that knows which namespace body textually
contains the declaration), then `qualifyDefSite` (`Rules.cpp`) consults it. **Provenance-
cosmetic only** — pass-2 resolution of `RuleNS::name` finds the re-exported symbol anyway
(the copy *is* the import); the only visible difference is which namespace `--expand`
prints. Two-line stamp + a lookup swap; do it if a `--expand` reader is ever confused.

### 9.4 Class-wide marker search (item L)

Today `marker "name"` searches only the **subject method's** body (`findMarkerSlot`,
`Rules.cpp`, recurses the subject's normalized body). Class-wide search would let a rule
matched on the class drop code at a marker in *any* of the class's methods. Deferred (P3
§15 Q2): subject-only keeps a marker's reach tied to what the rule matched, which is the
safer default. Widen only if a real case needs class-wide markers — the change is scoping
the `findMarkerSlot` walk over the class body instead of one method, plus a rule about
ambiguity (same marker name in two methods → M-error naming both).

### 9.5 Attribute named arguments (item M)

Attributes take **positional** args only (`@Route("GET", "/users")`), master dev. 4,
because the *language* has no named-argument facility anywhere — inventing one for
attributes first is a §1 special-case. When the language grows named args generally (a
separate, larger design), attributes get them for free by reusing that parse. Nothing
attribute-specific to design here; this row exists so the dependency is recorded (attributes
*wait on* a language feature, they don't *lack* a metaprogramming one).

### 9.6 Single-resolve fast path (item N)

Master dev. 1 dropped the proposal's "syntactic-only single-pass" staging mode: two staging
modes double the state space, and pass 2 is already skipped entirely when nothing expands
(`main.cpp:177`-area, gated on `changed_`). The optimization slot stays open; the
incremental cache (§7) subsumes it (a cached file skips the stage regardless of matcher
kind). **Explicitly not planned as its own feature** — folded into §7. Recorded so "why
didn't the fast path get built" has an answer.

### 9.7 Macro single-splice auto-hoisting (item O)

P3 M22 makes a macro that splices its parameter more than once an error (double-evaluation
has nowhere to hoist a binding in expression position). Auto-hoisting — lifting the argument
into a synthesized `comptime`/local binding so multi-use is safe — is Layer-D statement-
lifting machinery. Deferred as "the honest option teaches the workaround" (bind at the call
site). If built, it belongs with (A)'s body-rewriter machinery (both need statement-lifting
around an expression), which is why it's tagged Layer D and sequenced near A, not standalone.

### 9.8 Golden-`--expand` determinism fixture (item P)

P3 §14 notes the harness has no `--expand`-golden mechanism (the `.expected` files check
`--run` output values, not `--expand`'s dump). The determinism property (stable rule-firing
order) is exercised indirectly (`rule_marker`'s value-diff pins one stacking order,
`orderRules`'s `stable_sort` guarantees it). §6's round-trip acceptance test **subsumes and
strengthens** this: a source-shaped `--expand` that compiles to identical output is a far
stronger determinism check than a golden text diff. So item P is **resolved by §6**, not a
separate build — unless §6 slips, in which case a minimal golden-dump fixture (a new runner
comparing `--expand` output to a checked-in `.expand` file for `rule_orm`/`rule_marker`) is
the standalone fallback.

---

## 10. Diagnostics catalog (M30–M35)

Extends M01–M29 (master §8, P3 §12). All errors (Layer D is loud by construction).

| # | Sev | Trigger | Anchored at |
|---|---|---|---|
| M30 | E | `replace` in a non-`rewrites` rule, or `inject` in a `rewrites` rule (a rule is additive xor a rewriter) | the offending action clause |
| M31 | E | `$body` in expression position but the method's body is a statement block (not a single value expression) | the `$body` hole |
| M32 | E | a `rewrites` rule references `$body` zero times (silent obliteration) or more than once (duplicated side effects) | the `replace` template |
| M33 | E | rule conflict at a shared anchor: two `replace`s on one body, or two same-name+type `member of` injections (unless `distinct`) | the anchor, naming both rules |
| M34 | E | `reentrant` rule expansion did not converge within the round budget | the last-firing rule, note on the budget |
| M35 | E | `rewrites body of <bind>` names a bind that isn't a callable with a body | the `rewrites` clause |

(M35 is the `rewrites`-header analogue of the P3 M27 family for the `replace` action; split
out because it's caught at parse/collection time on the header, not at expansion.)

---

## 11. File-level change map and sequencing

| File | Change |
|---|---|
| `src/Ast.hpp` | `AnchorKind::BodyReplace`; `provenanceId` on `Stmt`/`Expr` (§5, demand-gated); `StmtKind::ForSplice` (§9.2, demand-gated) |
| `src/Parser.*` | `rewrites body of <bind>` header clause; `replace` action; `reentrant` header marker; statement-position `$for` in fragments (§9.2) |
| `src/Rules.hpp/.cpp` | `$body` hole in clone; `BodyReplace` branch in `expand()`; single-`$body` static check (reuse `countHoleRefs*`); confluence marks + M33; `reentrant` fixpoint loop + M34; `provenanceId` stamping (§5); per-file expansion cache (§7); `buildMetaValue` growth for `meta::Type`/`meta::Attr` (§9.1) |
| prelude (`Resolver.cpp`) | `meta::Type`, `meta::Attr`, `meta::AttrField` classes (§9.1, demand-gated) |
| `src/AstPrinter.cpp` / new `SourcePrinter` | source-shaped `--expand` (§6); `ForSplice` stmt rendering |
| `src/main.cpp` | `--expand` → source-shaped, `--ast-after-rules` → keep AST dump (demux the existing alias); `reentrant`-budget flag |
| `src/Resolver.cpp`/`Checker.cpp` | pass-1 comptime-root pre-check (§8); `provenanceId` consulted in diagnostics (§5) |
| `docs/reference.md` | Layer D section (currently says "Phase 4"); `$body`; `rewrites`/`replace` |

**Commit sequence** (each independently green, pushed as it lands — the aggressive-VC
discipline the Phase 1–3 arc followed):

1. **Body-replacing rules** (A) — `rewrites`/`replace`/`$body`, M30/M31/M32/M35. The
   headline capability; everything downstream (confluence, reentrant) is detection *around*
   it. Corpus: `rule_memoize` + `rule_timed`, each with a hand-written twin.
2. **Confluence** (B) — M33, the shared-anchor conflict marks; folds the existing
   `injectMember` collision into the unified family. Metatests: two-`replace` conflict,
   `replace`+additive defined order, `distinct` escape.
3. **`reentrant`** (C) — the gated fixpoint + M34 budget. Corpus: a two-rule chain (rule A
   emits an attribute rule B consumes) behind `reentrant`; a non-convergence hitting M34.
4. **Source-shaped `--expand`** (E) + its round-trip acceptance test (subsumes P). The
   verifiable-artifact upgrade; independent of 1–3.
5. **Pass-1 comptime pre-checking** (G) — diagnostic polish, standalone.
6. **Provenance IDs** (D) — *only if* a real diagnostic is shown ambiguous; else stays
   range-based. Demand-gated.
7. **Incremental caching** (F) — *only once* compile time is measured to regress; keys on
   D's stamps. Demand-gated, last.
8. **Deferred tail on demand** (H/I/J/K/L, the additive surface growths) — each its own
   small commit when a real driver appears. M/N/O/P are resolved-by-reference (M waits on a
   language feature, N folds into F, O rides A, P rides E).

Items 1–5 are the buildable Phase 4. Items 6–7 are demand-gated (built when a measurement
or a confusing diagnostic triggers them). Item 8 is the tail, each piece pull-based.

---

## 12. Testing plan

- **Corpus (oracle==IR==ELF, twin-verified where a hand-written equivalent exists):**
  - `rule_memoize` — `@Memo` on a pure method; `replace` wraps the body with a comptime-
    seeded cache-check using `$body` in statement position; twin proves identical output.
  - `rule_timed` — proposal §10.5 verbatim (`$body` capturing a return value via the
    arrow-body/single-return expression-position path); twin.
  - `rule_body_replace_arrow` — the minimal `=> expr` case, `$body` in expression position.
  - `rule_reentrant` — a rule chain that only terminates because the fixpoint converges;
    plus a `--expand` snapshot showing the two-round expansion.
  - `expand_roundtrip` — **the strong one**: `--expand` a meta-heavy program, compile the
    output as a fresh `.ext`, assert byte-identical run output (§6 acceptance).
- **metatests negatives:** one per M30–M35 — `replace` in an additive rule (M30), `inject`
  in a rewriter (M30), block-body `$body` in expression position (M31), zero-`$body` and
  double-`$body` templates (M32), two `replace`s on one body (M33), `member of` same-
  name+type non-`distinct` collision folded into M33, `reentrant` non-convergence (M34),
  `rewrites body of` a field (M35).
- **Determinism:** the `expand_roundtrip` test *is* the determinism guarantee for the
  rewriter output; `rule_reentrant`'s round-count is asserted stable.
- **Confluence composition:** a method carrying both an additive `top of body` and a
  `replace` — assert the §2.4 defined order (additive-then-replace) in the twin.
- **Zero-cost guard** unchanged: the whole legacy corpus stays byte-identical (`hasMeta`
  false → none of this runs); a program with rules but no `rewrites`/`reentrant` is
  unaffected by Phase 4 (the new branches are gated on the new markers).

---

## 13. Open calls for Leonard

| Call | Position taken here (recommendation) |
|---|---|
| **`$body` block-value in expression position** — auto-wrap in an IIFE, or M31 error? | **M31 (error)** for v1 — auto-wrap breaks P1 cost-identity silently. Revisit if the block-value wrapper case proves common. |
| **`rewrites body of <bind>` — require the explicit `body of m`, or allow bare `rewrites` with an implicit subject target?** | **Require explicit** — matches the proposal §10.5 spelling and reads at the rule header exactly what gets replaced. Bare `rewrites` stays the current parse-error until then. |
| **`reentrant` round budget default** | 8 rounds (small; a real chain converges in 1–2; 8 catches a runaway without masking a slow-but-legitimate fixpoint). `--comptime-budget`-family override. |
| **Provenance IDs (D) and caching (F): build now or hold for a trigger?** | **Hold** — both are demand-gated by explicit criteria (a confusing diagnostic; a measured compile-time regression). Building them speculatively pays a real cost (every diagnostic call site for D; a cache-invalidation surface for F) against a benefit that may not materialize. |
| **`meta.*` growth (H/I): grow the surface now, or wait for a `where`/`$for` driver that needs it?** | **Wait** — every field is a forever-API. The string form covers today's predicates; grow when a real predicate needs structure. |

---

## 14. Implementation notes (completed 2026-07-10)

The buildable scope (§11 items 1–5) is implemented, tested, and merged. Items 6–7
and the tail (§9) are deliberately **not** built — per this doc's own §13
recommendations they are demand-gated / pull-based, and no trigger has appeared.

**What landed (each an independently-green commit, per §11):**

1. **Body-replacing rules (A) — §2.** Grammar `rule N rewrites body of <bind> {
   … replace \`…\` }`; `rewrites`/`reentrant` are contextual header markers,
   `replace` an action verb. `AnchorKind::BodyReplace` + an `expand()` branch
   overwrite the subject's `memberBody`. `$body` is handled by a `verbatimClone_`
   path (no hygiene rename / def-site qualify — the original body moves as one
   unit) with statement- and expression-position forms per §2.2. `runRules()`
   now sweeps in two passes (additive then rewriters) for the §2.4 defined order.
   Diagnostics M30/M31/M32/M35 as specified. Corpus: `rule_body_replace_arrow`,
   `rule_timed`(+twin), `rule_memoize`(+twin).
2. **Confluence (B) — §3.** Two-`replace` M33 rode item 1; item 2 folded the
   `member of` same-name+type collision into the unified M33 family (naming both
   rules when both were injected) and added the §4.3 `distinct` escape.
3. **`reentrant` (C) — §4.** `runReentrantFixpoint` re-indexes and re-runs only
   `reentrant` rules to a fixpoint, converging via a `(rule, decl)` fired-pair
   set; `resolveNewAttrs` + `cloneStmt` attr-carrying let a rule match an
   attribute a prior round emitted. Budget `ComptimeOptions.reentrantRounds`
   (default 8, `--reentrant-budget`), M34 on non-convergence. Corpus:
   `rule_reentrant`.
4. **Source-shaped `--expand` (E) — §6.** `printProgramSource` re-emits compilable
   Leviathan; `--expand` → source, `--ast-after-rules` → the AST dump (demux).
   Provenance `// from rule …` comments via template-span containment. The strong
   acceptance: `tests/run_expand_roundtrip.sh` round-trips every meta program
   (24 pass; 1 `@no-roundtrip`), wired as `corpus_meta_expand_roundtrip`. This
   **subsumes item P** (§9.8).
5. **Pass-1 comptime pre-check (G) — §8.** `Checker::checkComptimeRoot` types a
   failed comptime root at a scope-complete position (tracked by
   `comptimeScope_`), into a throwaway sink, only on eval failure — a pure,
   regression-free message upgrade.

**§13 open calls — resolved as recommended:** `$body` block-value in expression
position → M31 (no IIFE); `rewrites body of <bind>` → explicit form required;
reentrant budget default 8; provenance IDs (D) and caching (F) → held (no
trigger); `meta.*` growth (H/I) → held.

**Notable findings (honest edges):**

- **Expression-position `$body` + an additive body rule interact by design.**
  An additive `top of body` rule makes a body multi-statement, so a later
  expression-position `$body` (which needs a single value expression) then
  correctly errors M31. Statement-position `$body;` composes with additive rules
  freely; the corpus `expand_roundtrip` fixture uses that split deliberately.
- **Reentrant non-convergence via `member of` hits M33 first**, not M34 — a
  fixed-name member re-injected into one class collides. A genuine M34 needs a
  regenerating injection that does not collide (e.g. `namespace N`), which the
  `rule_reentrant`/M34 tests use.
- **Source-printer fidelity:** lambda params re-emit **names only** (typed params
  from `$_params` forwarding do not reparse — types infer from context); string
  literals keep their raw quoted token; `@no-roundtrip` marks programs whose
  comptime writes stdout (that compile-time output interleaves with the dump).
- **The comptime pre-check's *visible* effect is bounded** by upstream leniency
  (the oracle rarely fails cleanly; the checker returns `Unknown` for many
  shapes rather than hard-erroring). The mechanism is correct and safe; it
  surfaces a better message exactly when a failing comptime root also produces a
  hard checker error — matching §8's "polish" framing.

**Deliberately not built (demand-gated / pull-based, per §11 & §13):** D
(`provenanceId`), F (incremental caching), and the §9 tail H/I/J/K/L; M/N/O/P are
resolved-by-reference (M waits on a language feature, N folds into F, O rides A,
P is subsumed by item 4's round-trip test).

---

*Companion to `designs/complete/techdesign-metaprog-phase3.md` (Phases 1–3, feature-complete) and
`designs/complete/techdesign-metaprogramming.md` (master design; Phase-4 checklist points here).
Implemented per the §11 commit sequence — items 1–5 built; 6–7 demand-gated; 8 pull-based.*
