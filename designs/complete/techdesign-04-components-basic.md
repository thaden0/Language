# Sonar — Tech Design 04: Basic Components

**Status:** design, pre-implementation. **Date:** 2026-07-12. **Track:** T04.
**Owns:** `sonar/src/components/{text,contentbar,input,button,checkbox,radio,progress,spinner}.lev`.
**Depends on:** T01–T03; T09's `App.every` for Spinner/indeterminate-Progress/Button-flash. F3 improves handler spelling only (lambdas until).
**Gates:** G-S2. **Difficulty:** M. **Risk:** LOW/MED (Input's cursor/scroll/width math is the concentrated risk).

Implements anchor C7/C8 consumption, C11 (each component ends with its attribute table — T06 aggregates), C14 rows (with corrections noted), R6 (leaf fluent wrappers), R7/lifecycle, R10 keys. **Conventions established here for all component tracks:** state-suffix theme keys (`.focused/.disabled/.selected`); per-component `bool enabled = true` where input is taken (disabled ⇒ `isTabStop()` false + `.disabled` styling) — an anchor gap adopted as convention and flagged to the anchor log; per-setter damage tables (visual → `invalidate()`, geometry-affecting → `invalidateLayout()`).

**Index-unit discipline (binding for every spec below):** string indices are BYTES (the language's rule); cursor/editing positions are SCALAR indices over `.chars()`; screen positions are CELLS via `glyphWidth` prefix sums. Every field states its unit.

---

## 1. Text — `class Text : Component`

(C14 marker correction: `ISingleLine` dropped — wrap makes Text multi-line; noted for the anchor log.)

State: `string text`, `Align align = Align::Start`, `Overflow wrap = Overflow::Clip` (`Scroll` invalid here → log + Clip). Fluent: `text(string)` (invalidateLayout — length changes geometry), `align(Align)` (invalidate), `wrap(Overflow)` (invalidateLayout).

`contentDesired(avail)`: split on `\n`; unwrapped → `Size(max line cell-width, lineCount)`; wrapped under bounded width → greedy word-wrap (spaces are the only break points v1; a word wider than the width hard-breaks at the cell boundary) → `Size(min(maxLine, avail.w), wrappedLineCount)`. **The wrap function is shared verbatim by paint** (one function, `Array<string> wrapLines(string, int cellWidth)` — measure and paint must never disagree).

`paintContent`: lines via wrapLines (or raw split when Clip), aligned per `align` within the content width, style `resolve("text", theme)`.

| attribute | expands to |
|---|---|
| `text` | `text(string)` |
| `align`, `wrap` | setters (enum-typed literals per C11) |

## 2. ContentBar — `class ContentBar : Component, ISingleLine`

Three segments (enterprise status bars): `setLeft/setCenter/setRight(string)` (each invalidates). Height 1 (`contentDesired = Size(sum widths + 2 gaps, 1)`). Paint: left at 0, right at `w - rightWidth`, center centered; when tight, drop order center → right → truncate-left-with-ellipsis (`…`). Theme: `contentbar`, parts inherit. Attributes: `left`, `center`, `right`, plus legacy `text` ⇒ setLeft.

## 3. Input — `class Input : Focusable, IValidatable, ISingleLine`

State: `string value` (bytes storage; edited at scalar granularity); `string placeholder`; `char? mask` (None = plain — the `T?` narrowing showcase; a masked input paints `mask` per scalar); `int maxLength = -1` (scalars; -1 unlimited); `int cursor` (SCALAR index, 0..scalarLen inclusive); `int scrollCells` (viewport offset, CELLS); `bool enabled = true`; validator `(string) => bool` + message.

**Editing keys (the normative table):** printable/paste-text → insert at cursor (respecting maxLength; paste truncates); Backspace/Delete → remove scalar before/at cursor; Left/Right → ±1 scalar; `C-Left/C-Right` → word jump (space-delimited); Home/`C-a` → 0; End/`C-e` → end; Enter → `on:submit(string)`. Every mutation: recompute scroll (below), fire `on:change(string)` (per keystroke — the contract; users debounce), invalidate. Editing chords are consumed by the focused Input during target dispatch — i.e. BEFORE the keymap fallback tier but AFTER the capture tier (R11 interplay: a global `C-a` capture-tier bind would win; apps that bind editing chords globally get what they asked for — documented).

**Cursor/scroll math (the risk concentrated into one function):** `int cellAt(int scalarIdx)` = prefix sum of `glyphWidth` over `value.chars()[0..scalarIdx)` (masked: `glyphWidth(mask) * idx`). Visible window `[scrollCells, scrollCells + innerW)`; after any cursor move: if `cellAt(cursor) < scrollCells` → `scrollCells = cellAt(cursor)`; if `>= scrollCells + innerW` → `scrollCells = cellAt(cursor) - innerW + 1`. A wide scalar half-visible at the left boundary renders blank in its clipped cell (the pin: skip-render, never tear). `cursorPos()` override → `Point(box.x + cellAt(cursor) - scrollCells, box.y)` when focused and visible, else None (the hardware-cursor contract, C8).

Paint: value (or `placeholder` in `input.placeholder` style when empty and unfocused), mask substitution, `input(.focused/.disabled).text`. `contentDesired = Size(20, 1)` default (width constraint-driven in practice). No selection in v1 (deferred; v2 sketch: anchor scalar + range styling).

`validate()`: intrinsic (validator over value) then T07-registered validators; `validationMessage()` per T07 ordering.

| attribute | expands to |
|---|---|
| `value`, `placeholder`, `maxLength` | setters |
| `mask` | `mask(char)` |
| `validator` | `validator((string) => bool)` |
| `on:change`, `on:submit` | `onChange`/`onSubmit` |

## 4. Button — `class Button : Focusable, ISingleLine`

State: `string label`; `string key = ""` (display-only accelerator suffix, rendered `label [^S]` when set — binding is the app's/`@Shortcut`'s job); `bool enabled`. Fire: Enter/Space when focused, mouse Press within box → `on:press()`. **Press flash:** one-frame Reverse-attr flash via `App.every`-free path — set `flashing_=true`, invalidate, arm a 100ms one-shot (App.every + immediate cancelEvery in the tick) clearing it; if T09 timers are unavailable (tests without pump ticks), the flash simply persists until next event — cosmetic-only degradation, accepted and documented. `contentDesired = Size(labelCells + 4, 1)` (`[ label ]` chrome). Theme: `button(.focused/.disabled)`, flash uses `+Attr::Reverse`.

Attributes: `label`, `key`, `enabled`; `on:press`.

## 5. CheckBox — `class CheckBox : Focusable, IValidatable`

`string label; bool checked; bool enabled;` Space/mouse toggles → `on:toggle(bool)` + invalidate. Paint: `[x] label` / `[ ] label` (glyphs fixed v1; theme keys `checkbox(.focused/.disabled)`). `validate()`: T07-registered only (no intrinsic). Attributes: `label`, `checked`, `enabled`; `on:toggle`.

## 6. RadioGroup — `class RadioGroup : Focusable, IValidatable`

`Array<string> options; int selected = -1; bool requireSelection = false; bool enabled;` ONE tab stop; Up/Down move selection within the group (arrow nav is the component's, per T03's ruling), Space/mouse selects → `on:select(int)`. Paint: vertical `(•) opt` / `( ) opt` list, selected row + focus styling (`radiogroup.item(.selected/.focused.selected)`). `contentDesired = Size(max option + 4, options.length())`. `setOptions` clamps `selected` (rebind safety). `validate()` fails when `requireSelection && selected == -1` (message "selection required" unless overridden). Attributes: `options`, `selected`, `requireSelection`; `on:select`.

## 7. ProgressBar — `class ProgressBar : Component, ISingleLine`

`int value; int max = 100; bool showPercent;` clamped ratio → filled cells `w * value / max`, glyph `█` + optional right-aligned `NN%` overlay; partial-eighth glyphs (`▏▎▍…`) for the boundary cell (spec'd table; falls back to full cells under a `bool coarse` toggle for narrow fonts). Indeterminate mode (`indeterminate(true)`): a 3-cell marquee advancing on an `App.every(80ms)` tick — **onAttach starts, onDetach cancels** (the lifecycle discipline pattern, stated as THE template other animated components copy). Theme: `progressbar`, `progressbar.fill`. Attributes: `value`, `max`, `showPercent`.

## 8. Spinner — `class Spinner : Component, ISingleLine`

`Array<string> frames` (default braille set `⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏`), `int intervalMs = 80`, frame index. onAttach → `token = Sonar::app().every(intervalMs, tick)`; tick → advance + invalidate; onDetach → cancel. `contentDesired = Size(maxFrameCells, 1)`. Attributes: `frames`, `interval`.

## 9. Milestones

| M | contents | difficulty |
|---|---|---|
| M1 | Text (+ wrapLines shared fn) + ContentBar | M |
| M2 | Button + CheckBox + RadioGroup | S/M |
| M3 | Input (editing table, cellAt math, cursorPos) | L |
| M4 | ProgressBar + Spinner (lifecycle-timer pattern) | S |
| M5 | validation wiring with T07 + attribute-table drift check with T06 | S |

## 10. Potential issues & mitigations

1. **Input cursor/scroll drift under mixed-width text** — single source of truth `cellAt` (one function; every path uses it), table-tested over ASCII/CJK/emoji mixes including the half-visible-wide-scalar pin.
2. **maxLength vs paste** — truncation at scalar granularity, never mid-scalar; tested.
3. **Button flash without a pump** — cosmetic degradation accepted (§4); snapshot tests assert the non-flash steady state.
4. **RadioGroup options rebind** — selected clamped in `setOptions`; event NOT fired on clamp (programmatic changes don't echo — the general rule: `on:*` fires for user interaction only; stated as a cross-component convention, adopted by T05).
5. **Per-keystroke on:change flood** — contractual; documented with the debounce pattern (an `App.every`-based debouncer example in the doc).
6. **Anchor gaps flagged** (not silently diverged): `enabled` convention; Text's marker correction; the "events fire on user interaction only" convention — all to the anchor log.

## 11. Testing plan

Per component: snapshot goldens per state (focused/unfocused/disabled/checked/selected/flash-off); Input editing byte-scripts (script → final value+cursor+scroll goldens, incl. word-jump, maxLength, CJK cursor math, placeholder↔value transitions, mask); wrapLines table (widths × texts, measure==paint agreement asserted); RadioGroup arrow/clamp scripts; validation matrix (intrinsic × registered ordering); ProgressBar boundary-glyph table; Spinner tick-count with manual pump. All via T10 harness; differential oracle/IR/LLVM.

## 12. Open questions

1. Input selection model — v2 (anchor scalar + range styling sketched).
2. Numeric/spinbox Input variant — v1.1 (validator + arrow steppers over this Input).

## 13. Implementation log

- 2026-07-12 — design written; not started.
- 2026-07-13 — implemented in full across the eight owned files
  (`sonar/src/components/{text,contentbar,input,button,checkbox,radio,progress,spinner}.lev`),
  plus `sonar/trident.toml` (added `src/components/*.lev` to `sources`) and
  `sonar/tests/components/` (a print-and-expect corpus test, `basic.lev`/`basic.expected`,
  covering wrap/align/measure, tight-width drop order, press/toggle/select events and
  validation, boundary-glyph/indeterminate rendering, manual timer-tick pumping, and
  Input's full editing/cursor/scroll/mask/paste/validate surface). Byte-identical across
  oracle, `--ir`, emit-C++ native, and LLVM (verified directly, not just via the corpus's
  own default engine — see below).

  **Not a T09 dependency in practice.** Despite this doc's own header line
  ("Depends on: ... T09's `App.every`"), `ProgressBar`'s marquee and `Spinner`'s frame
  tick use T01's already-built `ILifecycleHost`/`Component.__sonarRegisterTimer` pending-
  timer seam instead of calling `Sonar::app()` directly — that seam exists precisely so
  core/package code doesn't need the not-yet-defined `App` type (component.lev's own
  comment says so). Both components register their timer once (constructor time); it
  activates on every attach and cancels on every detach via `Component`'s existing
  `__sonarAttach`/`__sonarDetach`, with no onAttach/onDetach override needed in either
  file. `Button`'s press-flash was simplified relative to the doc's own "App.every one-
  shot, clear-after-100ms" sketch: it clears on the component's own next `paintContent`
  call instead of via a timer — the exact "persists until next event" cosmetic
  degradation this doc's own §4/§10.3 already accepts and documents, so no App
  dependency was introduced to approximate it more closely.

  **Header completion, not an amendment.** Every leaf class here also composes
  `Styleable` even though this doc's own class-header lines (e.g. "`class Text :
  Component`") don't spell it out. Every `paintContent` in this doc calls
  `resolve(key, theme)`, which only exists on that mixin — so `Styleable` was always
  implied by the doc's own bodies, just omitted from the abbreviated headers.

  **C5 marker correction (per this doc's own §1 instruction):** `Text` drops the
  `ISingleLine` marker C14's row lists for it — `wrap` makes `Text` potentially
  multi-line, contradicting the marker. Logged in `techdesign-00-overview.md`'s anchor
  log. Every other leaf here keeps its documented marker(s).

  **C5 also amended**: `ISingleLine`, `IMultiLine`, and `IValidatable` were declared for
  the first time — no track before T04 needed them, so despite being listed in C5 they
  didn't exist in `sonar/src/*.lev` yet. Declared in `text.lev` (this track's first file,
  alongside its other shared basics) rather than in T01-owned `component.lev`.

  **Five new compiler bugs found and worked around** (bug.md #50–#53, plus a T01 fix):
  #50, a single-quoted char literal (`' '`) fails to retype to `char` specifically in
  call-argument position (works in declarations/comparisons/returns) — every
  `KeyEvent.isChar('x')`-shaped call binds the literal to a local `char` first. #51, a
  nullable function type `((T) => R)?` fails to parse — `Input`'s `validator_` field is
  a non-nullable field defaulted to a permissive lambda plus a `hasValidator_` flag
  instead. #52, calling the result of an array-indexing expression directly
  (`arr[i]()`) fails to lower on the IR/native path — every `T07`-style
  `extraValidators_[i]()` call binds the element to a `var` first. **#53, the
  significant one**: a lambda that calls one of its own class's instance methods with a
  *bare* (implicit-`this`) receiver — the ordinary way to write it — silently loses the
  call on `--ir` and **segfaults on native/LLVM** once the lambda is stored (e.g. via
  `onKey`) and invoked later from a different call frame than where it was created.
  Every constructor-registered `onKey`/`onMouse`/`onPaste`/timer handler in this track
  spells the receiver explicitly instead (`this.firePress()`, `this.toggle()`,
  `this.moveSelection(...)`, `this.invalidate()`, `this.handleKey(e)`, ...) — field
  reads/writes inside the same lambdas are unaffected and needed no change. Also fixed,
  as part of this track (T01-owned `component.lev`, amended with justification logged
  in `techdesign-00-overview.md`'s anchor log): `TimerReg`/`ShortcutReg` were relying on
  positional struct auto-construction, which bug.md #38 already documents as silently
  dropping fields — undiscovered until this track's timer seam was actually invoked for
  the first time (a closure-typed field silently empty, throwing "cannot resolve call
  target" the first time anything tried to call it); both structs now use explicit
  constructors, #38's documented workaround.

  All bugs were caught by differential testing: `trident run` (oracle), a hand-
  concatenated single-file `--ir` run, `trident build` (emit-C++ native), and a direct
  `--native-obj` + link (LLVM) all produce byte-identical output against
  `sonar/tests/components/basic.expected` — `trident build`'s whole-program IR lowering
  in particular surfaced #52 and the `TimerReg`/`ShortcutReg` fix even though the
  corpus's own thin oracle smoke pass did not exercise those paths. T01's own
  differential suite (`sonar/tests/core`) and T02's (`sonar/tests/layout`) were re-run
  unchanged as a regression check after the `component.lev` fix and stayed green.
