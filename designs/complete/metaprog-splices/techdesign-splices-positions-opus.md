# Tech Design 01 — Splice Positions: `this.$f` (close A), `$f.type` (B1), `$ident(…)` (C)

**Status: IMPLEMENTED** (agent2, on top of the `refactor_1` TU split — `src/frontend/`,
`src/meta/`, `src/sema/`). Composite-identifier lexing (`src/frontend/Lexer.cpp`), the
dotted-hole type form and `$ident(...)` name-synthesis parse (`src/frontend/Parser.cpp`,
`ParserMeta.cpp`), the `TypeRef` hole fields / `Stmt::nameSynthArgs` (`src/core/Ast.hpp`),
and the clone-time splice + collision guard (`src/meta/RulesClone.cpp`,
`RulesExpand.cpp`) all ship. Diagnostics M37 (namespace-scope `$ident` collision), M38
(bad `$ident` arg / illegal synthesized identifier), M39 (unreifiable `$f.type` field)
fire with the exact message shapes this file specifies. The §1.4 `member of` multi-member
fix landed too (`RuleAction::tmplMembers` is now a list). Corpus:
`tests/corpus/meta/rule_{tojson,type_splice,ident_synth,member_multi}{,_twin}.ext` (green,
byte-identical twins, `--run`/`--ir`/`--expand` roundtrip) and
`tests/negative/rule_{ident_badarg,ident_collision,type_splice_badfield}.ext` (red, each
producing its exact designed diagnostic). `docs/reference.md` §Rules documents `$f.type`/
`$p.name` in type/identifier position, `name_$hole` concatenation, and `$ident(...)`
synthesis with its collision rule. Full meta corpus + C++ unit suites (parser/resolver/
checker/eval/meta) regression-clean.

**One documented, deliberately out-of-scope limitation** (pre-existing, not a defect in
this design): cross-namespace **type** consumption of a namespace-scope injected class
(`N::Foo x` from outside `N`) still fails — type resolution runs in resolve pass 1, before
the rule stage injects `N`, and the rule stage is gated on zero pass-1 errors
(`src/driver/main.cpp`). Cross-namespace *call* resolution is unaffected (the Checker runs
after injection). Reproduces with a literal class name too, so it predates this design.
Documented at `src/sema/Resolver.cpp` (the `::`-qualified type-resolution walk) and in
`docs/reference.md`; this file's own worked example (§3.5) follows the design's own
workaround and consumes the synthesized descriptor within its own namespace. A real fix
means deferring/softening pass-1 type errors across the whole meta pipeline — out of
scope here, filed as a follow-on if a track needs cross-namespace type consumption.

The reference attempt on branch `metaprog-splices-positions-attempt` (superseded by this
landing, built on the pre-`refactor_1` monolithic `Rules.cpp`/`Parser.cpp`) has been
deleted.

**Part of** `designs/metaprog-splices/` (see `-overview-opus.md`). **Complexity:** opus
(metaprog engine — `cloneExpr`/`cloneType`/`cloneStmt` + fragment parser + collision
semantics). **Grounding commit:** `cc071c3`, spiked live against `build/leviathan`.

Covers three asks that all live in the **hole → clone → splice** pipeline (overview §1):
- **(A) member-selector `this.$f`** — **already works**; this file closes it with a corpus
  twin and records the one adjacency the request missed.
- **(B1) identifier/type-position splicing of comptime strings** — `$f.type` as a type
  token, `$p.name` as an identifier, and identifier *concatenation* (`copy_$f`). New.
- **(C) `$ident(a, b)` name synthesis** — build a decl name from bound strings, with a
  rule-stage collision error. New.

---

## 1. (A) member-selector `this.$f` — close as done

### 1.1 The finding

The request (§1A) asserts `this.$f` "fails today," citing `Rules.cpp:1522-1534` as treating
`$f`-after-`.` as an attribute-value reification. **That is stale.** The member-selector
seam is now `Rules.cpp:2194-2225`, and it already handles a `$for`-bound `meta.*` object in
selector position — the comment at `Rules.cpp:2204-2211` documents exactly this
("`t.$m()` … splices its name as the member selector"). The path:

