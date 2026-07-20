# Tech Design: Rule-Template Splice Extensions — Overview

**Status:** design for review — no implementation yet. Closes
`designs/requests/request-metaprog-splices.md` (LA-15/16/17/22, plus the 2026-07-13
class/struct addendum). Companion to `designs/complete/techdesign-metaprog-phase4.md`
(Layers B–D, landed) and `designs/requests/accepted/request-metaprog-attr-values.md`
(the values side — **what** a template may read; this ticket is **where** a bound thing
may be spliced).

**Grounding commit:** `cc071c3` (current `agent2` HEAD). Every "already works / is a
parse error today" claim below was **run live against `build/leviathan` at this tree** —
the spike transcripts are inlined here in §2 and referenced by the per-item files, not
paraphrased. One spike overturned the request's own premise (item A, see §2.1) and one
surfaced an adjacent constraint the request did not mention (§2.7).

**Decision, up front.** The request lists six asks (A–F; D is self-superseded) plus an
addendum. The spikes reclassify them:

| Ask | Request's claim | **Live finding** | Verdict |
|---|---|---|---|
| **(A)** member-selector `this.$f` | "fails today" (cites `Rules.cpp:1522-1534`) | **already works** in both array-expr and statement position | **close as done** — add corpus, no engine change (§2.1, file 01) |
| **(B1)** ident/type-position splice of comptime strings (`$f.type`, `$p.name` as type + ident) | new | parse error `expected member name` | **new** — fragment-parser + `cloneType`/`cloneExpr` (file 01) |
| **(B2)** `$if (pred){…}$else{…}` conditional splice | new | parse error | **new** — its own primitive (file 02) |
| **(C)** `$ident(a,b)` name synthesis | new | parse error | **new** — fragment-parser + collision guard (file 01) |
| **(E)** named splice anchor `@Name();` / `at splice Name` | new (supersedes D) | the `Marker` anchor exists but is *subject-implicit*; cross-decl `at marker` fails | **new** — generalize `Marker` to a program-global site (file 03) |
| **(F)** `@attr(A,B);` grouped attributes | new (interim: individual decorators) | parse error | **new** — pure parse desugar (file 04) |
| **addendum** subject `class`/`struct` symmetry | ergonomics | `on class C` silently no-fires on `@Serializable struct S` | **new** — one-site matcher leniency (file 04) |

So **item (A) needs no code** — it was shipped by the attr-values landing (2026-07-19)
*after* this request was written (2026-07-07). The real engine work is **B1, B2, C, E**
(opus complexity — metaprog engine, real design decisions) and **F + addendum** (sonnet —
a parse desugar and a one-line matcher relaxation, no decision-making left once this doc
picks the spelling). D is dropped per the request's own supersession.

---

## 0. The files in this design

Split by implementation session (`docs/policies.md` "one design = one session"; 5 files
⇒ a folder):

| File | Items | Complexity | One-liner |
|---|---|---|---|
| **00** this overview | — | opus | map, spikes, shared pipeline, sequencing, shared diagnostics |
| **01** `-positions-opus` | A (close), B1, C | opus | the hole/clone splice-position family — `cloneExpr`/`cloneType`/`cloneStmt` + fragment parser |
| **02** `-conditional-opus` | B2 | opus | `$if/$else` — expansion-time branch selection |
| **03** `-named-anchor-opus` | E | opus | `@Name();` splice sites + `at splice Name` — cross-decl anchor |
| **04** `-desugars-sonnet` | F, addendum | sonnet | `@attr(A,B);` desugar + `on type C` / subject-kind leniency |

Each file is independently reviewable and mergeable; the only cross-file dependency is
that B1's *type-position* path (file 01) and B2's *typed fragments* (file 02) both touch
the same fragment parsers, so if both land they share the reserved-hole-head recognition
described in §3.3 — the sequencing in §5 orders them so the second inherits the first's
scaffold rather than re-adding it.

---

## 1. The shared machinery — the splice pipeline every item plugs into

