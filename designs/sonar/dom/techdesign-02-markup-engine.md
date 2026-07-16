# Sonar DOM — Tech Design 02: The Markup Engine (runtime parser, registry, `dom!`)

**Status:** design, pre-implementation. **Date:** 2026-07-15. **Track:** D02.
**Owns:** `sonar/src/dom/{markup,registry,builder}.lev`, `sonar/src/dom/templates/{dom_expander,dom_macro}.lev`.
**Depends on:** D01 (Document/meta recording), T04/T05 component surfaces (setter names), T06 as
prior art (`SonarParser`'s algorithm is adapted, never imported — the landed expander stays frozen),
F4 macros + LA-20 `import()` (landed). Probes: D-P4, D-P7.
**Gates:** G-D1. **Difficulty:** L. **Risk:** MED — registry drift between the two tiers is the
known failure mode; the drift test is load-bearing, not optional.

Implements anchor D-C1 (tag registry) and D-C9 (error catalog). The grammar below supersedes T06's
for the `dom!` tier; the landed `sonar!` macro is untouched and remains supported indefinitely.

---

## 1. Design position: one grammar, three tiers

| tier | spelling | holes | tags | cost |
|---|---|---|---|---|
| comptime | `dom!(\`markup\`)` | `{{any-expr}}` text bindings, `{expr}` attr values, `on:` handlers, `$for`/`$if` | registry + Capitalized user classes | zero runtime parse; expansion is typed construction code (`--expand`-legible) |
| asset | `FlexContainer(import("view.sonar"))` | `{{identifier}}` only | registry + `registerTag` extras | runtime parse of a comptime-hashed string |
| runtime | `FlexContainer(anyString)` | `{{identifier}}` only | registry + `registerTag` extras | runtime parse |

The **runtime registry is the single source of truth** for tags and attributes; the `dom!` emitter
mirrors it row for row and the D08 drift test holds them equal. The asymmetries are principled, not
accidental: an AOT language has no eval, so runtime tiers cannot evaluate expressions (`{{key}}`
resolves through D05's exposure/store) and cannot construct unregistered classes (Capitalized tags
are a comptime-tier feature, where the expansion is compiled).

## 2. Grammar (normative EBNF — extends T06's shapes; lowercase tags; fragments)

```
fragment  := (node | text)*                       -- ctor/builder accepts fragments
node      := '<' tag attrs '/>' | '<' tag attrs '>' fragment '</' tag '>'
tag       := lower-name | Capitalized-name        -- Capitalized: dom! tier only (E-D3 at runtime)
attrs     := (name '=' '"' literal '"' | name '=' '{' expr '}' | 'on:' name '=' '{' expr '}')*
text      := runs with '{{' expr '}}' holes       -- T06 whitespace rule: trim lines, join with ' '
for       := '$for' name 'in' expr '{' fragment '}'          -- dom! tier only
if        := '$if' expr '{' fragment '}' ('$else' '{' fragment '}')?   -- dom! tier only
```

- `{{…}}` is the binding hole (text position; renders via `Sonar::Dom::text(…)`, updates via D05).
  `${…}` (T06's one-shot interpolation) is **also accepted** in the dom! tier for continuity; it is
  a one-shot splice, never a binding — the distinction is the point, both documented.
- Escapes: `\{`, `\<`, `\$`, `` \` `` as in T06; additionally ``/`` are rejected in
  attribute literals (D01 meta-blob separators, E-D2).
- Multi-root fragments are legal everywhere a fragment host exists: the `FlexContainer(markup)`
  ctor appends all roots as children; `dom!` with multiple roots auto-wraps them in a vertical
  `FlexContainer` (visible in `--expand`; single-root payloads return the root itself).

## 3. Runtime side

### 3.1 `DomParser` (`markup.lev`)

Recursive descent adapted from the landed `SonarParser` (same balanced-brace capture, same
whitespace rule), with two upgrades: **line:col tracking** on every token (errors carry
`markup:12:8` + a caret line — the T08 TOML taxonomy), and fragment roots. Output: `DomTplNode`
trees (`class`, mirroring `TplNode`'s shape: element/text/hole kinds; `$for`/`$if` kinds parse but
throw E-D7 when reached by the runtime builder). Character tests via `.code()` — no char literals
in call-argument position. Emit buffers via `StringBuilder.add` (emit-C++ `(<<)` gap).

### 3.2 `DomRegistry` (`registry.lev`)

Parallel columns (D-C1 table preloaded at first touch):

```
Array<string> tagNames_;
Array<() => IComponent> factories_;
Array<(IComponent) => bool> matchers_;                       // tag reverse-match, D-P8
Array<(IComponent, string, string) => bool> appliers_;       // (node, attrName, literal) -> handled?
```

`registerTag(name, factory, matcher, applier)` — public; user components join the runtime tiers
with one call (duplicate name throws). `create(tag) -> IComponent`, `tagMatch(c) -> string`,
`apply(tag, node, name, value) -> bool`. Appliers are if/else ladders over attribute names (the
`keyCodeFromName` precedent — no `Map<string, enum>` shapes), typed per the T04/T05 tables:
ints via `toInt()` (E-D5 on None), bools `"true"/"false"` (E-D5 otherwise), enums via per-enum
name ladders (`align="Center"` → `Align::Center`, E-D5 with the legal-values list), strings verbatim.

**Common applier** (runs before per-tag rows, every tag): `id` (→ `doc.registerId` + meta),
`class` (space-split → D03 `addClass` each; `hidden` couples visibility), `hotkey` (→ D04
normalize + pending-shortcut), `action` (→ D04 meta), `width`/`height`/`flex`/`min*`/`max*`
(→ `Constraint` per the T06 folding rules: bare int = `Fixed`, `flex` sets both axes `Flex(n)`,
min/max merge into `Bounded`), `dock`/`row`/`col`/`rowSpan`/`colSpan`, `padding`
(→ `Insets::Uniform`), `hidden` (→ D03 semantic class), `title`, `theme` (E-D6: unsupported, the
T06 E9 precedent). `on:` at runtime is E-D8 (handlers are expressions; runtime wiring is
`node.on(event, fn)` after build — documented idiom).

### 3.3 `DomBuilder` (`builder.lev`)

`IComponent build(DomTplNode root, Document doc)` / `Array<IComponent> buildFragment(…)`:
depth-first: `registry.create(tag)` → common applier → per-tag applier (unhandled attr on a KNOWN
tag: E-D4 with the edit-distance-1 suggestion, the T06 E3 mechanic; on a registered-unknown tag:
stored as meta only) → `doc.__recordBuild(node, tag, names, values)` → children built and `add`ed
(parents first, so attach fires with live parent links — the T06 §3.1 ruling) → text runs become
`Text` children; `{{key}}` holes register D05 runtime bindings keyed by the identifier (E-D9 if the
hole is not a bare identifier at runtime tier).

`FlexContainer(markup)` (class itself owned by D03) delegates: parse → buildFragment → add each.
Build errors carry the tag path (`<flex> > <menu> > <menuitem>`) alongside line:col.

## 4. Comptime side (`templates/dom_expander.lev`, `templates/dom_macro.lev`)

```lev
macro dom(string payload) comptime { return meta::parseExpr(Sonar::Dom::expandDom(payload)); }
```

`expandDom(string) -> string` — a pure string→string engine (the T06 de-risking shape: fully
unit-testable at runtime, goldens against expansion strings). Emission mirrors the landed emitter's
conventions: `__sonar_<n>` locals, setter statements (never fluent chains), the `__sonarBuild`
thunk wrapper (**note:** bug #57 is fixed at source, so a bare IIFE is now legal — the thunk is
retained anyway for `--expand` uniformity with `sonar!`; revisiting is a one-line change, logged as
an open question). Differences from `expandSonar`:

- lowercase tags resolve through the D-C1 table to class names (`<menuitem>` → `MenuItem()`);
  Capitalized tags pass through (user classes).
- `{{expr}}` text holes emit a `Text` child + a **binding registration** instead of a one-shot
  concat: `Sonar::Dom::bindText(__doc, __sonar_7, "filename", () => Sonar::Dom::text(filename));`
  — the closure captures whatever the expression names (bare globals included: **this is the
  full-fidelity path for the target sketch**, D05 §3). The source text of the hole is passed for
  the serializer/inspector.
- common attrs emit the same effects the runtime applier performs, as typed code (`id="x"` emits
  BOTH `this.x = __sonar_n;` when an enclosing `this` context exists — the T06 field-binding rule —
  AND `__doc.registerId("x", __sonar_n);`); `hotkey`/`action`/`class` emit their D03/D04 calls.
- `$for`/`$if`/`on:`/`{expr}` attr holes: exactly T06's semantics.
- expansion binds `var __doc = Sonar::Dom::document();` once at the top of the thunk.

Error catalog shared with the runtime tier; at comptime a throw surfaces as the F4
`macro 'dom' body threw: <message>` diagnostic with the call-site span.

## 5. Error catalog (normative, both tiers; every message carries line:col + caret)

| # | condition |
|---|---|
| E-D1 | unclosed / mismatched tag (`'<menu>' opened at 3:5 is closed by '</menuitem>'`) |
| E-D2 | malformed attribute; reserved separator byte in a literal |
| E-D3 | unknown tag (runtime tier; names `registerTag` and the registry list) / unknown lowercase tag (dom! tier) |
| E-D4 | unknown attribute on a known tag (+ did-you-mean) |
| E-D5 | attribute literal fails its type (int/bool/enum; lists legal enum members) |
| E-D6 | `theme` attribute (deferred, the T06 E9 stance) |
| E-D7 | `$for`/`$if` reached by the runtime builder (comptime-tier features; says so) |
| E-D8 | `on:` handler in a runtime-tier parse (points at `node.on(…)`) |
| E-D9 | non-identifier `{{hole}}` at runtime tier (points at `expose()`/store or the dom! tier) |
| E-D10 | selector syntax (D01's, listed here for the one catalog) |
| E-D11 | unknown event name in `DomNode.on` |
| E-D12 | empty payload where a node is required |

## 6. Milestones

| M | contents | difficulty |
|---|---|---|
| M1 | DomParser (fragments, line:col, holes) + error catalog rows E-D1/2/5/12 | M |
| M2 | registry: D-C1 preload, factories/matchers/appliers, registerTag; common applier | M |
| M3 | builder: build/buildFragment, meta recording, text/holes, path-carrying errors | M |
| M4 | expandDom + `dom!` macro: registry-mirrored emission, bindings, fragments, $for/$if | M/L |
| M5 | tier-equivalence + drift harness with D08 (same markup → same tree, all tiers) | S |

## 7. Potential issues & mitigations

1. **Registry drift (the known killer)** — three mirrors exist (runtime appliers, dom! emitter,
   the docs' tables). Mitigation: the D08 drift test builds one all-tags/all-attrs corpus file and
   asserts (a) runtime tree == dom! tree via serializer + snapshot equality, (b) every registry row
   is exercised (coverage counted in the registry itself under a test flag).
2. **Comptime budget on large views** — the T06 O(n) single-pass discipline; the M0-style runtime
   harness for `expandDom` measures early; `--comptime-budget` escape documented.
3. **Attribute typing divergence between tiers** (runtime parses `"30"`; dom! emits `Fixed(30)`
   from the same literal) — the applier and emitter share one spec table in this doc (§3.2); the
   drift corpus includes every typed-attr shape.
4. **Fragment auto-wrap surprise** in `dom!` — visible in `--expand`, documented; single-root
   payloads unaffected.
5. **`import()` into a ctor param (D-P7)** — probe first; fallback is the T08 comptime-global
   shape, cosmetic only.
6. **Parser reentrancy/allocation churn** — one parser instance per parse, all state instance-local;
   no namespace-global carriers (#73 discipline).

## 8. Testing plan

Golden corpus: payload → (runtime tree serializer output, dom! `--expand` string, built-tree
snapshot) triples for every grammar production; every E-D row asserted with line:col; tier
equivalence (same payload, three tiers, one snapshot); registerTag user-component round-trip;
attribute typing matrix; fragment ctor + auto-wrap; the target-feel markup verbatim as a fixture.
Differential oracle/IR/LLVM; emit-C++ compile-only.

## 9. Open questions

1. Drop the `__sonarBuild` thunk now that #57 is fixed? (One line; wants a soak on all engines.)
2. `on:` runtime tier via named-action indirection (`on:press="save"` → action fire) — v1.1, would
   close the runtime-handler gap without eval.
3. `.dom` as a distinct asset extension — rejected v1: assets are grammar-agnostic strings; `.sonar`
   stays the convention.

## 10. Implementation log

- 2026-07-15 — design written; not started.