```cpp
// Rules.cpp:2195 — Decl-hole in member-name position: `this.$m` -> splice the selector.
if (e->kind == ExprKind::Member && isHole(e->text)) {
    ...
    } else if (it->second.hasVal && it->second.val.kind == VKind::Object && it->second.val.obj) {
        auto nit = it->second.val.obj->fields.find("name");   // $for-bound meta::Field/Method
        if (nit != ... && nit->second.kind == VKind::String) out->text = own(nit->second.s);
```

`own(...)` interns the name so the spliced `string_view` outlives the binding. This fires
identically whether `this.$f` appears in an **array-expression** element or a **statement** —
because both funnel through `cloneExpr`.

### 1.2 Live proof (promoted from overview §2.1)

The request's exact array-expression shape (`[ $for f : … this.$f … ]`):

```
$ build/leviathan --expand spikeA2.ext
    // inject `Array<int> vals() => [ $for f in C.fields : this.$f ];`
    public Array<int> vals() => [this.x, this.y];       # ← selector spliced per field
$ build/leviathan --run spikeA2.ext
1,2
```

Statement position is already a landed corpus fixture: `tests/corpus/meta/rule_stmt_for.ext`
writes `out = out.add($f.name + "=" + this.$f.toString());` and is green on
`--run`/`--ir`/`--expand`. So the request's toJson "Mechanism A" —
`[ $for f in C.fields : __atlEnc(this.$f) ]` — is a straight composition of two landed
capabilities (array `$for` splice + `this.$f` selector) and needs **no engine change**.

### 1.3 Acceptance for (A): a corpus twin

Add `tests/corpus/meta/rule_tojson.ext` + `rule_tojson_twin.ext` mirroring the request's
toJson shape (both a `$f.name` key array and a `__atlEnc(this.$f)` value array in one
generated method), plus the hand-written twin, asserting byte-identical `--run` output and
`--expand` legibility. This is the request's own acceptance item 1 for (A); it is the whole
deliverable for (A).

### 1.4 The adjacency to record (overview §2.7): single-member `member of`

`member of` carries **one** member — `RuleAction::tmplMember` is a lone `Stmt`
(`Parser.cpp:1673`, `parseMemberFragment`), and a two-member template silently keeps only
the first (`int a()=>1; int b()=>2;` → `b` vanishes, no error). The toJson case injects one
method so (A) is unaffected, but the **silent drop** is a real diagnostic gap. Minimal fix,
filed here, **not** bundled into (A)'s acceptance:

- Either make `parseMemberFragment` parse a **member list** (return `std::vector<StmtPtr>`;
  `expand`'s `member of` arm already loops `for (StmtPtr& member : members)`,
  `Rules.cpp:1757`, so the consumer is ready), **or**
- at minimum, **error M25-style on trailing tokens** after the first member so the drop is
  loud.

Recommendation: the list form (small, and the expand side already iterates). Sized in §5;
gated behind its own one-line corpus (`rule_member_multi.ext`).

---

## 2. (B1) identifier / type-position splicing of comptime strings

### 2.1 The two sub-gaps (spiked)

```
$ build/leviathan --run spikeBtype.ext      # inject `$for f in C.fields : $f.type copy_$f = this.$f;`
spikeBtype.ext:5:40: error: expected member name
```

The request (§1B first half) wants a bound value's `.type`/`.name` usable **as an
identifier and as a type token**: `$p.type local_$idx = ...;` and `$p.type::FromJson(v)`.
Decomposed:

