# Sonar — Tech Design 01: Core (Geometry, Style, Surface, Component, Container, Damage)

**Status:** implemented. **Date:** 2026-07-12. **Track:** T01.
**Owns:** `sonar/src/{geometry,style,surface,component,mixins,container,damage,errors}.lev`.
**Depends on:** F1 (Surface perf — buildable without, slow), F5 (leak-freedom — R7 detach discipline is the normative interim). No track deps; everything else depends on THIS.
**Gates:** G-S1. **Difficulty:** L. **Risk:** MED — the R5 diamond-collapse mixin architecture rests on probes P1/P5/P6/P7/P8/P10/P11 (anchor §8); **run them before writing any code**.

Implements anchor contracts C1, C2, C3, C6, C7, C8 verbatim (signatures are in the anchor; this doc adds semantics, algorithms, and file assignments — it does not restate every signature).

---

## 1. Geometry & style (`geometry.lev`, `style.lev`)

- `Rect` normal form: any empty result (w≤0 ∨ h≤0) normalizes to `Rect(Point(0,0), Size(0,0))` so empties compare/behave uniformly. `intersect` of disjoint rects → normalized empty; `cover` = bounding box (name avoids the type-syntax word "union"); `inset` clamps to empty on over-large insets; `contains` uses exclusive right/bottom (`x <= px < right()`).
- `Insets.plus` adds componentwise; `Insets::Uniform(n)` labeled ctor (probe P10 gates labeled struct ctors; fallback: `Sonar::insetsUniform(n)` free function).
- `Style.over(base)`: fg/bg where `this` is `Color::Default` take base's (Default = transparent); attrs OR. `withFg/withBg/withAttrs` return copies (structs are values; these are non-mutating).
- `Constraint` per anchor C2 with the clamp helper the layout tracks share: `int clampAxis(Constraint c, int proposed)` → `c.isConcrete() ? c.concrete : min(max(proposed, c.min), c.max)`. Lives here (T02 consumes).
- Enum ordering matters once: `Color`'s first member is `Default = -1` so bare `Color c;` auto-constructs to Default (probe P9 gates the negative carrier; fallback shifts carriers +1 with mapping at the ANSI boundary in T09 only).
- Consts per R4: `Sonar::Attr::{Bold=1,Dim=2,Italic=4,Underline=8,Reverse=16,Blink=32}`, `Sonar::Mod::{Shift=1,Alt=2,Ctrl=4}`, `Sonar::Unbounded`.
- `glyphWidth(char c) -> int` (`style.lev`): sorted `Array<int>` of range-pairs (start,end scalar) for width-2 (East Asian W/F + emoji presentation ranges), binary search; `WidthMode::EastAsian` adds the ambiguous-width ranges to the wide set. Combining marks: width 1 in v1 (documented limitation; v2 = width-0 merge). Everything else width 1; C0 controls never reach the table (Surface substitutes, §2).

## 2. Surface (`surface.lev`) — the cell model (anchor C6, cell format frozen)

One `Block cells` (w·h·8 bytes; renderer owns its own prev per R8). Helpers: `int packStyleFg(Style)`, cell encode/decode over `int32At/setInt32` (scalar) + `setByte` (style/attr/flag bytes); `cellOffset(x,y) = (y*width + x) * 8`.

**`put(x, y, c, s)` algorithm:**
1. Clip check against the top of the clip stack (pre-intersected rects; stack bottom = full surface). Out → no-op.
2. Control chars (scalar < 0x20, 0x7F) substitute space — Surface never stores controls (decoded input should never send them here; defensive).
3. `w = glyphWidth(c)`. Before writing, **heal neighbors**: if the target cell is a continuation cell, blank the lead cell to its left; if overwriting a lead cell whose width was 2, blank its continuation to the right. Then write the cell; if `w == 2`, write the continuation cell (flag bit 0 set, same style) — unless `x+1` is clipped/out-of-bounds, in which case write a space instead (no half-glyphs, ever — the invariant all three overwrite cases preserve).
- `writeText`: scalar iteration via `text.chars()`, advancing by glyphWidth, clipping per cell (partial visibility = per-cell clip decisions, correct for free).
- `fill(r, c, s)`: clip-intersect r; encode one 8-byte cell; fill the first row by writing the cell then **doubling blits** (`blit` the filled prefix onto itself at 2×, 4×, … — O(log n) natives via F1); blit row 0 onto each subsequent row. Pre-F1 interim: per-cell puts (same result, slower).
- `clear(s)` = fill(full, ' ', s). `resize(w,h)`: fresh Block, contents undefined, clip stack reset — caller (App) full-invalidates.
- `pushClip(r)` pushes `r ∩ current`; `popClip` throws `SonarException` on underflow.

## 3. Component (`component.lev`) — anchor C7 semantics

