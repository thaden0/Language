# Sonar — Design Document

**An enterprise TUI framework for Leviathan.**
Retained-mode component tree · flex/grid layout · compile-time templates · zero runtime reflection.

Status: design, pre-implementation.
Depends on: reference.md as of 2026-07-11 (rules/macros/comptime Layer A–D, `Block`, `char`, enums, MI + `distinct`, `bind`/`inject`, async event loop).

---

## 1. Design position

Sonar is **retained-mode**: the component tree is a persistent object graph, mutated in place, repainted by damage. This is the deliberate opposite of the Elm/BubbleTea rebuild-and-diff model, chosen because Leviathan's ownership + refcounting memory model rewards stable object identity and punishes per-frame graph reconstruction. One tree, built once, mutated forever.

Three consequences drive everything below:

1. **Damage tracking is load-bearing.** Since nothing is rebuilt, redraw efficiency must come from components declaring what changed. Every mutation path funnels through `Invalidate()`.
2. **Templates are compile-time sugar, not a runtime.** The `sonar!` macro expands tag markup into the exact retained-mode construction calls a user would hand-write. `leviathan --expand` shows the truth; there is no template engine in the binary.
3. **Capabilities are inherited, not flagged.** Multiple inheritance lets behavior (focus, scroll, styling) live in real base classes with real state, mixed into components à la carte, with `distinct` resolving the collisions MI actually produces.

**Backend note.** The run loop rides the language event loop (`await`, timers, stdin), which the reference marks interpreter-bound for emit-C++. Sonar v1 therefore targets the oracle/IR and LLVM lanes; the emit-C++ lane compiles everything except `App.run()`.

---

## 2. Architecture overview

```
App
 └─ run loop (async)
     ├─ input:   stdin bytes → KeyEvent/MouseEvent → focus routing → bubble
     ├─ layout:  constraint pass (top-down) over dirty subtrees
     ├─ sweep:   damage collection → dirty Rect list
     └─ paint:   components draw into Surface (Block-backed cell buffer)
                 → diff vs. previous frame → minimal ANSI escape writes
```

The four phases are the frame. A frame runs only when something is dirty; an idle app blocks on input and burns nothing.

### 2.1 The Surface (why `Block`)

Leviathan arrays are pure — `a[i] = v` rebinds a fresh array. That is exactly wrong for a frame buffer written thousands of times per frame. `Block` is the language's gated mutable byte buffer, and a terminal frame buffer is precisely the "marked room" it exists for.

A `Surface` is one `Block`, fixed-width cell encoding, **8 bytes per cell**:

| bytes | content |
|---|---|
| 0–3 | Unicode scalar (little-endian, `char.code()`) |
| 4 | foreground `Color` carrier |
| 5 | background `Color` carrier |
| 6 | `CellAttr` bit set (bold/dim/italic/underline/reverse/blink) |
| 7 | flags (bit 0: continuation cell for wide glyphs) |

Wide glyphs (CJK, emoji) occupy two cells: the scalar in the first, bit 7 set in the second. All width math is cell math, resolved once at write time via `Sonar::glyphWidth(char) -> int`.

```
class Surface {
    private Block cells;
    private Block prev;          // last-presented frame, for diffing
    readonly int width;
    readonly int height;

    new Surface(int w, int h) { width = w; height = h;
        cells = Block(w * h * 8); prev = Block(w * h * 8); }

    void put(int x, int y, char c, Style s) { ... }        // one cell write
    void writeText(int x, int y, string text, Style s) { ... }
    void fill(Rect r, char c, Style s) { ... }
    Block present();             // diff cells vs prev, emit escape bytes, swap
}
```

`present()` produces the escape-sequence bytes for exactly the cells that differ — damage tracking at the pixel (cell) grain, independent of the component-level damage that decided *who* repainted.

### 2.2 The sweep (damage model)

Every component carries a dirty flag and a resolved-box `Rect`. `Invalidate()` sets the flag and walks parent links to mark the path root-ward as *containing* damage (not *being* damaged). The sweep descends only into containers on that path, collects dirty rects, and the paint phase repaints only components intersecting them. Setters call `Invalidate()`; nothing else needs to.

