# Sonar DOM — Tech Design 00: Overview & Anchor

**Status:** design, pre-implementation. **Date:** 2026-07-15. **Suite:** Sonar DOM (tracks D01–D08).
**Target artifact:** `designs/sonar/target-feel.md` — the owner's acceptance sketch. Every track in
this suite exists to make that program (or its two-line-delta twin, §2) compile and run.
**Depends on:** the LANDED Sonar framework T01–T11 (all in `designs/complete/`), the landed
metaprogramming stack (F4 macros, Layer-B rules, comptime `import()`), tasks/await (LA-30),
`sysListDir`/`sysStat` filesystem floor (Track 08). **No unlanded language features are load-bearing
anywhere in this suite** — every fidelity gap has an in-suite mechanism; language requests filed here
are conveniences, not gates.
**Engine lanes:** oracle / IR / LLVM full; emit-C++ compiles the non-loop surface (the standing T09
lane matrix). ELF/X64Gen: no work, no gate, ever (frozen).

---

## 1. Mission

Give Leviathan's Sonar TUI framework a **DOM-shaped developer surface**: runtime markup, CSS-style
selectors (`query('#file-menu')`), class lists (`class="hidden"`), absolute positioning
(`position.method = PositionMethod::Absolute`), named action registries (`actions.add('open', …)`),
mustache text bindings (`{{filename}}`), Promise-based dialogs (`await openDialog.show()`), and the
missing `<textarea>` component — so a web developer's muscle memory writes working TUIs on day one.

The DOM module is **a layer, not a rewrite**. T01–T11 are landed, validated, and byte-identical
across engines; nothing here reopens them. New code lives in `sonar/src/dom/**` plus one new
component file; the only edits to landed files are three additive `Component` fields, one additive
`Styleable.resolve` extension, and the sanctioned Sonar bug #4 paint fix — all flagged in the
anchor log (§10) with the T05/T07/T11 additive-extension precedent.

## 2. The target feel, line by line (fidelity contract)

| sketch line | mechanism | fidelity |
|---|---|---|
| `SonarApp app = SonarApp();` | `class SonarApp : App` (D01) — owns the `Document`, `start()` alias | **literal** |
| `string \| None filename;` (top-level global) | ordinary landed union global | **literal** |
| `FlexContainer main = FlexContainer(\`…\`)` | `FlexContainer : Container` with a markup ctor (D02/D03). **Delta:** backtick raw strings are legal only in macro-argument position (reference.md §6.9), so the literal spelling is `dom!(\`…\`)` (comptime tier, full fidelity incl. expression holes) or `FlexContainer(import("main.sonar"))` (asset tier). `FlexContainer("…")` with a double-quoted string also works (runtime tier, escaped quotes). The backtick-ctor spelling lands for free if `designs/requests/request-string-literal-tail.md` (raw strings, already filed, owner sign-off pending) is accepted. | **two spellings today; literal pending an existing request** |
| lowercase tags `<contentbar>`, `<menu>`, `<menuitem>`, `<textarea>`, `<span>` | the frozen tag registry D-C1 (§5.1) | **literal** |
| `<menuitem id="file-menu" hotkey="^f">File</menuitem>` | `id` → Document index; `hotkey` → pending-shortcut binding via the landed T07 seam; label = text child | **literal** |
| `<textarea id="text">` | **new component** `TextArea` (D06) — nothing landed is a multi-line editor | **new work, spec'd** |
| `<span>{{filename}}</span>` | text binding: pull-sweep + diff (D05); in the `dom!` tier the hole captures the bare global directly — a plain `filename = …` write updates the span with **no registration line** | **literal (dom! tier)** |
| `app.add(main)` | ordinary `Container.add` + Document walk-index (D01) | **literal** |
| `FileDialog openDialog = FileDialog('open')` | `Sonar::Dialogs::FileDialog` (D07) over `sysListDir` — the sketch's `uses MyApp::Dialogs` becomes `uses Sonar::Dialogs` (the framework now ships it) | **literal modulo import line** |
| `fileMenu.position.method = PositionMethod::Absolute; fileMenu.position.x = main.query('#file-menu').position.x;` | `Position` reference-class per component with live get/set views (D03); `query` (D01) | **literal** |
| `fileMenu.actions.add('open', () => { filename = await openDialog.show(); … })` | `ActionRegistry` + responder-chain dispatch, slugged labels (`"Save As"` → `save-as`) (D04); handlers are ordinary lambdas, `await` inside is landed task machinery | **literal** |
| `main.query(text).value` | sketch typo for `query('#text')`; `DomNode.value()` uniform accessor (D01) | **literal (typo corrected)** |
| `app.start()` | alias for `run()` on `SonarApp` | **literal** |
| `hotkey="^+s"` | `^+s` is not landed chord grammar — the DOM `hotkey` attribute accepts a friendlier grammar and normalizes to `parseChord` form (`^+s` → `C-S-s`), D04 §3 | **literal via translation** |