1. **Type position.** `$f.type` (a `string`-valued field of the `meta::Field` object,
   built at `Rules.cpp:1503` as `v.obj->fields["type"] = vstr(decl->type->canonical)`) must
   be accepted where the grammar expects a *type name*. Today `cloneType` substitutes only
   **`declStmt`** holes (`Rules.cpp:2360-2365`):

   ```cpp
   if (isHole(t->name)) {
       auto it = b.find(t->name.substr(1));
       if (it != b.end() && it->second.declStmt) { out->name = declRefName(it->second); ... }
   }
   ```
   A `$f.type` is not a bare hole `$name` — it is a `Member` *expression* (`$f` `.` `type`)
   sitting in *type* position, which the fragment parser never accepts (hence "expected
   member name" — it tried to read `$f` then `.type` as a member access, not a type).

2. **Identifier position / concatenation.** `copy_$f` and `local_$idx` are **identifier
   fragments** stitched from literal text plus a hole. The lexer has no concatenation:
   `copy_$f` lexes as identifier `copy_` … then `$f` is a separate token — invalid.

### 2.2 Design

**Type position — a dotted-hole type form.** Extend the *type-name* production in the
fragment parsers (the same production `cloneType` mirrors) to accept a **`$bind.field`
dotted hole** where a type name is expected, recording it on the `TypeRef` as a
`holeField` pair (`{"$f", "type"}`). At clone time, `cloneType` (`Rules.cpp:2347`) gains a
branch: when `t` carries a dotted hole, look up `$f`, read its object's `type` (or `name`)
string field, and set `out->name` to that **string, verbatim, rename-exempt** — exactly as
the member-selector seam interns a name (§1.1). The resolved string is a canonical type
spelling (`decl->type->canonical`), so pass-2 resolves it as an ordinary type reference.

Only `.type` and `.name` are legal dotted-hole type fields (both are canonical-string fields
on `meta::Field`/`meta::Param`/`meta::Method`); any other field in type position is **M39**
(overview §3.4): `"'$f.<x>' has no reifiable type/name field usable as a type"`.

**Labeled-ctor position (`$p.type::FromJson(v)`).** This is the *type* dotted-hole (above)
followed by `::label` — i.e. a `Member` expr whose left is the spliced type. The type-side
substitution is the same; the `::FromJson` selector is ordinary source. Nothing special
beyond the type-position work, once the fragment parser accepts `$p.type` as the left of a
`::`-qualified expression. Confirmed shape: this is the same node the existing `$C()` →
`C::C()` constructor-intent rewrite already builds (`Rules.cpp:2325-2334`), so the pass-2
side is a solved shape.

**Identifier concatenation (`copy_$f`, `local_$idx`).** Handle in the fragment lexer under
`allowHoles`: when an identifier token is immediately adjacent (no whitespace) to a `$hole`,
lex the run as a single **composite-identifier** token that records its segments
(`["copy_", $f]`). At clone time this reuses the **`$ident` machinery** of item C (§3) —
`copy_$f` is exactly `$ident("copy_", f.name)` with sugar. So B1's identifier half and C
share one code path; C is the general form, `name_$hole` is its adjacency sugar. Collision
handling (M37) therefore covers both.

### 2.3 Worked expansion (target)

```
inject `$for f in C.fields : $f.type copy_$f = this.$f;` at member of C
```
on `@Ser class P { int x; string y; }` expands (per-field, rename-exempt names) to:
```
int    copy_x = this.x;
string copy_y = this.y;
```
and `$p.type::FromJson(v)` in a handler-binding rule expands to `int::FromJson(v)` /
`MyDto::FromJson(v)` — the Era-B typed-handler case the request gates on.

### 2.4 Why this stays hygienic

`copy_$f` and `$f.type` are **author-chosen names/types**, not template locals, so they must
bypass alpha-rename (overview §3.2). The type path already does (`cloneType` never renames).
The identifier path routes through the `$ident`/`declRefName` channel, which is
rename-exempt by construction. A composite identifier that happens to collide with a
use-site name is the author's declared intent for a *new* local (`copy_x` is meant to be a
fresh local); it is renamed only if it is itself a template local declared elsewhere — the
ordinary rule. No new hygiene rule; the existing two channels already partition correctly.

---

## 3. (C) `$ident(a, b, …)` — name synthesis with a collision guard

### 3.1 The ask and the spike

```
$ build/leviathan --run spikeC.ext          # inject `class $ident(C.name, "Cols") { }` at namespace J
spikeC.ext:5:29: error: expected '{'
```