Two grains of damage, deliberately:

- **Component grain** — who repaints (dirty flags, sweep).
- **Cell grain** — what reaches the terminal (`Surface.present()` diff).

A component that repaints identically costs a buffer write but zero terminal I/O.

---

## 3. Type inventory (new types)

### Interfaces

| type | role |
|---|---|
| `IComponent` | the universal contract: box, lifecycle, damage |
| `IContainer` | child management + layout ownership |
| `ILayoutStrategy` | pluggable layout algorithms |
| `IRenderer` | terminal backend contract (real TTY, test harness) |
| `IFocusPolicy` | tab-order / focus-ring strategy |
| `ITheme` | named style lookup |
| `ISingleLine` / `IMultiLine` | capability markers (no members) |
| `IValidatable` | `bool validate()` for form components |

### Behavior base classes (the MI mixins — §5)

| type | state it carries |
|---|---|
| `Component` | box, parent link, dirty flag, style — the `IComponent` implementation core |
| `Container` | child array, layout strategy — the `IContainer` core |
| `Focusable` | `focused`, focus events, tab-stop membership |
| `Scrollable` | `scrollX`/`scrollY`, viewport clamping, scroll events |
| `Styleable` | per-instance style overrides above the theme |
| `Bordered` | border style + the one-cell inset it implies |

### Value structs

| type | shape |
|---|---|
| `Point` | `int x; int y` |
| `Size` | `int w; int h` |
| `Rect` | `Point origin; Size size` + `contains`/`intersect`/`union` |
| `Style` | `Color fg; Color bg; int attrs` |
| `Constraint` | one axis: `int min; int max; int flex; int concrete` (`concrete = -1` → flexible) |
| `KeyEvent` | `char c; KeyCode code; int mods; bool handled` |
| `MouseEvent` | `Point at; MouseButton button; bool handled` |

Structs are the right kind here: copied, identity-free, dense — a `Rect` passed into layout math never aliases the tree.

### Enums

```
enum Color : int { Default = -1, Black = 0, Red, Green, Yellow, Blue,
                   Magenta, Cyan, White, BrightBlack, /* … */ BrightWhite = 15 }
enum BorderStyle { None, Single, Double, Rounded, Heavy, Dashed }
enum KeyCode : int { Char = 0, Enter, Escape, Tab, Backspace, Delete,
                     Up, Down, Left, Right, Home, End, PageUp, PageDown, Fn }
enum MouseButton { Left, Right, Middle, WheelUp, WheelDown }
enum Overflow { Clip, Scroll, Wrap }
enum Align { Start, Center, End, Stretch }
```

Enums buy exhaustive `match` in the input decoder — omitting a `KeyCode` arm is a compile error, which is exactly the guarantee an escape-sequence parser wants.

### Classes

| type | role |
|---|---|
| `App` | root container, run loop, renderer + theme injection site |
| `Surface` | the Block-backed cell buffer (§2.1) |
| `AnsiRenderer` / `TestRenderer` | `IRenderer` implementations |
| `FlexLayout`, `GridLayout`, `DockLayout`, `StackLayout` | `ILayoutStrategy` implementations |
| `Theme` | `ITheme` over a `Map<string, Style>` |
| `FocusRing` | default `IFocusPolicy`: document-order traversal |
| `CellAttr` | attr bit constants (`const int Bold = 1;` …) |

---

## 4. Core contracts

```
interface IComponent {
    Rect box;                            // resolved by layout; interfaces require fields,
                                         // the implementing class allocates once
    Constraint widthConstraint();
    Constraint heightConstraint();
    void measure(Size avail);            // bottom-up preferred-size pass
    void arrange(Rect assigned);         // top-down box assignment
    void paint(Surface s);
    void invalidate();
    bool dirty();
    IComponent? parent();
}

interface IContainer {
    IContainer add(IComponent c);
    IContainer remove(IComponent c);
    IContainer clear();
    Array<IComponent> children();
    IContainer setLayout(ILayoutStrategy l);
}

interface ILayoutStrategy {
    void measure(Array<IComponent> kids, Size avail);
    void arrange(Array<IComponent> kids, Rect content);
}
```

