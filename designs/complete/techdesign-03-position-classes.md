# Sonar DOM — Tech Design 03: Position, Classes, DOM Containers, and the Leaf-Paint Fix

**Status:** design, pre-implementation. **Date:** 2026-07-15. **Track:** D03.
**Owns:** `sonar/src/dom/{position,containers,classes}.lev` **+ the suite's only edits to landed
files** (flagged in the anchor log): `sonar/src/component.lev` (three additive fields + class-list
API), `sonar/src/mixins.lev` (`Styleable.resolve` class steps; the `paintBackground` #4 fix).
**Depends on:** T01/T02 (arrange/paint templates, FlexLayout), T09 overlays (the popover route),
F5 weak fields. Probes: D-P1, D-P2, D-P9.
**Gates:** G-D2. **Difficulty:** L. **Risk:** MED/HIGH — this track touches landed paint/resolve
paths; the full differential re-run is part of its definition of done, not an afterthought.

Implements anchor D-C5 (Position), D-C6 (classes). Owns the Sonar bug #4 fix (§6).

---

## 1. Additive `Component` extensions (the flagged edits, exact shape)

```lev
// component.lev — additive block, zero-cost when DOM unused
public Array<string> classes_ = [];
public Position? position_ = None;         // lazily created by position()
public int domSlot_ = -1;                  // Document meta side-table index (D01)

bool hasClass(string name);
void addClass(string name);                // no-op if present; couples 'hidden' (§4.1); invalidate()
void removeClass(string name);             // couples 'hidden'; invalidate()
void toggleClass(string name);
Position position() {                      // lazy; owner back-ref via weak (D-P9)
    if (position_ == None) position_ = Position(this);
    return position_ ?? Position(this);    // narrowed return
}
```

Precedent: T07's pending arrays and T11's binding registry were the same kind of sanctioned additive
C7 extension. `setVisible` is NOT modified — the hidden-class coupling lives in the class API
(one-directional writes both facts; §4.1) so landed `setVisible` callers see no behavior change.

## 2. `Position` (`position.lev`)

```lev
enum PositionMethod { Flow, Absolute }
enum AnchorEdge { Below, Above, LeftOf, RightOf, Over }

class Position {
    new Position(IComponent owner);        // stores weak IComponent? owner_ (F5; D-P9)
    // the slot axis: stored desires
    PositionMethod method;                 // set-view: write → owner invalidateLayout
    set method(PositionMethod m) { method = m; this.pingLayout(); }
    // live reads, stored writes (the D-C5 ruling)
    get x() => int;                        // ALWAYS the live arranged box.x() — read-side truth
    set x(int v);                          // stores desiredX_, pingLayout(); consumed when Absolute
    get y() / set y(int v);                // same
    int w; int h;                          // -1 = unset; set-views map to Constraint::Fixed on owner
    int z = 0;                             // paint order among absolute siblings (stable sort)
    // the enhancement over manual math (resize-proof)
    void anchorTo(IComponent target, AnchorEdge edge, int dx = 0, int dy = 0);
    void clearAnchor();
    // engine-facing
    Rect __resolveAbsolute(Size desired);  // anchor→coords or stored x/y; screen-space
}
```

- **Reads are live** (`get x()` returns `owner.box.x()`): this is what makes
  `main.query('#file-menu').position.x` correct after layout, and it means reading a Flow
  component's position is the natural "where is it on screen" query.
- **Writes are desires**, consumed by DOM containers when `method == Absolute`. Writing x/y on a
  Flow component is legal and inert-until-Absolute (the sketch writes x/y after setting method —
  order-independent by design).
- Set-views ping the owner through the weak back-ref (`weak IComponent? owner_` — the F5 non-owning
  back-edge, no cycle with `position_`); a dead owner makes pings silent no-ops (D-P9 pins).
- `anchorTo` records a weak target + edge + offsets; `__resolveAbsolute` computes from the target's
  CURRENT box each arrange (Below: `(t.x + dx, t.bottom() + dy)` …), so anchored floats track
  resizes — the sketch's manual `.position.y = target.y + 1` still works, `anchorTo(target, Below)`
  is the idiomatic form. A dead/detached anchor target freezes at the last resolved coords + logs.
- Probe D-P2 gates the set-view spelling; fallback = plain `setX/setY/setMethod` methods (fidelity
  delta anchor-logged).

## 3. DOM containers (`containers.lev`)

```lev
class FlexContainer : Container {          // D-P1 gates plain composition
    new FlexContainer();                   // vertical
    new FlexContainer(Axis a);
    new FlexContainer(string markup);      // parse + buildFragment + add each (D02)
    FlexContainer axis(Axis a); FlexContainer gap(int n);
    DomNode query(string sel); DomNodeList queryAll(string sel); DomNode? queryOrNone(string sel);
    ActionRegistry actions;                // D04; field, created at ctor (the sketch's `.actions.add`)
}
class Bar : FlexContainer {                // <contentbar>: horizontal, height Fixed(1) default
    new Bar(); new Bar(string markup);
}
```

**Absolute-aware arrange (the mechanism, one place):** `FlexContainer.arrange` partitions children:
`flow = children where position_ == None || method == Flow` → delegated to `layout_` exactly as
Container does; `abs = the rest` → each measured (`measure(Size(w>=0?w:Unbounded, h>=0?h:Unbounded))`
honoring Fixed overrides) then `arrange(position().__resolveAbsolute(desired))` — **screen-space**,
not parent-relative (the sketch reads another subtree's screen coords and writes them here; one
coordinate system, no translation surprises). Paint: flow children first (landed order), then abs
children by `(z, child order)` — so floats draw over flow siblings. The parent's clip still applies
(T01 rule): a float positioned outside its parent's content rect clips — **documented boundary**:
floating UI belongs on the app root (the sketch adds `fileMenu` to `app` — correct by construction)
or on the overlay stack (§5). `measure` ignores abs children (they don't occupy flow space —
CSS `position:absolute` semantics).

`SonarApp` inherits this via its root being… App is `Container`; SonarApp (D01) overrides
`arrange`/`paint` with the same partition (qualified base calls for the flow half). Landed non-DOM
containers (`ContentBox`, `SplitBox`, …) ignore Position entirely — v1 boundary, anchor-logged.

## 4. Classes (`classes.lev` + the mixins edit)

### 4.1 Semantic classes

`hidden` ⇔ `!visible()`: `addClass("hidden")` calls `setVisible(false)` after the list write;
`removeClass("hidden")` → `setVisible(true)`; `DomNode.show()/hide()` are sugar over these. The
coupling is one-way (class API → visibility); a direct `setVisible(false)` does NOT edit the class
list (landed callers unchanged) — `DomNode.visible()` reads the component fact, not the list.
`disabled` is reserved-but-inert v1 (enabled stays the T04 field; unifying is an open question).

### 4.2 Class-qualified theming (the `Styleable.resolve` extension — flagged edit)

The landed 6-step fold gains class steps, frozen as:

```
1. Style()                                  (base)
2. theme "default"
3. theme componentBase(key)                 e.g. "input"
4. theme componentBase + "." + class        e.g. "input.error"      ← NEW, per class, in list order
5. theme keyMinusState(key)                 e.g. "input.text"
6. theme key                                e.g. "input.focused.text"
7. theme key + "." + class… → NO —          class-qualified FULL keys, most-specific:
   for each class c: theme key-with-class   e.g. "input.error.focused.text"  ← NEW
8. instance override (exact key)
```

Concretely: after step 3 the fold layers `base.class` per class; after step 6 it layers
`fullKeyWithClass(key, c)` per class (the class segment inserts after the component segment —
`input.error.focused.text`). Later classes in the list layer over earlier (last-added wins ties).
Cost: +2 probes per class per resolve; classes are rarely >2 — within the resolve cost note's
budget. Theme files just declare the dotted keys (`[input.error]` — the TOML subset already parses
them); no theme.lev changes. Built-ins gain nothing (no built-in class keys v1).

## 5. Floating UI: the two routes (normative table)

| route | mechanism | input exclusivity | dismiss-on-outside | the sketch |
|---|---|---|---|---|
| **manual float** | Absolute position + `hidden` class toggling | none — it's an ordinary child | none (app manages) | exactly this (fileMenu) |
| **popover** | `DomNode.popover(anchor, edge)` → `pushOverlay(c, true, false, newOverlayGroup())`, auto-positioned via the Position anchor machinery; `dismiss()` pops | yes (R13) | yes | the idiomatic upgrade; Menu.openAt's generalization |

`popover()` is this track's one new DomNode verb (declared here, wired in D01's ladder): it reuses
`Menu`'s stored-coordinate self-arrange precedent but generalized — the OverlayHost-less overlay
arranges at `__resolveAbsolute`. Modal/dialog semantics stay T05's (no outside-dismiss), per the
inventory's deliberate Menu/Modal distinction.

