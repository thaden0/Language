# Tech Design: Metaprogramming Phase 3 — semantic depth

**Status:** feature-complete — third sync, 2026-07-05. All of §2–§11 are implemented,
including both §7×§9 follow-ons (deferred comptime folding + M29; clone-time macro
expansion in rule templates) and §11 (struct reification, stretch). Every corpus/metatest
item in §14's testing plan is now pinned (M21–M24/M28/M29 negatives; `rule_orm` +
`macro_safestrip` incl. its restored comptime-fold case + `comptime_reify_struct` +
`rule_defsite_qual` + `macro_in_rule_template` corpus tests, each oracle==IR==ELF).
Landed, by commit: §2/§3/§4/§6/§8 (`d2ba3c0`, merged `ef0ef3c`); §9 pipeline restructure +
conditional `uses` + the `Lower.cpp` closure-receiver fix + `skip_concat` harness mechanism
(`8fe37ad`); §5/§7-core/§10 (`6016c25`); §7×§9's two follow-ons + §11 + full metatest
pinning — in tree, uncommitted at this sync. Suite green (26/26).
Nothing is pending. The commit-sequence list in §13 and the testing-plan state notes in
§14 carry the final per-item detail.

**Deferred tail → `designs/complete/techdesign-metaprog-phase4.md`.** The in-phase deferrals scattered
through this doc — statement-position `$for` (§5), the def-site `homeNs` "true home"
refinement (§10), `meta.*` structured `Type` / attribute-value reflection (§3), class-wide
marker search (§15 Q2), and the golden-`--expand` fixture (§14) — are now gathered, with
the proposal's Layer D (`rewrites`/`replace`/`$body`) and the rest of Phase 4, into the
Phase 4 design (its §1 inventory maps each back to the doc/line that deferred it). Nothing
below is lost; it is tracked there.
**Scope:** the Phase 3 line from `designs/complete/techdesign-metaprogramming.md` §13, grounded in the
codebase as of `4a6a242` (Phases 1–2 built): `where` predicates, the `meta.*` reflection
surface, `$for` list splices, `$_args`/`$_params` well-known holes, expression macros,
the remaining anchors (`top/bottom of body`, `marker`, `namespace N`), platform-conditional
`uses` inside `comptime if` (Leonard's decision #4, deferred here from Phase 1), struct
reification (stretch), and the definition-site-qualification half of hygiene deferred from
Phase 2.

**Reading order:** assumes the main tech design; §numbers like §5.4 refer to it.

---

## 0. Contents

1. What exists now (the substrate Phase 3 builds on) + two gaps to close first
2. One unifying decision: two kinds of code position, one binding model
3. `meta.*` — the reflection value surface (minimal, forever-API-conscious)
4. `where` predicates
5. `$for` list splices
6. `$_params` / `$_args` well-known holes
7. Expression macros (`macro` decls + `name!(args)`)
8. Remaining anchors: body, marker, namespace
9. Platform-conditional `uses` in `comptime if` (the pipeline restructure)
10. Hygiene completion: definition-site qualification
11. Struct reification (stretch)
12. Diagnostics catalog (M19–M28)
13. File-level change map and sequencing
14. Testing plan
15. Risks and open calls for Leonard

---

## 1. What exists now, and two gaps to close first

Phase 2 left exactly the right hooks:

- `RuleMatch::where` is **parsed and stored** (`Parser.cpp:1041`) but the engine never reads
  it. **Gap A:** today a rule with a `where` clause silently fires as if the clause weren't
  there — the worst failure mode (silent wrong). Phase 3's first commit implements it; if
  Phase 3 were ever delayed, an interim guard must reject `where` loudly instead.
- `AnchorKind::{BodyTop, BodyBottom, Marker, NamespaceScope}` parse; the engine rejects them
  with "Phase 3".
- `Binding` currently has two variants: attr (a pointer to the evaluated field list,
  `AttrValue`) and decl (`Stmt*`/`Symbol*` + selector). **Gap B:** this split is what Phase 3
  outgrows — `$for` loop items, macro arguments, and `where` locals all need a *general
  value* variant. §2 unifies it.
- `Evaluator::evalComptime` pushes one empty env frame (`Eval.cpp`); seeding that frame with
  named locals is a two-line overload.
- The oracle's `Object` is `{Symbol* cls, unordered_map<string,Value> fields}` — the engine
  can construct `meta.*` values directly, no constructor invocation needed, and in-language
  methods on those classes (e.g. `hasAttr`) dispatch normally via `findMethod`.
- Fragment parsers (`parseStmtsFragment` / `parseMemberFragment` / `parseExprFragment`)
  re-lex templates in place with `$`-holes; `$_args` and `$for` already lex as ordinary
  `$`-identifiers (underscore is an identifier char), so **no lexer changes at all** in
  Phase 3 except none — even `!` for macro calls already lexes as `Bang`.

## 2. One unifying decision: two position kinds, one binding model

Phase 3 introduces several places where *ordinary comptime code* runs with rule bindings in
scope (`where`, the `$for` iterator expression) alongside the existing *template* positions
(quasiquote bodies). The rule that keeps this learnable:

> **In ordinary-code positions, bindings are plain names (`m`, `C`, `r`) — it's just
> comptime code with extra locals. In template positions, bindings are `$`-holes (`$m`,
> `$r.method`) — splices into quoted syntax.** `$` marks "escape from quotation," and only
> quotation has escapes.

To serve both, `Binding` unifies on `Value`:

```cpp
struct Binding {
    // The comptime VALUE of this binding: for an attribute, an Object of the
    // attribute class; for a decl, its meta.* reflection object (§3), built
    // lazily; for a $for item or macro argument, the item itself.
    Value val;
    bool hasVal = false;
    // Decl identity (kept for name-splicing and anchor targeting):
    Stmt* declStmt = nullptr;
    Symbol* declSym = nullptr;
    std::string_view selectorText;
    // Macro args only: the parsed argument expression, spliced as a subtree.
    const Expr* exprNode = nullptr;
};
```

Migration cost: Phase 2's `$r.method` value-hole path reads `attrVal->fields` — it becomes
"fold the field chain against `val.obj->fields`," which is *less* code (the attribute is now
just an Object). `attrValues_` (the `AttrUse* → AttrValue` cache) stays as the source the
Object is built from, so nothing upstream changes.

## 3. `meta.*` — the reflection value surface

Prelude additions (ordinary in-language classes; the engine builds their instances
field-directly, methods run on the oracle):

```
namespace meta {
    class Param  { string name; string type; }
    class Field  {
        string name; string type;
        Array<string> attrs;                       // resolved attribute names
        bool hasAttr(string n) => attrs.contains(n);
    }
    class Method {
        string name; string returnType;
        Array<meta::Param> params;
        Array<string> attrs;
        bool hasAttr(string n) => attrs.contains(n);
        int arity() => params.length();
    }
    class Class {
        string name;
        Array<string> bases;                       // canonical base spellings
        Array<meta::Field> fields;
        Array<meta::Method> methods;
        bool hasBase(string n) => bases.contains(n);
    }
}
```

Deliberate minimalism (§3.8 of the main design: every exposed field is a forever-API):

- **Types are canonical strings**, not `meta::Type` structs. `m.returnType == "void"`,
  `f.type == "int"` cover the realistic predicates; a structured `Type` can be added later
  without breaking the string fields. The canonical spelling is pass-1's
  `TypeRef::canonical` — already computed for every member (`Resolver::resolveMember`).
- `attrs` is names-only. Reading another attribute's *arguments* from `where` is out of
  scope (the rule that needs an attribute's data should match it).
- No `Span`, no mutation surface, no `Decl` supertype until demanded.

**Materialization** (engine, `buildMetaValue(const DeclInfo-ish&) → Value`): constructed on
first use per binding (lazy — most rules never use `where`), caching per `Stmt*` in a
`std::map<const Stmt*, Value>` for the stage's lifetime. Field/method lists come from the
decl's `body` (same loops `evalAttrArgs` and `injectMember` already do); `attrs` names come
from each member's `attrs` vector (`AttrUse::resolved->name`). The `meta::*` class symbols
are found once via `namespaceScope("meta")`.

**Attribute bindings** materialize as an Object of the *attribute's own class* (fields from
the cached `AttrValue`) — so `where r.method == "GET"` reads naturally and `$r.method` in
templates folds from the same object.

## 4. `where` predicates

Grammar already parsed: `match … (where <expr>)?`. Semantics:

- Evaluated **after** the structural match succeeds and bindings are built, **before**
  `expand`. Bindings are in scope as plain names (§2).
- Must yield `bool` — anything else is **M19** at the `where` expression's span, naming the
  rule. Evaluation failure (throw, hermeticity, budget) is **M20**, same span. Both errors
  fire once per offending *firing* (they may legitimately differ per decl).
- `false` → the rule silently doesn't fire for this decl (that's the feature).
- Runs under the same comptime gates as everything else (hermetic, shared step budget).

