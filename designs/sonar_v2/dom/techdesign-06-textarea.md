# Sonar DOM — Tech Design 06: `TextArea` (the multi-line editor component)

**Status:** design, pre-implementation. **Date:** 2026-07-15. **Track:** D06.
**Owns:** `sonar/src/components/textarea.lev` (a general Sonar component — components dir, not dom/).
**Depends on:** T01 (mixins, damage), T03 (keys/paste), T04 Input (the cursor-math discipline this
doc inherits wholesale), T08 theming. Prior art absorbed: `examples/recon/src/ui/textarea.lev`
(inventoried — a working skeleton whose gaps this spec closes: no cell-width cursor math, no
auto-scroll, dead `scrollX`, non-sticky columns, no PageUp/Down/word-jumps, no gutter, no wrap).
**Gates:** G-D3. **Difficulty:** L. **Risk:** MED — cursor/scroll math under mixed-width text is
the concentrated risk (the T04 Input lesson: one function owns the math; every path uses it).

The missing enterprise component: the target sketch's `<textarea id="text">` with
`main.query('#text').value`. Registered as tag `textarea` (D-C1); `value` ⇄ `text()` in DomNode's
ladder (D01).

---

## 1. Class & state

```lev
class TextArea : Focusable, Scrollable, Styleable, IMultiLine, IValidatable {
    // storage: one string per line, no trailing-newline sentinel (recon's model, kept)
    private Array<string> lines_ = [""];
    // cursor: SCALAR indices (chars()), per the T04 index-unit discipline
    private int curLine = 0;  private int curCol = 0;      // 0..scalarLen inclusive
    private int desiredCol_ = 0;                           // sticky column (CELLS — §3)
    public bool readOnly = false;
    private int tabWidth_ = 4;                             // Tab inserts spaces v1
    private bool gutter_ = false;                          // line numbers
    private int longestCells_ = 0;  private bool widthDirty_ = true;   // measure cache
    // validation (the Input pattern): permissive default + flag (the old #51 shape, kept as style)
    private (string) => bool validator_ = (string s) => true;  private bool hasValidator_ = false;
    private string validationMessage_ = "";
    // events (token model, T01 convention)
    on/off: onChange((string) => void), onCursor((int line, int col) => void), onSubmit? — NO (multi-line; Enter edits)
}
```

Fluent surface (R6): `text(string)` / `string text()`, `value(...)` aliases both, `readOnly(bool)`,
`tabWidth(int)`, `gutter(bool)`, `validator((string) => bool, string message)`. Setters state their
damage: `text` → `invalidateLayout` (geometry), `gutter`/`tabWidth` → `invalidateLayout`,
`readOnly` → `invalidate`.

**Index units (binding):** storage/cursor = scalars; screen = cells via `glyphWidth` prefix sums;
`text()` joins with `"\n"`. Every field/param in the file states its unit (the T04 rule).

## 2. Geometry

- `contentDesired(avail)` = `Size(gutterCells() + longestLineCells(), lines_.length())` — honest
  preference; the parent clamps (T02 contract), Scrollable picks up the overflow as `contentSize`.
  `longestLineCells()` is cached (`longestCells_`/`widthDirty_`): edits that can only grow compare
  the edited line inline; deletions/splits set the dirty flag; recompute is one O(total) pass on
  next measure. (No per-measure full scans — the 10k-line file case.)
- `gutterCells()` = 0 when off, else `digits(lineCount) + 1` (right-aligned numbers + one space),
  recomputed per paint (cheap), style key `textarea.gutter`.
- Viewport = `box.inset(chrome())` minus gutter columns; **no wrap in v1** (`Overflow::Scroll`
  semantics both axes; `wrap` is the flagged v1.1 — the wrapLines/measure-agreement machinery T04
  Text already owns is the ingredient, deferred deliberately).

## 3. Cursor & scroll math (the one-function rule)

`int cellAt(int line, int scalarIdx)` — prefix sum of `glyphWidth` over `lines_[line].chars()`
(the Input.cellAt discipline, per line). ALL of: painting the cursor, horizontal scroll clamping,
sticky-column targeting, and mouse hit→cursor mapping go through it — never a second
implementation.