Notes against the earlier sketch, now settled:

- **Layout is a strategy object owned by containers**, not methods smeared across `IComponent`. `SetWidth(int)` survives only as sugar that writes a concrete `Constraint`.
- **Fluent returns are concrete-typed.** Every builder-style setter on a class returns its own class (`TextBox setText(string) => …` returns `TextBox`), while the *interface* declares nothing fluent. Chains never decay to `IComponent` mid-expression, and the covariant-return question from the sketch is dissolved rather than answered: the interface carries queries and verbs, the class carries the chain.
- **Events are lists with bubble semantics.** `onKey(handler)` appends; dispatch walks focused-leaf → root, and any handler setting `evt.handled = true` (structs copy — handlers receive the event **by a mutable box**, `KeyEventBox`, a one-field class wrapping the struct) stops propagation. Capture phase (root → leaf) is available via `onKeyCapture`.

---

## 5. Multiple inheritance as the composition mechanism

This is where Sonar leans on the language hardest. Behaviors are **classes with state**, not interfaces with requirements, because Leviathan MI inherits implementation and `distinct` makes the collisions tractable.

```
class Component {                        // everyone's core
    Rect box = Rect(Point(0,0), Size(0,0));
    private bool dirty_ = true;
    private IComponent? parent_ = None;
    public void invalidate() { dirty_ = true; bubbleDamage(); }
    ...
}

class Focusable {
    public bool focused = false;
    private Array<() => void> focusHandlers = [];
    public void onFocus(() => void h) focusHandlers = focusHandlers.add(h);
    ...
}

class Scrollable {
    public int scrollX = 0;
    public int scrollY = 0;
    public void scrollTo(int x, int y) { ... }
    ...
}

class TextBox : Component, Focusable, Scrollable, IMultiLine {
    ...
}
```

