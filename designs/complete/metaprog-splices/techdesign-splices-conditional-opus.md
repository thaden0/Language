# Tech Design 02 — Conditional Splice: `$if (pred) { … } $else { … }` (B2)

**Status: IMPLEMENTED** (agent2). `StmtKind::ForkSplice` / `ExprKind::ForkSplice`
(`src/Ast.hpp`), fragment-parser productions incl. the `$else if` desugar and the
reserved-head recognition this file's dependency-01 was to provide (introduced
here since 01 had not yet landed — `src/Parser.cpp`), the expansion-time fold in
`cloneStmtInto` / `cloneArrayElements` reusing `materializeBindings` +
`evalComptimeAt` (`src/Rules.cpp`), and the `--ast`/`--expand` printers
(`src/AstPrinter.cpp`). Diagnostics M40 (non-bool condition) and M41 (`$else`
without `$if` / branch kind mismatch — the latter enforced structurally at parse
time) ship. Corpus: `tests/corpus/meta/rule_if_{splice,splice_twin,expr,expr_twin,
chain}.ext` (green, byte-identical twins, `--run`/`--ir`/`--expand` roundtrip) and
`tests/negative/rule_if_{badcond,dangling_else,kindmix}.ext` (red). `docs/reference.md`
documents `$if`/`$else`/`$else if`.

**Part of** `designs/metaprog-splices/` (see `-overview-opus.md`). **Complexity:** opus
(new expansion-time control-flow primitive in the clone pass). **Grounding commit:**
`cc071c3`, spiked live. **Depends on** file 01 only for the shared reserved-hole-head
recognition (`$if`/`$else`, overview §3.3) — land after 01 so it extends that scaffold.

Covers **B2** (request §1B, second half): a conditional splice inside templates so **one
binding rule emits per-source variants** (path vs query vs body) without one-rule-per-
combination explosion. The request is explicit that this is **not** statement-`$for` (that
landed, `tests/corpus/meta/rule_stmt_for.ext`) and **not** a runtime `if` — it is **branch
selection at expansion time; the template stays a template**.

---

## 1. The spike

```
$ build/leviathan --run spikeBif.ext
    # inject `$if (C.name == "P") { int tag() => 1; } $else { int tag() => 2; }` at member of C
spikeBif.ext:5:37: error: expected '('
```

`$if` lexes as an ordinary hole-name `Identifier` (text `"$if"`, `Lexer.cpp:233-245`), so
the fragment parser reads it as a bare hole and then chokes on the following `(`. Unbuilt.

**What already exists and why it is not this.** There is an **item-level `comptime if`
fold** — `Rules.cpp:88` ("comptime-if spliced in at item level … is visible here") and the
"splice, don't nest" machinery at `Rules.cpp:990-1100`. That folds a `comptime if` written
in **real source** (its taken branch's items/statements splice flat into the surrounding
tree, untaken branch never compiled). B2 is the **same idea moved inside a quasiquote
template**: the predicate is evaluated per rule-firing against the firing's bindings, and
the taken branch's fragment is spliced into the clone. Reusing the *evaluation* half
(`evalComptimeAt`) is the whole point (§3.2); the *placement* half is new because it happens
during `cloneStmt`/`cloneExpr`, not during the top-level item walk.

---

## 2. Grammar