All six asks are variations on one existing pipeline. Reading it once here means the
per-item files can point at line numbers instead of re-explaining it.

**Parse.** A rule's `inject \`…\`` template is a *quasiquote*: `parseRuleAction`
(`Parser.cpp:1606-1678`) records the backtick span, then re-lexes the payload **with holes
enabled** (`Lexer` `allowHoles=true`, `Lexer.hpp:13-16`) so `$name` lexes as a single
`Identifier` token whose text keeps the `$` (`Lexer.cpp:233-245`). The anchor picks which
*fragment parser* runs over those tokens (`Parser.cpp:1671-1677`):
`parseMemberFragment` (`member of`), `parseItemsFragment` (`namespace N`), or
`parseStmtsFragment` (everything else, `Parser.cpp:1685+`). The template is parsed **once**,
at rule-declaration time, into an ordinary AST subtree with `$`-prefixed leaves.

**Match + bind.** `tryMatch` (`Rules.cpp:1526`) binds names: the subject (`m`/`C`) as a
`declStmt` binding (`Rules.cpp:1570-1579`), enclosers, the fired attribute as an `Object`
value (`Rules.cpp:1553-1567`), and `$for` loop variables. `materializeBindings`
(`Rules.cpp:1512`) turns each into a comptime `Value` for `where`/`$for` evaluation;
`buildMetaValue` (`~Rules.cpp:1460-1510`) builds the `meta::Field`/`meta::Method` object
(its `name`, `type`, `params` fields — the ones B1 splices).

**Expand.** `expand` (`Rules.cpp:1735`) clones the template subtree per firing through
`cloneExpr`/`cloneStmt`/`cloneType`/`cloneParam`, substituting holes as it goes, then
places the clone at the anchor. The hole-substitution sites — **the exact seams every new
splice position hooks** — are:

| Seam | File:line | Handles today |
|---|---|---|
| `cloneExpr` member-selector hole (`this.$m`) | `Rules.cpp:2194-2225` | decl-name **and** `$for`-bound `meta.*` object → its `name` field |
| `cloneExpr` name-hole (`$C`, bare `$m`) | `Rules.cpp:2235-2254` | decl name; primitive/array value reify |
| `cloneExpr` free-name def-site qualify | `Rules.cpp:2255-2304` | hygiene: template-local rename, `NS::` qualification |
| `cloneType` type-position hole (`$C` as a type) | `Rules.cpp:2347-2372` | **declStmt only** — the gap B1 fills |
| `cloneStmt` decl-name / selector hole | `Rules.cpp:2424-2449` | decl name; `$for`-bound object name |
| anchor placement dispatch | `Rules.cpp:1746-1875` | ctor/body/member/marker/namespace/replace |

Two invariants the files preserve. **Hygiene** (`Rules.cpp:2255-2304`): template-declared
locals alpha-rename; free names def-site-qualify to the rule's namespace or stay bare for
use-site resolution. **Provenance**: `--expand` re-emits the post-rules tree as compilable
source (spikes below use it as ground truth), and every new node carries the call-site span.

---

## 2. The spikes (run at `cc071c3`)

The load-bearing evidence. Each is reproduced in the relevant per-item file; collected here
so the reclassification table in the header is auditable in one place.

### 2.1 (A) `this.$f` — the request's premise is stale, it already works

The request (§1A) says a `$for`-bound field after `.` "is currently treated as an
attribute-value reification … so the second `$for` fails today," citing `Rules.cpp:1522-1534`.
That code moved; the member-selector seam is now `Rules.cpp:2194-2225` and **explicitly
handles a `$for`-bound `meta.*` object in selector position** (splices its `name` field —
comment at `Rules.cpp:2204-2211`). Live, in the request's exact array-expression shape:

```
$ build/leviathan --expand spikeA2.ext      # inject `Array<int> vals() => [ $for f in C.fields : this.$f ];`
    // from rule J::s @ 9:1
    public Array<int> vals() => [this.x, this.y];
$ build/leviathan --run spikeA2.ext
1,2
```