## 6. The leaf-paint fix (Sonar bug #4 — owned here)

**Ruling:** a leaf that repaints must first clear its box. `Styleable.paintBackground` becomes
unconditional: fill `box` with `' '` at the resolved `"background"` style (a `Default/Default/0`
resolve = clear-to-terminal-default — which IS the fix: stale glyphs from the previous, longer
content are overwritten). Opt-out: `bool paintsBackground_ = true` — set false by exactly two known
deliberate-transparency sites: `DebugOverlay` (paints over the live tree) and `OverlayHost` non-scrim
mode. StackLayout-style deliberate layering over siblings is the documented casualty (a transparent
leaf now clears its box) — sanctioned by the bug's own filed ruling ("a dirty() leaf repaint must
clear its whole box to the resolved background before painting content").

**Definition of done for this change alone:** the ENTIRE landed suite re-runs on all four engines
(`sonar/tests/**` + examples under `SONAR_SCRIPT=1`); goldens that legitimately change (snapshots
that previously showed inherited stale/parent pixels inside a leaf box) regenerate via `regen.sh`
with each diff reviewed and the file-manager fixed-width workaround retired; bug #4's workaround
is fully retired (no separate footgun registry to close a row in — `docs/footguns.md` was retired
2026-07-19).

## 7. Milestones