Request §1C: build a declaration name from bound strings — `class $ident(C.name, "Cols")`
→ `class UserCols` — and (bug.md #22 explicitly cited) **name collision with anything in
scope must be a rule-stage error, never a crash, never silent shadowing.**

### 3.2 Grammar and parse

`$ident` is a reserved hole-head (overview §3.3). In **any name-defining position** a
fragment parser reaches — a `class`/`struct`/`attribute` name, a member name, a `namespace`
member's name — accept `$ident ( arg , … )` where each `arg` is a comptime-`string`
expression (a `$hole.field`, a bare `$hole` reifying to a string, or a string literal).
Parse to a small AST node: a **name-synthesis descriptor** attached to the decl's `name`
slot (a `NameSynth { args: Vec<Expr> }` carried where `Stmt::name`/selector text would be).

Positions (v1): **declaration names only** — the request's cases are `class $ident(…)` and
compile-checked column descriptors. Expression-position `$ident` (calling a synthesized
function) is deferred (§6) — no consumer, and it reopens the pass-2 resolution question the
`$C()` hygiene work (`Rules.cpp:2320-2334`) settled for the matched-class case only.

### 3.3 Expansion

In `cloneStmt` (`Rules.cpp:2424`, the decl-name-hole seam) and `cloneType` for the type-ref
case, when the name slot carries a `NameSynth`:

1. Evaluate each arg to a comptime `Value` via the **existing** `materializeBindings` +
   `evalComptimeAt` path (`Rules.cpp:1512`, `1608`) — the same evaluator `where`/`$for` use,
   so `C.name`, `$f.name`, and string literals all Just Work and nothing new is added to the
   evaluator.
2. Require every arg to be `VKind::String`; otherwise **M38**: `"$ident argument N is not a
   comptime string (got '<v>')"`.
3. Concatenate. Validate the result is a legal identifier (starts alpha/`_`, no separators);
   else **M38** with the offending synthesized text.
4. Set `out->name` to the interned result (`own(...)`), **rename-exempt** (author-chosen).

### 3.4 The collision guard (M37) — the load-bearing requirement

Per the request and bug.md #22, a synthesized name that collides is a **rule-stage error
naming both sites**. Two collision surfaces, both already have a model:

- **Member-position** (`$ident` naming a method/field): reuse `injectMember`'s M33 check
  verbatim (`Rules.cpp:2569-2604`) — it already errors on a same-name+type collision and
  names the injecting rule. M37 is M33 with the message adjusted to name the *synthesized*
  name and its source args.
- **Namespace/top-level position** (`class $ident(…)` injected `at namespace N`): the
  `NamespaceScope` anchor appends items to a namespace body (`Rules.cpp:1674-1675`,
  `parseItemsFragment`), and there is **no collision check there today**. Add one: before
  appending a synthesized-named decl, scan the target namespace's existing members (and the
  global scope for a root-level inject) for a same-name decl; on hit, **M37**:
  ```
  rule 'J::s' synthesizes declaration 'UserCols' (from $ident(C.name,"Cols")) but a
  declaration 'UserCols' already exists at <file:line> — synthesized names must be unique
  ```
  Both sites named: the rule + args on one side, the pre-existing decl's span on the other.

This is the *only* genuinely new confluence logic in file 01, and it is deliberately narrow:
it fires **only** for `$ident`-synthesized names (an ordinary literal-named injected decl
keeps whatever collision behavior it has today), so it cannot regress existing rules.

### 3.5 Worked expansion (target)

```
namespace App {
    attribute Entity { }
    rule cols {
        match @Entity on class C
        inject `class $ident(C.name, "Cols") {
            $for f in C.fields : string $f = $f.name;
        }` at namespace App
    }
}
@Entity class User { string fullName; int age; }
```
expands to:
```
class UserCols {
    string fullName = "fullName";
    string age      = "age";
}
```
— Track 06's compile-checked column descriptor, replacing boot-validated `col("name")`
strings. A second `@Entity class User` elsewhere, or a hand-written `class UserCols`, trips
M37 with both sites named.

---

## 4. Diagnostics (this file's rows of the shared catalog, overview §3.4)

| Code | Trigger | Message shape |
|---|---|---|
| **M37** | C: synthesized name collides in target scope | `rule '<r>' synthesizes '<name>' (from $ident(…)) but '<name>' already exists at <site> — synthesized names must be unique` |
| **M38** | C: `$ident` arg not a comptime string, or result not a legal identifier | `$ident argument <n> is not a comptime string (got '<v>')` / `$ident produced '<text>', not a legal identifier` |
| **M39** | B1: dotted-hole in type position with no reifiable type/name field | `'$f.<x>' has no reifiable type/name field usable as a type` |
| (existing M25) | §1.4: trailing tokens after a `member of` member (proposed) | reuse the "unreachable / unexpected" M25 shape |