`$if` / `$else` are reserved hole-heads (overview §3.3). Inside any fragment the parser
already handles (`parseStmtsFragment`, `parseMemberFragment`, `parseItemsFragment`,
`parseRuleAction`'s array/expr elements), recognize:

```
$if ( <comptime-expr> ) <frag> ( $else if ( <comptime-expr> ) <frag> )* ( $else <frag> )?
```

where `<frag>` is a brace-delimited group **of the same fragment kind as the enclosing
template** — statements in a body/ctor template, one-or-more members in a `member of`
template, items in a `namespace` template, or a single expression when the `$if` sits in
expression position (e.g. an array element `[ … $if(p) { a } $else { b } … ]`). The `$else
if` chain is sugar for nested `$if`, desugared at parse time.

Parse to a new node **`ForkSplice`** — a sibling of the landed `ForSplice`
(`StmtKind::ForSplice`, `Rules.cpp:1994-2058`; `ExprKind::ForSplice` for the array case,
`Rules.cpp:2036`). `ForkSplice` carries: `cond` (the comptime predicate expr), `thenFrag`,
`elseFrag` (each a fragment subtree; `elseFrag` may itself be a `ForkSplice` for the chain).
Two shapes, mirroring `ForSplice`:
- **`StmtKind::ForkSplice`** for statement/member/item position.
- **`ExprKind::ForkSplice`** for expression (array-element / argument) position.

This mirrors the existing `ForSplice` precedent exactly, so it slots into the same clone
dispatch and the same `--expand`/`--ast-after-rules` printers with no new node *category*.

---

## 3. Expansion

### 3.1 Where it hooks

`ForkSplice` is folded in the **same pass** and the **same helpers** that already expand
`ForSplice`:
- Statement/member/item position: `cloneStmtInto` (the statement-list analogue of
  `cloneArrayElements`, referenced at `Rules.cpp:1751`, `1786`, and defined near the
  `ForSplice` handling `Rules.cpp:1994-2058`). Today `cloneStmtInto` special-cases
  `StmtKind::ForSplice`; it gains a sibling branch for `StmtKind::ForkSplice`.
- Expression position: `cloneArrayElements` (`Rules.cpp:2036`, which already special-cases
  `ExprKind::ForSplice`) gains an `ExprKind::ForkSplice` branch.

### 3.2 The fold (reuses the `where`/`comptime-if` evaluator verbatim)

For a `ForkSplice` during clone:

1. Evaluate `cond` with the firing's bindings via `materializeBindings` + `evalComptimeAt`
   (`Rules.cpp:1512`, `1608`) — **the identical call `where` and item-level `comptime if`
   already make.** So `C.name == "P"`, `$p.attr("Query") != None`, `f.hasAttr("Body")`, etc.
   all evaluate with zero new evaluator surface.
2. Require `VKind::Bool`; otherwise **M40** (mirrors the `where`-clause bool check at
   `Rules.cpp:1614-1617`, same message shape).
3. Splice **only the taken branch's fragment** into the output vector (or return the taken
   expression), cloning it through the ordinary `cloneStmt`/`cloneExpr` path so its own
   holes, `$for`s, nested `$if`s, and hygiene all apply. The untaken branch is **never
   cloned** — matching the request's "template stays a template, untaken branch not emitted"
   and the item-level fold's "untaken branch not compiled" semantics (`reference.md:2208`).

Because folding produces a *flat* splice into the enclosing vector (not a nested block), a
`$if` whose branches are statement fragments composes with a surrounding `$for` and with
sibling statements exactly like `ForSplice` does — the "splice, don't nest" invariant
(`Rules.cpp:987-990`) is preserved, and `--expand` shows the resolved branch as ordinary
source with no residual `$if`.

### 3.3 Fragment-kind consistency (M41)

Both branches of a `$if` must produce the **same fragment kind** as the position they sit
in — you cannot have `thenFrag` yield statements and `elseFrag` yield a bare expression in
one array element. This is checked at **parse time** (the fragment parser knows its own
kind) and re-asserted at fold time; violation is **M41**. `$else` without a preceding `$if`,
or an unterminated chain, is the same code. This keeps the printer total: every `ForkSplice`
resolves to nodes of a single, known kind.

---

## 4. Worked expansion — the Era-B binding case the request gates on

One binding rule emitting path/query/body variants (request's forcing function), instead of
one rule per source combination:

```
namespace Web {
    attribute Param { string source; }     // "path" | "query" | "body"
    rule bindParam {
        match @Handler on method m in class C
        inject `$for p in m.params :
                    $if (p.attr("Param")?.argStr(0) == "path") {
                        $p.type $p = ctx.pathVar($p.name);
                    } $else if (p.attr("Param")?.argStr(0) == "query") {
                        $p.type $p = ctx.query($p.name);
                    } $else {
                        $p.type $p = $p.type::FromJson(ctx.body());
                    }` at top of body
    }
}
```
For a handler `void show(@Param("path") int id, @Param("body") UserDto dto)` expands to:
```
int     id  = ctx.pathVar("id");
UserDto dto = UserDto::FromJson(ctx.body());
```
Note the composition: `$if` selects the marshaling shape per parameter, `$p.type` (file 01,
B1) supplies the type and the labeled-ctor `UserDto::FromJson`, `$for` iterates the params.
Three splice primitives, one rule, zero combinatorial explosion — the exact request goal.

---

## 5. Diagnostics (this file's rows)

| Code | Trigger | Message shape |
|---|---|---|
| **M40** | `$if` condition not a comptime `bool` | `rule '<r>' $if-condition must be bool (got '<v>')` (mirrors `Rules.cpp:1614`) |
| **M41** | `$else` without `$if`, or branches of mismatched fragment kind | `$else without a preceding $if` / `$if branches must both produce <kind>s in this position` |

Reuses the comptime evaluator's own failure diagnostic (`Rules.cpp:1609-1612`,
"where-clause evaluation failed") for a condition that throws/steps-out — same wording, keyed
to `$if` — so a runaway or erroring predicate is caught by the existing step budget, not a
new path.