| M | contents | difficulty |
|---|---|---|
| M1 | probes D-P1/P2/P9 recorded; Component additive block + class-list API | S/M |
| M2 | Position (views, live reads, anchors, __resolveAbsolute) | M |
| M3 | FlexContainer/Bar (markup ctors deferred to D02 integration; absolute arrange+paint; z) | M |
| M4 | resolve class steps + theme corpus; semantic classes | M |
| M5 | the #4 paint fix + full differential re-run + golden regen + footguns row closure | M |
| M6 | popover route over the overlay stack | S/M |

## 8. Potential issues & mitigations

1. **This track edits landed paint/resolve paths.** Every edit ships behind the full four-engine
   differential re-run (M5 is its own milestone precisely so the re-run is scoped and reviewable);
   resolve changes are pure additions to the fold order (a theme with no class keys resolves
   byte-identically — pinned by a regression golden).
2. **#65-family recurrence (D-P1)** — FlexContainer is `Container`+nothing (single inheritance!) so
   the classic hazard shape doesn't even arise; `Bar : FlexContainer` likewise. The probe still runs
   (belt) and the menu.lev redeclaration fallback is named (suspenders).
3. **Struct-copy no-op on Position** — Position is a class precisely so `node.position().x = 5`
   mutates the real thing; the T01 §9.4 footgun cannot occur. Pinned by test.
4. **Weak owner races** (D-P9) — a Position outliving its component: pings no-op on dead weak reads;
   `__resolveAbsolute` never runs for a detached owner (arrange only reaches attached children).