The two deltas the flagship example (D08) annotates: (1) `dom!(\`…\`)` / `import()` in place of a
bare backtick constructor argument; (2) `uses Sonar::Dialogs`. Everything else runs as written
(with the sketch's typos fixed: the unclosed `</textarea>`, `fineMenu`, the empty `if ()`, the
undefined `path`, and the first menuitem's label `File`→`Open`).

## 3. Architecture

```
                    ┌──────────────── comptime ───────────────┐
   dom!(`markup`) ──► expandDom(string)->string ─► meta::parseExpr ─► typed construction code
                    └──────────────────────────────────────────┘
   FlexContainer(import("x.sonar")) ──► comptime string ──┐
   FlexContainer(runtimeString) ──────────────────────────┴─► DomParser ─► DomBuilder + registry
                                                                    │
                 SonarApp ── Document ──┬── id index / node meta ◄──┘  (tag, attrs, classes, actions)
                                        ├── selector engine ── query()/queryAll() ─► DomNode wrappers
                                        ├── binding table ── sweep (pull+diff) ─► component setters
                                        └── serializer ── outerMarkup()
   Position (per component) ─► DOM containers partition flow/absolute at arrange
   ActionRegistry (per node, side-table) ─► responder-chain fire; hotkeys via T07 pending seam
```

**Three markup tiers, one grammar** (D02): the comptime `dom!` macro (full expression holes,
zero runtime parse cost, T06's cost-identical guarantee), the comptime-`import()`-fed runtime
builder (external `.sonar` assets), and the fully-runtime string parse (dynamic markup). The runtime
registry is the single source of tag/attribute truth; the `dom!` emitter mirrors it and a drift test
(D08) asserts agreement — the T06 M5 discipline.

**The retained tree stays the substrate.** DOM nodes ARE Sonar components; `DomNode` is a thin
wrapper (jQuery-style), never a parallel tree. Damage, layout, focus, overlays, theming — all landed
machinery, consumed through the seams the API inventory pinned (`__sonarAttach`/`__sonarDetach`,
`ILifecycleHost`, `pushOverlay(c, dismissOnOutsidePress, inputTransparent, group)`,
`Styleable.resolve`, the handler token model).

## 4. Track map & file ownership (disjoint; the parallel-tracks convention)

| track | doc | owns |
|---|---|---|
| D01 Document, selectors, DomNode, SonarApp | techdesign-01 | `sonar/src/dom/{document,node,selector,app}.lev` |
| D02 Markup engine (runtime + `dom!`) | techdesign-02 | `sonar/src/dom/{markup,registry,builder}.lev`, `sonar/src/dom/templates/{dom_expander,dom_macro}.lev` |
| D03 Position, classes, DOM containers, paint fix | techdesign-03 | `sonar/src/dom/{position,containers,classes}.lev` + flagged additive edits to `sonar/src/component.lev`, `sonar/src/mixins.lev` |
| D04 Actions & hotkeys | techdesign-04 | `sonar/src/dom/actions.lev` |
| D05 Bindings (`{{…}}`) | techdesign-05 | `sonar/src/dom/binding.lev` |
| D06 TextArea | techdesign-06 | `sonar/src/components/textarea.lev` |
| D07 Dialogs | techdesign-07 | `sonar/src/dom/dialogs.lev` |
| D08 DevTools, testing, examples, delivery | techdesign-08 | `sonar/src/dom/devtools.lev`, `sonar/tests/dom*/**`, `sonar/examples/editor-dom/**`, README/CHANGELOG/trident.toml lines |

Cross-track contracts are frozen HERE (§5); a track needing to change one flags the anchor log
(§10) rather than silently diverging — the T01–T11 anchor-log discipline.

## 5. Frozen contracts

### 5.1 D-C1 — the tag registry (normative table)

Lowercase tag → component. Runtime tier: unknown tags are a loud `E-D3` error unless registered via
`Sonar::Dom::registerTag(name, factory, matcher, applier)`; `dom!` tier: Capitalized tags pass
through as direct class construction (the T06 user-component rule), lowercase unknown tags error at
expansion.

| tag | component | | tag | component |
|---|---|---|---|---|
| `flex` | `FlexContainer` (new, D03) | | `list` | `ListView` |
| `contentbar` / `bar` | `Bar` (new horizontal container, D03) | | `table` | `TableView` |
| `statusbar` | `ContentBar` (the T04 3-segment leaf) | | `tree` | `TreeView` |
| `span` / `text` | `Text` | | `tabs` | `Tabs` |
| `textarea` | `TextArea` (new, D06) | | `split` | `SplitBox` |
| `input` | `Input` | | `grid` | `GridBox` |
| `button` | `Button` | | `box` | `ContentBox` |
| `checkbox` | `CheckBox` | | `menubar` | `BarMenu` |
| `radiogroup` | `RadioGroup` | | `menu` | `Menu` |
| `progress` | `ProgressBar` | | `menuitem` | `MenuItem` |
| `spinner` | `Spinner` | | `separator` | `MenuSeparator` |
| | | | `modal` | `Modal` |

**Naming ruling (anchor-logged):** the target sketch nests children inside `<contentbar>`, so that
tag maps to the new `Bar` container; the landed 3-segment `ContentBar` leaf gets `<statusbar>`.

**Common attributes** (every tag): `id`, `class`, `hotkey`, `action`, `width`/`height`/`flex`,
`minWidth`/`maxWidth`/`minHeight`/`maxHeight`, `dock`, `row`/`col`/`rowSpan`/`colSpan`, `padding`,
`hidden`, `on:<event>` (`dom!` tier only — a handler is an expression). Per-tag attributes mirror
the landed T04/T05 tables (D02 §4 carries the full aggregated registry).

### 5.2 D-C2 — DomNode (the uniform wrapper)

`query`/`queryAll` return `DomNode`/`DomNodeList`, never raw `IComponent` (the raw component stays
reachable via `.raw()`). Uniform surface: `id()/setId`, `value()/value(string)`, `text()/text(string)`,
`checked()/checked(bool)`, `enabled()/enabled(bool)`, `show()/hide()/visible()`, `position()`,
`actions()`, `attr(name)/attr(name, value)` (re-applies through the registry), class-list ops
(`addClass/removeClass/toggleClass/hasClass`), tree ops (`parent()/children()/query()/queryAll()`),
`on(event, handler)`, `outerMarkup()`. **`query` misses THROW** (`SonarDomException`) — the §12.6
loudness rule, exactly like `Map.at` on a missing key; `queryOrNone()` is the probing form.

### 5.3 D-C3 — selector grammar (frozen EBNF)

```
selector-list := selector (',' selector)*
selector      := compound ((' ' | '>') compound)*
compound      := [tag] ('#' name | '.' name | '[' name ('=' value)? ']')+ | tag
```
v1: tag, `#id`, `.class`, `[attr]`, `[attr=value]`, descendant (space), child (`>`), grouping (`,`).
Matching is rightmost-first with ancestor verification; a bare `#id` selector takes the Document
index fast path. Deliberately absent v1 (documented, additive later): pseudo-classes, sibling
combinators, attribute operators (`^=` etc.).

### 5.4 D-C4 — Document

One per `SonarApp`: the id index (validated cache over the tree — the tree is truth, the index is a
cache; stale hits re-walk), per-node meta side-table (tag, applied attrs, action registry) keyed by
an additive `Component.domSlot_` int, the binding table (D05), the exposure table (`expose(key,
getter)` for the runtime tier), and the serializer. `Sonar::Dom::document()` reaches the current
one; the global is written only through a namespace free-function setter (the `__setCurrentApp`
lowering rule).

### 5.5 D-C5 — Position

`enum PositionMethod { Flow, Absolute }` (v1). `Position` is a **reference class** hanging off every
component (lazy additive field), with get/set **views** so writes invalidate: reads of `x()/y()`
always return the live arranged `box` origin (that is what makes
`main.query('#file-menu').position.x` work); writes store the desired origin, consumed only when
`method == Absolute`. `w`/`h` overrides map onto `Constraint::Fixed`. `z` orders absolute siblings.
`anchorTo(target, edge, dx, dy)` re-resolves each arrange (the resize-proof enhancement over the
sketch's manual `y+1` math). **Absolute placement is honored by DOM containers**
(`FlexContainer`/`Bar`/`SonarApp` root) — landed containers ignore it (documented v1 boundary);
coordinates are screen-space; the parent's clip still applies, so floating UI belongs on the app
root or the overlay stack (D03 §5 spells the popover route).

### 5.6 D-C6 — classes

`Array<string>` class list per component (additive field + `addClass/removeClass/toggleClass/hasClass`,
each invalidating). `hidden` is a **semantic class**: presence ⇔ `visible() == false`, both
directions. Classes feed selectors (`.class`) and theming: `Styleable.resolve` gains class-qualified
steps (most-specific-first, additive to the landed 6-step fold — D03 §4 freezes the new chain).

### 5.7 D-C7 — actions

`ActionRegistry` (per-node, side-table-backed, created on demand): `add(name, () => void)`,
`remove(name)`, `has(name)`, `setEnabled(name, bool)`, `fire(name) -> bool`. Dispatch walks the
responder chain (node → ancestors → app). A `MenuItem`/`Button` built from markup fires
`action` attr if present, else `slug(label)` (`"Save As"` → `save-as`). Disabling an action greys
every bound item. `SonarApp` pre-registers `quit`.

### 5.8 D-C8 — bindings

`{{expr}}` in text position registers a **pull binding**: `() => Sonar::Dom::text(expr)` (dom!
tier: any expression, bare globals included; runtime tier: identifier keys resolved against
`expose()`/`doc.data`). The Document sweep re-evaluates getters, string-diffs, and applies through
ordinary setters (damage falls out). Sweep points: frame start, plus a sweep-frame scheduled by
DOM-owned async resolutions (dialogs). Worst-case staleness is one frame; the common paths are
same-frame (probed, D-P6). Push-based per-field reactivity remains T11's job; the two bridge (D05 §6).

### 5.9 D-C9 — errors

`class SonarDomException : Exception, ISonarException` (catchable as the Sonar family). Every parser
error carries `line:col` + a caret into the markup (the T08 TOML taxonomy discipline); the D02 error
catalog (E-D1…E-D12) is normative. Query misses throw with the selector text.

### 5.10 D-C10 — determinism & testing doctrine

Everything headless-testable via the landed harness: `SonarTest.harness()` + `pumpOnce` +
`TestRenderer` snapshots; new selector-driven helpers (`q`/`click`/`type`) land in D08. Every track
ships differential oracle/IR/LLVM goldens with ONE shared `.expected`; emit-C++ compile-only lane.

## 6. Probe register (run before implementation; a red probe invokes its named fallback)

| # | probe | gates | fallback if red |
|---|---|---|---|
| D-P1 | `class X : Container, Styleable` on current master resolves `__sonarChildren`/`arrange`/`paint` to Container's (the #65-family source fix holds for a NEW class) | D03 containers | redeclare the override set forwarding to `children()`/`layout()` — the shipped menu.lev pattern |
| D-P2 | set-accessor views on a plain class field (`set x(int v)` pinging an owner via a `weak IComponent?` back-ref) — write-through + invalidate on all four engines | D03 Position | Position methods (`setX(int)`) instead of views; sketch fidelity drops to `position.setX(…)`, anchor-logged |
| D-P3 | overloads on union-typed params: `text(string\|None)` vs `text(int)` vs `text(string)` resolve per call site | D05 hole rendering | holes must be string-typed; a union hole is E-D9 with the `?? ""` guidance |
| D-P4 | `Map<string, () => void>` / `Array<() => void>` field churn (add/remove/invoke) on LLVM native — closures as collection values | D02 registry, D04 actions | parallel arrays only (already the default shape); if arrays-of-closures also red, STOP and escalate |
| D-P5 | `Promise<string \| None>` — construct pending, resolve with a string AND with None, await from an event-handler task | D07 dialogs | `Promise<FileResult>` (a tiny class with `string path; bool cancelled`), DomNode-style |
| D-P6 | write-after-await visibility: button handler resolves a promise; parked continuation writes a global; a sweep-frame armed at resolve observes the write (all engines) | D05 sweep | rely on frame-start sweep + follow-up frame (one-frame staleness, documented) |
| D-P7 | `import("x.sonar")` comptime string passed to a runtime constructor parameter | D02 asset tier | bind to a `comptime string` global first (the T08 shape) |
| D-P8 | narrowing an `IComponent` to each mapped component class inside the registry matcher closures (`(c) => c is Button`) incl. multi-mixin leaves | D01 tag selectors | matchers only for DOM-owned classes; tag selectors on landed-class nodes gated on node meta |
| D-P9 | `Rect`/`Point` reads through a `weak` back-ref field after owner detach (owner freed → `None`) — no crash, clean None path | D03 Position | strong back-ref + explicit `__sonarDetach` clearing (documented cycle-break) |
| D-P10 | 5k-node build/query/teardown churn: id-index + meta side-table growth stays flat (no #73-shaped global-array growth; columns pre-sized/compacted) | D01 | compaction threshold tuning; global carriers moved onto the Document instance (locals-then-assign-once) |

## 7. Known-bug & footgun compliance (binding on every track)

- **#74 [P0.3, OPEN]** — no `Array<Struct>.add` loops, anywhere. Every DOM collection is either an
  array of **reference classes** (DomNode, Binding, NodeMeta rows) or **parallel primitive columns**
  (the theme.lev precedent). Structs never enter growing arrays.
- **#73 [P2, OPEN]** — no namespace-global growing arrays; every growing carrier lives on an
  instance (Document/registry objects), and bulk builds fill a local then assign once.
- **#72** — dialogs surface network nothing; not applicable.
- **Fixed-but-disciplined**: explicit `this.` receivers in every stored lambda (#53 discipline —
  fixed at source, kept as house style); no char literals in call-argument position (#50 shape —
  `.code()` comparisons in the parser, the expander precedent); `arr[i]()` binds to a local before
  the call (#52 shape); `Chord`-style rule — any value shape carrying an enum field that lives in an
  array is declared `class` until the #41-family soak coverage says otherwise.
- **By-design rows honored**: ref-class collectors for accumulating params; no narrowing reliance
  across early returns; namespace-global writes only via bare-assignment free functions
  (`__setCurrentDocument` mirrors `__setCurrentApp`); structs snapshot `this` in closures — anything
  a closure observes is a class.
- **Sonar bug #4** (stale glyphs on shrink) is **owned and fixed by D03** — bindings shrink
  strings constantly, so the DOM suite cannot ship on the workaround. The fix (unconditional leaf
  background clear, opt-out flag) runs the full differential suite + golden regen (D03 §6).
- **P0.3 stop-the-line scope check**: #74's construct (`Array<Struct>.add` loops) appears nowhere in
  this suite's designs — the per-construct gate does not block implementation start.

## 8. Gates & timeline

| gate | date | exit criteria |
|---|---|---|
| G-D1 | 2026-07-16 | probes D-P1…P10 recorded in `sonar/PROBES.md`; D01+D02 runtime core: markup builds a tree, `query`/`queryAll`/serializer green, `dom!` expands the same program the runtime tier builds |
| G-D2 | 2026-07-17 | D03+D04: absolute positioning + classes + class theming + #4 paint fix (full differential re-run); actions/hotkeys drive menus |
| G-D3 | 2026-07-18 | D05+D06: `{{…}}` bindings sweep on all engines; TextArea editing corpus green |
| G-D4 | 2026-07-19 | D07: FileDialog open/save over a scripted fs sandbox; `await show()` resolves; **the corrected target-feel program runs end-to-end** |
| G-Dv1 | 2026-07-20 | D08: inspector overlay, selector test helpers, editor-dom example in the differential matrix, README/CHANGELOG, 0.2.0 |

**Escalation protocol (the standing convention):** an implementing agent that hits a compiler defect,
a red probe with no listed fallback, or a contract contradiction **STOPs on that item and escalates**
— file the repro, log the anchor, move to the next milestone; never improvise a cross-track contract
change inline.

## 9. Language requests (filed with this suite)

- `designs/requests/request-stat-isdir.md` (**new**, filed by D07): a directory-type predicate
  (`sysStat` field 3 or `sysIsDir`) — today the only test is `sysListDir(child) != None`, an O(N)
  listdir-per-entry probe inside FileDialog listings. Interim fallback spec'd and shipped.
- `designs/requests/request-string-literal-tail.md` (**existing**, stake added by this suite): general
  raw-string literals would make `FlexContainer(\`…\`)` legal verbatim. Not a gate — `dom!` and
  `import()` cover it.
- Explicitly NOT requested: global `@Reactive`/set-views (the pull sweep makes it unnecessary —
  D05 §2 records the analysis), runtime reflection (the registry's matcher closures cover tag
  narrowing), eval (runtime expression holes stay out of scope by design).

## 10. Anchor log

- 2026-07-15 — suite authored. Rulings recorded: `contentbar`→`Bar` container / `statusbar`→landed
  `ContentBar` leaf (§5.1); DOM `hotkey` grammar normalizes `^+s`-style specs onto the frozen chord
  grammar (D04 §3); `query` misses throw (§5.2, the §12.6 loudness rule); `start()` is an alias, not
  a rename — `run()` remains canonical; the additive-edit register is: `component.lev` (`classes_` +
  API, lazy `position_` + accessor, `domSlot_` int — all zero-cost when DOM is unused), `mixins.lev`
  (`resolve` class steps + the #4 `paintBackground` fix with opt-out). Any track needing more edits
  to landed files flags here first.
