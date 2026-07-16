# Sonar — Tech Design 02: Layout (Constraints + Flex/Grid/Dock/Stack)

**Status:** design, pre-implementation. **Date:** 2026-07-12. **Track:** T02.
**Owns:** `sonar/src/layout/{flex,grid,dock,stack}.lev`.
**Depends on:** T01 (Constraint/clampAxis, Component layout fields, damage). No language gates.
**Gates:** G-S1. **Difficulty:** M. **Risk:** LOW/MED (the constrained-flex fixpoint and the wrap re-measure protocol are the two subtle spots).

Implements anchor C5 `ILayoutStrategy` (`Size measure(Array<IComponent> kids, Size avail)` / `void arrange(Array<IComponent> kids, Rect content)`); consumes C2 Constraint, C7 layout fields (`dock`, grid fields, per-axis constraints, `visible()`), C3 enums (Axis/Dock/Align/Overflow). All math is integer cell math; determinism is a contract (same inputs ⇒ same boxes, all engines).

---

## 1. The two-phase protocol, precise

- App's layout phase (T09) collects highest `layoutDirty_` roots (T01 damage services) and runs `measure(availFromCurrentBox)` then `arrange(currentBox)` per root.
- `avail` semantics: an axis of `Sonar::Unbounded` = unconstrained (Scrollable content measure).
- **Desired may exceed avail.** A child's `desired()` is its honest preference; the PARENT clamps at arrange. This contract is what makes scrollbars possible (ContentBox reads the overflow) — pinned here, cited by T05.
- **Overflow boxes are legal.** When mins don't fit, children keep their mins and boxes may extend past the content rect; paint clipping (T01) handles visibility. Layout never emits negative sizes (floor at 0).
- **Wrap protocol (width-then-height):** a wrapping child's height depends on its assigned width. FlexLayout's measure is two-pass when the cross axis is vertical-with-wrappers: pass 1 measures at `avail`; pass 2, after main-axis widths are resolved, re-measures ONLY children whose `contentDesired` differs under the assigned width (Text advertises this by returning different heights for different widths — no flag needed; the strategy re-measures flexed children at their final main size when cross is unbounded-or-fills). Cost bounded: ≤2 measures per child per pass.

## 2. FlexLayout

`FlexLayout(Axis a)`; `gap(int)`; `align(Align)` (cross axis; default Stretch). No justify in v1 (children pack from Start; flex absorbs slack) — v2 note.

**measure(kids, avail):** sum of children's clamped main-axis desired + gaps between VISIBLE children; cross = max child cross desired. Returns that as the container's desired content size.

**arrange(kids, content):**
1. Partition visible children: concrete (main-axis `isConcrete()`) vs flexible (`flex > 0`) vs rigid (flex 0, not concrete → they take clamped desired).
2. `fixed = Σ concrete + Σ rigid-clamped-desired + gaps`; `remaining = max(0, content.main − fixed)`.
3. Distribute `remaining` by weights with the **left-to-right remainder rule**: `base_i = remaining * flex_i / totalFlex` (integer division); leftover `remaining − Σ base` cells go +1 each to the first `leftover` flex children in order. Worked example (normative test): 10 cells, weights [1,1,1] → [4,3,3].
4. **Clamp fixpoint:** clamp each flex share to its min/max; freed/needed cells re-distribute among still-unclamped flex children by the same rule; repeat. Terminates in ≤ N iterations (each iteration permanently pins ≥1 child at a bound or reaches stability — the standard argument, stated in the doc so nobody "simplifies" it).
5. Walk the main axis assigning boxes (gaps between visible children only — hidden children consume neither space nor gaps); cross axis per Align: Start/Center/End use the child's clamped cross desired; Stretch assigns content cross clamped to the child's max.
6. `totalFlex == 0` with remaining > 0: slack stays empty at the End.

## 3. GridLayout

`GridLayout(int rows, int cols)`; `rowConstraint(i, c)` / `colConstraint(i, c)` (default `Constraint::Flex(1)`); `gap(int)`.

