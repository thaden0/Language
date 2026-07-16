# Sonar — Tech Design 00: Overview, Contracts, and Gates

**Status:** design, pre-implementation. **Date:** 2026-07-12.
**Supersedes:** `overview-sonar.md` (kept as the original sketch; where the two disagree, THIS
document wins). Ground truth inputs: `infodemp.md` (verified file:line map), `chat-transcript.md`
(review + research findings).

Sonar is the **enterprise TUI framework for Leviathan**: a retained-mode component tree,
flex/grid/dock/stack layout, two-grain damage rendering over a `Block`-backed cell surface,
MI-mixin capability composition, DI-based theming, attribute-rule wiring, and a compile-time
template layer (`sonar!`). It ships as a **trident package** (checked user code — none of the
prelude's limitations apply), never touching `src/**` or `runtime/**`.

This doc is the **anchor**: the document map, the rulings register, the **frozen cross-track
contracts** (C1–C14), the language-feature surfaces (F1–F6), gates/timeline, conventions, the
verified-syntax cheat sheet, and the probe/risk register. Track docs conform to it exactly;
changing a frozen contract requires an escalation note here, not a silent divergence.

---

## 1. Document map, dependencies, difficulty

### 1.1 Language-side designs (in `designs/`, compiler/runtime scope)

| ID | doc | what | difficulty | risk | depends on |
|---|---|---|---|---|---|
| F1 | `designs/complete/techdesign-block-bulk-ops.md` | `Block.fill/blit/equals/mismatch` natives (**landed**) | S | LOW | — |
| F2 | `designs/techdesign-terminal-floor.md` | winsize + signals-as-streams (subsumes `designs/terminal-winsize.md`, `designs/signals.md`) | S/M | LOW | — |
| F3 | `designs/complete/techdesign-bound-method-references.md` | bound `obj.method` value refs (**landed**) | M | LOW/MED | — |
| F4 | `designs/techdesign-procedural-macros.md` | comptime code→code macros (the `sonar!` enabler) | L | MED | — |
| F5 | `designs/techdesign-weak-references.md` | `weak` fields (cycle-free retained tree) | L/XL | HIGH | — |
| F6 | `designs/techdesign-covariant-return.md` | covariant-return interface satisfaction | M | MED | — |

Recommended landing order: **F1 → F2 → F6 → F3 → F4 → F5** (cheapest first; F5 is the long pole
and nothing in waves 1–2 blocks on it thanks to R7's detach discipline).

### 1.2 Framework designs (in `designs/sonar/`, pure `.lev` package scope)

| ID | doc | owns (package files) | difficulty | gates on |
|---|---|---|---|---|
| T01 | `techdesign-01-core.md` | `sonar/src/{geometry,style,surface,component,container,damage,errors}.lev` | L | F1 (perf), F5 (leak-free; interim R7) |
| T02 | `techdesign-02-layout.md` | `sonar/src/layout/*.lev` | M | T01 |
| T03 | `techdesign-03-events-input.md` | `sonar/src/{events,input,focus,keymap}.lev` | M/L | T01 (no language gates) |
| T04 | `techdesign-04-components-basic.md` | `sonar/src/components/{text,contentbar,input,button,checkbox,radio,progress,spinner}.lev` | M | T01–T03; F3 (ergonomics only) |
| T05 | `techdesign-05-components-composite.md` | `sonar/src/components/{contentbox,splitbox,gridbox,tabs,listview,tableview,treeview,menu,modal,debugoverlay}.lev` | L | T01–T04 |
| T06 | `techdesign-06-templates.md` | `sonar/src/templates/*.lev` (+ `.sonar` grammar) | L | **F4 (hard)**, F3, T04/T05 attr tables |
| T07 | `techdesign-07-rules-attributes.md` | `sonar/src/attributes.lev` | S/M | T01, T09 timers; buildable TODAY |
| T08 | `techdesign-08-theming-di.md` | `sonar/src/{theme,toml}.lev` | M | T01 |
| T09 | `techdesign-09-runloop-terminal.md` | `sonar/src/{app,runloop,terminal,ansi_renderer,cursor,log}.lev` | L | T01, T03; F2 (resize), F1 (diff perf) |
| T10 | `techdesign-10-testing-delivery.md` | `sonar/tests/**`, `sonar/trident.toml`, `sonar/examples/**`, `test_renderer.lev` | M | all |
| T11 | `techdesign-11-reactivity.md` | v1.5 — field-hole compile-time reactivity | M/L | F4, T06; F5 (soft) |

Disjoint file ownership is normative (house convention): a track writes only its own files.

---

## 2. Design position (unchanged from the sketch, confirmed by review)

1. **Retained mode.** One persistent tree, mutated in place, repainted by damage. Leviathan's
   ARC + pure arrays punish per-frame rebuilds and reward stable identity.
2. **Two-grain damage.** Component grain decides *who repaints* (dirty flags + sweep); cell
   grain decides *what reaches the terminal* (renderer diffs the cell buffer — F1 `mismatch`
   makes this a native scan).
3. **Templates are compile-time sugar.** `sonar!` expands to exactly the construction code you
   would hand-write; `--expand` shows it; no template engine in the binary.
4. **Capabilities are inherited classes**, not flags: MI mixins with `distinct`-resolved
   collision points.
5. **Engine lanes:** oracle + IR + **LLVM** are the run lanes; **emit-C++ compiles everything
   except `App.run()`** (no event loop in that lane); **ELF/X64Gen is frozen and is never a
   target, never a gate**.

---

## 3. Rulings register (R1–R16)

Decisions made now so track docs don't re-litigate. Each carries its rationale.

- **R1 — Events are classes, not structs.** `KeyEvent`/`MouseEvent`/`PasteEvent` are reference
  types: identity + a mutable `handled` flag is exactly what reference types are for. The
  sketch's `KeyEventBox` contortion is dead.
- **R2 — `BorderStyle::NoBorder`**, not `BorderStyle::None` (nominal collision with the
  language's `None` unit).
- **R3 — No `App::Current()`.** The language has no class-static plumbing. The running app is a
  namespace global reached by `Sonar::app()` (throws `SonarException` if no app is running).
  Inside `namespace Sonar`, write the global with a **bare** name (`currentApp = this;`), never
  `Sonar::currentApp = ...` (known qualified-write lowering hazard — cheat sheet §7).
- **R4 — Bit-sets are namespace consts, not enums and not class statics.** `Sonar::Attr::Bold`,
  `Sonar::Mod::Ctrl` — nested namespaces of `const int`. Enums are closed value types, wrong for
  OR-able flags; class statics don't exist.
- **R5 — Mixins derive from `Component`; diamonds collapse.** `Focusable`, `Scrollable`,
  `Styleable`, `Bordered`, `Container` are all `: Component`. A leaf like
  `TextBox : Focusable, Scrollable` gets ONE collapsed `Component` core (the language's diamond
  rule — the `IOStream : InStream, OutStream` precedent), so every mixin can call
  `invalidate()` legitimately. `distinct` is used only where two mixins deliberately keep
  separate slots (`changed()` on Focusable vs Scrollable). **Leaf base-list order is
  significant**: collapse keeps the *later* base's implementation, so the ordering convention is
  *core-most first, decorators last* (e.g. `class ContentBox : Container, Scrollable, Bordered`).
  Probes P7/P8 verify engine reality before implementation.
- **R6 — Fluent chains are leaf-typed.** Interfaces declare fluent verbs (`IContainer add(...)`)
  only under F6 covariance; **mixin bases expose void setters that invalidate**
  (`void setBorder(BorderStyle)`), and leaf components may add thin fluent wrappers returning
  their own type (`ContentBox border(BorderStyle b) { setBorder(b); return this; }`). Rationale:
  the language has no self-types; F6 fixes interface-vs-class decay but not base-vs-leaf decay.
  Templates bypass fluency entirely (attributes expand to setter statements).
- **R7 — Detach discipline until F5 lands (then belt-and-suspenders).** `remove()`, `clear()`,
  overlay dismissal, and `App` teardown MUST null child `parent_` links (`setParent(None)`) and
  fire `onDetach()`. Once F5 lands, `parent_` becomes `weak IComponent?` and the discipline is
  redundancy, not correctness. Until then every detach path is a leak-fix path — normative, not
  advisory.
- **R8 — Renderer owns the frame diff; `Surface` is a pure model.** `Surface` is the cell
  buffer + write API; `IRenderer.present(Surface)` diffs against the renderer's own previous
  frame and emits escapes (AnsiRenderer) or records the grid (TestRenderer — trivial snapshots,
  no escape parsing). This moves the sketch's `Surface.present()` into `AnsiRenderer`.
- **R9 — Chrome/insets via the collapse-override.** `Component` declares
  `Insets chrome() => padding;` and empty `void paintChrome(Surface s) {}`; `Bordered`
  (being `: Component`, R5) overrides both. `Container.contentRect()` = `box` inset by
  `chrome()`. No cross-base gymnastics needed.
- **R10 — Theme keys are dotted strings** (`"input.focused.border"`) with a defined fallback
  chain; T08 owns the canonical key registry; T04/T05 declare the keys each component consumes.
  Themes load from TOML parsed **in-language** (never via json — bug #30 blocks JSON on LLVM).
  Runtime theme *switching* mutates the bound `Theme` instance (`setTheme` swap + full
  invalidate); DI (`bind ITheme`) selects the instance at compile time and cannot rebind at
  runtime — stated honestly.
- **R11 — Keymap wins at capture.** Modifier chords bound in `App.keymap()` resolve during the
  capture phase at the root, before focus routing; unmodified printable keys always reach the
  focused component. A focused component that genuinely needs a bound chord byte registers a
  scoped keymap (T03 defines scoping). Rationale: global accelerators (`^S`) must beat a
  focused TextBox.
- **R12 — Handler registration returns an `int` token**; removal is by token (`offKey(t)`).
  Method references have no identity (LA-25), so remove-by-value is impossible by design.
- **R13 — Overlays are an App-level stack.** `App.pushOverlay(c)` / `App.popOverlay()`; overlays
  paint above the root in push order and the **top overlay owns input exclusively** (modal
  semantics); `Modal` is sugar over it. Focus is saved on push and restored on pop.
- **R14 — Component-grain damage gates paint; cell-grain gates I/O.** The sweep repaints only
  dirty subtrees into the Surface; the renderer diff decides terminal writes. A component that
  repaints identical cells costs buffer writes, zero I/O.
- **R15 — Sonar is a checked trident package.** Full checker coverage, narrowing, `T?` — all
  fine (the prelude-narrowing caution does NOT apply to package code). Package root `sonar/`,
  namespace `Sonar` (PascalCase, the Lcurl precedent).
- **R16 — No `console.write` during a running app.** Diagnostics go through `Sonar::log(string)`
  (ring buffer) + the `DebugOverlay` component; T09 owns the pipeline.

Out of scope for v1 (recorded so nobody re-opens them silently): RTL text, grapheme clusters
(cell math is scalar-based + `glyphWidth`), IME/dead keys, 256-color/truecolor (cell format is
Surface-internal, so widening later is non-breaking — see C6), Windows legacy conhost, sixel.

---

## 4. Frozen language-feature surfaces (F1–F6)

The framework docs code against THESE exact surfaces; the language docs design their
implementation. Deviation = escalation here.

### F1 — Block bulk ops

```lev
class Block {
    void fill(int off, int len, int value);              // value 0..255 else throws
    void blit(int dstOff, Block src, int srcOff, int len); // memmove semantics (overlap-safe)
    bool equals(Block other);                             // false on length mismatch
    int  mismatch(Block other, int from);                 // first differing index >= from, -1 if none;
                                                          // throws on length mismatch
}
```
Bounds are checked against **each view's own len**; all four are natives on oracle+IR (shared
clause), emit-C++, and LLVM. `mismatch` is the frame-diff primitive.

### F2 — Terminal floor (winsize + signals)

```lev
class WinSize { int rows; int cols; }
namespace term { WinSize size(); }                        // TIOCGWINSZ; \x1b[6n fallback; default 24x80
// floor: int sysWinSize(int fd, int field)               // 0=rows 1=cols, -1 fail

namespace signal {
    const int INT = 2;  const int QUIT = 3;  const int TERM = 15;
    const int HUP = 1;  const int USR1 = 10; const int WINCH = 28;
    InStream<int> on(int sig);                            // signal-as-stream over sysWatch
}
// floor: sysSignalOpen(Array<int>) -> fd; sysSignalNext(fd) -> int; sysSignalClose(fd)
```
Raw mode already ships (`term::enableRaw()`); entering raw mode installs restore-and-reraise
handlers for TERM/HUP/INT/QUIT. SIGWINCH delivery coalesces ("at least one tick after the last
change"). Raw mode clears ISIG, so Ctrl-C arrives as byte `0x03` through the input path.

### F3 — Bound method references

`obj.method` in value position is a bound function value: type = the method's signature minus
the receiver. **v1 receiver restriction: a bare local identifier or `this` — nothing else**
(`a.b.method`, `getFoo().method` are compile errors: "bind the receiver to a local first").
Each evaluation is a fresh value (no identity). No generic methods. Dispatch is exactly the
eta-expansion lambda's (`(args) => obj.method(args)`), i.e. the landed runtime-class dispatch
rule. The checker-only implementation uses the ordinary lambda path; framework code may write
`box.onKey(this.onKeyDown)` directly.

### F4 — Procedural macros

```lev
macro sonar(string payload) comptime {
    // ordinary comptime Leviathan code: parse the payload, build source text,
    // return a parsed fragment.
    return meta::parseExpr(generated);
}
var app = sonar!(`<App title="demo">...</App>`);          // quasiliteral arg = raw string
comptime string tpl = import("views/editor.sonar");
var app2 = sonar!(tpl);                                   // any comptime string works
```
New machinery: `Ast` comptime value kind; `meta::parseExpr(string) -> Ast` /
`meta::parseStmts(string) -> Ast` (parse errors report at the macro call site with fragment
text + offset); quasiliteral accepted as a macro-call argument (raw payload string); a
`comptime`-bodied macro form whose body runs on the hermetic comptime oracle (`import()`
allowed, `sys*` denied, step-bounded); returned `Ast` splices at the call site; `--expand`
round-trips. Macro-generated identifiers use the `__sonar_` prefix (legal source names —
`$`-names are compiler-internal only).

### F5 — Weak references

```lev
class Component {
    weak IComponent? parent_ = None;     // field-only modifier; type MUST be T? with T a
}                                        // class or interface type
```
Store does not retain; read yields `T?` — `None` once the referent is destroyed; narrowing
applies as usual. Recommended implementation (the language doc argues it): per-thread weak
table + proxy cells + a stolen low bit of the header `meta` word (sizes are 16-aligned) so
non-weak objects pay one branch at free — **no ABI header widening**. Interpreters carry a
`weak_ptr`-backed value kind. Weak edges crossing a `spawn`/`Channel` copy boundary flatten to
`None` (the referent wasn't copied; a copy is a different object). Not allowed on strings,
arrays, maps, Blocks, closures in v1 (weak = non-owning back-edge to an entity).

### F6 — Covariant-return interface satisfaction

An interface method requirement is satisfied — **and consumed, contributing no second slot** —
by a class method with the same name, identical parameter canonicals, and a return type
*assignable to* the required return. Interfaces allocate nothing (info.md §8), so exactly one
runtime slot (the class's) survives: no §3.4a shared-arity hazard, nothing for name+arity
runtime dispatch to disambiguate. Scope v1: interface satisfaction only; class-override
covariance is an explicit non-goal (today same-name different-canonical **coexist** as slots —
a footgun to design around, not extend). Parameters stay invariant.

---

## 5. Frozen framework contracts (C1–C14)

All code below is normative surface. Bodies shown are semantics sketches; track docs supply the
real ones. Everything lives in `namespace Sonar` unless stated.

### C1 — Namespaces, globals, errors

```lev
namespace Sonar {
    App? currentApp = None;                       // written bare from inside the namespace (R3)
    App app();                                    // narrows currentApp; throws if None
    int glyphWidth(char c);                       // 0/1/2 cell width (T01 owns the table)
    string version();                             // "0.1.0"
    void log(string msg);                         // ring buffer, DebugOverlay drains (R16)

    namespace Attr { const int Bold = 1; const int Dim = 2; const int Italic = 4;
                     const int Underline = 8; const int Reverse = 16; const int Blink = 32; }
    namespace Mod  { const int Shift = 1; const int Alt = 2; const int Ctrl = 4; }
    const int Unbounded = 1073741823;             // layout "no max" sentinel

    interface ISonarException : IException { }
    class SonarException : Exception, ISonarException { }
}
```

### C2 — Geometry & style value structs

```lev
struct Point  { int x; int y; }
struct Size   { int w; int h; }
struct Insets { int top; int right; int bottom; int left;
                new Uniform(int n) { top = n; right = n; bottom = n; left = n; }
                Insets plus(Insets o); int h() => left + right; int v() => top + bottom; }
struct Rect {
    Point origin; Size size;
    int x() => origin.x;   int y() => origin.y;
    int w() => size.w;     int h() => size.h;
    int right() => origin.x + size.w;             // exclusive
    int bottom() => origin.y + size.h;            // exclusive
    bool isEmpty() => size.w <= 0 || size.h <= 0;
    bool contains(int px, int py);
    bool contains(Point p) => contains(p.x, p.y);
    Rect intersect(Rect o);                       // empty rect when disjoint
    Rect cover(Rect o);                           // bounding box ("union" avoided — type-syntax word)
    Rect inset(Insets i);
    Rect shift(int dx, int dy);
}
struct Style {
    Color fg; Color bg; int attrs;                // bare decl => Default/Default/0 (Color order, C3)
    Style withFg(Color c); Style withBg(Color c); Style withAttrs(int a);
    Style over(Style base);                       // this wins where fg/bg != Default; attrs OR
}
struct Constraint {
    int min; int max; int flex; int concrete;
    new Constraint()  { min = 0; max = Sonar::Unbounded; flex = 0; concrete = -1; }  // size-to-content
    new Fixed(int n)  { concrete = n; min = n; max = n; flex = 0; }
    new Flex(int weight) { min = 0; max = Sonar::Unbounded; flex = weight; concrete = -1; }
    new Bounded(int mn, int mx, int weight) { min = mn; max = mx; flex = weight; concrete = -1; }
    bool isConcrete() => concrete >= 0;
}
```

### C3 — Enums (closed sets; exhaustive `match` everywhere they're consumed)

```lev
enum Color : int { Default = -1, Black = 0, Red, Green, Yellow, Blue, Magenta, Cyan, White,
                   BrightBlack, BrightRed, BrightGreen, BrightYellow, BrightBlue,
                   BrightMagenta, BrightCyan, BrightWhite }   // Default FIRST: bare decl => Default
enum BorderStyle { NoBorder, Single, Double, Rounded, Heavy, Dashed }
enum KeyCode : int { Char = 0, Enter, Escape, Tab, BackTab, Backspace, Delete, Insert,
                     Up, Down, Left, Right, Home, End, PageUp, PageDown,
                     F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12 }
enum MouseButton { NoButton, Left, Middle, Right, WheelUp, WheelDown }
enum MouseKind   { Press, Release, Move, Drag }
enum Overflow    { Clip, Scroll, Wrap }
enum Align       { Start, Center, End, Stretch }
enum Axis        { Horizontal, Vertical }
enum Dock        { Top, Bottom, Left, Right, Fill }
enum WidthMode   { Standard, EastAsian }
```

### C4 — Event classes (R1)

```lev
class KeyEvent {
    KeyCode code; char ch; int mods; bool handled = false;
    new KeyEvent(KeyCode c, char character, int m) { code = c; ch = character; mods = m; }
}
class MouseEvent {
    MouseKind kind; MouseButton button; int x; int y; int mods; bool handled = false;
    new MouseEvent(MouseKind k, MouseButton b, int px, int py, int m) { ... }
}
class PasteEvent { string text; bool handled = false; new PasteEvent(string t) { text = t; } }
```

### C5 — Core interfaces

```lev
interface IComponent {
    Rect box;                                     // the field requirement; Component allocates (P1)
    Dock dock; int gridRow; int gridCol; int gridRowSpan; int gridColSpan;  // T02 amendment, §10 log
    Constraint widthConstraint();
    Constraint heightConstraint();
    void measure(Size avail);                     // avail axis of Sonar::Unbounded = unconstrained
    Size desired();                               // valid after measure
    void arrange(Rect assigned);
    void paint(Surface s);
    void invalidate();                            // content damage
    void invalidateLayout();                      // geometric damage
    bool dirty();
    bool subtreeDamaged();
    IComponent? parent();
    void setParent(IComponent? p);                // engine-internal; add/remove call it
    bool visible();
    void onAttach();                              // fired by add / overlay push
    void onDetach();                              // fired by remove / clear / overlay pop / teardown
}
interface IContainer {
    IContainer add(IComponent c);                 // F6: classes return their own type
    IContainer remove(IComponent c);              // MUST run detach discipline (R7)
    IContainer clear();
    Array<IComponent> children();
    IContainer setLayout(ILayoutStrategy l);
    ILayoutStrategy layout();
}
interface ILayoutStrategy {
    Size measure(Array<IComponent> kids, Size avail);   // returns desired content size
    void arrange(Array<IComponent> kids, Rect content);
}
interface IRenderer {
    Size size();
    void acquire(bool altScreen, bool mouse, bool bracketedPaste);
    void release();                               // idempotent; crash path calls it too
    void present(Surface s);                      // diff vs renderer-owned prev frame (R8)
    void setCursor(Point? pos);                   // None = hidden
    void bell();
}
interface IInputSource {                          // DI seam so tests can script input
    void start((string) => void onBytes);         // byte-clean chunks (escape bytes survive)
    void stop();
}
interface ITheme { Style style(string key); bool has(string key); }
interface IFocusPolicy {
    Focusable? first(IComponent root);
    Focusable? next(IComponent root, Focusable current);
    Focusable? prev(IComponent root, Focusable current);
}
interface IValidatable { bool validate(); string validationMessage(); }
interface ISingleLine { }
interface IMultiLine { }
```

### C6 — Surface (T01 owns; format internal-but-documented)

```lev
class Surface {
    int width; int height;                         // caller-read-only; resize() is the sole writer
    new Surface(int w, int h);
    void put(int x, int y, char c, Style s);      // clip-aware; wide glyphs write continuation cell
    void writeText(int x, int y, string text, Style s);  // scalar-decodes, clips, wide-aware
    void fill(Rect r, char c, Style s);
    void clear(Style s);
    void pushClip(Rect r);                        // intersects with current clip
    void popClip();
    Block cells();                                // renderer read access
    int cellOffset(int x, int y);                 // byte offset of (x,y)
    void resize(int w, int h);                    // SIGWINCH path; contents undefined after
}
```
**Cell format (frozen v1, 8 bytes LE):** [0..3] Unicode scalar; [4] fg carrier (`code & 0xFF`,
255 = Default); [5] bg carrier; [6] Attr bits; [7] flags (bit 0 = wide-glyph continuation).
Truecolor is a v2 widening hidden behind this API.

### C7 — Component base (T01 owns; abbreviated to the normative surface)

```lev
class Component {
    Rect box = Rect(Point(0, 0), Size(0, 0));
    Insets padding = Insets(0, 0, 0, 0);
    Dock dock = Dock::Fill;                       // read by DockLayout
    int gridRow = 0; int gridCol = 0; int gridRowSpan = 1; int gridColSpan = 1;  // GridLayout
    // engine-internal state: contentDirty_, layoutDirty_, childDamage_, parent_, hidden_,
    //                        width_/height_ constraints, desired_, handler lists
    Constraint widthConstraint(); Constraint heightConstraint();
    void setWidth(Constraint c); void setHeight(Constraint c);        // invalidateLayout
    void measure(Size avail);                     // template: clamp(contentDesired(avail))
    Size contentDesired(Size avail) => Size(0, 0);// leaf hook — the ONE method leaves override for sizing
    Size desired();
    void arrange(Rect assigned);                  // writes box; clears layoutDirty_
    void paint(Surface s);                        // template: paintBackground → paintContent → paintChrome
    void paintBackground(Surface s); 
    void paintContent(Surface s) { }              // leaf hook
    void paintChrome(Surface s) { }               // Bordered overrides (R9)
    Insets chrome() => padding;                   // Bordered overrides: padding + border inset (R9)
    void invalidate(); void invalidateLayout();   // set flag + bubble childDamage_ rootward
    bool dirty(); bool subtreeDamaged();
    IComponent? parent(); void setParent(IComponent? p);
    bool visible(); void setVisible(bool v);      // hidden components skip layout + paint
    void onAttach() { }  void onDetach() { }      // lifecycle hooks (Timer cleanup, focus restore)
    int onKey((KeyEvent) => void h);        void offKey(int t);
    int onKeyCapture((KeyEvent) => void h); void offKeyCapture(int t);
    int onMouse((MouseEvent) => void h);    void offMouse(int t);
    int onMouseCapture((MouseEvent) => void h); void offMouseCapture(int t);
    int onPaste((PasteEvent) => void h);    void offPaste(int t);
}
```

### C8 — Container and the mixins (R5: all `: Component`)

```lev
class Container : Component {
    IContainer add(IComponent c);                 // setParent(this), onAttach, invalidateLayout
    IContainer remove(IComponent c);              // detach discipline (R7): setParent(None), onDetach
    IContainer clear();
    Array<IComponent> children();
    IContainer setLayout(ILayoutStrategy l); ILayoutStrategy layout();   // default FlexLayout(Vertical)
    Rect contentRect();                           // box.inset(chrome())
    // measure/arrange delegate to layout over visible children; paint draws children in order
    // (z = child order, last on top); children painted under pushClip(contentRect())
}
class Focusable : Component {
    bool focused = false;                         // engine-written
    void setTabStop(bool on); bool isTabStop();   // default true
    int onFocusChange((bool) => void h); void offFocusChange(int t);   // arg: gained
    Point? cursorPos() => None;                   // Input/TextBox override; App reads after paint
    distinct void changed() { }                   // focus-flavored internal hook
}
class Scrollable : Component {
    int scrollX = 0; int scrollY = 0;
    Size contentSize = Size(0, 0);                // set by owner during measure
    void scrollTo(int x, int y);                  // clamps to content − viewport; invalidate
    void scrollBy(int dx, int dy);
    distinct void changed() { }                   // scroll-flavored internal hook
}
class Styleable : Component {
    void setStyle(string key, Style s);           // per-instance override layer (bracket-sugar map)
    Style resolve(string key, ITheme theme);      // theme fallback chain + instance overrides (R10)
}
class Bordered : Component {
    BorderStyle border = BorderStyle::NoBorder;
    string title = "";
    void setBorder(BorderStyle b);                // invalidateLayout (chrome changes)
    void setTitle(string t);                      // invalidate
    Insets chrome() => ...;                       // padding + 1-cell inset when border != NoBorder (R9)
    void paintChrome(Surface s) { ... }           // border glyphs + title (R9)
}
```

### C9 — App & run loop surface (T09 owns)

```lev
class App : Container {
    new App();
    App title(string t);
    App altScreen(bool on = true);
    App mouse(bool on = false);
    App bracketedPaste(bool on = true);
    App fpsCap(int fps = 60);
    App wideGlyphs(WidthMode m = WidthMode::Standard);
    App onResize((Size) => void h);
    void run();                                   // blocks the calling task until quit()
    void quit();  void quitWith(int code);        // code via env.setExitCode
    Size screen();                                // cells: w = cols, h = rows (axis mapping!)
    Keymap keymap();
    void focus(Focusable f);  Focusable? focused();
    void pushOverlay(IComponent c); void popOverlay();     // R13; Modal rides this
    int every(int intervalMs, () => void cb);     // loop timers; @Sonar::Timer rides this
    void cancelEvery(int token);
    void requestFrame();                          // manual damage kick (rare)
    void pumpOnce();                              // test hook: one input→layout→paint→present pass
    void debugOverlay(bool on);
}
class Keymap {
    int bind(string chord, () => void action);    // duplicate chord in one keymap = throws (loud)
    void unbind(int token);
    bool handle(KeyEvent e);                      // true = consumed
}
```
**Chord grammar (frozen):** `[C-][M-][S-]key` with `^X` ≡ `C-X`; `key` = printable char |
KeyCode name (`Enter`, `Tab`, `F5`...). Examples: `"^S"`, `"M-x"`, `"C-M-Left"`, `"S-Tab"`.

**Frame phases (frozen order):** input → layout (dirty subtrees) → sweep → paint (dirty
components, clipped) → present (renderer diff) → cursor (`focused().cursorPos()` →
`renderer.setCursor`). Tree mutations from handlers take effect next frame; no reentrant
synchronous layout. Idle = blocked on watches/timers, zero CPU.

### C10 — Theme & DI wiring (T08 owns)

```lev
class Theme : ITheme {
    new Theme();                                  // empty; Theme::Default()/Dark()/Light()/HighContrast()
    new FromToml(string tomlText);                // in-language mini-TOML (R10); throws SonarException
    Theme set(string key, Style s);               // fluent (leaf rule R6 trivially satisfied)
    void setTheme(Theme other);                   // runtime switch: swap contents + app-wide invalidate
}
// composition root:
bind ITheme => Theme::Default();
bind IRenderer => AnsiRenderer();
bind IFocusPolicy => FocusRing();
bind IInputSource => StdinSource();
```
Bind interfaces/reference classes only (value-struct binds are rejected by the checker — so a
`Style` is passed, never bound). Scoped rebinding (a Modal subtree with its own theme) uses a
lexical `bind` in the subtree's construction scope.

### C11 — Template attribute mini-contract (T06 elaborates; T04/T05 feed it)

Common attributes on every tag: `id`, `width`, `height`, `minWidth`, `maxWidth`, `minHeight`,
`maxHeight`, `flex`, `dock`, `row`, `col`, `rowSpan`, `colSpan`, `theme`, `hidden`, `padding`,
`on:key`. `id="name"` assigns the constructed node to the enclosing class's **declared** field
`name` (missing/mismatched field = compile error at the template). Every component doc (T04/T05)
MUST include an "attribute → setter/event mapping" table; T06 aggregates them.

### C12 — Testing seams (T10 owns)

`TestRenderer : IRenderer` records grids + cursor + acquire/release calls; `snapshot() -> string`
(format frozen in T10: text grid, then an optional style-annotation block — two channels so
text-only assertions don't churn). `ScriptedInput : IInputSource` feeds byte scripts.
`App.pumpOnce()` drives deterministic frames. Goldens live in `sonar/tests/golden/`;
differential doctrine: byte-identical snapshots across oracle/IR/LLVM.

### C13 — Package shape

```toml
# sonar/trident.toml
name    = "sonar"
version = "0.1.0"
sources = ["src/*.lev", "src/components/*.lev", "src/layout/*.lev", "src/templates/*.lev"]
assets  = ["themes/**"]
```
Examples in `sonar/examples/<app>/` each with their own `trident.toml` + `[[dep]] path = "../.."`.

### C14 — Capability matrix (which mixins each component composes)

As the sketch §5 table, with R5 semantics (all mixins `: Component`) and these deltas:
`Tabs.add(string tabLabel, IComponent c)` is the two-arg labeled add; `ListView`/`TableView`/
`TreeView` consume source interfaces defined in T05 (`IListSource` etc. — virtualized, paint
only the viewport); `Modal : Container, Focusable, Bordered` rides the overlay stack; new
`DebugOverlay : Component` (R16). Component docs restate their own row.

---

## 6. Gates & timeline (aggressive-pursuit posture)

| gate | contents | target |
|---|---|---|
| G-S0 | F1 + F2 landed (language floor) | 2026-07-14 |
| G-S1 | T01 + T02 + T03 implemented; probe suite P1–P11 green | 2026-07-16 |
| G-S2 | F3 + F6 landed; T04 implemented | 2026-07-17 |
| G-S3 | T05 + T07 + T08 implemented; example apps boot | 2026-07-19 |
| G-S4 | F4 landed; T06 template layer end-to-end (`--expand` round-trips) | 2026-07-21 |
| G-S5 | F5 landed; `parent_` flipped to `weak`; churn corpus flat | 2026-07-23 |
| G-v1 | T09 polish + T10 full suite + docs; tag sonar 0.1.0 | 2026-07-25 |
| G-v1.5 | T11 reactivity | 2026-07-31 |

Waves may start before their gate's language feature lands wherever a stated interim exists
(lambdas before F3; detach discipline before F5; polling resize stub before F2 if needed).

## 7. Conventions, STOP protocol, cheat sheet

**House rules (binding):** `.lev` only, never `.ext`. Framework tracks never touch
`src/**`/`runtime/**` — a language gap found mid-implementation is a STOP: record it in the
track doc's log + file `/bug.md`-style repro (implementers escalate; design agents record in
their doc's §6 only). X64Gen/ELF frozen; nothing gates on ELF. Designs completed → move to
`designs/sonar/complete/` (framework) / `designs/complete/` (language) per house convention.
Every doc keeps an append-only implementation log.

**Verified-syntax cheat sheet** (facts design examples must respect — sources: infodemp §8,
reference.md):

1. No truthiness: conditions are `bool`; absence tests are `x == None` / `!= None` (narrowing),
   `?.` and `??` exist.
2. No `static`: use namespace consts/functions and labeled constructors (`Theme::FromToml`).
3. Enums: `::` members, `code()`/`toString()`/`fromCode`, exhaustive `match`; not bit-flags (R4).
4. Maps: write via `m[k] = v` bracket sugar (`.with/.without` missing on LLVM — bug #18).
5. Arrays are pure: `arr = arr.add(x)`; handler lists rebind (fine at UI frequency).
6. Structs copy; `mutating` marks self-writing methods; structs are final (no inheritance).
7. Inside `namespace NS`, write globals bare (`x = v`), never `NS::x = v` (lowering hazard).
8. Bug #34 workaround: within a class, declare lambda-taking overloads BEFORE string-taking
   same-name overloads.
9. No JSON on the LLVM lane (bug #30) — hence in-language TOML for themes.
10. Strings are byte-counted; scalar view via `.chars()` / `.at(i)`; wide-cell math via
    `Sonar::glyphWidth`.
11. `char` literals: single-quoted strings re-type to `char` by expected type.
12. Method references: unbound `C::m` landed; **bound `obj.m` gates on F3** — until then,
    lambdas (block-body lambdas exist).
13. Dispatch: interface-typed AND class-typed receivers dispatch on the runtime class (landed
    class-method-dispatch); overridden overload sets sharing an arity are a compile error —
    keep overridden methods single-arity per name.
14. `using Type x = expr;` gives deterministic `close()` on every block exit; `IDisposable`.
15. Don't `await` a bare global Promise across `spawn` (bug #35); Channels are the portal.
16. Attribute args: positional, comptime-const int/float/bool/string only.

## 8. Probe & risk register (run before G-S1; owners: T01/T10)

| # | probe | fallback if red |
|---|---|---|
| P1 | inherited field (`Component.box`) satisfies `IComponent`'s field requirement | move `box` to accessor methods on the interface |
| P2 | direct `obj.field(args)` call on a closure-typed field | `var h = obj.field; h(args)` |
| P3 | `is`/`match` narrowing on class types (`c is Focusable`) | capability query methods on Component |
| P4 | `Array<IComponent>` heterogeneous paint dispatches to runtime class | store as interface + rely on landed dispatch (expected green) |
| P5 | struct fields auto-construct recursively (`Rect` in `Component`) | explicit ctor inits |
| P6 | `distinct` on METHODS keeps two base slots, `::`-qualified reachable | rename per-mixin hooks |
| P7 | diamond collapse shares ONE `Component` core across two mixin paths | single-inheritance chain (Focusable : Component; Scrollable : Focusable) — ugly but ordered |
| P8 | collapse keeps the LATER base's method (mixin override wins) | leaf overrides `paintChrome` explicitly with `this.Bordered::paintChrome(s)` |
| P9 | enum with negative carrier (`Default = -1`) | shift carriers +1, map at the ANSI boundary |
| P10 | labeled constructors on structs (`Constraint::Fixed(3)`) | namespace factory functions |
| P11 | mixin base ctor chains through a diamond ($init runs sanely twice) | leaf ctor sets fields directly, skips base ctor calls |

Risk highlights: F5 is the only ABI-adjacent feature (mitigated by the no-header-change
side-table recommendation + R7 interim); F4's error-span attribution is the known soft spot
(mitigation: carry the macro-call span; REQUIRED diagnostics section in T06); SGR-minimizing
diff emitter correctness (mitigation: golden escape-stream tests + a dumb-full-repaint debug
mode in AnsiRenderer).

## 9. Testing doctrine

Unit (layout math, decoder tables, theme resolution — table-driven), snapshot (TestRenderer
goldens, two-channel format), differential (oracle/IR/LLVM byte-identical; emit-C++
compile-only for anything touching `run()`), soak (mount/unmount churn under the detach
discipline; post-F5, the compiler-side churn corpus gains a parent↔children cycle program),
input scripting (ScriptedInput byte scripts incl. split escape sequences), and example apps as
integration tests. T10 owns the harness; every track doc ships its test plan.

## 10. Implementation log (append-only)

- 2026-07-12 — doc created; contracts C1–C14, rulings R1–R16, features F1–F6 frozen; track
  set T01–T11 + language set F1–F6 commissioned.
- 2026-07-12 — T01 core landed. P1–P11 are green on all four active engines. C6's
  `readonly` dimension spelling was corrected to caller-read-only mutable fields because
  `Surface.resize` is itself a frozen writer; T01's lifecycle and damage internals use
  `__sonar*` interface hooks so open user hooks cannot bypass detach cleanup and compiled
  backends do not depend on class-narrowing for framework-internal capabilities.
- 2026-07-12 — T02 layout landed (`designs/complete/techdesign-02-layout.md`); **C5
  amended** (escalation, per this section's own rule): `IComponent` gained five field
  requirements — `Dock dock; int gridRow; int gridCol; int gridRowSpan; int gridColSpan;`
  — already satisfied verbatim by `Component`'s existing C7 declarations (zero changes to
  `Component`). Forced by a real finding, not preference: `DockLayout`/`GridLayout` need
  those `Component`-only fields off `IComponent`-typed values, and `if (c is Component) …`
  narrowing — validated as generally correct with a small synthetic probe — turned out to
  silently evaluate `false` on the emit-C++ backend specifically for `Component`'s actual
  shape (bug.md #36 [P1], not root-caused; oracle/IR/LLVM all narrow correctly). This is
  exactly the failure mode T01's own log line above was already guarding against
  ("compiled backends do not depend on class-narrowing for framework-internal
  capabilities") — C5's widening keeps that guarantee intact instead of quietly losing
  emit-C++ coverage. `Container`'s default `layout_` is now `FlexLayout(Axis::Vertical)`,
  completing the handoff T01's own `container.lev` comment named; the bootstrap
  `CoreVerticalLayout` is retired.
- 2026-07-12 — T03 events/input/focus/keymap landed (`designs/complete/techdesign-03-events-input.md`
  carries the full log). Byte-identical on all four active engines after two new compiler
  bugs were found and worked around in-package (bug.md #40 `Map<string, EnumType>`
  segfault past one entry; #41 `Array<struct-with-enum-field>` goes stale after unrelated
  heap activity — `Chord` is a `class`, not a `struct`, because of it). `KeyEvent`'s C4
  predicates (`isChar`/`isCtrl`/`matches`) live in `component.lev` alongside the classes
  T01 already declared there, not in `events.lev` — classes cannot be reopened across
  files, only namespaces merge. `Sonar::log`/`handlerErrorCount()` (C1/R16) were
  implemented in `events.lev` since no track owned them yet.
- 2026-07-13 — T04 basic components landed
  (`designs/complete/techdesign-04-components-basic.md` carries the full log). C5
  **amended**: `ISingleLine`/`IMultiLine`/`IValidatable` were declared for the first time
  (no earlier track needed them) in `sonar/src/components/text.lev` rather than
  `component.lev`, to stay inside this track's own file ownership. **Marker correction
  per this track's own design doc**: `Text` drops the `ISingleLine` marker its own C14
  row listed — `wrap` makes `Text` potentially multi-line, which contradicts the marker's
  meaning; every other T04 leaf keeps its documented marker(s). Every T04 leaf also
  composes `Styleable` (for `resolve()`) even though the design doc's class headers
  don't spell it out — the doc's own `paintContent` bodies call `resolve(key, theme)`,
  which only exists on that mixin, so this is a header completion, not an amendment.
  `ProgressBar`/`Spinner`'s "App.every" language in the design doc resolves to T01's
  already-built `ILifecycleHost`/`__sonarRegisterTimer` pending-timer seam (component.lev
  §92's own comment: built precisely so the core doesn't depend on the not-yet-defined
  `App` type) — so despite the design doc's header line, this track has **no T09
  dependency**, the same "seam via parameters/registration" shape T01 used for
  `ILifecycleHost` itself and T03 used for its dispatch functions. Byte-identical on all
  four active engines after five new compiler bugs were found and worked around
  in-package (bug.md #50–#53, plus fixing T01's `TimerReg`/`ShortcutReg` — the timer/
  shortcut pending-registration structs in `component.lev` — to use explicit constructors
  instead of positional auto-construction, per bug.md #38's documented workaround,
  since the bare form was silently dropping their closure-typed `action` field and this
  track was the first to actually invoke a registered timer). #53 is the significant one:
  a lambda calling an instance method with a bare (implicit-`this`) receiver, once stored
  in a field and invoked from a different call frame, silently loses the call on `--ir`
  and **segfaults on native/LLVM** — every constructor-registered `onKey`/`onMouse`/
  `onPaste`/timer handler in this track spells the receiver explicitly
  (`this.firePress()`, `this.toggle()`, `this.handleKey(e)`, ...) as the workaround. A
  grep of T01–T03's own sources found no existing instance of the bug's trigger shape, so
  it does not appear to be a live landmine in already-merged code, but is worth an owner
  ruling on whether to fix the resolution or reject the bare-receiver form at check time.
- 2026-07-13 — **T05 M1–M4 landed** (`ContentBox`/`GridBox`/`SplitBox`/`Tabs`/`ListView`/
  `TableView`/`TreeView`, `designs/complete/techdesign-05-components-composite.md` §8 carries
  the full log). **M5**/**M6** (`BarMenu`/`MenuItem`/`Modal`/`DebugOverlay`) were **not**
  implemented — correctly gated on T09 (`App`/overlay-stack), which did not exist yet.
- 2026-07-14 — **T05 M5/M6 landed — track COMPLETE** (doc moved to `designs/complete/`),
  now that T09 has landed: `OverlayHost`/`Modal`/`alert`/`confirm`, `BarMenu`/`Menu`/
  `MenuItem`/`MenuSeparator`, `DebugOverlay`, riding an extended (additive, default-param)
  T09 overlay stack (`pushOverlay` flags, `popOverlayGroup`, focus save/restore,
  `__sonarDetachTree`). Byte-identical on oracle/IR/**LLVM** (§8). Deepened bug #65's root
  cause: **every `Container` override of a `Component` method** (`__sonarChildren`,
  `contentDesired`, `arrange`, `paint`, `__sonarContentRect`) misresolves for a multi-mixin
  leaf, so the framework tree-walks see no children — each `Container` leaf redeclares the
  set forwarding to the working `children()`/`layout()` accessors.
- 2026-07-14 — **T07 landed — track COMPLETE** (`sonar/src/attributes.lev`, doc in
  `designs/complete/`): `@Sonar::Shortcut`/`@Timer`/`@Validator` + their three Layer-B rules
  + `validateTree`/`ValidationFailure`, on the landed metaprog substrate + T04's already-
  landed Component pending/flush plumbing. Byte-identical on oracle/IR/LLVM.
  Found and filed a serious, previously-latent multi-mixin dispatch defect family
  (bug.md, full repros, independently re-verified against real builds): an intermediate
  mixin's own method (e.g. `Container.paint()`/`arrange()`/`contentRect()`,
  `Scrollable.scrollTo()`) did not reliably dispatch its internal calls to a sibling
  mixin's override (e.g. `Bordered`) composed only at the leaf — reproduced on the oracle
  and `--ir`, not just LLVM, and broke R5's diamond-collapse model for any
  `Container`+sibling or `Scrollable`+`Bordered` leaf that didn't redeclare the affected
  method itself. **Fixed at the source during this same merge** (commit `028ec6a`, landed
  on `origin/master` concurrently with this track): `this.Base::method()` qualified calls
  and unqualified sibling-mixin-override dispatch were both root-caused and repaired in
  `Checker.cpp`/`CGen.cpp`/`LlvmGen.cpp`/`Eval.cpp`/`Lower.cpp`. This track's in-package
  workarounds (redeclaring `arrange`/`paint`/`contentRect`/`scrollTo` on every affected
  leaf rather than relying on the inherited intermediate-mixin call) are left in place —
  redundant but harmless now, not reverted, since they cost nothing and the "never revert
  validated work" convention applies. The fix did NOT extend to a closely related,
  **still-open** defect (verified against the rebuilt compiler, not assumed): `Container.
  paint()`'s own inherited children-loop — a `for`-loop over an `Array<IComponent>` field,
  not a same-class method call — still silently paints nothing for any leaf composing
  `Container` with a second mixin, reproducing on the oracle and `--ir` too; the leaf-level
  `paint()` redeclaration workaround above is therefore still load-bearing, not merely
  redundant. Also found and filed (still open at track close): an LLVM-only
  `Array<struct-with-nested-struct-field>` corruption after a later, unrelated `Rect`-field
  write on the housing object — hits any `Component` subclass, since `box` gets written by
  every layout pass; `TableColumn`/`TreeRow` declared `class` per the existing bug-#41/
  `Chord` precedent, not the design's literal `struct` spelling — plus a
  `Bordered.paintChrome` byte-offset/character-index bug, an unrelated LLVM-only
  child-loop-in-clip blank-text render (a `for`-loop over `Array<IComponent>` executes but
  its writes don't survive, vs. the still-open children-loop bug above where the loop body
  never runs at all), a char-literal-in-ternary-branch misevaluation, and a `pushClip`+2-
  writes LLVM segfault; see `bug.md`'s current numbering for the live set.
- 2026-07-13 — T06 templates landed (`designs/complete/techdesign-06-templates.md` carries
  the full log). F4 was already in, so the whole layer (M0–M5) shipped in one pass:
  `expandSonar` engine + the one-line `sonar!` macro, M0 string goldens, a post-F4 live-
  component round-trip, and the external `.sonar`/`import()` path. Registry covers C11 +
  all eight T04 tables; T05 composite tags pass through as unknown tags until T05 lands.
  Two forced deviations, both filed as compiler bugs and worked around in-package: the
  expansion routes through a `Sonar::__sonarBuild` thunk instead of a bare
  `(() => {...})()` IIFE (bug.md — an imported-package symbol won't resolve inside an
  immediately-invoked lambda body), and every character test in the engine uses
  `.code()`/a two-char compare, never a char literal (bug.md — a char literal doesn't
  retype to `char` inside comptime-evaluated code, silently making `c == '<'` always
  false at comptime; this is what would otherwise make the engine behave differently at
  runtime vs. inside the macro). The §3.1 IIFE and the char-literal spellings both revert
  cleanly once those bugs land — see `bug.md`'s current numbering (renumbered once since
  this entry was first written, to resolve a collision with T05's own concurrent filings).