- **Measure template:** `measure(avail)` sets `desired_ = Size(clampAxis(width_, d.w), clampAxis(height_, d.h))` where `d = contentDesired(avail)`; `contentDesired` is THE leaf sizing hook (default `Size(0,0)`). `avail` axes may be `Sonar::Unbounded`. `desired()` valid after measure. `arrange(assigned)` writes `box`, clears `layoutDirty_`.
- **Paint template:** `paint(s)` = `paintBackground(s)` (fills box with the resolved background when the component is Styleable-resolved to a non-transparent bg; plain Components skip) → `paintContent(s)` (leaf hook) → `paintChrome(s)` (empty; Bordered collapse-overrides per R9/P8).
- **Damage (the two flags + path marker):**
  ```
  invalidate():        if (!contentDirty_) { contentDirty_ = true; bubble(); }
  invalidateLayout():  if (!layoutDirty_)  { layoutDirty_ = true; contentDirty_ = true; bubble(); }
  bubble():            walk parent links; on each ancestor set childDamage_ = true,
                       stopping early if already set (amortizes to O(1) per event);
                       at the root (parent == None), notify App.scheduleFrame() if attached.
  ```
  `dirty() => contentDirty_`; `subtreeDamaged() => childDamage_ || contentDirty_ || layoutDirty_`.
- **Handler lists:** per event kind a parallel pair `Array<(KeyEvent) => void> handlers` + `Array<int> tokens` (pure arrays rebind — fine at UI frequency, and dispatch snapshots the array value at fire time for free); `onX` appends both and returns the next value of a per-component counter; `offX` filters both by token. Fire helpers `fireKey/fireKeyCapture/...` used by T03.
- **Lifecycle:** `onAttach`/`onDetach` open hooks (default empty) + the T07 plumbing this track hosts: `pendingShortcuts_`/`pendingTimers_` registration lists, flushed to `Sonar::app()` on attach, unbound/cancelled on detach (`__sonarBindShortcut`/`__sonarRegisterTimer` helpers — see T07 §3 for the injected callers). T11's binding registry (`__sonarBind`/`__sonarFieldChanged` + `Map<string, Array<() => void>>`) also lives here, cleared on detach.
- Visibility: `visible()==false` ⇒ skipped by layout, paint, hit-test, focus collection (all four stated; consumers cite this line).

## 4. Mixins (`mixins.lev`) — R5: all `: Component`, diamonds collapse