- **Track sizing:** concrete tracks take their cells; flex tracks share the remainder by the same L-to-R rule (reuse the flex distribution function — one implementation, exported from `flex.lev`). `auto` (content-sized) tracks are an **explicit v2 deferral** (need per-track measure passes; sketch: max of spanning children's desired apportioned across tracks) — v1 tracks are Fixed/Flex/Bounded only.
- **Placement:** children by `gridRow/gridCol` + spans; a spanning child's cell = the covered tracks + interior gaps. Out-of-range indices clamp with a `Sonar::log` warning (loud, not fatal — templates make static mistakes visible; runtime clamping beats a crash in a resize storm). Overlapping children: both painted, later wins visually — not an error (documented).
- measure: Σ track constraints' clamped naturals + gaps per axis.

## 4. DockLayout

The shrinking-rect walk, in child order: Top/Bottom docks measure the child (cross = current rect's width) and slice its clamped main-axis desired off that edge; Left/Right likewise for columns; `Dock::Fill` children receive the remaining rect — multiple Fill children all get the SAME remaining rect (overlay; use StackLayout semantics knowingly or don't — documented choice, keeps the walk one-pass). Classic shape: menubar Top, statusbar Bottom, sidebar Left, editor Fill. measure: sum of edge slices + max fill desired (approximation documented: docks are usually the screen root where desired is moot).

## 5. StackLayout

All visible children measure with the content size and arrange to the full content rect. Z = child order (paint order, T01). The overlay/Modal substrate (R13 rides it via OverlayHost — T05).

## 6. Cross-cutting rules

- Strategies READ component layout fields and CALL measure/arrange — they never mutate components otherwise (invariant).
- **No invalidation from inside layout**: measure/arrange implementations must not call setters that invalidate; T09's in-frame assertion catches violations in debug mode.
- Allocation honesty: the arrange pass builds small local arrays (partitions/shares) — pure-array rebinds at layout frequency are fine; no tree copies.

## 7. Milestones

| M | contents | difficulty |
|---|---|---|
| M1 | flex.lev: distribution fn (shared), FlexLayout complete + fixpoint | M |
| M2 | dock.lev + stack.lev | S |
| M3 | grid.lev (tracks/spans/clamping over the shared distribution fn) | M |
| M4 | wrap re-measure protocol + Text integration test (with T04) | M |

## 8. Potential issues & mitigations

1. **Fixpoint non-termination** — the ≤N argument (§2.4) is part of the doc; the implementation asserts iteration ≤ childCount and throws SonarException past it (a bug, made loud).
2. **Wrap re-measure O(n²) in deep trees** — re-measure only fires when cross is unbounded/stretch AND the child's width changed from pass 1; bounded, profiled in the dashboard example.
3. **Gap accounting with hidden children** — gaps collapse with their child (decided §2.5); `Fixed(0)`-sized visible children DO take a gap slot (visibility, not size, controls gaps) — pinned by test.
4. **Struct-copy no-op** in strategy code (mutating a copied Constraint) — the T01-named classic; strategies treat Constraints as read-only inputs.
5. **Span off-by-ones** (covered tracks + interior gaps) — the 3×2-grid-with-2-col-span worked example is a normative test.

## 9. Testing plan

Table-driven per strategy: constraint sets → expected boxes, including every worked example above; property checks (Σ flex boxes + gaps == content when unclamped; no negative sizes; repeat-run determinism); degenerate tables (zero children, zero content, min-sum > avail overflow boxes, totalFlex 0); grid clamp/overlap/span cases; dock multi-fill; stack z; wrap two-pass with a fake wrapping child (heights = f(width) stub). All via T10 harness, differential oracle/IR/LLVM.

## 10. Open questions

1. `justify` (main-axis distribution: Center/End/SpaceBetween) — v1.1, additive.
2. Auto grid tracks — v2 (sketch in §3).

## 11. Implementation log

- 2026-07-12 — design written; not started.
- 2026-07-12 — M1–M4 implemented in full: `sonar/src/layout/{flex,grid,dock,stack}.lev`.
  `FlexLayout` (§2, the shared `distributeFlexShares` clamp-fixpoint helper +
  the §2.3 [4,3,3] worked example + the wrap re-measure protocol),
  `GridLayout` (§3, tracks via the shared distribution fn, span placement,
  clamp-with-loud-warning intent — see the `Sonar::log` note below),
  `DockLayout` (§4, shrinking-rect walk, multi-Fill-shares-one-rect), and
  `StackLayout` (§5) are all present and match the design's algorithms and
  worked examples exactly (hand-verified — see the test suite). `trident.toml`
  gained `"src/layout/*.lev"` per anchor C13.
- 2026-07-12 — completed the handoff T01 anticipated in its own log
  (`container.lev`'s comment: "T02 replaces this bootstrap-equivalent with
  FlexLayout(Axis::Vertical)"): `Container`'s default `layout_` is now
  `FlexLayout(Axis::Vertical)` and the bootstrap `CoreVerticalLayout` class
  is removed. This is a one-line edit to a T01-owned file completing a
  documented, anticipated integration point, not a scope violation — T02 is
  the track that supplies the type T01's comment names.
- 2026-07-12 — wrap re-measure protocol (§1, §8.2): implemented as
  "re-measure a child (at `Size(finalWidth, Unbounded)`) iff its final
  main-axis size differs from its pass-1 `desired()`, and only on
  `Axis::Horizontal`" — this is §8.2's own literal trigger ("the child's
  width changed from pass 1"), applied uniformly to concrete/rigid/flex
  children rather than restricted to "flexed children" (§1's looser prose)
  — concrete children are additionally given their exact final width on the
  *first* measure pass (§2 `measure()` passes `Size(concrete, avail.cross)`
  instead of the raw avail), so they never need a second pass at all. No
  cross-call state is kept on the `FlexLayout` instance: `arrange()` reads
  `child.desired()` at its own top, before doing any flex math, which is
  still pass-1's value because nothing re-measures between the container's
  `measure()` and `arrange()` calls in the same frame (§1's two-phase
  contract). Bound: ≤2 measures/child/frame, matching §1's stated cost bound.
  Verified against a stub wrapping component (`WrapProbe` in the test
  suite, height = f(width)) exactly as §9 prescribes ("fake wrapping child");
  the **Text integration test** half of M4 is out of scope until T04 exists.
- 2026-07-12 — **found and worked around a real compiler bug, not a design
  gap**: reading `Component`-only fields (`dock`, `gridRow`, `gridCol`,
  `gridRowSpan`, `gridColSpan`) off an `IComponent`-typed value via
  `if (c is Component) return c.dock; ...` — needed because those fields
  live on `Component` (C7) but are not part of the `IComponent` (C5)
  interface contract — is unanimously correct on oracle/IR/LLVM but silently
  evaluates the `is` test to `false` on the emit-C++ backend for this
  specific class shape (verified: a smaller synthetic interface/class of
  similar size does NOT reproduce it; not root-caused). Filed as `bug.md`
  **#36 [P1]** (silent wrong value on an actively-maintained engine, no
  diagnostic). Rather than accept emit-C++ coverage loss, worked around by
  promoting those five fields to field **requirements** on `IComponent`
  itself (`sonar/src/component.lev`) — already satisfied verbatim by
  `Component`'s existing declarations (§8's "the implementing class's
  declaration is the first and only instance" rule), so `Component` needed
  zero changes — and reading them directly with no `is` test anywhere in
  `dock.lev`/`grid.lev`. This is a one-line-per-field, additive, backward-
  compatible extension to a T01-owned interface, not a redesign; it sidesteps
  bug #36 entirely rather than routing around it per-call-site. All eight
  engine × suite combinations (oracle/IR/emit-cpp/LLVM × `core`/`layout`)
  pass after the fix.
- 2026-07-12 — one further contract gap found, resolved as an interim
  matching T01's own precedent (its log's two "contract contradictions"):
  §3's "Out-of-range indices clamp with a `Sonar::log` warning" names a
  function that does not exist yet — `Sonar::log`/the ring buffer is T09's
  deliverable (R16), and T09 has not landed. `GridLayout` clamps
  out-of-range `gridRow`/`gridCol` correctly (silently, no warning) as an
  interim; the warning call itself is left for whichever track lands
  `Sonar::log` to wire in, since T02 does not own that file.
- 2026-07-12 — test suite: `sonar/tests/layout/{layout.lev,layout.expected,
  trident.toml}`, direct `ILayoutStrategy.measure()`/`.arrange()` calls
  (table-driven per §9) covering every worked example in this doc (the
  [4,3,3] distribution, the clamp fixpoint, the dock classic shape, the
  3×2-grid-with-2-col-span), plus overflow boxes, `totalFlex == 0` slack,
  measure() sum+gaps/cross=max, all four `Align` modes (including a
  genuine Stretch-fills case, not just a Fixed-clamped one), the wrap
  protocol, multi-Fill-shares-one-rect, dock overflow floored at 0,
  out-of-range grid clamping, grid cell overlap (both arranged, no error),
  a Fixed+Flex mixed grid track, repeat-run determinism, and hidden-child
  gap/space exclusion. Every value hand-computed and cross-checked against
  the implementation. Passes byte-identical on oracle, IR, emit-C++, and
  LLVM (`sonar/tests/core` re-verified on all four engines too, since
  `component.lev`/`container.lev` changed).