5. **Screen-space coords vs. moved parents** — an absolute child of a moved FlexContainer stays at
   its screen coords (CSS-absolute-to-viewport, not to-parent). Documented loudly; anchorTo is the
   track-the-thing answer; parent-relative mode is an open question (v1.1 enum member `Relative`).
6. **Hidden-class one-way coupling confusion** — spelled in docs + a test asserting both directions
   of the DomNode API and the inertness of raw `setVisible` on the list.

## 9. Testing plan

Position matrix (method/x/y/w/h/z writes → arranged boxes, live reads after layout, anchor edges ×
resize scripts, dead-anchor freeze); absolute partition (flow siblings unaffected, measure excludes
abs, paint order by z, clip at parent edge); class API + hidden coupling + selector visibility;
resolve fold: class-key layering table incl. no-class byte-identity regression; the #4 fix: the
exact file-manager shrink repro (`"# Project readme"` → `"name = \"demo\""` leaves no `e`),
DebugOverlay transparency retained; popover open/dismiss/focus-restore. Differential
oracle/IR/LLVM + emit-C++ compile-only; full-suite re-run at M5.

## 10. Open questions

1. `Relative` (parent-relative) positioning — v1.1, additive enum member + one branch in
   `__resolveAbsolute`.
2. Unify `disabled` class with the T04 `enabled` convention — wants a pass over every input
   component; v1.1.
3. Per-class style memoization if the resolve cost note's budget is ever hit — the T08 memo design
   already sketches it; gated on a profile.

## 11. Implementation log