Implementation: `Evaluator` gains
`Value evalComptime(Expr* e, const std::unordered_map<std::string, Value>& locals,
std::string& err, bool& failed)` — identical to the existing overload except the pushed env
frame is seeded from `locals`. `tryMatch` ends with:

```cpp
if (m.where) {
    auto locals = materializeBindings(out);        // name -> meta value (§3)
    std::string err; bool failed = false;
    Value v = eval_.evalComptime(m.where.get(), locals, err, failed);
    if (failed)                { M20; return false; }
    if (v.kind != VKind::Bool) { M19; return false; }
    if (!v.b) return false;
}
```

Example that must work (the ORM filter without `$for`):

```
rule timedOnly {
    match @Timed on method m in class C : IService
    where m.returnType != "void" && m.arity() <= 2
    inject `...` at bottom of C.constructor
}
```

## 5. `$for` list splices

The ORM example (`proposal §10.2`) is the driver:

```
inject `Schema schema() => Schema($t.name,
          [ $for f in C.fields.where((x) => x.hasAttr("Column")) : Col($f.name, $f.type) ]);`
       at member of C
```

**Grammar** (fragment mode only): inside an **array literal**, an element may be

```
$for <ident> in <expr> : <element-template>
```

- The **iterator expression** is ordinary comptime code — bindings as plain names, full
  stdlib available (`C.fields.where(...)` — so `$for` needs no `where` clause of its own;
  the language's own `where` method is the filter). Parsed by the fragment parser with a
  small mode: on seeing element-initial `Identifier("$for")`, parse
  `ident`, expect `in`, parse `parseExpr(0)` up to the `:` (colon is not an infix operator,
  so `parseExpr` stops there naturally — same property the ternary relies on), expect `:`,
  parse the element template as `parseExpr(0)`.
- **AST:** one new `ExprKind::ForSplice` — `text` = loop var, `a` = iterator expr, `b` =
  element template. Parser-produced only inside fragments; `AstPrinter` renders it
  (`$for f in … : …`); Resolver/Checker/Lower never see one (substitution replaces it, and
  an unexpanded ForSplice outside a template is a parse error since `$` doesn't lex in
  normal mode).
- **Expansion** (in `cloneExpr`, `Array` case): for a `ForSplice` element — evaluate the
  iterator with bindings-as-locals; result must be `VKind::Array` (**M21** otherwise); for
  each item, extend a scratch `Bindings` with loop-var → `Binding{val=item}` and clone the
  element template once, appending each clone to the array literal being built. Nested
  `$for` falls out (the inner clone runs with the extended bindings).
- The element template is a *template position*: the loop var is referenced as `$f`, and
  `$f.name` folds via the unified value-hole path (§2). A bare `$f` whose item is a
  primitive reifies directly; a bare `$f` whose item is an Object is still M10 (splice a
  field).

Statement-position `$for` (repeating whole statements) is **not** in Phase 3 — the array
form covers the known cases, and statement repetition invites the template-as-program smell
the proposal warned about. Revisit on demand.

**[landed]** As specified: `ExprKind::ForSplice` (text/a/b reuse), `parseArrayElement` in
the parser (the `$for` head can only ever lex inside a fragment — `$` doesn't lex in
normal mode — so no ambiguity with ordinary arrays), expansion in a dedicated
`cloneArrayElements` (iterator evaluated with `materializeBindings`, the same env `where`
uses; M21 on non-array; loop var bound per item into an EXTENDED copy of the bindings, so
nested `$for` sees the outer var), AstPrinter rendering. A bare `$item` whose value is a
primitive reifies directly, per the design; an Object item still demands a field splice.
One adaptation from the proposal's §10.2 example: the proposal wrote
`f.attr(Column).name` (reading an attribute's ARGUMENT through reflection), but §3's
deliberately-minimal `meta.*` surface reflects attribute NAMES only (`attrs:
Array<string>`) — attribute-value reflection is a forever-API addition §3 chose not to
make. The landed `rule_orm` corpus test (with hand-written twin, oracle==IR==ELF) uses
`C.fields.where((x) => x.hasAttr("Column"))` and names columns by field name. If
attr-value reflection is ever wanted, it is a §3 decision (a `meta::Attr {name, fields}`
object), not a `$for` gap.

## 6. `$_params` / `$_args` well-known holes

Sugar for "the matched callable's parameter surface," so route templates stop hand-writing
`(HttpRequest req) => this.$m(req)` shapes that must match the method:

- **`$_params`** — legal only where a *parameter list* is being cloned (lambda params,
  member params in a `member of` template). During `cloneExpr`/`cloneStmt` param-vector
  cloning: a `Param` whose *name* is `$_params` (type null) is replaced by clones of the
  subject method's `params` (types and names included).
- **`$_args`** — legal only as a *call argument* element: a `Name` argument spelled
  `$_args` expands to one `Name` per subject parameter, in order, spelling each parameter's
  name.
- Both resolve against the **subject binding** specifically (not an arbitrary `$x`): they
  are defined as "the matched declaration's params/args." Subject not callable → error at
  the template span (reuses M27's wording family).
- The names spliced by `$_args` refer to the *cloned parameter names* — hygiene interaction:
  parameter names spliced by `$_params` are **not** gensym-renamed (they must line up with
  `$_args`); they live in a fresh lambda/member scope, so capture is structurally
  impossible.

```
inject `router.add($r.method, $r.path, ($_params) => this.$m($_args))`
       at bottom of C.constructor
```

## 7. Expression macros — `macro` decls + `name!(args)`

The `safeStrip` case (proposal §10.4), Layer D-lite: additive at the expression level.

**Declaration** (namespace-level, contextual keyword like `rule`):

```
macro safeStrip(e) => `($e ?? "").trim()`;
```

- Recognized by `Identifier("macro") Identifier LParen`. Reuses `StmtKind::Rule` with a new
  `isMacroDecl` flag (per the main design's note): `name`, param names in `generics`
  (a string_view vector we already carry — repurposed, documented), one `RuleAction` holding
  `tmplExpr` from `parseExprFragment`. Collected and detached alongside rules.
- Scoping: **identical to attributes/rules** — a call site sees macros of namespaces its
  file imports; ambiguity across imports is an error naming candidates (reuse the attr
  machinery's shape); `NS::name!(…)` qualifies.

**Call site:** `name!(args)` — in `parsePostfix`, after a `Name`/`Member` base, `Bang`
followed by `LParen` becomes a macro call (today that sequence is a parse error, so the
grammar slot is free — no adjacency check needed). Represented as `ExprKind::Call` with a
new `isMacroCall` flag on `Expr`; sets `sawMeta_`.

**Expansion** — REVISED at implementation time. As originally written this section said
expansion rides "the same pass that folds `comptime` exprs". §9 (built first) makes that
impossible: macro scoping reads the imports map, and §9's whole point is that the imports
map is computed only AFTER the comptime fold (a taken branch's `uses` must be visible to
scoping). Macro resolution therefore cannot run during the fold walk — during that walk
there is no imports map yet, and resolving against a pre-fold map would contradict this
section's own "scoping identical to attributes/rules" clause (a conditionally-imported
macro would never resolve). **As landed**, expansion is a SECOND walk over the folded tree,
after the imports recompute (pass D in §9's lettering), gated by an engine flag so the
fold walk's shared machinery doesn't expand early:

- Resolve the macro by name through the call site file's `effective` namespaces (**M23**
  unknown; ambiguity error like M02); `NS::name!(…)` pins the namespace directly.
- Arity must match the parameter list exactly.
- Each argument is bound as `Binding{exprNode = arg}` — an **expression subtree**, spliced
  where `$param` appears in the template via `cloneExpr` of the argument. A macro call
  nested in an ARGUMENT (`dbl!(inc!(3))`) expands bottom-up naturally (children are walked
  first) — no re-entry machinery involved.
- **Single-splice rule:** a macro parameter referenced **more than once** in the template is
  **M22** (at the macro decl, once): splicing twice would evaluate the argument's side
  effects twice, and expression position offers nowhere to hoist a binding. Checked
  statically right after collection (count `$param` occurrences), so it fires even for
  macros nobody calls yet. *(landed)*
- **No re-entry:** if the spliced result still contains a macro call (a macro's own
  template calling a macro), that's **M24** — same fixpoint stance as rules (§8 of the
  main design); gate later if a real case appears. *(landed)*
- Hygiene: expression templates can't declare locals, so gensym doesn't apply; definition-
  site qualification (§10) is what makes a macro's references to its own namespace's
  helpers survive splicing into foreign files. *(landed — the clone runs with the macro's
  declaring namespace as the qualification context)*

**§7×§9 interaction — two positions the split walk cannot reach** (both discovered at
implementation; both LANDED — see commits 5b/5c in §13):

1. **A macro call inside a `comptime`-marked expression** (`comptime string s =
   safeStrip!(…);`). The fold walk evaluates comptime sites via `evalComptime` directly —
   the evaluator hits the unexpanded call node ("cannot resolve call target") because
   macros are not runtime symbols, and the fold happens before macro expansion is possible
   at all (see above). The original §14 test line "incl. under a `comptime` fold" assumed
   the one-walk design and is unachievable as-is.

   **Resolution — deferred folding (Layer C splits in two), landed (commit 5b):**
   - *C-early* (the existing fold walk, pre-imports): folds comptime-`if`s and every
     comptime site whose subtree contains **no** macro call (`findMacroCall` predicate,
     already implemented for M24).
   - *C-late* (rides the macro-expansion walk, post-imports): a comptime site that DOES
     contain a macro call is left marked during C-early (its `isComptime` stays, its
     global stays undefined) and folds during the second walk — macros expand in its
     subtree first, then the ordinary fold runs. Deferred sites fold in source order
     relative to each other (same traversal).
   - *One-direction rule, extended:* a comptime-`if` **condition** containing a macro call
     is a hard error (**M29**) — the condition feeds the imports map that macro resolution
     itself needs; deferring it is the fixpoint the design forbids. Same stance and
     wording family as "a comptime if cannot depend on what a rule injects" (§9).
   - *Documented corner, verified:* a C-early site that reads a macro-containing (C-late)
     comptime var fails — the read runs before the deferred var's global exists (e.g.
     `comptime int b = a.length();` after a macro-deferred `comptime string a = ...;`
     fails "cannot resolve call target 'length'": `a` reads as the evaluator's undefined-
     global default, not a string). Loud, source-anchored, and the fix (hoist the macro
     out of the var, or make the reader comptime-late by giving it a macro call too) is
     mechanical. No bespoke diagnostic in v1.
   - Implementation note: `walkExpr`'s comptime branch and `walkStmt`'s Var/ExprStmt
     comptime branches all gained the identical shape — `if (findMacroCall(...)) { if
     (!macroExpansionEnabled_) return; /* expand children, then the node itself if IT is
     the call */ }` before falling through to the existing fold. A shared
     `walkExprChildren(Expr*)` factors the structural descent so the ordinary and deferred
     paths don't duplicate it. `foldTopLevelItem`'s SEPARATE item-level comptime-`if`
     handling (§9 step 3) needed its own M29 check too — it does not share `walkStmt`'s
     `StmtKind::If` case.
   - Corpus: `macro_safestrip.ext`'s `comptime string baked = safeStrip!(...)` case,
     restored — verified genuinely folded to a literal (`--expand` shows `"baked at
     compile time"`, not the call). Metatests: a CLEAN compile check, plus two M29
     ERRORS checks (item-level and nested-in-a-function-body).

2. **A macro call inside a rule/anchor template** (`inject \`log = tag!(x)\` …`). Rule
   templates clone during rule matching (after the macro walk), so the injected clone
   carries an unexpanded macro-call node into pass 2 — today that dies as "unknown
   function 'tag'" (loud but unhelpful, and the capability is legitimately useful: a rule
   author composing their own namespace's macros).

   **Resolution — expand at clone time, def-site scoped, landed (commit 5c):** after
   cloning a template, run macro expansion over the clone. Template spans point into the
   RULE's file, so the existing file-based scoping resolves against the rule file's
   imports — definition-site scoping falls out for free, consistent with §10's philosophy
   (the rule author's names mean what they mean where the rule was written). M24 applies
   to the result unchanged.
   - Implementation: two new `expandMacrosInClone` overloads (one `StmtPtr&`, one
     `std::vector<StmtPtr>&`) — save/force/restore `macroExpansionEnabled_` around a
     `walkStmt`/`walkItems` call over JUST the fresh clone, reusing the same walk rather
     than a bespoke walker. Wired into all five `expand()` anchor branches (member,
     ctor top/bottom, body top/bottom, marker, namespace) right after each clone
     completes.
   - Corpus: `macro_in_rule_template.ext` — the macro lives in a namespace (`Fmt`) the
     USE SITE never `uses` (only the rule's own namespace `Web` does, internally), proving
     the resolution is genuinely definition-site and not accidentally use-site.

Both positions now behave as the design originally intended.

## 8. Remaining anchors

### 8.1 `top of body` / `bottom of body`

Target: **implicitly the subject** (`m`) — the grammar has no target identifier for body
anchors, and the subject is the only principled referent. Subject must be a callable member
with a body (**M27**: "`top of body` needs a callable subject; `f` is a field").

- Normalize the body to a `Block` (same normalization `injectIntoCtors` does).
- `BodyTop`: insert the cloned statements at position 0. Works for every body shape —
  arrow bodies become `{ injected…; return expr; }`.
- `BodyBottom`: if the last statement of the normalized body is a `Return` (which is what an
  arrow body *is*), appending after it is unreachable — **M25**, pointing at the anchor with
  a note naming the subject's arrow body. Additive rules that need "after the result is
  computed" semantics are Layer D's wrap territory, not an additive bottom-append; the error
  says so.
- Multiple firings append in rule order after the top-insertions, same accumulation
  discipline as ctor anchors.

The `@Timed`-shaped *wrapping* use case remains Phase 4 (`rewrites`); `top of body` covers
additive prologues (tracing, precondition checks) now.

### 8.2 `marker "name"`

The author-placed anchor — Leonard's original `[top]`/`[bottom]` block idea, principled:

- **Statement form:** `@anchor("audit")` in statement position. Parsed (statement-start
  `At` + `Identifier("anchor")` + `( StringLit )`) into a **`StmtKind::Empty` carrying the
  marker name in `name`** — deliberately Empty: every existing pass already ignores Empty,
  so zero switch churn anywhere, and at lower time a leftover marker is a no-op by
  construction. `AstPrinter` shows `Marker "audit"` when an Empty has a name.
- **Injection:** `inject `…` at marker "audit"` — searched **within the subject's body
  only** (v1): walk the matched decl's normalized body recursively; at each Empty-with-name
  == marker, insert the cloned statements **after** the marker (the marker itself stays, so
  several rules can stack onto one marker, in rule order). Not found → **M26** at the
  anchor, naming the subject.
- Markers in bodies the rule didn't match are untouched — reach stays governed by matching,
  not by marker presence.

### 8.3 `namespace N`

Adds declarations at namespace scope (registration tables, generated helpers):

- `N` in the grammar is a **literal namespace name** (not a binding) — a rule targets a
  known home for its generated decls, typically its own namespace.
- Template fragment kind: **items** — a new `parseItemsFragment` looping
  `parseTopLevelItem` (statement fragments can't produce function/class decls; this is why
  the anchor needs its own entry point).
- **Injection = reopen:** wrap the cloned items in a fresh `namespace N { … }` Stmt and
  append it to `program.items`. §12's reopen-and-merge semantics does the rest in pass 2 —
  no tree surgery to find the "real" namespace node, and injection order stays visible in
  `--expand`. Duplicate top-level collisions surface through pass-2's ordinary machinery.

## 9. Platform-conditional `uses` in `comptime if`

The Phase 1 restriction (M15) lifts. The problem shape: `uses` affects the P-4 imports map,
which the stage itself scopes attributes/rules/macros by, and pass-1 resolution has already
walked the tree before the engine runs. The design that avoids any extra resolve pass:

1. **Resolver pass 1 skips comptime-`if` branches** — `resolveStmtTypes` returns early for
   an `If` with `isComptime` (both branches unresolved, no diagnostics from either). This
   also fixes a latent Phase-1 gap: today the *untaken* branch is resolved in pass 1 and can
   emit type errors even though it is "not compiled" — Zig's rule (and our stated one) is
   that only syntax is unconditional. Branches contain only statements and `uses` (class
   declarations don't parse inside blocks), so gather/shapes are unaffected.
2. **The engine folds comptime before anything reads imports.** `run()` splits the current
   interleaved walk into ordered passes (lettering AS BUILT — the original plan put macro
   expansion inside pass B/E; §7's revised expansion section explains why it got its own
   post-imports walk instead):
   - A. `collectRules` (+ macro decls) + M22 static validation
   - B. **comptime fold walk** (vars / ifs / exprs — no attribute processing, no macro
     expansion; once the §7 deferral lands, this is "C-early": macro-containing comptime
     sites are left un-folded here)
   - C. **recompute `imports_`** — `computeFileImports(files_, program)` moves *into the
     engine* (from `main.cpp`), running on the post-fold tree
   - D. **macro-call expansion walk** (§7) — a second walk over the same tree, now that
     `imports_` exists to scope against; re-walking is safe because every pass-B fold
     site replaces itself with a fresh non-comptime node or clears its `isComptime` once
     folded, so nothing re-folds or re-fires a compile-time side effect. (Once the §7
     deferral lands, deferred "C-late" comptime sites also fold here, post-expansion.)
   - E. attribute resolution walk (Layer A)
   - F. decl indexing + rule matching/expansion (Layer B)
   - G. dangling warnings
3. **Top-level flatten:** when a folded `comptime if` at item level (top level or namespace
   body) yields a `Block`, splice the block's statements **inline** into the parent items
   vector instead of leaving a nested Block — so a `uses` in the taken branch lands at item
   level where both `computeFileImports` and pass-2 `processImports` see it. (Inside
   function bodies the current replace-with-Block behavior stays.)
4. Pass 2 re-resolves the whole post-fold tree as always — the taken branch's code gets its
   real resolution there. Nothing else in the stage depends on pass-1 `uses` processing:
   attribute/rule/macro lookup goes through `namespaceScope(NS)` directly (the namespace's
   own scope, not the imported copies), keyed by the recomputed `effective` sets.

Consequence to document: a `comptime if` condition **cannot depend on anything a rule
injects** (folding precedes rules) — the stage has one direction, no fixpoint. That is the
design's standing no-reentry stance, now stated for Layer C too.

`main.cpp` change: it no longer computes `rimports` (drops ~4 lines; the engine ctor takes
`files` only). `--imports` mode keeps its own call, now documenting that it reports
*pre-fold* imports.

**[landed — two field notes from implementation]**

1. **What conditional `uses` does and does not select.** The recomputed map governs
   *metaprogramming* scope only: attribute/rule/macro lookup resolves through
   `namespaceScope(NS)` against the post-fold `effective` sets, so a taken branch's `uses`
   decides which lib's rules/attributes fire (proven by the `comptime_uses` project test,
   oracle == IR == ELF). It does **not** make hand-written *type names* conditionally
   resolvable: pass 1 resolves everything outside comptime-`if` branches unconditionally,
   before the fold, so e.g. `class Plugin : IPlugin` with `IPlugin` declared only in
   conditionally-imported namespaces is an "unknown type" error in pass 1. This follows
   from steps 1/4 above (only *branches* are skipped) — recorded so it is read as the
   designed boundary, not a bug.
2. **The `comptime_uses` test is exempt from the §12 concatenation invariant — by
   necessity, not convenience.** A file's visibility is `declaresInto ∪ uses ∪ {std}`
   (`Project.hpp`). Concatenating the test's sources produces a file that *declares*
   both `LibA` and `LibB` itself, so both `Handler`s become visible unconditionally
   (M02 ambiguity) regardless of the comptime-`if` — a different program, not an
   equivalent one. Any program whose meaning depends on per-file `uses` selecting among
   same-named symbols declared by *other* files is inherently layout-dependent, which is
   exactly what this feature exists to express and exactly what concatenation erases.
   Mechanism: `tests/run_project.sh` honors a per-test `skip_concat` marker file (first
   line = reason, printed as a SKIP); the oracle == IR == ELF checks still run in full.

## 10. Hygiene completion: definition-site qualification

Phase 2 shipped alpha-renaming; the second half (§7.1) becomes necessary the moment macros
and namespace-anchored templates routinely splice code into *foreign* files:

- During clone, for each free `Name` expr that is **not** a hole, **not** a renamed
  template-local, and **not** `this`: look it up in the **defining namespace's scope** (the
  rule's/macro's `ns`, via pass-1 sema). If it resolves to a symbol declared in that
  namespace (function, class, global var) *and* the namespace is not `<root>`, rewrite the
  node to the qualified form `NS::name` (a `Member` with `colon=true` over a `Name`).
- Names that don't resolve there stay bare and resolve at the injection site in pass 2 —
  the deliberate channel for `this`-relative members and prelude names (which resolve the
  same everywhere).
- Member names after `.`/`::` are never touched (they resolve by receiver type).
- ~~One subtlety inherited from `uses`-copying (the Phase-1 ambiguity fix): qualification
  uses the *declaring* namespace — if the rule's namespace itself imported the name from
  elsewhere, we qualify to where the symbol actually lives (`Symbol` identity is checked
  against each candidate namespace's own scope, reusing `resolveAttr`'s dedupe
  approach).~~ **Found unimplementable as written.** `uses`-copying makes a borrowed name
  and a home-declared name IDENTICAL in Scope data (both are a `Symbol*` entry in the
  namespace's `names` map — the copy IS the import mechanism), and `resolveAttr`'s dedupe
  only prefers a named namespace over `<root>`; it cannot tell two named namespaces
  apart. Answering "which namespace's body textually declares this symbol" needs
  information nothing currently records. **As landed:** a free name found in the
  rule's/macro's own namespace scope qualifies to THAT namespace (`NS::name`), full stop.
  This is correct whenever the injected reference resolves — which is every case except a
  rule namespace that (a) borrows a helper via its own `uses` AND (b) the borrowed name is
  then qualified to a namespace that doesn't re-export it... and since `uses`-copying is
  exactly a re-export into the borrowing namespace's scope, pass-2 resolution of
  `RuleNS::name` finds the copied symbol anyway. The subtlety is thus about *provenance
  cosmetics* (`--expand` shows `RuleNS::helper` instead of `TrueHome::helper`), not
  correctness — wait-and-see. **Follow-up if ever needed:** a `homeNs` field on `Symbol`,
  stamped at declaration time by the Resolver's gather (the only place that knows), then
  qualification consults it — a two-line stamp plus a lookup swap.

This lands as its own commit with a dedicated test: a rule namespace with a private helper
`fmt()`, a use-site file with its *own* `fmt()`, and the template's call must hit the
rule's.

## 11. Struct reification (stretch)

`comptime Point p = f();` — folding a value-struct result. The blocker is that the language
has no object-literal syntax, so a struct value must reify as a **constructor call**:

- Reify `Object` values **only** when `cls->isValue` and the struct declares a constructor
  whose parameter list corresponds **1:1, by name and order, to its fields** (the
  `Pair::Of(a, b)`-style shape). Then reify as `Name(cls)(reify(field₁), …)` — a Call whose
  callee is the class name, arguments in field order.
- Anything else — reference classes, structs without a field-shaped ctor — stays **M28**
  ("not reifiable; give `Point` a constructor taking (int x, int y) matching its fields, or
  return primitives/arrays"). Reference classes stay non-reifiable permanently (identity
  has no compile-time meaning — main design deviation 5).
- Nested structs recurse; a struct containing a reference-class field is M28.

Marked stretch: nothing in §3–§8 depends on it, and the ORM/routes drivers don't need it.
Ship it last or let it slip to Phase 4 without blocking.

**[landed]** As specified, in `reify()`'s new `VKind::Object` case: field order taken from
`cls->decl->body` (declaration order, same source `buildMetaValue`'s meta.Class walk
uses); a field-shaped ctor found by exact arity + per-position name match against that
field list; reifies to a `Call` whose callee is a bare `Name(cls->name)`, args in field
order via recursive `reify()` (nested structs and struct-valued fields fall out for free
from that recursion, no special-casing needed). Reference classes (`!cls->isValue`) and
struct-without-matching-ctor both hit the pre-existing `return false` path, surfaced by
the (updated) generic "not reifiable" message at every caller. Verified: a flat struct
(`Point`), a struct nesting another struct (`Line` holding two `Point` fields, via a
function call rather than a bare literal — `diagonal()`), M28 on a reference class, and
M28 on a struct whose ctor doesn't match its fields — all in `comptime_reify_struct.ext`
(the flat+nested cases, oracle==IR==ELF, `--expand` confirming genuine constructor-call
folding) and `test_meta.cpp` (both M28 shapes).

## 12. Diagnostics catalog (extends M01–M18)

| # | Sev | Trigger | Anchored at |
|---|---|---|---|
| M19 | E | `where` didn't yield bool | where expr, naming the rule |
| M20 | E | `where` evaluation failed (throw/hermetic/budget) | where expr + reason |
| M21 | E | `$for` iterator didn't yield an array | iterator expr |
| M22 | E | macro parameter spliced more than once in its template | macro decl (static, at collection) |
| M23 | E | unknown macro `name!` (or ambiguous across imports) | call site |
| M24 | E | macro expansion produced another macro call (no re-entry) | inner call, note at outer |
| M25 | E | `bottom of body` after a value return (arrow body) — unreachable | anchor, note at subject |
| M26 | E | `marker "n"` not found in the subject's body | anchor, naming subject |
| M27 | E | body/`$_params`/`$_args` anchor or hole on a non-callable subject | anchor/template span |
| M28 | E | struct not reifiable (no field-shaped ctor / reference class) | comptime site |
| M29 | E | macro call inside a `comptime if` **condition** (the condition feeds the imports map macro resolution needs — the one true cycle; §7's deferral cannot apply) | the call, note naming the `comptime if` |

Plus one behavior change: **M15 is deleted** (`uses` inside `comptime if` becomes legal,
§9). M29 is a Phase-3-implementation addition (see §7's "§7×§9 interaction") — landed in
both the item-level (`foldTopLevelItem`) and nested (`walkStmt`'s `If` case) comptime-`if`
paths, each reporting exactly once (the C-late re-walk revisits the same untouched node
and must not double-report).

## 13. File-level change map and sequencing

| File | Change |
|---|---|
| `src/Ast.hpp` | `ExprKind::ForSplice`; `Expr::isMacroCall`; `Stmt::isMacroDecl` |
| `src/Lexer.*` | — (nothing) |
| `src/Parser.*` | `macro` decls; `name!(` postfix; `@anchor("…")` statement; `$for` element parsing in fragments; `parseItemsFragment` |
| `src/Resolver.cpp` | skip type resolution inside comptime-`if` branches (§9.1) |
| `src/Eval.*` | `evalComptime` locals overload |
| `src/Rules.hpp/.cpp` | unified `Binding` (§2); `meta.*` materialization + cache; `where` in `tryMatch`; `$for`/`$_params`/`$_args` in clone; macro collection/expansion; body/marker/namespace anchors; pass split + in-engine `computeFileImports` + top-level flatten (§9); def-site qualification (§10); reifier struct case (§11) |
| prelude (`Resolver.cpp`) | `namespace meta { Param, Field, Method, Class }` |
| `src/main.cpp` | drop `rimports` plumbing (engine computes; ctor signature shrinks) |
| `src/Lower.cpp/.hpp` | *(unplanned; discovered by §14's `rule_forward_args`)* closure conversion captures the receiver under `"this"`; class context survives into lambda bodies; `thisReg()` replaces hardcoded-r0 receiver sites |
| `src/AstPrinter.cpp` | ForSplice, macro decls/calls, named-Empty markers |
| docs | reference §6.9 additions; techdesign Phase-3 checklist → link here |

**Commit sequence** (each independently green, pushed as it lands — the aggressive-VC
discipline):

1. ✅ **Binding unification + `meta.*` + `where`** (closes Gap A/B; M19/M20) — the highest
   silent-wrong risk goes first. *(landed in `d2ba3c0`)*
2. ✅ **Pipeline split + conditional `uses`** (§9; deletes M15) — restructures `run()` while
   the engine is still small; everything later builds on the split passes. *(landed; see
   the §9 field notes — includes the `skip_concat` harness mechanism and, riding the same
   verification pass, the `Lower.cpp` this-capturing-closure fix described in §14)*
3. ✅ **`$for` + `$_params`/`$_args`** (M21, M27-part) — unlocks the ORM corpus example.
   *(`$_params`/`$_args` landed in `d2ba3c0`; `$for`/M21 landed in `6016c25`, with
   `rule_orm` + twin)*
4. ✅ **Body + marker + namespace anchors** (M25/M26/M27). *(landed in `d2ba3c0`)*
5. ✅ **Expression macros** (M22/M23/M24). *(core landed in `6016c25` — decl/call
   parsing, collection, M22 static check, M23 unknown+ambiguous, qualified `NS::name!`,
   M24, bottom-up argument nesting, `macro_safestrip` corpus test.)*
   - 5b. ✅ **Deferred comptime folding** — C-early/C-late split (macro-containing
     comptime sites fold after the macro walk) + **M29** (macro in a comptime-`if`
     condition, both item-level and nested) + the `macro_safestrip` comptime-fold case
     restored + metatests (a CLEAN compile check, two M29 ERRORS checks). The
     C-early-reads-C-late unknown-name corner is verified (see §7) but not given a
     dedicated metatest — it degrades to an ordinary "cannot resolve" error, not a new
     diagnostic, so there is nothing M-numbered to pin.
   - 5c. ✅ **Template-clone macro expansion** — macro calls inside rule/anchor templates
     expand right after cloning, def-site scoped (template spans land in the rule's file,
     so existing file-based scoping already resolves correctly); `macro_in_rule_template`
     corpus test (macro's namespace never `uses`d at the use site — proves the scoping is
     genuinely definition-site) + a metatest.
   *(5b/5c in tree, uncommitted at this sync)*
6. ✅ **Def-site qualification** (§10) with its capture test. *(landed in `6016c25`;
   verified via the doc's own scenario: rule-NS `fmt()` beats a same-named use-site
   `fmt()` — now also a formal corpus test, `rule_defsite_qual`, output `web:1`, plus a
   metatest. The re-export/true-home refinement is recorded in §10 as provenance-cosmetic
   and deferred — `Symbol::homeNs` if ever needed)*
7. ✅ **Struct reification** (M28) — stretch, done anyway. *(landed — in tree,
   uncommitted at this sync; `comptime_reify_struct` corpus test — flat + nested structs,
   oracle==IR==ELF — plus two M28 metatests: reference class, mismatched-ctor struct)*
8. ✅ Docs + memory sync. *(this sync — third and final pass for Phase 3; every item
   above is landed, every corpus/metatest line in §14 is pinned, suite green 26/26)*

## 14. Testing plan

- **Corpus (each with an oracle-run `.expected`, run on `--run`/`--ir`, ELF-verified
  manually or via twin):**
  `rule_where` (predicate gates firings; a twin proves the gated set), `rule_orm`
  (proposal §10.2 nearly verbatim: `@Table`/`@Column`, `$for` over `C.fields` filtered by
  `hasAttr`, `member of` schema method — **with hand-written twin**, the Phase 3
  acceptance gate), `rule_forward_args` (`$_params`/`$_args` route lambda), `rule_body_top`
  (additive prologue), `rule_marker` (two rules stacking on one `@anchor`),
  `rule_ns_inject` (namespace-anchored registration table), `macro_safestrip`
  (`x!` in expressions, incl. under a `comptime` fold), `comptime_uses` **project**
  (two lib namespaces, a comptime-if selecting one; fire/no-fire of the selected lib's
  rules proves imports recompute — oracle==IR==ELF via the project harness).

  **[state]** All landed, all oracle==IR, ELF-verified (manually, since the automated ELF
  corpus runner scans only `tests/corpus/*.ext` directly — it does not recurse into
  `tests/corpus/meta/`): `rule_where`, `rule_forward_args`, `rule_body_top`, `rule_marker`,
  `rule_ns_inject`, `rule_orm` + `rule_orm_twin` (byte-identical — note the meta.*-surface
  adaptation recorded in §5's landed note: columns are named by field, since attribute
  VALUES aren't reflected), and `macro_safestrip` — including its `comptime`-fold case,
  restored once commit 5b's deferral landed (`--expand` confirms the value is genuinely
  baked to a literal, not left as a call) — (all in `tests/corpus/meta/`), plus
  `comptime_uses` (`tests/corpus/project/`, carrying the `skip_concat` marker — §9 field
  note 2). Three tests beyond this plan's original list, added for the §7×§9 follow-ons
  and §11: `rule_defsite_qual` (§10's capture scenario, output-proof: `web:1`, not
  `userland:1`), `macro_in_rule_template` (5c: a macro call inside a rule template,
  scoped to a namespace the use site never `uses`), `comptime_reify_struct` (§11: flat
  and nested value-structs folding to constructor calls).
  Field note: `rule_forward_args` passed the oracle but failed `--ir` —
  not a stage bug, a pre-existing lowering gap it exposed: closure conversion
  (`Lower.cpp`) never captured the receiver, so `this` inside any lambda was the closure
  object (the injected `($_params) => this.$m($_args)` route lambda, but equally
  hand-written `this.m()`, bare member reads/writes, and bare self-method calls in
  closures). Fixed alongside §9: `MakeClosure` now snapshots the receiver under the
  keyword name `"this"`, the class context survives into lambda bodies, and every
  implicit-receiver site lowers through `thisReg()` instead of a hardcoded r0. All forms
  now agree oracle == IR == ELF (no new opcodes; both native backends' name-keyed
  capture ops cover it).
- **metatests negatives:** one per M19–M28; plus: `where` referencing an unbound name
  (M20 path), `$for` over a non-array, macro ambiguity across two imports, marker in a
  *non-matched* method's body (M26), `$_args` in a non-call position, def-site
  qualification capture attempt (rule-NS helper vs same-named use-site function — output
  must prove the rule's was called).
  **[state]** All landed in `tests/test_meta.cpp` (70 checks, 0 failures). M19/M20 and
  M25/M26/M27 negatives; the old M15-restriction test replaced by two CLEAN tests proving
  `uses` inside both taken and untaken comptime-`if` branches now compiles (§9); M21
  (non-array iterator); M22 (double-splice, static); M23 unknown AND ambiguous, with the
  qualified `NS::name!` escape proven CLEAN; M24 template re-entry, with a CLEAN check
  proving argument-position nesting (`dbl!(inc!(3))`) does NOT trip it; M28 both shapes
  (reference class, mismatched-ctor struct); M29 both shapes (item-level and nested
  comptime-`if` condition); a CLEAN check for a macro call under a `comptime` fold
  (5b) and one for a macro call inside a rule template (5c); a CLEAN check for §10's
  capture scenario (the output-proof version is the `rule_defsite_qual` corpus test).
- **Determinism:** golden `--expand` for `rule_orm` and `rule_marker` (stacking order).
  **[state]** Not built as a separate automated fixture — no `.expand`-golden mechanism
  exists in the harness (would need a new runner script + CMake target, since `--run`'s
  `.expected` files check output values, not `--expand`'s AST-shaped dump). The property
  this line cares about — deterministic rule-firing order — is exercised indirectly but
  repeatedly: `rule_marker.expected` pins one exact stacking order, `orderRules()`'s
  `stable_sort` is what guarantees it stays that order run over run, and both `rule_orm`
  and `rule_marker` run on every `ctest` invocation across both engines. A dedicated
  golden-`--expand` fixture would catch a *shape* regression (same values, different AST)
  that a value-diff test cannot — worth adding if that ever proves to be a real failure
  mode, not manufactured now for its own sake.
- **Zero-cost guard** unchanged: the whole legacy corpus must stay byte-identical (`hasMeta`
  false → none of this runs).

## 15. Risks and open calls for Leonard

| Risk / call | Position taken here |
|---|---|
| `meta.*` field set is a forever-API | Kept to 4 classes, strings for types; growth is additive |
| Macro single-splice rule may surprise | It's the honest option in expression position; error text teaches the workaround (bind at the call site). Alternative — auto-hoisting via statement-lifting — is Layer D machinery |
| `generics` field reuse for macro params | Zero-AST-growth hack; documented inline. If it reads too clever in review, a dedicated `macroParams` vector is +8 bytes/Stmt |
| Conditional `uses` can't see rule-injected code | Stated one-direction pipeline; matches the no-reentry stance everywhere else |
| Statement-position `$for` omitted | Array form covers known drivers; add on demand |
| **Q1:** should `where` see `meta` classes only, or also user comptime globals? | Design says both (it's ordinary comptime code; `defineGlobal`'d comptime vars are already visible). Confirm that's wanted |
| **Q2:** marker search = subject's body only, or any body in the matched class? | Subject-only (v1) keeps reach tied to the match; widen later if a real case needs class-wide markers |
| **Q3:** struct reification in-phase or slip to 4? | Sequenced last; either answer is fine |

---

*Companion to `designs/complete/techdesign-metaprogramming.md` (Phases 1–2 marked DONE there). On
acceptance, implementation follows the §13 commit sequence.*
