# Sonar — Tech Design 06: The Template Layer (`sonar!`)

**Status:** design, pre-implementation. **Date:** 2026-07-12. **Track:** T06.
**Owns:** `sonar/src/templates/*.lev` (the `sonar!` procedural macro — comptime Leviathan code shipped IN the package), the `.sonar` file grammar.
**Depends on:** **F4 procedural macros (HARD gate — nothing here builds without it)**; F3 bound refs (handler splices); T01/T02 (construction targets); T04/T05 (the per-component attribute tables — this doc's §3.4 table aggregates them). LA-20 comptime `import()` + trident `assets` (landed) for external templates.
**Gates:** G-S4. **Difficulty:** L, risk MED (diagnostics quality is the make-or-break).

Conforms to anchor: §4/F4 frozen macro surface, C11 attribute mini-contract, R6 (expansion uses setter statements, never fluent chains), cheat-sheet §7.

---

## 1. Design position

`sonar!` is **compile-time sugar with zero runtime**: the macro body (ordinary comptime Leviathan code, per F4) parses the tag payload, builds Leviathan source text, and returns `meta::parseExpr(generated)`. The expansion is exactly the construction code a user would hand-write; `leviathan --expand` shows it; distrust is answered by reading it.

Three consequences:
1. The template grammar is **package-defined, not language-defined** — the language sees only a string.
2. All template errors must be reported **against the template text** (call-site span + offset per F4), with the payload-caret rendering F4 specifies. This doc owns the error catalog.
3. Cost-identical to hand-written code — no reflection, no builder indirection beyond what the user would write.

## 2. The grammar (normative EBNF)

```
template   := node
node       := '<' Tag attrs '/>'
            | '<' Tag attrs '>' content* '</' Tag '>'
content    := node | text | hole | for | if
attrs      := (attr)*
attr       := Name '=' '"' literal '"'          -- typed literal
            | Name '=' '{' balanced-expr '}'    -- expression hole
            | 'on:' Name '=' '{' balanced-expr '}'
            | 'id' '=' '"' Name '"'
text       := any run without '<' '$' '{'       -- whitespace-trimmed per line
hole       := '${' balanced-expr '}'            -- text interpolation
for        := '$for' Name 'in' balanced-expr '{' content* '}'
if         := '$if' balanced-expr '{' content* '}' ('$else' '{' content* '}')?
```

- **Tag** = a class name resolved in the caller's scope (any component, including user-defined ones — the macro emits `Tag(...)` and ordinary resolution applies; `uses Sonar;` at the call site is the normal setup). Tags are NOT a closed set — the macro never validates the class exists; the compiler does, at the expansion's re-resolve, with the call-site span. This is deliberate: user components work for free.
- **`balanced-expr`** = raw text captured to the matching close brace, brace-counting through nested `{}` and skipping string literals. The macro does NOT parse expressions — it splices them textually and lets the compiler parse the expansion (errors land with call-site spans; acceptable v1, flagged §6.2).
- **Whitespace**: each text line is trimmed; lines that trim to empty vanish; surviving lines join with a single space. Explicit spaces via `${" "}`. (Markup indentation must not leak into `Text` content — the rule that makes templates formattable.)
- **Escapes**: `\{`, `\<`, `\$`, `` \` `` in text position produce the literal character. The payload arrives raw from the quasiliteral; backtick escaping is only needed for inline (non-`import`) templates.

## 3. Expansion (normative shapes)

### 3.1 The construction lambda

A template expands to an **immediately-invoked block-body lambda** so it works in any expression position and can carry statements (`id` assignments, `$for` loops):

```lev
// sonar!(`<ContentBox title="Files" border={BorderStyle::Single}>
//            <ListView id="files" flex="1"/>
//        </ContentBox>`)
// expands to:
(() => {
    var __sonar_0 = ContentBox();
    __sonar_0.setTitle("Files");
    __sonar_0.setBorder(BorderStyle::Single);
    var __sonar_1 = ListView();
    __sonar_1.setHeight(Constraint::Flex(1));   // via the layout-sugar table
    this.files = __sonar_1;                     // id binding
    __sonar_0.add(__sonar_1);
    return __sonar_0;
})()
```

Rules: node locals are `__sonar_<n>` (F4's uniqueness convention — a per-expansion counter); construction order is document order; `add` calls happen after the child's own attrs/children (bottom-up attach so `onAttach` fires once, on the final tree... **correction**: attach order — children are added to parents as encountered so parent links exist top-down; `onAttach` fires per add; a child added to a not-yet-attached parent fires again when the ROOT attaches? No — C7's onAttach fires on `add`. The expansion adds leaves to parents before the root is returned/attached, so subtree adds fire onAttach with a detached root. **Ruling:** onAttach semantics are "attached to a parent," not "attached to the running app" — components needing app services (timers) must tolerate `Sonar::app()` at attach time (it exists whenever a running app constructs UI) — documented, and the alternative (defer attach events until rooted) recorded as v2 if it bites.)

`this` inside the lambda: block-body lambdas capture `this` (landed closure semantics) — `id` binding and `on:` handler references work inside methods/constructors of the enclosing class. Using `id`/`this.method` handlers in a free-function context is a compile error at the expansion (no `this`) with the call-site span — error catalog entry E7.

### 3.2 Attribute forms → code

| form | expansion | notes |
|---|---|---|
| `attr="lit"` | `node.setAttr(<typed lit>);` | literal typed by the T04/T05 table: string / int / bool / enum member (`border="Single"` → `BorderStyle::Single` — the table names each attr's enum) |
| `attr={expr}` | `node.setAttr(expr);` | raw splice, compiler checks |
| `on:event={expr}` | `node.onEvent(expr);` | expr is a handler: a lambda, or a bound ref (`this.save`, F3) |
| `id="name"` | `this.name = node;` | user must have DECLARED the field; type errors surface at re-check with call-site span |
| `width="30"` / `width={c}` | `node.setWidth(Constraint::Fixed(30))` / `setWidth(c)` | int literal ⇒ Fixed |
| `flex="1"` | `setWidth/setHeight(Constraint::Flex(1))` on the PARENT's main axis — the macro knows the parent's axis only for FlexLayout parents with a literal axis; otherwise `flex` applies to BOTH axes' Flex — v1 ruling: `flex` sets **both** axes to `Flex(n)` (correct for the dominant flex-in-flex case), `width`/`height` override per-axis. Documented loudly. |
| `minWidth/maxWidth/minHeight/maxHeight="n"` | folded into a `Constraint::Bounded` per axis (macro merges the axis's min/max/flex attrs into ONE setter call) |
| `dock="Top"` | `node.dock = Dock::Top;` |
| `row/col/rowSpan/colSpan="n"` | grid fields |
| `padding="1"` | `node.padding = Insets::Uniform(1);` |
| `theme="key"` | `node.setStyle(...)` v1: maps to Styleable's instance override of key `"base"`? — **v1 ruling:** `theme` attr sets the component's theme-key PREFIX override (`node.themePrefix("key")`, a Styleable addition T04 owns... not in anchor). **Deferred:** `theme` attr is v1.1 pending a Styleable prefix ruling; the macro rejects it with E9 "not yet supported" — honest beats broken. |
| `hidden={b}` | `node.setVisible(!b);` |
| `tabLabel="x"` | consumed by the PARENT `Tabs` expansion: `parent.add("x", child)` instead of `add(child)` |

### 3.3 Text, holes, control

- Text runs (with `${...}` holes) expand to a `Text` child: `parent.add(Text().text("Hello " + (name) + "!"));` — holes concatenate with `+` (no string interpolation exists in the language; concat is the expansion). A hole of non-string type relies on `+`'s overloads or the user writes `${x.toString()}` — E-catalog documents the error shape.
- `$for item in expr { ... }` → a runtime loop in the lambda: `for (var item in expr) { <content expansion, adding to the current parent> }`. The loop variable is in scope for holes/attr-exprs inside.
- `$if cond { A } $else { B }` → `if (cond) { ...A... } else { ...B... }`. When `cond` is comptime-evaluable the macro CANNOT know (it doesn't evaluate) — v1: always runtime `if`; the "comptime-if" of the sketch is an optimization the compiler's comptime folding may do for free on constant conditions. Honest note replacing the sketch's claim.

### 3.4 The aggregated attribute registry

This doc carries the full table: the C11 common set (§3.2 above) + every row from T04 §per-component tables + T05 §per-component tables, in one appendix the macro implementation reads as its data table (a comptime `Map<string, ...>`-shaped decision tree in the macro source; the tables in the docs are the normative source, the macro mirrors them, and a drift test (T10) asserts macro behavior against the doc-derived list).

### 3.5 External templates

```lev
comptime string tpl = import("views/editor.sonar");
var view = sonar!(tpl);
```
`.sonar` files are template payloads (no backticks, no escaping needed); `assets = ["views/**"]` in the app's trident.toml declares them (LA-20). Designers edit `.sonar`; expansion is identical. Convention: one root node per file; the file name is advisory only.

## 4. Error catalog (normative — implementers copy these)

| # | condition | message shape (all carry call-site span + payload offset caret per F4) |
|---|---|---|
| E1 | unclosed tag / mismatched `</Tag>` | `sonar!: '<ContentBox>' opened at offset N is closed by '</ListView>'` |
| E2 | malformed attr (no `=`, bad quoting) | offset caret |
| E3 | unknown attribute for a KNOWN registry component | `sonar!: 'ContentBox' has no attribute 'titel' (did you mean 'title'?)` — Levenshtein-1 suggestion; unknown TAGS pass through (user components), unknown attrs on unknown tags expand as `setAttr` and fail at compile with call-site span |
| E4 | `id` on a `$for` body node | forbidden (N nodes, one field) |
| E5 | unbalanced `{` in a hole | offset caret |
| E6 | `$for`/`$if` syntax | offset caret |
| E7 | `id`/`this.` handler outside a class context | "template uses 'this' but the enclosing scope has none" |
| E8 | empty payload / no root node | |
| E9 | `theme` attr (deferred) | "not supported in v1 — use setStyle after construction" |

## 5. Milestones

| M | contents | difficulty |
|---|---|---|
| M1 | tokenizer + node parser over the payload string (pure comptime .lev; array-of-strings emit buffer + joinToString — the O(n) emit discipline) | M |
| M2 | expansion emitter: nodes/attrs/common table, id, on:, text/holes | M |
| M3 | `$for`/`$if`, tabLabel routing, Bounded-merge, error catalog E1–E9 | M |
| M4 | external `.sonar` + assets wiring + `--expand` round-trip verification | S |
| M5 | the drift-test data table + component registry appendix sync | S |

All gate on F4 landing; M1–M3 can be written and unit-tested EARLY against F4's design by structuring the macro as ordinary functions `string expandSonar(string payload)` testable at RUNTIME (call it in a normal program, assert output strings) before wiring the one-line `macro sonar(string p) comptime => meta::parseExpr(expandSonar(p));` — **this is the de-risking move: the entire template engine is testable today, pre-F4, as a string→string library.** Milestone M0 (before F4): the runtime-testable engine + goldens.

## 6. Potential issues & mitigations

1. **Comptime budget on large views** — a 500-node view runs the parser+emitter under the ~100M step budget. Mitigation: O(n) single-pass design, the M0 runtime harness measures steps-proxy (wall time) early; `--comptime-budget` escape documented.
2. **Attr-expression errors point at the call site, not the exact hole** (v1 splices text). Mitigation: expansion emits each attr on its own line so compiler line info inside the expansion is stable under `--expand`; v2: per-hole sub-span mapping if F4 grows offset-mapping support.
3. **Registry drift** (T04/T05 tables vs macro behavior). Mitigation: the M5 drift test — a generated program touching every registered attribute, snapshot-asserted.
4. **Two same-named user components in different namespaces** — tags splice unqualified; resolution follows the caller's imports. Qualified tags `<A::B>` supported by passing the name through verbatim (grammar allows `::` in Tag). Documented.
5. **Handler holes before F3** — `on:key={this.onKey}` fails until bound refs land. Mitigation: the macro is F4-gated anyway (later than F3 in the landing order); examples use lambdas where independence matters.
6. **Whitespace surprises** — the trim rule is aggressive. Mitigation: documented with examples; `${" "}` idiom; goldens pin it.

## 7. Testing plan

M0 runtime-harness goldens: payload → expansion-string pairs for every grammar production and every error (error tests assert message + offset). Post-F4: `--expand` round-trip corpus (the same payloads through the real macro; expansion compiles; snapshot the built tree via TestRenderer); an end-to-end example app built entirely from one `.sonar` file; differential oracle/IR/LLVM.

## 8. Open questions

1. `theme` attribute semantics (E9 deferral) — needs a Styleable prefix ruling with T04/T08.
2. Two-way binding sugar (`value<->{this.field}`) — v2, after T11 reactivity.
3. Slot/children-projection for user components (`<MyPanel><slot.../></MyPanel>`) — v2.

## 9. Implementation log

- 2026-07-12 — design written; not started. M0 (runtime-testable engine) is startable before F4 lands.
- 2026-07-13 — **IMPLEMENTED IN FULL (M0–M5) and landed.** F4 had already
  landed (info.md §0), so the whole layer shipped in one pass, not just the
  pre-F4 M0 slice.
  - **Code:** `sonar/src/templates/expander.lev` (the `expandSonar(string) ->
    string` engine — parser, emitter, aggregated registry, error catalog
    E1–E9) + `sonar/src/templates/macro.lev` (the one-line
    `macro sonar(string p) comptime { return meta::parseExpr(Sonar::expandSonar(p)); }`).
    `sonar/trident.toml` sources gained `src/templates/*.lev` (C13 shape).
  - **Tests:** `sonar/tests/templates/` — the M0 golden harness driving
    `expandSonar` as a pure string→string library over every grammar production
    and every error (E1–E9), string-asserted against `golden.expected`.
    `sonar/tests/templates-macro/` — the post-F4 round-trip: the real `sonar!`
    over live T04 components (Container/Text/Input/Button/CheckBox), proving the
    expansion compiles, captures `this` for `id`/`on:` inside a method, and
    drives `$for`/`$if` with runtime values (asserts a 6-child tree, id-bound
    fields, and an `on:press` handler firing). `sonar/tests/templates-sonar/` —
    the external `.sonar` + `import()` + `assets` path (§3.5). All green on the
    oracle/IR lane via `trident run`; expansion is pre-lowering so all engines
    agree.
  - **Registry scope (§3.4):** the C11 common set + all eight **T04** component
    tables are landed with exact verified setter names. **T05 composite tags are
    not yet landed**, so they currently fall through the `isKnownTag` gate as
    unknown (pass-through) tags — correct and harmless (they construct + set via
    the `set<Attr>` convention and re-check at compile), and they enter the
    registry verbatim as T05 lands. The M5 drift discipline is the registry's
    single source: the per-component `if` ladder in `SonarEmitter.registryStmt`
    mirrors the T04/T05 doc tables one row per attribute.
  - **DEVIATION from §3.1 (forced, documented in-code):** the expansion is
    `Sonar::__sonarBuild(() => {...})` (a one-line generic thunk-invoker), NOT
    the spec'd bare IIFE `(() => {...})()`. A raw immediately-invoked lambda
    currently fails to resolve *imported-package* call targets in its body at
    runtime (`Sonar::Text()`, `Constraint::Fixed(...)` throw "cannot resolve
    call target") — **bug.md #57** — while the identical block-body lambda
    invoked through an ordinary call resolves them. Semantically identical;
    revert to the raw IIFE when #57 lands.
  - **Two compiler gaps found + filed (house-rule STOP → escalate):**
    **bug.md #57** (imported symbol unresolvable inside an IIFE, above) and
    **bug.md #58** (a single-quoted char literal does not retype to `char`
    inside comptime-evaluated code, so `c == '<'` is silently always-false at
    comptime). #58 is why every character test in the engine goes through
    `ceq(c, "<")` / `.code()` rather than a bare char literal — that is what
    lets the one engine run identically at runtime (goldens) and at comptime
    (the macro). Standalone repros at `sonar/tests/bug-repros/bug5{7,8}/`.
  - **Rulings recorded while implementing:**
    - §3.3 text-content model is uniform: `<Text>hi</Text>` emits a **child**
      `Text().text("hi")` added to the parent, so wrapping bare text around a
      non-container leaf (`<Text>hi</Text>`) is an authoring error — the
      idiomatic form is `<Text text="hi"/>`. Kept per design; noted as a v1.1
      candidate (route text content to a leaf's own `.text()`).
    - §2 whitespace rule is applied per text segment, so an inline space before
      a hole (`Hello ${x}`) is trimmed to `"Hello" + (x)`; `${" "}` is the
      documented explicit-space idiom (goldens pin it).
    - E7 (`id`/`this.` in a `this`-less scope) is left to the compiler's
      re-check of the emitted `this.name = ...` (the macro cannot see call-site
      context), as §3.1 anticipates; E4 (`id` inside `$for`) IS macro-detected.
    - Inline `sonar!("...")` with a double-quoted payload is not supported
      because Leviathan's own lexer interpolates a literal `${` — callers use
      raw backtick payloads (or `import()`ed `.sonar` files), both exempt. The
      M0 harness builds `${` by concatenation for the same reason.
