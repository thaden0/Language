# Tech Design 04 — Desugars: `@attr(A, B);` grouped attributes (F) + subject `class`/`struct` symmetry (addendum)

**Part of** `designs/metaprog-splices/` (see `-overview-opus.md`). **Complexity:** sonnet —
both are mechanical: F is a pure parse-level desugar with a working interim spelling, and the
addendum is a one-site matcher relaxation whose *decision* is made in this doc (leaving no
design choices at implementation time). No engine-semantics or codegen risk. **Grounding
commit:** `cc071c3`, spiked live. Independent of files 01–03 — land first (overview §5).

Covers **(F)** (request §1F, LA-26, P2) and the **2026-07-13 addendum** (LA-... subject-kind
`class`/`struct` symmetry, P2).

---

## 1. (F) Statement-position grouped attributes `@attr(A, B);`

### 1.1 Ask and spike

Request §1F: the entity spelling `@attr(PrimaryKey, AutoIncrement);` as a statement preceding
a field, where the bare identifiers name declared attributes — **sugar for
`@PrimaryKey @AutoIncrement` decorating the next declaration**. Pure parse-level desugar; the
interim spelling (individual decorators) "works today and stays legal."

```
$ build/leviathan --run spikeF.ext          # @attr(PrimaryKey, AutoIncrement); before a field
spikeF.ext:4:37: error: expected type name
```

Confirmed new (the parser reads `@attr(` as the start of a decorated declaration and chokes).
The interim spelling is verified working — `@PrimaryKey @AutoIncrement int id;` parses today
(ordinary stacked decorators).

### 1.2 Design — desugar at parse, before anything else sees it

`@attr(Name1, Name2, …);` in **statement/member position** desugars to attaching
`@Name1 @Name2 …` to the **next declaration** in the same body. This happens entirely in the
parser's statement/member loop; nothing downstream (resolver, rule engine, checker) learns a
new node type.