- **Sticky column:** vertical moves (Up/Down/PageUp/PageDown) target `desiredCol_` (CELLS, captured
  at the last horizontal move/edit) mapped back to the nearest scalar boundary ≤ target on the new
  line (`scalarAtCell`, the inverse walk — same function family). Horizontal moves and edits reset
  `desiredCol_ = cellAt(curLine, curCol)`. (Recon's clamp-only behavior is the bug this fixes.)
- **Auto-scroll (`ensureCursorVisible()`, called after EVERY cursor/content mutation):**
  vertical — `scrollY` clamps so `curLine ∈ [scrollY, scrollY + viewH)`; horizontal — with
  `c = cellAt(curLine, curCol)`: `c < scrollX → scrollX = c`;
  `c >= scrollX + viewW → scrollX = c - viewW + 1`. Rides `Scrollable.scrollTo` (contentSize kept
  current from the §2 cache) so wheel routing/scrollbars stay coherent.
- **Wide-glyph edge rule:** a wide scalar half-visible at the left viewport edge renders blank in
  its clipped cell (skip-render, never tear — the Input pin, applied per line).
- `Point? cursorPos()` — **box-relative** (the sonar-bugs #2 contract, cited so nobody regresses
  it): `Point(chromeLeft + gutterCells() + cellAt(curLine,curCol) - scrollX, chromeTop + curLine - scrollY)`
  when focused+visible and the cell is inside the viewport, else `None`.

## 4. Editing (the normative key table)

| key | effect |
|---|---|
| printable char | insert at cursor; col+1 |
| Enter | split line at cursor |
| Backspace | delete scalar before cursor; at col 0: join with previous line (cursor to junction) |
| Delete | delete scalar at cursor; at line end: join next line up |
| Left / Right | ±1 scalar, wrapping across line boundaries |
| Up / Down | ±1 line, sticky column |
| PageUp / PageDown | ±(viewH − 1) lines, sticky column |
| Home / `C-a` | col 0 |
| End / `C-e` | line end |
| `C-Home` / `C-End` | document start / end |
| `C-Left` / `C-Right` | word jump (space-delimited, the Input rule) |
| Tab | insert `tabWidth_` spaces (readOnly-gated like all edits; **Tab-as-focus is surrendered while a TextArea is focused** — BackTab still moves focus; documented, the editor convention) |
| paste (PasteEvent) | multi-line insert: split on `\n`, insert/splitLine alternately (recon's algorithm, kept) |

Every mutation: apply → maintain width cache → `ensureCursorVisible()` → fire `on:change(text())`
(per keystroke, the T04 contract; users debounce) → `invalidate()`; line-count or longest-width
changes → `invalidateLayout()`. Cursor-only moves fire `on:cursor` + invalidate (caret repaint).
Mouse: Left press maps (x,y) → (line via `y+scrollY`, scalar via cell-walk on that line) and
focuses. readOnly: edits no-op (nav/scroll still live), `.disabled`-style NOT applied (readOnly ≠
disabled; theme key state `.readonly` reserved v1.1).

All handlers are constructor-registered with explicit `this.` receivers (#53 discipline); char
tests via `.code()` (no char literals in call-arg position).

## 5. Paint

Per visible row `r` in `0..viewH-1`: `li = r + scrollY`; gutter (right-aligned number, `textarea.gutter`,
current line gets `.focused` variant when focused); line content = cell-slice of `lines_[li]` from
`scrollX` for `viewW` cells (wide-glyph edge rule both ends); **each row pads with spaces to the
full viewport width** — the component clears its own rows, so shrinking content never strands
glyphs regardless of D03's #4 fix status (belt AND suspenders; also what makes it correct under a
transparent theme). Rows past `lineCount` clear to background. Style: `textarea` /
`textarea.focused` / `textarea.disabled` (+ `textarea.gutter`). `contentSize` updates here too
(paint never mutates layout facts — it reads the §2 cache; the update happens in the mutation path,
restated to keep the T02 no-invalidate-in-paint invariant).

## 6. Validation & DOM integration

`IValidatable.validate()`: intrinsic validator (when set) then T07-registered `@Validator` list —
the Input ordering, verbatim. `validationMessage()` likewise. DOM: tag `textarea`, attrs `value`
(initial text), `readOnly`, `tabWidth`, `gutter`, `maxLength`? — **no maxLength v1** (scalar-count
policy on multi-line text is app domain; anchor-logged as a deliberate T04-table divergence);
events `on:change`, `on:cursor`.

## 7. Milestones

| M | contents | difficulty |
|---|---|---|
| M1 | storage/fluent/measure cache + plain paint (no gutter) + text()/value round-trip | M |
| M2 | cellAt/scalarAtCell + full key table + paste + sticky column + ensureCursorVisible | L |
| M3 | cursorPos + mouse mapping + gutter + readOnly + theme keys | M |
| M4 | validation + DOM registry row + drift entries; recon migration note (below) | S |

M4 files a note in `examples/recon/RESEARCH.md` proposing recon swap its local TextArea for the
framework one (a separate, owner-approved change — not done as a side effect).

## 8. Potential issues & mitigations

1. **Cursor drift under CJK/emoji** — the one-function rule + the T04 test tables replayed per
   line (ASCII/CJK/emoji mixes, boundary cells both edges); `cellAt`/`scalarAtCell` round-trip
   property test.
2. **Width-cache staleness** — deletions mark dirty rather than patch (correct beats clever);
   property test: cache == brute-force recompute after every scripted edit sequence.
3. **Large-file perf** — paint is O(viewport); edits O(line); measure O(total) only when
   width-dirty. A 10k-line scripted soak pins timings don't regress (relative, not absolute).
4. **Tab focus surrender** — an all-TextArea form can't Tab out forward. BackTab works; documented
   loudly with the `^Tab`-style app-keymap recipe. (The alternative — Tab moves focus, `C-t`
   inserts — violates every editor's muscle memory; ruled.)
5. **Pure-array line edits** (`lines_[i] = s` rebind) — COW-on-refcount makes the uniquely-owned
   case in-place (landed §11); no `Array<Struct>.add` shapes anywhere (#74-clean: strings, not
   structs).
6. **on:change flood** — contractual (T04); the debounce recipe cross-referenced.

## 9. Testing plan

Byte-script editing corpus (the Input pattern: script → final text+cursor+scroll goldens):
navigation matrix incl. sticky column across short lines, word jumps, doc home/end, page moves;
edit matrix (insert/backspace/delete at line starts/ends/joins, Enter splits, Tab, paste
multi-line incl. CRLF normalization — `\r` stripped at setText/paste, anchor-logged); CJK cursor +
both viewport edges; auto-scroll follow scripts both axes; gutter goldens (width growth at 9→10
lines); readOnly matrix; mouse-to-cursor table; validation ordering; DomNode `value` round-trip;
snapshot goldens per state. Differential oracle/IR/LLVM; emit-C++ compile-only.

## 10. Open questions

1. Selection model (anchor scalar + range styling + clipboard) — v2; the field layout reserves
   nothing (additive when it comes; `textarea.selection` theme key namespace noted).
2. Undo/redo (edit-op journal) — v2, pairs with selection.
3. Soft wrap (`wrap(bool)`) — v1.1; reuses Text's wrapLines + a wrapped-row↔line map; the
   measure/paint-agreement rule applies doubly.
4. Syntax/gutter decorations API (diagnostics margins) — v2, wants the class/style system on rows.

## 11. Implementation log

- 2026-07-15 — design written; not started.
- 2026-07-19 — implemented (all milestones M1–M4). `sonar_v2/src/components/textarea.lev` is the
  new leaf; `<textarea>` registered in `dom/registry.lev`; attr appliers (`value`/`readOnly`/
  `tabWidth`/`gutter`, no `maxLength` per §6) in `dom/builder.lev`; `value`/`text` narrowing-ladder
  rows + the new `cursor` event wired in `dom/node.lev`; serializer `leafText` row in
  `dom/document.lev`. One-function cursor math (`cellAt`/`scalarAtCell`) drives paint, scroll
  clamping (rides `Scrollable.scrollTo` with a live `contentSize` from the longest-line cache),
  sticky column, `cursorPos` (box-relative), and mouse hit-mapping. Differential test
  `sonar_v2/tests/dom-textarea/` (edit/nav/word/sticky/CJK-cells/paste-CRLF/scroll/gutter/
  cursorPos/readOnly/mouse/validate/DOM-round-trip) passes byte-identical on oracle+IR+LLVM;
  emit-C++ takes the standard DOM async-gap compile skip. Recon migration note filed in
  `examples/recon/RESEARCH.md` (owner-approved swap left for a dedicated change, per M4).
  Deviation from §1's fluent sketch: getter/`setX` naming (`setText`/`setValue`) follows the
  repo's landed component convention (ContentBar/Input) rather than a same-name getter+setter
  overload; `readOnly`/`tabWidth`/`gutter`/`validator` fluent setters kept as designed.