M-codes are reserved across the whole design in the overview; file 02 owns M40–M41, file 03
owns M42–M43.

---

## 5. File-level change map

| # | Change | File(s) | Size |
|---|---|---|---|
| 1 | (A) corpus twin `rule_tojson{,_twin}.ext` | `tests/corpus/meta/` | test-only |
| 2 | Composite-identifier lexing (`copy_$f`) under `allowHoles` | `src/Lexer.cpp` (~233-245) | ~15 lines |
| 3 | Fragment parser: dotted-hole in type position (`$f.type`) + reserved `$ident(…)` in name positions | `src/Parser.cpp` (type-name + name productions) | ~40 lines |
| 4 | `TypeRef` dotted-hole field + `NameSynth` descriptor on decl name slot | `src/Ast.hpp` | ~6 lines |
| 5 | `cloneType`: dotted-hole → reified type/name string (rename-exempt); **M39** | `src/Rules.cpp:2347-2372` | ~15 lines |
| 6 | `cloneStmt`/name-hole: `NameSynth` eval + concat + identifier check; **M38** | `src/Rules.cpp:2424-2449` | ~25 lines |
| 7 | Namespace-scope collision check + generalize `injectMember` M33 message → **M37** | `src/Rules.cpp:1674-1675`, `2569-2604` | ~25 lines |
| 8 | (adjacency, own corpus) `member of` member-list parse + `rule_member_multi.ext` | `src/Parser.cpp:1673`, `Rules.cpp:1746-1758` | ~15 lines |
| 9 | Corpus for B1 + C (below) + reference.md update | `tests/corpus/meta/`, `docs/reference.md:2172-2189` | test/doc |

Reuse everywhere: the comptime evaluator (`evalComptimeAt`), interning (`own`),
`declRefName`, and the M33 collision loop are all existing — this file adds parse productions
and clone-time branches, not new evaluation or new phases.

---

## 6. Alternatives / out of scope

- **Expression-position `$ident` (call a synthesized function).** Deferred — no consumer in
  the request (all cases are declaration names), and it reopens pass-2 hygiene beyond the
  matched-class `$C()` case already solved. Add when a track needs it.
- **`$ident` with non-string args (int → name).** Rejected for v1: forces a stringify policy
  (`$ident(C.name, idx)` — decimal? padded?) with no requester. `$ident(C.name, "" + idx)`
  via comptime string concat is available if ever needed.
- **General identifier interpolation `${expr}` anywhere.** Rejected — larger surface than the
  request; `$ident(…)` + `name_$hole` sugar cover every cited case and keep the synthesis
  points greppable.
- **Multi-member `member of`** — flagged (§1.4), fixed under its own corpus, not part of A/B1/C
  acceptance.

---

## 7. Testing & acceptance

Corpus pairs (`tests/corpus/meta/`), each green on `--run`/`--ir`/`--expand`:
- `rule_tojson{,_twin}.ext` — (A) close-out: key array + `__atlEnc(this.$f)` value array,
  byte-identical to hand-written.
- `rule_type_splice{,_twin}.ext` — (B1): `$f.type copy_$f = this.$f;` per field, and a
  handler-binding shape using `$p.type::FromJson(v)`.
- `rule_ident_synth{,_twin}.ext` — (C): `class $ident(C.name,"Cols")` descriptor, byte-equal
  to a hand-written `UserCols`.
- `rule_ident_collision.ext` (red-corpus) — (C): a hand-written `UserCols` beside the
  synthesized one → **M37**, asserting both sites are named.
- `rule_ident_badarg.ext` (red) — **M38**; `rule_type_splice_badfield.ext` (red) — **M39**.
- `rule_member_multi{,_twin}.ext` — (§1.4) two-member `member of` (or the trailing-token
  error, whichever the adjacency fix lands).

**Definition of done:** all corpus green; full meta suite regression-clean; `reference.md`
§Rules updated to list `$f.type`/`$p.name` in type/identifier position, `name_$hole`
concatenation, and `$ident(…)` synthesis with its collision rule; the (A) close-out lets
Track 03 delete toJson Mechanism B against the byte-equivalence proof.
