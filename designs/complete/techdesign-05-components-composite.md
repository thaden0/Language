# Sonar — Tech Design 05: Composite & Data Components

**Status:** design, pre-implementation. **Date:** 2026-07-12. **Track:** T05.
**Owns:** `sonar/src/components/{contentbox,splitbox,gridbox,tabs,listview,tableview,treeview,menu,modal,debugoverlay}.lev`
**Depends on:** T01 (core), T02 (layout), T03 (events/focus), T04 (basic components, conventions). Language: F3 improves handler ergonomics only (lambdas until); F5 makes Modal/Tab churn leak-free (R7 detach discipline is the normative interim).
**Gates:** G-S3. **Difficulty:** L overall, risk MED (TableView/TreeView virtualization and Modal focus rules are the hard parts).

Conforms to anchor `techdesign-00-overview.md`: C5/C7/C8 contracts, C11 attribute tables, C14 matrix, R5 (mixin base order: core-most first, decorators last), R6 (leaf fluent wrappers), R7 (detach discipline — every remove/dismiss path here is a leak-fix path), R10 (theme keys), R13 (overlay stack).

---

## 1. Scope

The container and data-heavy components: scrolling content regions, split panes, grids, tabs, the three virtualized data views (list/table/tree), menus, modals, and the debug overlay. These are the components enterprise apps actually live in; the design goal is that each is complete enough to build from this doc alone.

Shared conventions inherited from T04 and restated as binding here:

- **State-suffix theme keys**: `<component>[.<state>].<part>` with states `.focused`, `.disabled`, `.selected`. Fallback resolution is T08's chain (most-specific → component → `default`).
- **`enabled` convention** (T04's flagged anchor gap, adopted here identically): components that take input carry `bool enabled = true`; disabled ⇒ `isTabStop()` false + `.disabled` theme suffix.
- **Setter/damage table discipline**: every setter states whether it calls `invalidate()` (visual) or `invalidateLayout()` (geometric).
- **Attribute tables**: every component ends with its template-attribute table (C11); T06 consumes these verbatim.

## 2. Data-source interfaces (owned here, consumed by ListView/TableView/TreeView)

Virtualization is the enterprise requirement: a 100k-row table must paint only the viewport. The views therefore consume **source interfaces**, never materialized arrays (with array convenience overloads):

```lev
interface IListSource  { int count(); string itemAt(int i); }
interface ITableSource { int rowCount(); int colCount(); string cellAt(int r, int c); }
interface ITreeSource  {
    int rootCount();            TreeNodeId rootAt(int i);
    int childCount(TreeNodeId n); TreeNodeId childAt(TreeNodeId n, int i);
    string labelAt(TreeNodeId n);  bool hasChildren(TreeNodeId n);
}
struct TreeNodeId { int id; }     // opaque handle; the SOURCE owns meaning
```

Rationale: sources keep the tree pure and identity-free (a `TreeNodeId` is a value; expansion state lives in the VIEW as a `Map<int,bool>` keyed by `id`), and they make lazy loading (`childCount` hitting a database) the source author's choice. `ArrayListSource` (wraps `Array<string>`) ships as the convenience; `ListView.items(Array<string>)` sugar constructs it.

**Change-notification ruling:** sources are polled, not observed, in v1. Mutating the backing data requires the caller to invoke `view.refresh()` (marks content damage + re-clamps selection/scroll). An observer interface is v2 — an observed source needs weak back-refs (F5) to avoid view↔source cycles; deferred deliberately with that rationale.

## 3. Component specifications

### 3.1 ContentBox — `class ContentBox : Container, Scrollable, Bordered, IMultiLine`

The general scrolling, bordered content region. Base order per R5: Container core first, Scrollable, then Bordered (decorator last so its `chrome()`/`paintChrome` collapse-override wins).

State: `Overflow overflow = Overflow::Clip;` plus everything inherited. Fluent wrappers (R6): `border(BorderStyle)`, `title(string)`, `overflow(Overflow)`, `pad(Insets)`.

Behavior:
- `overflow == Clip`: children arranged into `contentRect()`; anything larger clips (T02's rule — layout may overflow, paint clips).
- `overflow == Scroll`: measure children with `avail = Size(contentW, Sonar::Unbounded)` (vertical scroll v1; horizontal when the child's min width exceeds content — both axes supported), record `contentSize` from the layout's returned desired, arrange into a virtual rect of `contentSize`, then **paint with a translation**: children paint at `box`-relative positions minus `(scrollX, scrollY)`, clipped to `contentRect()`. Design note: translation is implemented by arranging children into the virtual rect and having `paintChildren` push a clip and offset the Surface writes — Surface has no offset transform, so ContentBox arranges children at *already-translated* boxes and re-arranges on scroll. Scrolling therefore costs an arrange pass over the subtree — acceptable (scroll = full-viewport repaint anyway); recorded as issue §5.1 with the v2 alternative (a Surface offset register).
- `overflow == Wrap` is invalid for a container — construction-time `SonarException`.

Scrollbars: painted by `paintChrome` **after** the border — a 1-cell vertical bar on the right inner edge when `contentSize.h > viewport.h` (thumb size/pos = proportional, min 1 cell), horizontal on the bottom inner edge. Scrollbars consume no layout space (they overlay the last content column/row — the classic TUI trade; documented). Theme keys: `contentbox.scrollbar`, `contentbox.scrollbar.thumb`.

Input: wheel events (T03 routes them here as nearest Scrollable), `PageUp/PageDown` scroll by `viewport.h - 1`, arrows scroll by 1 when it (rather than a focusable child) is focused... ContentBox is **not** Focusable (C14) — arrows/PageUp reach it only via bubble from a focused descendant that declined them, or via wheel. Stated plainly: keyboard scrolling of a box with no focusable children requires wrapping content in a focusable leaf or app-level keymap; recorded as an accepted v1 boundary (issue §5.2).

| attribute | expands to |
|---|---|
| `borders`, `title`, `padding`, `overflow` | corresponding setters |

Damage: `scrollTo` → invalidate (content shifts, geometry unchanged); `border/title` → invalidateLayout/invalidate per T01's Bordered.

### 3.2 SplitBox — `class SplitBox : Container, Bordered`

Exactly two children (add() beyond 2 throws `SonarException` — loud, per the language's loudness doctrine). State: `Axis axis`, `int ratioPct = 50` (percentage of the first pane), `bool resizable = true`, `int minFirst = 3`, `int minSecond = 3`, divider drag state.

Layout: bypasses the pluggable strategy — SplitBox **is** its own layout (setLayout throws; documented deviation from IContainer's generality, justified: the divider is a hit-target the strategy model doesn't express). First pane gets `clamp((content.main - 1) * ratioPct / 100, minFirst, content.main - 1 - minSecond)`; divider = 1 cell; second gets the rest. Integer determinism per T02.

Divider rendering: `│`/`─` line in theme key `splitbox.divider`, `.focused` variant while dragging. Resize: mouse Press on the divider cell begins drag (MouseKind::Drag updates ratio from the pointer position; Release ends); keyboard: when `resizable`, the divider is reachable via a chord the APP binds (no default — documented; enterprise apps bind e.g. `C-M-Left/Right`), plus `on:resize(int newRatioPct)` fires on every change.

| attribute | expands to |
|---|---|
| `axis`, `ratio`, `resizable` | setters (`ratio` = int percent) |

Issue: ratio drift on container resize — ratio is the source of truth (percent), pane sizes recompute; min clamps may make the effective ratio differ from stored ratio (stored value is NOT back-written — prevents feedback loops; documented).

### 3.3 GridBox — `class GridBox : Container, Bordered`

Thin, honest sugar over `GridLayout` (T02): constructor `GridBox(int rows, int cols)` installs the strategy; `rowConstraint(int i, Constraint c)` / `colConstraint(int i, Constraint c)` / `gap(int)` forward to it. Children position via their C7 grid fields (`row/col/rowSpan/colSpan` attributes in templates). Adds nothing else — exists so templates read `<GridBox rows="3" cols="2">` naturally.

### 3.4 Tabs — `class Tabs : Container, Focusable`

State: `Array<string> labels_` parallel to children; `int selected = 0`; `int tabStripScroll = 0` (cells, when the strip overflows).

API: **`Tabs add(string tabLabel, IComponent c)`** (the anchor C14 two-arg add). Plain `add(IComponent)` labels it `"Tab N"` (kept legal so IContainer holds; logged via `Sonar::log`). `remove` drops the parallel label; `select(int i)` clamps, fires `on:select(int)`, invalidates old+new panes.

Layout: row 0 (or rows 0..0 + a separator line when bordered) is the strip; the selected child arranges into the remaining content rect; **non-selected children are arranged to an empty rect and skipped by paint** (they stay attached — state preserved; damage from a hidden tab's mutations is absorbed because paint skips it — spelled in §5.3).

Strip rendering: ` label ` cells per tab, selected gets `tabs.selected` theme (+ underline attr), focused view adds `.focused`; overflowing strip scrolls with `‹`/`›` indicators, keeping the selected tab visible. Keys (as the focused tab stop): Left/Right move selection; Char digits 1–9 jump; the strip is one tab stop, the pane's focusables are their own stops (document order = after the Tabs node).

| attribute | expands to |
|---|---|
| `selected` | `select(int)` |
| child `tabLabel` | routes the child through `add(label, child)` |

### 3.5 ListView — `class ListView : Focusable, Scrollable, Bordered`

State: `IListSource source_` (default empty), `int selected = -1`, `bool multi = false`, `Map<int,bool> checked_` (multi mode), `int anchorIdx = -1` (shift-range v2 — field reserved, unused v1).

Not a Container — rows are **painted, not child components** (the virtualization decision; a row component per item would rebuild the tree the framework exists to avoid).

contentDesired: width = `min(longest visible label, avail.w)` is a trap (requires scanning the source) — v1 rule: desired = `Size(avail.w, count)` capped by constraints; width fills (lists are almost always flexed); documented.

Paint (the virtualization core, the pattern TableView/TreeView copy):
```
first = scrollY; last = min(count, scrollY + viewport.h)
for i in first..last-1: paint row (i - scrollY) from source.itemAt(i)
```
Exactly `viewport.h` calls to `itemAt` per repaint. Row style: `listview.item`, `.selected`, `.focused.selected` (focus+selection distinct — the classic two-state rendering), multi mode prefixes `[x]`/`[ ]`.

Keys: Up/Down move selection (auto-scroll to keep visible — `ensureVisible(i)` = `scrollTo` clamp math); PageUp/PageDown by viewport; Home/End; Space toggles check (multi); Enter fires `on:activate(int)`; selection change fires `on:select(int)`. Mouse: Press selects row under pointer; double-press = activate (double = two presses within 400ms on the same row — the loop's clock via T09; documented constant).

`refresh()`: re-clamps `selected`/`scrollY` to the new count, invalidates.

| attribute | expands to |
|---|---|
| `items` | `items(Array<string>)` sugar |
| `selected`, `multi` | setters |

### 3.6 TableView — `class TableView : Focusable, Scrollable, Bordered`

State: `Array<TableColumn> columns_`; `ITableSource source_`; `int selectedRow = -1`; `bool sortable = false`; `int sortCol = -1`; `bool sortAsc = true`; header height 1.

```lev
struct TableColumn { string title; Constraint width; Align align; }
```

Column sizing reuses T02's flex distribution verbatim (columns are a one-axis flex problem — stated so the implementation SHARES the flex function, not a copy; the shared helper lives in T02's ownership and is called here).

Paint: header row (`tableview.header`, sort indicator `▲/▼` suffix on `sortCol`), then virtualized data rows exactly as ListView; per-cell truncation with ellipsis to column width; column separator `│` optional (`bool separators`).

Sorting is **delegated**: clicking a header (or `s` key cycling on the selected column v1: no — keys stay minimal: sorting is mouse + `on:sort` only, apps re-sort their source and call refresh) fires `on:sort(int col, bool asc)`; the view NEVER sorts data (it can't — it has a source, not rows). Sort indicator state is view-owned (`sortCol/sortAsc` set before the event fires). This keeps 100k-row sort where it belongs (the data layer) — the enterprise-correct division, stated loudly.

Keys: Up/Down/PageUp/PageDown/Home/End as ListView (row granularity); Left/Right scroll horizontally when total column width exceeds viewport (cell-wise, `scrollX`); Enter → `on:activate(int row)`. Events: `on:select(int row)`, `on:sort(int,bool)`, `on:activate(int)`.

| attribute | expands to |
|---|---|
| `columns` | `columns(Array<TableColumn>)` |
| `sortable`, `selected` | setters |

### 3.7 TreeView — `class TreeView : Focusable, Scrollable, Bordered`

State: `ITreeSource source_`; `Map<int,bool> expanded_`; `int selected = -1` (index into the FLATTENED visible list); `Array<TreeRow> flat_` cache.

```lev
struct TreeRow { TreeNodeId node; int depth; bool expandable; bool expanded; }
```

The flatten cache is the design center: visible rows = pre-order walk of expanded nodes, rebuilt on expand/collapse/refresh (`rebuildFlat()`), O(visible). Selection/scroll index into `flat_`. This turns TreeView into "ListView over flat_" — paint and key handling are shared shape with ListView (again: reuse, not copy — the shared row-scroller logic factored into a package-internal helper both use; the helper is this track's file, named `RowScroller`, spec'd inline).

Rendering per row: `depth * indent` spaces + expander glyph (`▸`/`▾`/` `) + label; theme `treeview.item(.selected/.focused.selected)`, `treeview.expander`.

Keys: Up/Down/PageUp/PageDown/Home/End on flat rows; Right expands (or moves to first child if already expanded); Left collapses (or jumps to parent); Enter → `on:activate(TreeNodeId)`; Space toggles expand. Events: `on:select(TreeNodeId)`, `on:expand(TreeNodeId, bool)` (fires BEFORE rebuild so lazy sources can populate — the lazy-load hook, spelled: handler loads children, returns, rebuild queries childCount fresh).

Recursion note: `rebuildFlat` is iterative with an explicit stack (`Array<TreeNodeId>`) — deep trees must not ride the call stack; stated as an implementation requirement.

### 3.8 BarMenu / MenuItem — `class BarMenu : Container, Focusable`, `class MenuItem : Focusable`

BarMenu: a horizontal top-level strip of menu titles; children are `MenuItem`s (leaf commands) or `Menu` groups:

```lev
class Menu : Container {  // a dropdown: children are MenuItem/Menu(submenu)/MenuSeparator
    string label;
}
class MenuItem : Focusable, ISingleLine {
    string label; string chord = "";        // display + auto-bind
    bool enabled = true;
    int onSelect(() => void h); ...
}
class MenuSeparator : Component { }
```

Open behavior: activating a BarMenu title pushes the dropdown as an **overlay** (R13: `Sonar::app().pushOverlay(panel)`) positioned under the title; submenus push further overlays to the right. Esc/click-outside pops (click-outside = a mouse press whose hit-test inside the top overlay fails → pop; the overlay-input-exclusivity rule R13 makes "outside" detectable at the App dispatch layer — T09 hook, named here as the `dismissOnOutsidePress` overlay flag, part of `pushOverlay`'s v1.1... **kept v1**: `App.pushOverlay(IComponent c, bool dismissOnOutsidePress = false)` — an anchor-compatible default-parameter extension, flagged for the anchor log).

Keys while open: Up/Down navigate items (skipping separators/disabled), Right/Left enter/leave submenu, Enter activates (fires the item's `on:select`, pops ALL menu overlays), chord text renders right-aligned (`menu.chord` theme key). The chord itself is bound app-wide via T07's `@Shortcut` or manual keymap — MenuItem's `chord` field is display + optional auto-bind: `autoBind(true)` registers `keymap().bind(chord, ...)` on attach, cancels on detach (R7/lifecycle discipline).

Focus: opening saves focus, closing restores (rides R13's built-in save/restore). BarMenu itself is one tab stop; F10 as the conventional activation is an app-level bind, not built in (documented).

Theme: `menu.item(.focused/.disabled)`, `menu.title(.focused)`, `menu.separator`, `menu.chord`.

### 3.9 Modal — `class Modal : Container, Focusable, Bordered`

State: `string dismissKey = "Escape"` (chord string); size via ordinary constraints (centered by the overlay layer: the overlay stack arranges each overlay via StackLayout + a centering wrapper — design: `pushOverlay` wraps non-App-sized overlays in an internal `OverlayHost : Container` that centers per the child's desired size; spec'd here since Modal is the consumer).

Behavior: `open()` = `Sonar::app().pushOverlay(this)`; `close()` = pop + fire `on:dismiss`. Focus trap per R13/T03 (policy walks only the top overlay). Backdrop: OverlayHost paints a dim scrim (`modal.scrim` theme key: bg-only style painted over the whole screen — cell-grain damage keeps the cost one full repaint on open/close, fine). dismissKey handled in the Modal's own key handling (capture at its subtree root). **Detach discipline (R7): `close()` pops the overlay AND the OverlayHost clears its child — both parent links nulled, `onDetach` fires through the subtree** — the leak-critical path, tested explicitly.

Convenience statics-shaped API (namespace functions, no statics in the language): `Sonar::alert(string title, string msg)`, `Sonar::confirm(string title, string msg) -> Promise<bool>` — Promise-shaped dialogs riding the language's tasks (`await Sonar::confirm(...)` in an event handler suspends that task, the loop keeps running — the async showcase; resolve on button press). Difficulty flag: promise-resolving UI needs a resolve callback plumbed into Button handlers — straightforward closure, shown in a worked example.

### 3.10 DebugOverlay — `class DebugOverlay : Component`

Renders the `Sonar::log` ring (T09 owns the ring; this reads it) + frame stats (last frame ms, damage rect count, cells diffed — counters T09 exposes via `Sonar::frameStats()`, a struct); toggled by `App.debugOverlay(bool)` which pushes/pops it as a non-modal overlay (input passes through — R13 exclusivity applies only to the TOP overlay... conflict: DebugOverlay on top would eat input. Resolution: `pushOverlay(c, dismissOnOutsidePress, bool inputTransparent = false)` — a second flagged anchor extension: input-transparent overlays are skipped by dispatch's "top overlay" selection. Logged for the anchor.)

## 4. Milestones

| M | contents | difficulty | gates |
|---|---|---|---|
| M1 | ContentBox + GridBox (+ OverlayHost substrate) | M | T01/T02 |
| M2 | Tabs + SplitBox | M | M1 |
| M3 | RowScroller helper + ListView | M | T03 |
| M4 | TableView + TreeView | L | M3 |
| M5 | BarMenu/Menu/MenuItem + Modal (+ alert/confirm) | L | M1, R13 plumbing in T09 |
| M6 | DebugOverlay + refresh/soak passes | S | T09 stats |

## 5. Potential issues & mitigations

1. **ContentBox scroll = re-arrange subtree** (translation via arrange). Cost O(subtree) per scroll tick. Mitigation now: arrange is cheap integer math; mitigation v2: a Surface origin-offset register making scroll pure-paint. Decision recorded, revisit on profiling.
2. **Keyboard-scrolling a ContentBox with no focusable children** — unreachable by tab. Accepted v1; workaround documented (app keymap binds); v2: `focusableScroll(true)` making it a tab stop.
3. **Hidden tab panes accumulating damage** — a background tab's timer-driven children keep invalidating. Mitigation: Tabs' paint skip absorbs it (cell-grain diff sees no change), but the layout pass may still churn — Tabs marks non-selected subtrees `visible=false` (skipped by layout AND paint per C7) on selection change, restores on select. This is the normative mechanism, not paint-skip.
4. **Source mutation without `refresh()`** — stale counts crash row math. Mitigation: every paint re-reads `count()` and clamps scroll/selection defensively (cheap), so the worst case is stale visuals, never OOB throws.
5. **Overlay API extensions** (dismissOnOutsidePress, inputTransparent) extend anchor C9's `pushOverlay` — compatible (default params), flagged in the anchor log rather than silently diverging.
6. **Double-click detection needs a clock** — T09 must expose `Sonar::nowMs()` (loop time). Flagged as a T09 dependency line item.
7. **Deep menu chains** — each submenu is an overlay; popping ALL on activate must walk correctly. Mitigation: menu overlays tagged (OverlayHost carries an `int group` id); `popOverlayGroup(group)`.
8. **TreeNodeId collisions** — sources own id uniqueness; the view Maps by `id`. Documented contract; a colliding source corrupts expansion state only (never crashes).

## 6. Testing plan

Snapshot goldens per component per state (T10 TestRenderer): ContentBox scroll positions + scrollbar thumb math; SplitBox ratios + min clamps + drag scripts; Tabs strip overflow + selection; ListView/TableView/TreeView virtualization windows (a 10k synthetic source, assert `itemAt` call count == viewport rows via a counting test source — the virtualization proof), selection/scroll key scripts, sort event firing, lazy tree expansion; menu open/navigate/activate scripts incl. focus save/restore assertion; Modal detach-discipline test (open/close 1000× — parent links all None, via a probe walking the detached subtree); alert/confirm promise resolution. All differential across oracle/IR/LLVM.

## 7. Open questions

1. Shift-range multi-select in ListView (anchorIdx reserved) — v1.1.
2. Column resize by mouse in TableView — v1.1 (needs per-column drag targets like SplitBox).
3. Sticky/frozen columns — v2.

## 8. Implementation log

- 2026-07-12 — design written; not started.
- 2026-07-13 — **M1–M4 landed in full** (`ContentBox`/`GridBox`/`SplitBox`/
  `Tabs`/`ListView`/`TableView`/`TreeView`, plus the `IListSource`/
  `ITableSource`/`ITreeSource`/`TreeNodeId`/`ArrayListSource`/`RowScroller`
  substrate from §2/§3.5, all in `sonar/src/components/{contentbox,gridbox,
  splitbox,tabs,listview,tableview,treeview}.lev`). Verified: `ContentBox`/
  `GridBox`/`SplitBox`/`Tabs` byte-identical on oracle+`--ir`; `ListView`/
  `TableView`/`TreeView` byte-identical on oracle+`--ir`+LLVM. Full
  regression across `sonar/tests/{contentbox_gridbox,splitbox_tabs,listview,
  tableview_treeview,components,core,layout,events}` passes. **M5**
  (`BarMenu`/`MenuItem`/`Modal` + `Sonar::alert`/`confirm`) and **M6**
  (`DebugOverlay`) were **not implemented** — not an oversight, this
  matches the milestone table's own pre-existing gate ("M1, R13 plumbing in
  T09" / "T09 stats"): `App`, `App.pushOverlay`/`popOverlay`, `Sonar::app()`,
  `Sonar::frameStats()` do not exist anywhere in `sonar/src` (T09,
  `techdesign-09-runloop-terminal.md`, is itself unimplemented). Building
  Modal/BarMenu/DebugOverlay per spec requires that machinery; inventing a
  local stand-in inside this track's files would both diverge from the
  spec's actual contract and collide with T09's own file ownership when it
  eventually lands, so this doc is deliberately **not** moved to
  `designs/complete/` — 3 of its 10 owned files remain unimplemented,
  blocked on an external, not-yet-built dependency, not on anything this
  track could resolve itself. `OverlayHost` (the `Modal`-adjacent
  centering substrate the milestone table lists under M1) was likewise
  deliberately deferred alongside M5 rather than built speculatively: it
  exists solely to serve `pushOverlay`, and building it now would mean
  guessing at integration details (paint order relative to the root, hit-
  test priority) that are properly T09's to define. One design-prose
  concern resolved cleanly during implementation: §3.5's "double-press ...
  the loop's clock via T09" turned out not to be a T09 dependency at all —
  `std::sysNow()` already exists as a plain language native and is what
  `ListView`/`TableView` actually use.

  This track's implementation surfaced a serious, previously-latent
  compiler defect family in ordinary multi-mixin dispatch — not a backend
  quirk, confirmed to reproduce on the oracle and `--ir` (not just LLVM)
  for its core manifestation. Filed in `/bug.md` with full repros, all
  independently re-verified against real builds before being trusted. Two
  of the four dispatch-family findings were **fixed at the source during
  this track's own merge with `origin/master`** (commit `028ec6a`, landed
  concurrently and independently by another agent, re-verified against the
  rebuilt compiler rather than trusted from the commit message alone): a
  qualified `this.Base::m()` base-call that ran against a detached/default
  receiver (silently wrong on oracle/`--ir`, segfaulted on LLVM — the
  documented "reuse a base implementation" idiom is now usable), and an
  unqualified call to an overridden method, from within a method declared
  on an *intermediate* mixin, that resolved against the declaring class's
  own chain instead of the runtime object's. This track's in-package
  workarounds for both (inlining base-method logic instead of qualifying;
  redeclaring the affected method on the leaf) are left in place —
  redundant but harmless, not reverted, per the "never revert validated
  work" convention. **Still open** (re-verified against the rebuilt,
  post-fix compiler — the source fix did NOT cover this variant):
  `Container.paint()`'s own inherited children-loop — a `for`-loop over an
  `Array<IComponent>` field, not a same-class method call — silently
  paints nothing at all for *any* leaf composing `Container` with a second
  mixin, reproducing on the oracle and `--ir` too; every `Container`-based
  leaf in this track (`ContentBox`/`GridBox`/`SplitBox`) therefore still
  needs its `paint()` redeclaration, which remains load-bearing, not
  merely defensive. Confirmed-safe patterns used throughout: a method
  declared on the root `Component` calling a leaf hook (`measure()` →
  `contentDesired()`, `paint()` → `paintContent()`/`paintChrome()`) is
  unaffected, and a method declared directly on the leaf calling an
  inherited override is unaffected — every `Scrollable`+`Bordered` leaf
  without `Container` (`ListView`/`TableView`/`TreeView`) redeclares
  `scrollTo` alone (the one `Scrollable` method that calls `this.chrome()`
  internally) and otherwise uses plain root-template leaf hooks. Also
  filed, all still open: `Bordered.paintChrome` indexing border glyphs by
  byte offset instead of character index (throws on every real border
  style; fixed by decoding via `.chars()` in every leaf that paints a
  border), an LLVM-only `Container.paint()` child-loop-inside-a-clip that
  renders text blank (the loop runs but its writes don't survive — a
  different symptom from the still-open children-loop bug above, where the
  loop body never runs at all), a char literal used directly as a ternary
  branch evaluating wrong once it reaches a native call argument (bind to
  a local and use plain `if`/`else` instead), `Surface.pushClip()`
  followed by 2+ `put`/`fill` calls segfaulting on LLVM (avoided
  throughout by never calling `pushClip` in any of this track's own
  `paintContent` bodies), and an LLVM-only `Array<struct>` whose element
  struct has a *nested*-struct field corrupting once a *different*,
  unrelated `Rect`-typed field on the housing object is written afterward
  (hits every `Component` subclass that caches such an array, since `box`
  gets written by every real layout pass; `TableColumn`/`TreeRow` are
  declared as `class`, not the design's literal `struct` spelling, as the
  confirmed fix, matching the existing `Chord`/bug-#41 precedent — a
  distinct trigger from #41's own enum-field shape, not fixed by the same
  commit that fixed #41). None of the still-open bugs were fixed at the
  source (`sonar/src/{container,mixins}.lev`, `src/**`, `runtime/**` — all
  out of this track's file-ownership scope); all are worked around
  in-package. `distinct`-qualified member access is unaffected throughout.
  `bug.md`'s numbering churned twice during this track — once to resolve a
  collision with an unrelated bug already landed on `origin/master`, and
  again during the final merge for the same reason plus the two now-fixed
  entries being retired — see `bug.md`'s current numbering for the live
  set; entry numbers cited earlier in this log's own T05 milestone
  sections above are stale pointers, superseded by this paragraph.
- 2026-07-14 — **M5 + M6 landed in full — track COMPLETE** (`OverlayHost`,
  `Modal`, `Sonar::alert`/`Sonar::confirm` in `components/modal.lev`;
  `MenuSeparator`/`MenuItem`/`Menu`/`BarMenu` in `components/menu.lev`;
  `DebugOverlay` in `components/debugoverlay.lev`), now that **T09 has landed**
  and the milestone-table gate ("M1, R13 plumbing in T09" / "T09 stats") is
  satisfied. The T09 overlay stack was extended with the deferred-here R13
  surface it always anticipated (all additive, default-param, flagged per §5
  issue 5): `pushOverlay(c, dismissOnOutsidePress, inputTransparent, group)`,
  `popOverlayGroup`, `popOverlayComponent`, `topInputOverlay`, `newOverlayGroup`,
  R13 focus save/restore on push/pop, and `__sonarDetachTree` (the R7 recursive
  subtree teardown the leak-critical close paths ride). `App.debugOverlay(bool)`
  now wires the real `DebugOverlay` as an input-transparent overlay. The mouse
  dispatch's `outsidePress`/`overlayInputTransparent` seams (already present in
  T03's `events.lev`, built for exactly this) are now consumed by
  `App.onMouseEvent`.

  **Verified byte-identical on oracle, IR, AND LLVM** across
  `sonar/tests/{modal,menu,debugoverlay}` — the full LLVM lane, not just
  oracle/IR: `alert`/`confirm` open+close, `confirm` Promise resolution (Yes →
  true, Esc → false, resolved-once guard), the **1000× open/close detach soak
  (zero overlay/parent-link leaks)** proving R7, menu open/navigate (Down skips
  a separator)/activate (fires select, pops the group, restores focus)/Esc
  dismiss/click-outside dismiss/click-inside activate, DebugOverlay stats+ring
  paint and input-transparent push/pop. Direct component paint is LLVM-clean
  because these paint bodies take **no `pushClip`** (children are arranged
  strictly inside content); at write time overlay paint *through the live loop*
  (`App.paintOverlays`' own `pushClip`) was believed to remain oracle/IR-only
  on LLVM (the ContentBox/Tabs precedent) — **superseded same-day, see the
  2026-07-14 entry below.**

  **The M1–M4 #65 finding, deepened to its root and now the load-bearing
  workaround for M5/M6.** M1–M4 filed #65 as "the inherited `Container.paint()`
  children-loop silently paints nothing for a multi-mixin leaf." Building the
  Container-based overlays here isolated the actual mechanism: **every method
  `Container` overrides from `Component` — not just `paint`, but
  `__sonarChildren`, `contentDesired`, `arrange`, `__sonarContentRect` — resolves
  to `Component`'s base version for a leaf composing `Container` with any second
  mixin.** The `__sonarChildren` case is the newly consequential one: it silently
  returns `[]`, so **every framework tree-walk (focus, hit-test, damage sweep,
  and the overlay detach that R7 depends on) sees no children** — a bare
  `Container` leaf would leak its entire subtree on every overlay pop. The
  retiring fix is a resolver MI-vtable defect out of package scope (`src/**`);
  `known_bugs_1.md` #65 was deepened with this finding. Every `Container` leaf in
  M5/M6 therefore redeclares the whole override set forwarding to the WORKING
  `children()`/`layout()` accessors (Container-*new* methods with no `Component`
  base to mis-pick, so they resolve correctly). Also carried, all still-open,
  all worked around in-package: #59 (`Bordered.paintChrome` byte-indexing —
  reimplemented via `.chars()`), #61 (`contentRect()`'s `this.chrome()` from an
  intermediate mixin — redeclared on the leaf), #53 (explicit lambda receivers),
  and `is`-narrowing an indexed element requires binding it to a local first.
  No `src/**` file was touched; all of M5/M6 is package-level. `OverlayHost`
  (the M1-table centering substrate) was built here alongside M5 rather than
  under M1, since it exists only to serve `pushOverlay` and its integration
  details (paint order, hit-test) are T09's, which had to land first.

  **Concurrent-fix note (post-merge, same day):** during the final sync,
  `origin/master` landed source fixes for the entire multi-mixin dispatch family
  this track worked around — #65 (`054e159`, "diamond override must beat a
  base-inherited copy in member collapse"), #64, #59, #61, and #41/#40/#49/#36.
  The redeclaration + `.chars()`-paintChrome + no-`pushClip` workarounds in
  `modal.lev`/`menu.lev` are therefore now **redundant but retained**
  (never-revert-validated-work): re-verified byte-identical on oracle/IR/LLVM
  against the rebuilt post-fix compiler. A future Container leaf likely no longer
  needs the full set — verify before relying on either state.

- 2026-07-14 (later same day) — **`ContentBox`/`GridBox`/`SplitBox`/`Tabs`
  (M1–M4) are now ALSO byte-identical on LLVM**, correcting the M1–M4 log
  entry's "oracle+`--ir`" framing and the M5/M6 entry directly above: the
  `App.paintOverlays`-style live-loop `pushClip` path was re-tested (`App`,
  real `pumpOnce()`, `Sonar::alert` rendered end-to-end) and paints correctly,
  and `sonar/tests/contentbox_gridbox` now passes on `--build-native` too (was
  previously the one documented LLVM DIFF in the whole Sonar suite). Root
  cause: the `Surface.put`-after-`pushClip` LLVM segfault this track filed as
  `known_bugs_1.md #70` (a value-struct field read through a boxed
  `Array<Rect>` element dereferencing a garbage payload) was already fixed —
  hours *before* it was even filed — by `904bcbd` ("bug.md #41: route
  non-flat value structs through the boxed ARC path"), part of the same
  concurrent-fix wave noted above; the filing was against a stale
  `build/liblvrt.a` (the runtime archive CMake target, `lvrt`, hadn't been
  rebuilt after merging that commit — only `leviathan`/`trident` had).
  Confirmed by an isolated `git worktree` build pinned to `904bcbd` alone
  (predating both the filing and any later commit): a self-contained repro
  passes clean, 3/3 runs. `#70` is retired from `known_bugs_1.md` accordingly.
  Unrelated: a *different* bug, independently found the same day and also
  numbered `#70` by its author (a namespace-shadowed dynamic-dispatch defect
  in `Lower.cpp`'s call lowering, `commit c2ec027`) is a real, separate,
  correctly-fixed defect — its "`bug #70` fixed" commit message is a numbering
  collision with this track's filing, not a duplicate report of the same bug.
  This track's redeclaration/no-`pushClip` workarounds remain **redundant but
  retained** throughout (never-revert-validated-work) — the note two entries
  above already covers that.