- 2026-07-15 — design written; not started.
- 2026-07-17 — **D03 IMPLEMENTED (M1–M6) in `sonar_v2/`** (same v2 package as D01/D02).
  New files `sonar_v2/src/dom/{position,containers,classes}.lev`; additive edits to
  `sonar_v2/src/component.lev` (`position_` now lazy `Position?`, `__sonarAbsolute`
  predicate), `sonar_v2/src/mixins.lev` (`Styleable.resolve` class steps + the `#4`
  `paintsBackground_` opt-out), plus `dom/{node,app}.lev` (popover verb; SonarApp
  absolute-aware root arrange + the base-ctor fix below). **Verified oracle/IR/LLVM
  byte-identical** (`sonar_v2/tests/dom-layout/`, golden `layout.expected`); emit-C++
  is the sanctioned async/native skip (App run loop). Existing `tests/dom` + `tests/markup`
  stay green.
  - **M2 `position.lev`** — full `Position`: `weak IComponent? owner_` back-edge (F5,
    **D-P9 green** — no cycle, dead-owner reads `None` and no-ops), get/set accessor views
    for `method/x/y/w/h/z`. **D-P2 GREEN, no fallback taken:** get/set accessors — including
    a COMPUTED getter with NO backing slot for the live `x`/`y` reads (`=> owner.box.x()`) —
    lower byte-identical on all three engines, so the sketch's `.position.x` (live read) /
    `.position.x = 7` (stored desire) spelling lands verbatim rather than dropping to
    `setX/setY`. Live reads return `box` origin; writes store desires consumed only when
    `Absolute`; `w/h` set-views map to `Constraint::Fixed`; `anchorTo`/`__resolveAbsolute`
    resolve from the target's live box (dead-anchor freeze at last coords).
  - **M1 `component.lev`** — `position_` is a lazy `Position?` (the real Position needs an
    owner ctor arg, so the D01 interim's eager `Position()` no longer type-checks);
    `position()` creates it with `this` as the weak owner on first touch. `__sonarAbsolute()`
    reads `position_` WITHOUT lazy-creating, so a flow child never allocates a Position.
  - **M3 `containers.lev`** — `FlexContainer : Container`, `Bar : FlexContainer` (**D-P1
    green**: single inheritance, the #65 shape never arises). The flow/absolute partition +
    absolute arrange + z-stable-sort are FREE FUNCTIONS (`flowOf/absOf/absByZOf/arrangeAbs`)
    so the SonarApp root reuses the identical mechanism — one implementation of D-C5's
    "honored by DOM containers AND the app root". `measure` ignores absolute children;
    markup ctors delegate to D02's `buildMarkupFragment`.
  - **M4 `classes.lev` + `mixins.lev`** — `classBaseKey`/`classFullKey` insert the class
    after the component segment; `Styleable.resolve` folds them in as steps 4 and 7 (most
    specific before the instance override). A theme with no class keys resolves
    byte-identically (misses are transparent no-op layers — pinned by the `plain`/`err`
    pair in the test). Semantic `hidden` coupling was already on Component (D01 interim);
    retained.
  - **M5 the `#4` leaf-paint fix** — the unconditional `paintBackground` box-clear was
    ALREADY landed in this package (inherited from `sonar/`; `docs/footguns.md`'s #4 debt is
    effectively closed and the full suite already green on it). D03 adds the design's
    `paintsBackground_ = true` opt-out gate; `DebugOverlay` and `OverlayHost` set it false.
    Behavior-neutral here — both already fully override `paint()` and never reach
    `paintBackground` — so the flag is the sanctioned switch for a FUTURE transparent leaf,
    not a live behavior change (no golden churn).
  - **M6 popover** — `DomNode.popover(anchor, edge)`/`dismiss()` over the overlay stack;
    `PopoverHost` (a `Container` overlay) self-arranges its child at `__resolveAbsolute`
    (the `Menu` stored-coordinate precedent, generalized). SonarApp gained the absolute-aware
    root `arrange`; root painting rides the landed damage-driven walk (`renderFrame` ->
    `paintDamaged`), so a root-`paint` override is unnecessary (root-float z-order is child
    order, v1).

  **Findings / forced deviations (all footgun-compliant, none a compiler STOP):**
  - **SonarApp (D01) never ran its base `App()` ctor.** Leviathan base ctors are EXPLICIT
    (`Base::Ctor()`, reference §4.5) — they do NOT auto-run — so D01's `new SonarApp()` (which
    assumed an auto-run "base initializer") left `currentApp`/`ILifecycleHost`/the single-app
    guard/the root FlexLayout all UNSET. Invisible to the D01/D02 tests (none call
    `Sonar::app()`), but D03's popover needs the overlay stack via `Sonar::app()`. Fixed by
    adding `App::App();` as SonarApp's first ctor statement; `Bar` likewise now runs
    `FlexContainer::FlexContainer()`. Worth a footgun-doc row (assumed-auto-run base ctors).
  - **Absolute layout needs measure-before-arrange.** Flow children collapse to origin if a
    container is `arrange`d without a prior `measure` (they carry `desired_ = 0`); the landed
    frame loop always measures first, and the test mirrors that. Absolute children are
    self-measured inside `arrangeAbs`, so they are unaffected.
  - **No accessor over a shared same-name slot for live `x`.** `get x()` is purely computed
    (delegates to a `liveX()` method) and `set x()` writes a distinct `desiredX_` — the two
    accessors deliberately share no backing slot, which is what lets reads be live while
    writes are stored.
- 2026-07-18 — **Anchor-edge coverage extended** in the `dom-layout` differential test
  (§9 "anchor edges × resize scripts"). The landed test asserted only `Below`, leaving
  every other `anchorX`/`anchorY` branch untested; the new `anchors` section pins all five
  `AnchorEdge`s (`Below/Above/LeftOf/RightOf/Over`) against a known-box absolute target, a
  target-move resize script (the floats track the move — the enhancement over the sketch's
  manual `.y = t.y + 1`), and `clearAnchor` (falls back to the stored desired coords).
  Golden regenerated; **oracle/IR/LLVM byte-identical**, full suite still green. Dead-anchor
  freeze (§8.4) stays out of the golden deliberately — it fires only when the `weak` target
  is collected, whose timing is not cross-engine deterministic, so a diff-based assertion
  would be flaky; the freeze branch remains covered by construction (`__resolveAbsolute`'s
  `else`).