---

## 6. File-level change map

| # | Change | File(s) | Size |
|---|---|---|---|
| 1 | `StmtKind::ForkSplice` + `ExprKind::ForkSplice` (cond/then/else fields) | `src/Ast.hpp` | ~8 lines |
| 2 | Fragment parser: `$if`/`$else`/`$else if` productions (all fragment kinds) → `ForkSplice`; parse-time M41 kind check; `$else if` desugar | `src/Parser.cpp` (fragment parsers, ~1685+) | ~50 lines |
| 3 | `cloneStmtInto`: `ForkSplice` branch — eval cond, splice taken fragment; **M40** | `src/Rules.cpp` (near `ForSplice`, 1994-2058) | ~25 lines |
| 4 | `cloneArrayElements`: `ExprKind::ForkSplice` branch (expr position) | `src/Rules.cpp:2036` | ~15 lines |
| 5 | `--expand` / `--ast-after-rules` printers: `ForkSplice` prints as its resolved branch (post-fold it is already gone; pre-fold AST dump prints the fork) | printer (follows `ForSplice` precedent) | ~10 lines |
| 6 | Corpus + `reference.md` update | `tests/corpus/meta/`, `docs/reference.md:2203-2227` (comptime section, note the template analogue) | test/doc |

Every hard part — predicate evaluation, hygiene, step-budget safety, flat-splice placement —
is **reused** from `ForSplice` and the item-level `comptime if` fold. `ForkSplice` is
deliberately shaped as `ForSplice`'s twin so it inherits the same clone/print/expand
plumbing rather than introducing a parallel one.

---

## 7. Alternatives / out of scope

- **Reuse `comptime if` syntax verbatim inside templates** (no `$` sigil). Rejected: inside a
  quasiquote a bare `comptime if` is ambiguous with a *runtime* `comptime if` the author
  wants in the **generated** code (rare but legal). The `$` sigil marks "fold at expansion,
  not in the output," consistent with every other template hole. `$if` reads as
  expansion-time; `comptime if` in a template stays runtime-generated.
- **`$match`/`$switch` splice.** Deferred — no requester; `$else if` chains cover the cited
  path/query/body case. Add if a track needs many-way dispatch on a comptime value.
- **`$if` selecting between anchors** (splice site A vs B). Out of scope — `$if` selects
  *fragments at one anchor*, not anchors; anchor selection is the rule head's job.
- **Short-circuit side effects.** N/A — the predicate is a hermetic comptime expression under
  the existing step budget; it cannot mutate program state (the standing comptime invariant,
  `reference.md:2211-2216`).

---

## 8. Testing & acceptance

Corpus pairs, green on `--run`/`--ir`/`--expand`:
- `rule_if_splice{,_twin}.ext` — the §4 path/query/body binding rule, byte-identical to a
  hand-written per-source twin.
- `rule_if_expr{,_twin}.ext` — `$if` in **array-element** position (`[ … $if(p){a}$else{b} … ]`)
  to prove the expression path.
- `rule_if_chain.ext` — `$else if` chain desugar (3-way).
- `rule_if_badcond.ext` (red) — non-bool condition → **M40**.
- `rule_if_dangling_else.ext` / `rule_if_kindmix.ext` (red) — **M41**.
- `--ast-after-rules` on a pre-fold template shows the `ForkSplice`; `--expand` shows only the
  taken branch (untaken never appears) — the request's "template stays a template" acceptance.

**Definition of done:** corpus green; full meta suite regression-clean; `reference.md`
documents `$if`/`$else`/`$else if` as the expansion-time template conditional (distinct from
runtime `comptime if`); Track 02 Era-B collapses its per-source binding rules to one.