Per anchor C8. Semantics beyond the contract:
- `Focusable`: `focused` is engine-written only (T03's focus move writes it + fires `onFocusChange(bool gained)` + invalidates); `cursorPos()` default None.
- `Scrollable`: `scrollTo(x,y)` clamps to `max(0, contentSize − viewportSize)` per axis (viewport = the owner's contentRect size, queried via `box`/`chrome()`), then invalidates; `scrollBy` = relative.
- `Styleable`: `Map<string, Style>` overrides (bracket-sugar writes); `resolve(key, theme)` = instance override if present, else T08's chain via `theme.style(...)` fold — the chain algorithm is T08's; this method is its entry.
- `Bordered`: `chrome() => padding.plus(border == BorderStyle::NoBorder ? Insets(0,0,0,0) : Insets::Uniform(1))` — the R9 collapse-override of Component's `chrome()`; `paintChrome` draws the box-drawing set for its BorderStyle (6 glyph sextets tabled in the file) + the title (truncated with ellipsis, offset 2, only when border present); `setBorder` → invalidateLayout (chrome changes geometry), `setTitle` → invalidate.
- The `distinct void changed()` pair on Focusable/Scrollable stands as the language showcase (probe P6); leaves invoke qualified: `this.Focusable::changed();`.
- **Base-list order convention (normative for all component tracks):** core-most first, decorators last — collapse keeps the LATER base's implementation (P8), so `class ContentBox : Container, Scrollable, Bordered` gets Bordered's chrome/paintChrome.

## 5. Container (`container.lev`)

- `add(c)`: descendant-cycle guard (walk c's would-be ancestors; adding a node to its own descendant throws `SonarException`), append, `c.setParent(this)`, `c.onAttach()`, `invalidateLayout()`, return this.
- `remove(c)`: filter children (rebind), **`c.setParent(None)`, `c.onDetach()`** (R7 — the leak-fix line, normative), `invalidateLayout()`. `clear()` = remove-all with the same per-child discipline.
- `measure/arrange` delegate to `layout_` over VISIBLE children (default `FlexLayout(Axis::Vertical)`); `contentRect() => box.inset(chrome())`.
- `paint` override: background → chrome → `pushClip(contentRect())` → children in order (z = order, last on top) → `popClip`. (Chrome before children so borders underdraw content edges consistently; scrollbars — a ContentBox concern — repaint after children in ITS paintChrome, which runs in the leaf's collapse order.)

## 6. Damage sweep (`damage.lev`)

Frame-phase services for T09:
- `collectLayoutRoots(root) -> Array<IComponent>`: descend only where `subtreeDamaged()`; collect nodes with `layoutDirty_` whose ancestors have none (highest dirty roots).
- `paintDamaged(root, s)`: descend where `subtreeDamaged()`; for each `dirty()` component: clip-push its box ∩ ancestor content rects, `paint`, pop; clear `contentDirty_`/`childDamage_` on the way out. Complexity O(damaged paths), not O(tree) — the two-grain model's component grain (R14).

## 7. Errors (`errors.lev`)

`ISonarException : IException`; `SonarException : Exception, ISonarException` (per C1). Thrown by: clip underflow, non-positive Surface sizes, add-cycle, add-beyond-arity (SplitBox), duplicate chord (Keymap, T03), no-app `Sonar::app()`.

## 8. Milestones

| M | contents | difficulty | gated by |
|---|---|---|---|
| M0 | **Probe suite P1–P11 green or fallbacks invoked** (programs sketched in T10; results to PROBES.md) | S | — |
| M1 | geometry + style + enums + consts + glyphWidth + errors | M | P5/P9/P10 |
| M2 | Surface (format, put/heal, writeText, fill-doubling, clip, resize) | M | F1 for speed only |
| M3 | Component (measure/paint templates, damage, handlers, lifecycle) | M | P1 |
| M4 | Mixins + Container (collapse architecture, detach discipline) | M/L | P6/P7/P8/P11 |
| M5 | damage.lev sweep services | S | M3/M4 |

## 9. Potential issues & mitigations

1. **Probe failures** — each has a named anchor fallback (P7 → single-inheritance mixin chain; P8 → explicit leaf overrides with qualified calls; P1 → accessor-based interface; P10 → factory functions). Fallbacks change SHAPE not surface; the anchor log records any invocation.
2. **Mixin ctor double-init through the diamond** (P11): mixin constructors must be default-only ($init field defaults; no logic) so a twice-run init is idempotent. Convention stated; leaf ctors call each base ctor per language rule.
3. **Wide-glyph healing at clip edges** — the heal writes may target clipped cells; healing bypasses the clip (it repairs an invariant, not draws content). Stated explicitly; tested.
4. **Struct-copy no-op bug** (`var r = box; r.origin.x = 5;` mutates a copy) — the classic; named in docs; internal code always reassigns (`box = box.shift(...)` style).
5. **Handler mutation during dispatch** — snapshot semantics fall out of pure arrays; pinned by a T03 test, relied on here.
6. **fill-doubling off-by-ones** at odd widths — the doubling loop caps at remaining length each step; the position-sweep test pins it.

## 10. Testing plan

Probe programs first (T10 M1). Then: geometry table tests (incl. empty normal form, exclusive edges); cell encode/decode roundtrip; put/heal three-case matrix + clip-edge heal; writeText wide/clip goldens; fill-doubling sweep vs per-cell reference; damage traces (invalidate/bubble/sweep order pinned via an instrumented tree); detach-discipline walk (remove/clear null parents, onDetach fires, counts asserted); Container z-order + clip snapshot. All differential oracle/IR/LLVM via T10 harness.

## 11. Open questions

1. `protected`-like visibility for engine-internal members (the language has public/private) — engine-internal fields are public-with-`_`-suffix convention v1; revisit if the language grows visibility levels.

## 12. Implementation log

- 2026-07-12 — design written; not started.
- 2026-07-12 — M0 complete: P1–P11 are green on oracle, IR, emit-C++, and LLVM;
  results and standalone programs are in `sonar/PROBES.md` and
  `sonar/tests/probes/`. No named fallback was invoked.
- 2026-07-12 — M1–M5 implemented in the eight owned `sonar/src/*.lev` modules.
  The focused package test covers geometry normalization, style layering and
  width modes, Surface cell healing/clipping/control substitution/fill, handler
  snapshots, detach discipline, cycle rejection, mixin collapse, scrolling,
  and damage roots on all four active engines.
- 2026-07-12 — two contract contradictions were resolved during implementation:
  Surface dimensions are externally read-only by convention but internally
  mutable because the frozen `resize(w,h)` method must update them; and T01 uses
  an internal vertical strategy with the same bootstrap behavior until T02
  supplies the public `FlexLayout(Axis::Vertical)` type it owns.
- 2026-07-12 — supporting compiler gaps found by the real package were fixed
  with corpus coverage: namespace-local enum desugaring/qualified globals and
  qualified `fromCode` lowering, nominal optional-union assignment across
  namespace spellings, and documented reference-class identity equality.