And in statement position (matching the landed `tests/corpus/meta/rule_stmt_for.ext`, which
already writes `this.$f.toString()`): also green. So the toJson "Mechanism A" the request
gates on **is expressible today**; file 01 closes (A) with a corpus twin proving it, and
records the one adjacency the request did not (§2.7).

### 2.2 (B1) `$f.type` in type / identifier position — new

```
$ build/leviathan --run spikeBtype.ext      # inject `$for f in C.fields : $f.type copy_$f = this.$f;`
spikeBtype.ext:5:40: error: expected member name
          inject `$for f in C.fields : $f.type copy_$f = this.$f;` ...
```

Two gaps: `$f.type` is not accepted as a **type token** (`cloneType` only substitutes
`declStmt` holes, `Rules.cpp:2360-2365`, not a string-valued `meta.*` field), and
`copy_$f` is not accepted as an **identifier fragment** (no name-concatenation in the
fragment lexer). File 01.

### 2.3 (B2) `$if (pred){…}$else{…}` — new

```
$ build/leviathan --run spikeBif.ext
spikeBif.ext:5:37: error: expected '('    # `$if` lexes as a hole-name, then the parser is lost
```

There is an **item-level `comptime if` fold** already (`Rules.cpp:88`, `Rules.cpp:990-1100`
"splice, don't nest"), but it operates on real source items, not inside a quasiquote
template. `$if` is its expansion-time analogue and is unbuilt. File 02.

### 2.4 (C) `$ident(a,b)` — new

```
$ build/leviathan --run spikeC.ext          # inject `class $ident(C.name, "Cols") { }` ...
spikeC.ext:5:29: error: expected '{'
```