Mechanics, mirroring the existing `@anchor("name")` statement fork (`Parser.cpp:1000-1020`,
which already special-cases `@` at statement start):
1. On `@` followed by the contextual identifier `attr` and `(`, parse a **comma-separated
   list of bare attribute-name identifiers** (optionally `NS::`-qualified paths, same as an
   ordinary decorator's name), then require `)` `;`.
2. Buffer these as pending `AttrUse`s (the same struct a normal `@Decorator` produces).
3. Attach them to the **next parsed declaration** in the body — prepended to whatever
   decorators that declaration already carries, so `@attr(A); @B int x;` == `@A @B int x;`.
4. A trailing `@attr(…);` with **no following declaration** in the body is a parse error
   (message: `"@attr(...) must precede a declaration"`) — never silently dropped.

Because the output is *identical* `AttrUse` nodes to the stacked-decorator spelling, rule
matching, attribute-value reflection, `uses`-scoping, and `--expand` all treat the two
spellings as the same program. `--expand` may canonicalize to stacked decorators (the
interim spelling) — acceptable, since the request states both are legal and equivalent; the
round-trip stays compilable.

`attr` is a **contextual keyword** only in this `@attr( … );` statement shape — a user
attribute or symbol literally named `attr` elsewhere is unaffected (the fork requires
`@` `attr` `(` at statement start, a shape no existing valid program uses — it is a parse
error today).

### 1.3 Change map (F)

| # | Change | File(s) | Size |
|---|---|---|---|
| 1 | `@attr(A, B);` statement fork → pending `AttrUse`s attached to next decl; error on trailing | `src/Parser.cpp:1000-1020` (beside `@anchor`) + the body decl loop | ~30 lines |
| 2 | Corpus + reference.md | `tests/corpus/meta/rule_attr_group{,_twin}.ext`, `docs/reference.md:2127-2148` (attributes) | test/doc |

No `Ast.hpp`, `Rules.cpp`, or `Resolver.cpp` change — the desugar produces existing nodes.

### 1.4 Testing (F)

- `rule_attr_group.ext` + `rule_attr_group_twin.ext` — `@attr(PrimaryKey, AutoIncrement); int id;`
  vs the stacked `@PrimaryKey @AutoIncrement int id;` twin; assert **byte-identical
  `--ast`/`--expand`/`--run`** (they must parse to the same tree). A rule matching `@PrimaryKey`
  fires identically on both spellings.
- `rule_attr_group_dangling.ext` (red) — `@attr(A);` at end of a body → the "must precede a
  declaration" error.

---

## 2. Addendum — subject `class`/`struct` matching symmetry

### 2.1 The trap (spiked)

Encloser-kind matching (`in class C`) is deliberately lenient: `class` also matches `struct`
and `interface` ancestors (`Rules.cpp:1586-1589`). Subject-kind matching (`on class C`) is
**exact** — `declKindMatches` (`Rules.cpp:1341-1350`) requires `di.kindWord == kind` (with
only the `method`≡`function` equivalence). Consequence, confirmed:

```
$ build/leviathan --run spikeK.ext          # rule: match @M on class C ... ; subject is `@M struct S`
spikeK.ext:3:1: warning: attribute '@M' matched no imported rule (missing 'uses K'?)
Uncaught RuntimeException: cannot resolve call target 'z'      # rule silently did not fire
# `on struct C` twin → prints 9
```

Worse than silent: the dangling-attribute warning (`Rules.cpp:2607-2638`) **misattributes**
the no-fire to a missing `uses`, sending the author down the wrong trail. Any contract that
must cover both value structs and reference classes (Track 03's `@Serializable` on DTOs *and*
entities) needs a full `class`/`struct` **pair** of every subject-position rule, with no
compiler help for a missing twin.

### 2.2 The decision

The request offers two spellings; this doc **picks (b) — a unifying subject kind-word `on
type C`** — and *also* keeps (a)'s spirit by making the choice explicit rather than implicit:

- **`on type C`** matches `class` **or** `struct` **or** `interface` (the subject-position
  analogue of the encloser's existing leniency). New, explicit, opt-in.
- **`on class C` stays exact**, and **`on struct C` stays exact** — authors who want
  class-only or struct-only keep it, and no existing rule changes behavior.

**Why (b) over (a) (making `on class` also match `struct`):** silently widening `on class` to
match structs is a behavior change to every existing rule and erases the author's ability to
be class-only — exactly the property `on struct C`'s exactness exists to give (the addendum
notes it). A new `on type` keyword is additive, self-documenting at the rule site ("I mean
both"), and collapses Track 03's five class/struct pairs to five `on type` rules without
touching anyone who wrote `on class` deliberately. It mirrors the encloser clause's intent
while keeping subject exactness the default — the addendum's own framing ("the exactness is
the documented default").

### 2.3 Design

Two one-liners plus a diagnostic fix:

1. **`type` as a subject kind-word.** In `declKindMatches` (`Rules.cpp:1341-1350`), add: when
   the rule's `kind == "type"`, match `di.kindWord ∈ {class, struct, interface}`. (Parse:
   `on type C` — `type` accepted as a kind-word in the subject clause, `Parser.cpp` rule-match
   parsing beside the existing `class`/`struct`/`field`/… set.)
2. **Bindings unaffected.** The subject binds as its actual declaration
   (`Rules.cpp:1570-1579`), so `$C.name`, `C.fields`, etc. reflect the real struct/class — a
   `type`-matched rule reads the concrete kind with no new surface.
3. **Diagnostic-trap fix (independent, ship regardless).** The misleading "missing `uses`"
   warning fires because a `class`-only rule *exists* for `@M` but did not match the struct.
   Tighten `warnDanglingAttrs` (`Rules.cpp:2620-2628`): only suggest "missing `uses`" when a
   rule for that attribute exists **whose subject kind could match this declaration's kind**;
   otherwise emit a distinct note — `"@M has a rule 'K::r' that matches 'class' subjects, but
   this is a 'struct' — did you mean 'on type' / 'on struct'?"`. This removes the wrong-trail
   footgun even for authors who keep `on class`/`on struct` exact.

### 2.4 Change map (addendum)

| # | Change | File(s) | Size |
|---|---|---|---|
| 1 | `on type C` kind-word parse in rule-match subject clause | `src/Parser.cpp` (rule-match subject parse) | ~5 lines |
| 2 | `declKindMatches`: `type` matches class/struct/interface | `src/Rules.cpp:1341-1350` | ~4 lines |
| 3 | `warnDanglingAttrs`: kind-aware hint (fix the misleading `uses` warning) | `src/Rules.cpp:2620-2628` | ~10 lines |
| 4 | Corpus + reference.md subject-kind list | `tests/corpus/meta/`, `docs/reference.md:2166-2171` | test/doc |

### 2.5 Testing (addendum)

- `rule_subject_type{,_twin}.ext` — one `on type C` rule firing on **both** a `@M class` and a
  `@M struct` in one program, byte-identical to the hand-written class+struct twin pair.
- `rule_subject_class_exact.ext` — `on class C` still does **not** fire on a struct (exactness
  preserved), and now emits the **corrected** kind-aware note instead of the misleading
  `uses` warning.
- `rule_subject_struct_exact.ext` — `on struct C` unchanged.
- Regression: every existing subject-position corpus (`rule_member`, `rule_orm`, …) unchanged —
  no `on class` rule alters behavior.

---

## 3. Out of scope

- Widening `on class` to match structs (spelling (a)) — rejected in §2.2; would change every
  existing rule.
- `@attr(...)` in **declaration** position (redundant with stacked decorators) — statement
  position only, matching the request's spelling.
- `@attr(...)` carrying **arguments** per attribute (`@attr(Column("x"), PrimaryKey)`) —
  deferred; the request's sugar is for bare marker attributes. Attributes needing args use the
  ordinary `@Column("x") @PrimaryKey` decorator spelling, still legal.

## 4. Definition of done

Both corpus sets green on `--run`/`--ir`/`--expand`; full meta suite regression-clean;
`reference.md` documents `@attr(A, B);` grouped-attribute sugar and the `on type C` subject
kind-word; Track 03 collapses its five `class`/`struct` rule pairs to five `on type` rules and
deletes the interim twins (`packages/atlantis/src/json/serializable.lev`). The misleading
dangling-attribute warning no longer fires on a kind-mismatched subject.
