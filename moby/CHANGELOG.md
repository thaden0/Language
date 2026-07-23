# Moby Changelog

## 0.2.0 (2026-07-20)

### DOM layer (D01–D08)

The v0.2.0 release adds the full Moby DOM surface — a web-developer-friendly layer on
top of the retained T01–T11 component tree. The component tree is the DOM; the new code
annotates and wraps it without replacing it.

**D01 — Document, selectors, DomNode, MobyApp**
- `MobyApp : App` owns a `Document` and adds `query`/`queryAll`/`queryOrNone` sugar
- CSS-style selector engine: tag, `#id`, `.class`, `[attr]`, `[attr=value]`, descendant,
  child (`>`), group (`,`); bare `#id` hits the fast-index path
- `DomNode`: jQuery-style wrapper with `.value()`, `.text()`, `.checked()`, `.enabled()`,
  `.show()`/`.hide()`, `.position()`, `.actions()`, `.on()`, `.outerMarkup()`, `.popover()`
- `Document` meta side-table (tag, attrs, action registry, id index); parallel-column
  storage, no growing namespace globals (#73), no Array<Struct> (#74)

**D02 — Markup engine (runtime + `dom!`)**
- Three markup tiers: `dom!(\`…\`)` comptime, `import("x.moby")` asset, runtime string
- `DomParser`: hand-rolled recursive-descent, E-D1…E-D12 error catalog with `line:col` +
  caret markers; `DomBuilder` with post-build action wiring
- `DomRegistry`: singleton, parallel-column factories/matchers, `registerTag` user extension

**D03 — Position, classes, DOM containers, paint fix**
- `Position` reference-class: live reads (`x`/`y` return the arranged box origin),
  stored writes consumed only when `method == Absolute`; `anchorTo(target, edge)`
- `FlexContainer` (`<flex>`) and `Bar` (`<contentbar>`) with absolute-aware arrange/paint
- `hidden` semantic class: `node.hide()` ↔ `visible() == false`
- Bug #4 paint fix: unconditional leaf background clear on shrink (replaces the workaround)

**D04 — Actions & hotkeys**
- `ActionRegistry`: `add`/`remove`/`fire`/`setEnabled`; responder chain (first-holder-wins,
  nearer-disabled-shadows); parallel closure columns (D-P4 shape)
- Markup wiring: `action=""` opt-out, explicit attr, auto-slug (`"Save As"` → `save-as`)
- `normalizeHotkey`: DOM grammar (`^`, `!`, `+` prefixes) → canonical `C-`/`M-`/`S-` form
- Greying: `setEnabled` walks all bound items instantly — no per-frame polling

**D05 — Bindings (`{{…}}`)**
- Pull-sweep: `Document.__sweepBindings()` re-evaluates getters, diffs, applies on change
- `dom!` tier: bare global writes update bound spans on the next frame (no observe hook)
- Runtime tier: `expose(key, getter)` + `doc.set(key, value)` key-resolution order
- Exception containment: throwing getter keeps last good value; tombstone + compaction
  at 25% dead AND ≥64 dead (#74/#73 churn flatness)

**D06 — TextArea**
- `TextArea`: multi-line editing with gap-buffer storage, cursor navigation, word-wrap
- `gutter`, `readOnly`, `tabWidth` attrs; `onChange`/`onCursor` events

**D07 — Dialogs**
- `Moby::Dialogs::FileDialog("open"|"save")`: directory listing via `std::sysListDir` +
  `std::isDir` (no O(N) probe — `request-stat-isdir.md` fulfilled); `show()` → `Promise<string|None>`
- `Moby::Dialogs::PromptDialog`: labeled input + validator + OK/Cancel
- `Moby::Dialogs::alert`/`confirm` re-exports (one-stop `uses Moby::Dialogs`)

**D08 — DevTools, testing, examples, delivery** (this release)
- `Moby::DevTools::inspector(app, hotkey="F12")`: DOM inspector overlay (tree/detail/
  highlight/query bar); pushed via `pushOverlay` input-exclusive; self-exclusion of the
  overlay subtree from the tree source; `inspector-highlight` class on selection
- `tests/harness/dom_helpers.lev`: `q`/`click`/`type`/`chord`/`textOf`/`expectText` +
  `encodeMouse` (SGR `\x1b[<b;x;yM/m` — the chord encoder's mouse twin)
- Drift harness tiers 1–4: tier equivalence (dom! ↔ runtime ↔ asset), registry coverage,
  ladder coverage (value/text/checked/enabled per tag), serializer round-trip
- `examples/editor-dom/`: the target-feel program, corrected and running, with scripted
  session golden test
- `moby/trident.toml`: `src/dom/*.lev` glob already covers devtools (no line added)
- `moby/README.md`: DOM section, selectors, classes, actions, bindings, inspector,
  testing-with-selectors guide, quickstart example

### Known changes visible in goldens

- **Bug #4 paint fix** (D03): unconditional leaf background clear on shrink. Tests that
  used the workaround (`clearOnShrink=false` flag) will see a one-cell background
  difference on narrow → wide → narrow sequences. Re-run goldens with `tests/regen.sh`
  after this release.