No identifier-synthesis primitive. File 01 (shares the fragment-parser reserved-head with
B1's `copy_$f`).

### 2.5 (E) cross-decl named anchor — new; the `Marker` anchor is subject-implicit

```
$ build/leviathan --run spikeE.ext          # rule matches @Injectable class; injects `at marker "binds"`;
                                            #   the @anchor("binds") sits in a DIFFERENT function addBindings()
spikeE.ext:5:9: error: marker "binds" not found in 'UserService'
```

The `Marker` anchor resolves against `subjectOf(b)` — the matched declaration's own body
(`Rules.cpp:1806-1822`). Item (E) needs the marker to be a **program-global site** the rule
reaches regardless of which declaration it matched. File 03.

### 2.6 (F) `@attr(A,B);` grouped attrs, and the addendum — new

```
$ build/leviathan --run spikeF.ext          # @attr(PrimaryKey, AutoIncrement); before a field
spikeF.ext:4:37: error: expected type name

$ build/leviathan --run spikeK.ext          # `match @M on class C` rule; subject is `@M struct S`
spikeK.ext:3:1: warning: attribute '@M' matched no imported rule (missing 'uses K'?)
Uncaught RuntimeException: cannot resolve call target 'z'      # rule silently did not fire
# twin with `on struct C` prints 9 — subject-kind matching is exact (Rules.cpp:1341-1350)
```

Both in file 04. Note the addendum's trap is *worse than silent*: the dangling-attr warning
(`Rules.cpp:2607-2638`) misattributes the no-fire to a missing `uses`, sending the author
down the wrong path.

### 2.7 Adjacent constraint surfaced by the spikes (not in the request)

A `member of C` inject carries **one** member (`RuleAction::tmplMember` is a single `Stmt`,
`Parser.cpp:1673`); a two-member template silently keeps only the first:

```
$ build/leviathan --run spike2mem.ext       # inject `int a() => 1; int b() => 2;` at member of C
1
Uncaught RuntimeException: cannot resolve call target 'b'
```

None of A–F needs multi-member `member of` (the toJson/serializer cases inject one method,
or use `$for` to repeat one member shape). File 01 records it as a **known constraint with a
diagnostic gap** — today `b()` vanishes with no error — and proposes the minimal fix (parse
the member fragment as a list, or at least error on trailing tokens). Flagged, not bundled.

---

## 3. Cross-cutting design decisions

### 3.1 No new layer, no reentrancy change, hygiene unchanged

The request's own constraint (§preamble): "no new layer, no reentrancy, hygiene rules
unchanged." Every item honors it. B1/C/F/E add **splice positions and anchors**, not new
evaluation phases; B2 adds a template control-flow node folded during the *existing* clone
pass. The `reentrant` fixpoint (`Rules.cpp` §4) is untouched — generated code is not
re-scanned for `$if`/`$ident` (M24's "generated output is never re-macro-expanded" extends
unchanged).

### 3.2 Comptime-string splices are alpha-hygiene-exempt by construction

A name synthesized by `$ident(…)` (C) or a type spliced from `$f.type` (B1) is a **string
chosen by the rule author**, not a template-declared local — so the alpha-rename machinery
(`Rules.cpp:2255-2304`) must *not* fire on it (it would rename a name the author intends to
be use-site-visible). The files route these through the same channel as the existing
decl-name splice (`declRefName`, which is already rename-exempt), and — critically for C —
add the **collision check** the request demands: a synthesized name colliding with anything
in the target scope is a rule-stage error naming both sites (the `injectMember` M33 pattern,
`Rules.cpp:2569-2604`, generalized to `namespace`-scope decls). This is the bug.md #22
guardrail the request calls out for (C).

### 3.3 Reserved hole-heads: `$if`, `$else`, `$ident`

`$if`/`$else`/`$ident` all lex today as ordinary hole-name `Identifier` tokens (text
`"$if"` etc.) — harmless, because a user rule binding a name *literally* `if`/`ident` is
already impossible (keyword / and these are additive). The fragment parsers gain a single
recognition point: an `Identifier` whose text is one of these reserved heads dispatches to
the new production instead of the generic hole path. This is additive and greppable; the
per-item files (02 for `$if`/`$else`, 01 for `$ident`) share the constant table introduced
here so there is one list of reserved heads, not two.

### 3.4 Diagnostics are rule-stage, named, and `--expand`-visible

Every new failure mode is a compile-time diagnostic at the rule/template site (never a crash
or silent drop), consistent with the M-series (`Rules.cpp` M25–M36). The shared numbering:

| Code | Item | Trigger |
|---|---|---|
| **M37** | C | `$ident(…)`-synthesized name collides with an existing decl in scope (names both sites) |
| **M38** | C | `$ident(…)` argument is not a comptime `string` (or not name-legal after concatenation) |
| **M39** | B1 | `$f.type` / `$p.type` used in type position but the bound value has no reifiable `type` field |
| **M40** | B2 | `$if` condition is not a comptime `bool` (mirrors the `where`-clause M at `Rules.cpp:1614`) |
| **M41** | B2 | `$if`/`$else` structurally malformed (e.g. `$else` without a preceding `$if`, unbalanced fragment kinds across branches) |
| **M42** | E | `at splice Name` finds zero (or, without `multi`, more than one) `@Name()` site in the program |
| **M43** | E | `@Name()` splice-marker attribute is not a declared `attribute`, or is used in declaration position (it is statement-position only) |

Codes are reserved contiguously so the four files don't collide; each file owns the rows it
introduces and states the exact message shape.

### 3.5 `--expand` remains the acceptance oracle

Per the request's acceptance (§2.2): "`--expand` renders all splices." Each item's corpus
pair is a rule example + a hand-written twin whose `--expand`/`--run`/`--ir` output is
byte-identical — the `rule_routes`/`rule_orm` discipline already in `tests/corpus/meta/`.
The overview's spikes already exercise `--expand` as truth (§2.1); the files promote each
spike to a checked fixture.

---

## 4. What this unblocks (from the request, for the record)

- **(A)** Track 03 toJson "Mechanism A" / ORM appliers — *available now* (§2.1); the flip is
  a corpus + deletion of Mechanism B, no engine wait.
- **(B1/B2)** Era-B typed handler binding (Track 02): `$p.type` handler signatures + one
  binding rule emitting path/query/body variants via `$if` instead of one-rule-per-combination.
- **(C)** Track 06 compile-checked column descriptors (`class UserCols`), replacing
  boot-validated `col("name")` strings.
- **(E)** Track 04 `@Injectable` DI wiring and R10 route registration — generated `bind`/
  `AddRoute` statements land at a user-chosen, lexically-scoped `@InjectBindings()` /
  `@InjectRoutes()` site, dissolving the marker-file/collect() indirection.
- **(F)** entity spelling sugar `@attr(PrimaryKey, AutoIncrement);`.
- **addendum** collapses Track 03's five class/struct rule *pairs* to five rules and removes
  the silent-no-fire trap for every future "works on DTOs and entities" contract.

---

## 5. Sequencing

Independent-first, decision-load ascending:

1. **File 04 (F + addendum), sonnet** — pure parse desugar + one-line matcher leniency; no
   dependency on anything else; immediately removes the addendum's silent trap. Land first.
2. **File 01 (A close + B1 + C), opus** — (A) is corpus-only (land immediately, in parallel
   with 04). B1 + C introduce the fragment-parser reserved-head scaffold (§3.3) and the
   `cloneType`/name-synthesis + M37/M38/M39 collision machinery.
3. **File 02 (B2), opus** — reuses file 01's reserved-head recognition for `$if`/`$else`.
   Land after 01 so it extends the scaffold rather than forking it.
4. **File 03 (E), opus** — orthogonal to 01/02 (anchor side, not hole side); can proceed in
   parallel with 02. Sequenced last only because it is the largest single new mechanism
   (program-global splice-site index).

Each step is corpus-gated and independently green before the next.

---

## 6. Out of scope

- **(D)** cross-decl marker anchor — self-superseded by (E) in the request; not designed.
- Layer-D `rewrites`/`$body` extensions — separate ticket (attr-values D).
- Named/typed-arg attribute plumbing beyond what F needs — `request-metaprog-attr-values.md`
  items C/G, not here.
- Multi-member `member of` (the §2.7 adjacency) — flagged in file 01 with a minimal fix, not
  bundled into any item's acceptance.
- Any runtime-reflection surface — everything stays compile-time, cost-identical to
  hand-writing (the language's standing metaprog invariant).

---

## 7. Source map (shared; per-item files add their own)

| Topic | File:line |
|---|---|
| `parseRuleAction` anchors | `src/Parser.cpp:1606-1678` |
| fragment parsers (`parseStmtsFragment` et al.) | `src/Parser.cpp:1685+` |
| `@anchor("name")` marker statement parse | `src/Parser.cpp:1000-1020` |
| lexer holes (`allowHoles`, `$name`) | `src/Lexer.hpp:13-16`, `src/Lexer.cpp:233-245` |
| `AnchorKind` enum | `src/Ast.hpp:296-311` |
| `tryMatch` (bind subject/attr/encloser) | `src/Rules.cpp:1526-1620` |
| `declKindMatches` (subject exact; addendum) | `src/Rules.cpp:1341-1350` |
| encloser class/struct leniency (the model for the addendum) | `src/Rules.cpp:1586-1589` |
| `materializeBindings` / `buildMetaValue` | `src/Rules.cpp:1512`, `~1460-1510` |
| `expand` anchor dispatch | `src/Rules.cpp:1735-1875` |
| `Marker` expansion + `findMarkerSlot` | `src/Rules.cpp:1806-1833`, `2502-2520` |
| `injectMember` / M33 collision (model for M37) | `src/Rules.cpp:2569-2604` |
| `cloneExpr` hole seams | `src/Rules.cpp:2170`, `2194-2225`, `2235-2304` |
| `cloneType` type-position hole | `src/Rules.cpp:2347-2372` |
| `cloneStmt` name/selector hole | `src/Rules.cpp:2424-2449` |
| dangling-attr warning (addendum's misleading path) | `src/Rules.cpp:2607-2638` |
| metaprog reference | `docs/reference.md:2149-2350` |
| landed corpus precedents | `tests/corpus/meta/rule_{stmt_for,member,marker,orm,attr_values}.ext` |