A `TextBox` *is* focusable and *is* scrollable — the state and the methods arrive by inheritance, not by hand-delegation boilerplate per component (the C#/Java tax) and not by trait-object indirection (the Rust tax).

**Where `distinct` earns its keep.** Both `Focusable` and `Scrollable` reasonably want an internal `changed()` hook with the same signature. Same name + same type across bases is exactly the collision rule:

```
class Focusable  { distinct void changed() { /* focus ring repaint */ } ... }
class Scrollable { distinct void changed() { /* viewport re-clamp  */ } ... }

class TextBox : Component, Focusable, Scrollable, IMultiLine {
    void onCursorMoved() {
        this.Focusable::changed();       // qualified — both slots live, both reachable
        this.Scrollable::changed();
    }
}
```

No diamond ambiguity, no silent override, both behaviors keep their slot. Components that mix in only one of them never see qualification at all.

**Same-name different-type coexistence** also gets used deliberately: `Bordered` declares `BorderStyle border` while `Styleable` may carry `string border` (a theme key). Different types → separate members by the language's own resolution rule, no `distinct` needed.

### Component capability matrix

| component | Component | Container | Focusable | Scrollable | Bordered | marker |
|---|---|---|---|---|---|---|
| `Text` | ● | | | | | `ISingleLine` |
| `Input` | ● | | ● | | | `ISingleLine`, `IValidatable` |
| `TextBox` | ● | | ● | ● | | `IMultiLine` |
| `Button` | ● | | ● | | | `ISingleLine` |
| `CheckBox` / `RadioGroup` | ● | | ● | | | `IValidatable` |
| `ContentBar` | ● | | | | | `ISingleLine` |
| `ContentBox` | ● | ● | | ● | ● | `IMultiLine` |
| `SplitBox` | ● | ● | | | ● | |
| `GridBox` | ● | ● | | | ● | |
| `Tabs` | ● | ● | ● | | | |
| `ListView` | ● | | ● | ● | ● | |
| `TableView` | ● | | ● | ● | ● | |
| `TreeView` | ● | | ● | ● | ● | |
| `BarMenu` | ● | ● | ● | | | `ISingleLine` |
| `MenuItem` | ● | | ● | | | `ISingleLine` |
| `Modal` | ● | ● | ● | | ● | |
| `ProgressBar` / `Spinner` | ● | | | | | `ISingleLine` |

---

## 6. Layout

Two axes, kept separate end to end:

- **Constraint** (per component, per axis): `concrete` cells, or `min`/`max` bounds with a `flex` weight.
- **Strategy** (per container): how children's constraints resolve into boxes.

```
enum Dock { Top, Bottom, Left, Right, Fill }

box.setLayout(FlexLayout(Axis::Vertical))
   .add(bar.dock(Dock::Top))
   .add(editor.flex(1))
   .add(status.dock(Dock::Bottom));
```

Strategies shipped in v1:

| strategy | model |
|---|---|
| `FlexLayout` | one axis, flex-grow weights, min/max clamping — the workhorse |
| `GridLayout` | fixed rows × cols, spans, per-track constraints |
| `DockLayout` | edge-pinned children, last child fills |
| `StackLayout` | all children get the full content rect (modals, overlays) |

The layout pass is two-phase (`measure` up, `arrange` down) and runs only over subtrees containing damage whose kind is *geometric* (constraint or child-set changes); pure content damage (text changed, same box) skips straight to paint. `Constraint` being a value struct matters here: the measure pass traffics entirely in copies, never aliasing the tree it is measuring.

Cell-grid honesty: all layout math is integer cell math. Flex remainders distribute left-to-right (deterministic, testable); there is no fractional layout to round.

---

## 7. Events, focus, input

**Decode.** Raw stdin bytes → `KeyEvent`/`MouseEvent` in the input phase. The escape-sequence state machine is a `match` over `KeyCode`-classified prefixes; enum exhaustiveness keeps the decoder total.

**Route.** Key events go to the focused leaf; the event then **bubbles** leaf → root through parent links. Containers may intercept in **capture** (root → leaf) for app-global chords. Mouse events hit-test the tree by `Rect.contains` and follow the same two-phase dispatch.

**Focus.** `App` owns an `IFocusPolicy` (default `FocusRing`: document order, `Tab`/`Shift-Tab`, wraparound). Any `Focusable` is a tab stop unless `tabStop(false)`. Focus changes fire `onBlur`/`onFocus` and invalidate both endpoints.

**Handlers are method references.** `menuItem.onSelect(this.save)` — first-class, type-checked, no string lookup. Per the reference, method references have no identity equality, so removal is by token: `var t = box.onKey(h); box.offKey(t);` (`onKey` returns an `int` token; `Array` semantics make the internal handler list a rebind, which is fine at handler-registration frequency).

---

## 8. The template layer (`sonar!`)

The entire metaprogramming stack (Layers A–D) gets used, each for what it is:

### 8.1 Expression macro — the entry point

`sonar!(\`<App>…</App>\`)` is a Layer D-lite expression macro over a quasiquote payload. The tag grammar is template payload, not Leviathan syntax — the macro parses it **at compile time** and splices ordinary construction code. Guarantees inherited from the rules layer:

- **Cost-identical to hand-written code.** Zero runtime reflection, zero template parser in the binary.
- **`--expand` is the escape hatch.** The expansion is compilable source; a user who distrusts the magic reads exactly the `Add`/`set*` chain they'd have written.
- **Hygienic.** Template-internal locals alpha-rename; user identifiers in `{…}` holes splice as expressions in the caller's scope.

### 8.2 Template grammar

```
<Tag attr="literal" attr={expression} on:event={handlerRef} id="fieldName">
    children…
    ${interpolation}                     // text nodes: ordinary string interpolation
    $for item in expr { <Tag…/> }        // list splice → repeated construction
    $if expr { <Tag…/> } $else { … }     // comptime-if when expr is comptime, runtime add() otherwise
</Tag>
```

| attribute form | expands to |
|---|---|
| `attr="lit"` | `node.setAttr(lit)` — literal, type-checked at splice |
| `attr={expr}` | `node.setAttr(expr)` — expression hole, evaluated at construction |
| `on:event={ref}` | `node.onEvent(ref)` — method-reference splice |
| `id="name"` | binds the node to the enclosing class's field `name` (§8.3) |
| `flex`/`dock`/`width`/`height`/… | layout constraint sugar → `Constraint` writes |
| `theme="key"` | `node.useStyle(inject ITheme, "key")` |

### 8.3 `id` binding — the unification move

The macro matches `id="buffer"` against a same-named field on the enclosing class (the `in class C` encloser shape) and injects `this.buffer = <node>;` into the expansion. Fields bound this way satisfy definite assignment because the splice lands inside the constructor that invoked the macro. A missing or type-mismatched field is a **compile error at the template**, not a runtime null.

### 8.4 External templates — comptime `import()`

Views can live beside code, declared as build inputs:

```
comptime string tpl = import("views/editor.sonar");
var app = sonar!(tpl);                   // macro receives a comptime string — same pipeline
```

`assets = ["views/**"]` in `trident.toml` makes the template a hashed, deterministic build input. Designers edit `.sonar` files; the compiler still expands everything statically.

### 8.5 Rules — declarative cross-cutting registration

Layer B rules give Sonar attribute-driven wiring with no registry code:

```
namespace Sonar {
    attribute Shortcut { string chord; }
    rule bindShortcuts {
        match @Shortcut(s) on method m in class C : Component
        inject `App::Current().keymap().bind($s.chord, this.$m)` at bottom of C.constructor
    }
}
```

```
class Editor : ContentBox {
    @Sonar::Shortcut("^S") void save() { ... }     // that's the whole feature
    @Sonar::Shortcut("^Q") void quit() { ... }
}
```

Additive, hygienic, visible in `--rules`/`--expand`. The same shape powers `@Sonar::Validator` on form fields and `@Sonar::Timer("250ms")` on tick methods.

### 8.6 Compile-time reactivity (v1.5, designed now)

`text={this.count}` where the hole is a **field** (not an arbitrary expression) expands to a subscription: the macro emits a setter-shadowing `set count(int v)` view on the enclosing class (via a `member of` injection) that writes the slot and calls `boundNode.setText(v.toString())`. Reactivity resolved at expansion time — the update graph is static code, not a runtime observer table. Arbitrary-expression holes stay one-shot; only field holes are reactive, and the rule is loud in `--expand`. This is the framework's differentiator and is speced now so v1's expansion shapes don't foreclose it.

---

## 9. Dependency injection — theming and backends

`bind`/`inject` is the configuration system. No config objects threaded through constructors:

```
// composition root (main file)
bind ITheme => Theme::FromToml(import("themes/abyss.toml"));   // comptime-read, runtime-built
bind IRenderer => AnsiRenderer();
bind IFocusPolicy => FocusRing();

App app = App();
```

- Components resolve their theme with `inject ITheme` at construction; per-instance `Styleable` overrides layer above it.
- **Tests bind `TestRenderer`**, which renders into a `Surface` and exposes `string snapshot()` — golden-file TUI tests with zero terminal, differential-testable across engines like everything else in the ecosystem.
- Scoped rebinding (a `Modal` subtree with a different theme) uses lexical `bind` in the subtree's construction scope, which the binding rules already give for free.

---

## 10. App, run loop, configuration

```
class App : Container, Bordered {
    new App() { ... }
    App title(string t);
    App altScreen(bool on = true);       // alternate screen buffer (default on)
    App mouse(bool on = true);           // SGR mouse reporting (default off)
    App fpsCap(int fps = 60);            // damage-coalescing cap
    App onResize((Size) => void h);
    void run();                          // async: input ∥ timers ∥ frame
    void quit();
    Size screen();                       // current terminal size
    Keymap keymap();
}
```

`run()` is an async loop over the language event loop: `await` on stdin, timers for `Spinner`/cursor blink/`@Sonar::Timer`, SIGWINCH → re-arrange root → full damage. Terminal state (raw mode, alt screen, mouse) is acquired in `run()` and released via `IDisposable` `using` discipline, including on uncaught exceptions — a crashed TUI that wrecks the shell is a framework bug, so restore lives in the `using` teardown, not in happy-path code.

### Configuration surface

| knob | where | default |
|---|---|---|
| theme | `bind ITheme` | built-in `Theme::Default()` |
| renderer | `bind IRenderer` | `AnsiRenderer` |
| focus policy | `bind IFocusPolicy` | `FocusRing` |
| alt screen / mouse / fps | `App` fluent setters | on / off / 60 |
| unicode width mode | `App.wideGlyphs(WidthMode)` | `WidthMode::East Asian off` |
| keymap | `app.keymap().bind(chord, ref)` or `@Shortcut` | platform defaults |
| comptime template roots | `assets = [...]` in `trident.toml` | none |

---

## 11. Component & attribute reference

Common attributes (every component, via `Component`/template sugar): `id`, `width`, `height`, `minWidth`, `maxWidth`, `minHeight`, `maxHeight`, `flex`, `dock`, `theme`, `hidden`, `on:key`.

| component | own attributes | own events |
|---|---|---|
| `Text` | `text`, `align`, `wrap` (`Overflow`) | — |
| `Input` | `value`, `placeholder`, `mask`, `maxLength`, `validator` | `on:change`, `on:submit` |
| `TextBox` | `text`, `wrap`, `readOnly`, `gutter` (line numbers), `tabWidth` | `on:change`, `on:cursor` |
| `Button` | `label`, `key` (accelerator) | `on:press` |
| `CheckBox` | `label`, `checked` | `on:toggle` |
| `RadioGroup` | `options` (`Array<string>`), `selected` | `on:select` |
| `ContentBar` | `text`, `align` | — |
| `ContentBox` | `borders` (`BorderStyle`), `title`, `padding`, `overflow` | — |
| `SplitBox` | `axis`, `ratio`, `resizable` | `on:resize` |
| `GridBox` | `rows`, `cols`, `gap`; children take `row`, `col`, `rowSpan`, `colSpan` | — |
| `Tabs` | `selected`; children take `tabLabel` | `on:select` |
| `ListView` | `items`, `selected`, `multi` | `on:select`, `on:activate` |
| `TableView` | `columns`, `rows`, `sortable`, `selected` | `on:select`, `on:sort` |
| `TreeView` | `roots`, `expanded` | `on:select`, `on:expand` |
| `BarMenu` | — (children are `MenuItem`) | — |
| `MenuItem` | `label`, `key`, `enabled` | `on:select` |
| `Modal` | `title`, `borders`, `dismissKey` | `on:dismiss` |
| `ProgressBar` | `value`, `max`, `showPercent` | — |
| `Spinner` | `frames`, `interval` | — |

---

## 12. Feature list (summary)

- Retained-mode tree; two-grain damage (component dirty flags + cell-level frame diff)
- `Block`-backed `Surface` with wide-glyph-correct cell math
- Flex / Grid / Dock / Stack layout strategies; constraint structs; integer-deterministic
- Capability mixins via MI (`Focusable`, `Scrollable`, `Bordered`, `Styleable`) with `distinct`-resolved collision points
- Two-phase event dispatch (capture/bubble), focus ring, mouse hit-testing, chord keymap
- `sonar!` compile-time templates: `id` field binding, method-reference handlers, `$for`/`$if`, external `.sonar` files via comptime `import()`
- Attribute rules: `@Shortcut`, `@Validator`, `@Timer` — additive, `--expand`-visible
- DI-based theming and renderer selection; `TestRenderer` snapshot testing, engine-differential
- Async run loop with clean terminal teardown via `IDisposable`
- v1.5 (speced): field-hole compile-time reactivity

## 13. Open questions

1. **Handler-list churn.** Pure-`Array` handler lists rebind per registration; fine at UI frequency, but a hot `on:cursor` path may want the StringBuilder-style uniqueness-mutation optimization once it generalizes to fields.
2. **Reference cycles.** Parent links (`parent_`) + child arrays form cycles under refcounting — the language's "one asterisk." Design intent: children hold the strong edge downward, `parent_` is a raw back-reference by convention; needs a stated ownership rule before implementation.
3. **`GridBox` track sizing** — v1 ships fixed + flex tracks; `auto` (content-sized) tracks need the measure pass to run per-track and is deferred until real usage justifies it.
4. **Windows console lane** — escape handling assumes VT; conhost legacy mode is out of scope for v1.