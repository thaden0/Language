# Sonar DOM — Tech Design 08: DevTools, Testing, Examples, Delivery

**Status:** design, pre-implementation. **Date:** 2026-07-15. **Track:** D08.
**Owns:** `sonar/src/dom/devtools.lev`, `sonar/tests/dom*/**` (suite-wide corpora incl. the drift
harness), `sonar/tests/harness/dom_helpers.lev`, `sonar/examples/editor-dom/**`, plus the delivery
lines: `sonar/trident.toml` sources, `sonar/README.md` DOM section, `sonar/CHANGELOG.md` (0.2.0).
**Depends on:** every D-track (it tests them); T05 TreeView/SplitBox (inspector chrome), T10 harness
(TestRenderer/SonarTest/ScriptedInput — extended, never modified in place).
**Gates:** G-Dv1 (rolling from G-D1: the drift harness lands with D02). **Difficulty:** M. **Risk:** LOW.

The developer-experience half of the suite: see the tree, poke it live, script it in tests, and read
one example that IS the target feel.

---

## 1. The DOM Inspector (`devtools.lev`)

`Sonar::DevTools::inspector(SonarApp app, string hotkey = "F12")` — installs a toggle binding; the
overlay is built on first open:

```
┌ Inspector ──────────────────────────────── F12 to close ┐
│ ┌ tree (TreeView) ──────────┐┌ detail ─────────────────┐│
│ │ ▾ flex#main               ││ <menuitem id="file-menu">│
│ │   ▾ contentbar            ││ box: 0,0 20x1  z:0 Flow  │
│ │     ▾ menu                ││ classes: (none)          │
│ │       menuitem#file-menu  ││ attrs: hotkey="^f"       │
│ │   textarea#text           ││ actions: (via chain) open│
│ │   ▾ contentbar            ││ style keys: menu.item…   │
│ │     span                  ││ binding: —               │
│ └───────────────────────────┘└──────────────────────────┘
│ query> [ #file-menu        ]  1 match (Enter cycles)    │
└──────────────────────────────────────────────────────────┘
```

- **Tree**: a `TreeView` over an `ITreeSource` adapter whose `TreeNodeId.id` is the Document
  `domSlot_` (nodes without a slot get one on first inspection — the side-table absorbs hand-built
  trees). Labels: `tag#id.class…` from Document meta + the D01 reverse matchers. Lazy children via
  `__sonarChildren()` — the landed lazy-source hook.
- **Detail** (a `Text` rebuilt on selection): live `box`, Position method/desires/z/anchor, class
  list, recorded attrs, effective action key + resolving registry, hotkeys bound, binding source
  text + last value + last sweep delta (D05 exposes counters), resolved style-key trail for the
  component's base key (the `Styleable.resolve` fold, replayed verbally — the theming debugger).
- **Highlight**: selecting a node adds the `inspector-highlight` class to it (Reverse-attr style
  shipped in every built-in theme's class keys — the D03 class-theming system eating its own
  dogfood) and removes it from the previous selection; closing the inspector clears it.
- **Query bar**: an `Input`; Enter runs `queryAll` (errors render inline — E-D10's line:col), shows
  the match count, cycles selection through matches on repeated Enter.
- **Mechanics**: pushed via `pushOverlay(panel, false, false, devtoolsGroup)` — input-EXCLUSIVE
  while open (deliberate: inspecting IS the task; the F12/Esc close path is the exit), docked right
  (Absolute position, `anchorTo(app-root, RightOf-inset)` — D03 machinery), live-refresh throttled
  to selection changes + a manual `r` rebind (no per-frame re-walk; the D01 §7.4 rule).
- `DebugOverlay` (T05) is unchanged — stats/log stay its job; the inspector links to it
  (`d` toggles) rather than absorbing it.

## 2. Selector-driven test helpers (`tests/harness/dom_helpers.lev`)

Additions to the `SonarTest` namespace (new file — the landed harness files are not edited):

```lev
DomNode  q(App app, string selector);            // query via the app's Document; throws like query
void     click(App app, string selector);        // center-of-box SGR mouse Press+Release bytes
void     type(App app, string selector, string text);   // click-to-focus, then text bytes
void     chord(App app, string spec);            // encodeChord(normalizeHotkey(spec)) — DOM hotkey grammar
string   textOf(App app, string selector);       // q(...).text()
void     expectText(App app, string sel, string want, string label);  // eq() sugar
```

`click` extends the chord encoder's philosophy to mice: `encodeMouse(x, y, button, press) -> string`
(SGR `\x1b[<b;x;yM/m` bytes, 1-based) added HERE (the encoder reverses the landed decoder table —
the encode→decode round-trip test extends to mouse rows). Everything drives the REAL input path
(ScriptedInput → decoder → dispatch) — no synthetic-event side door, so tests exercise what users
exercise. Each helper pumps once after delivery (the deterministic one-chunk-per-pump model).

## 3. The drift harness (`tests/dom-drift/`)

The suite's registry-coherence backstop (D02 §7.1 names it load-bearing):

1. **Tier equivalence**: one corpus file exercising every registered tag × every attribute, built
   through (a) the runtime parser and (b) `dom!`; assert `outerMarkup` equality and snapshot
   equality after one pump.
2. **Registry coverage**: under a test flag the registry counts applier hits; the corpus must touch
   every row (a new attr without a corpus line fails).
3. **Ladder coverage**: every tag × DomNode `value`/`text`/`checked`/`enabled` — result or expected
   throw per a table (D01 §7.2).
4. **Serializer round-trip**: parse→serialize→parse structural equality (D01 §5.1 guarantee).

## 4. The flagship example (`examples/editor-dom/`)

**The target-feel program, corrected and running** — the suite's acceptance test and its README's
first code block. `main.lev` is the sketch with: `dom!(\`…\`)` payloads (delta #1),
`uses Sonar::Dialogs` (delta #2), the typo fixes (closed `</textarea>`, `fileMenu`, label
`Open`, save-as prompting via `PromptDialog` for its path, complete `$if`), and zero other
deviations — `SonarApp`/`FlexContainer`/`query('#file-menu')`/`position.method = Absolute`/
`actions.add('open', …)`/`{{filename}}`/`await openDialog.show()` all verbatim. A `README.md` maps
sketch line → mechanism → owning design doc (the fidelity table, anchor §2, instantiated).

Ships like every T10 example: own `trident.toml` (`[[dep]] path = "../.."`), runnable
`trident run`, and under `SONAR_SCRIPT=1` binds the harness, scripts a session (open the File menu
by hotkey, cancel the dialog, type in the textarea, toggle the menu by mouse), and snapshots — a
differential test on oracle/IR/LLVM like the other five examples. (FileDialog's fs listing runs
over the example's own directory — deterministic because the corpus runs with cwd = test dir.)

## 5. Delivery

- `sonar/trident.toml`: `sources += ["src/dom/*.lev", "src/dom/templates/*.lev"]` (one line, C13
  shape — components/textarea.lev is already globbed).
- `sonar/README.md`: a "Sonar DOM" section — the three markup tiers, selectors, classes, actions,
  bindings, dialogs, the inspector, "testing your app with selectors"; the editor-dom example
  embedded as the quickstart.
- `sonar/CHANGELOG.md`: `0.2.0` — the DOM layer, TextArea, Dialogs, the #4 paint fix (called out as
  a golden-visible change), inspector.
- `sonar/tests/runtests.sh`: picks up `tests/dom*` + the new example by its existing globs; no
  runner changes expected (verified at M1).
- `docs/footguns.md`: close the #4 row (D03); add none (the suite introduces no new footguns — a
  claim the composition corpus keeps honest).

## 6. Milestones

| M | contents | difficulty |
|---|---|---|
| M1 | dom_helpers (q/click/type/chord + encodeMouse + round-trip rows) | M |
| M2 | drift harness tiers 1–4 (lands WITH D02; grows per track) | M |
| M3 | inspector: tree/detail/highlight/query bar | M/L |
| M4 | editor-dom example + scripted session + README mapping | M |
| M5 | delivery lines + CHANGELOG + full-matrix green sweep | S |

## 7. Potential issues & mitigations

1. **Inspector inspecting itself** — its own overlay subtree is excluded from the tree source
   (filtered by group id); documented (querying `#inspector` from the query bar is the one fun
   exception, allowed).
2. **Golden brittleness across the suite** — the T10 rules apply (two-channel snapshots; textOnly
   for behavior; style-channel only where style IS the subject); the #4 paint fix's regen (D03 M5)
   happens BEFORE this track's goldens freeze — ordering stated.
3. **Mouse encode drift vs. decoder** — the round-trip test row-covers the SGR matrix (buttons ×
   press/release × coords incl. 1-based edges).
4. **Example flakiness via real fs** — the scripted session points the dialog at the example's own
   `views/` dir (three fixed files); no timestamps/sizes in any snapshot.
5. **Helpers on a plain `App`** — `q`/`click` need a Document; on plain App they throw the
   no-SonarApp error (E-family message points at SonarApp). Landed-suite tests keep using the
   landed harness untouched.

## 8. Testing plan

This track IS testing; its own meta-tests: helper unit rows (click hits the right box center under
scroll/offset; type focuses first; chord normalization matrix), inspector scripts (open via F12,
tree navigation mirrors the fixture, query bar cycles, highlight class appears/clears, self-exclusion),
drift harness red-path checks (a deliberately-broken registry row fails each tier), example session
snapshot on all three run lanes.

## 9. Open questions

1. Inspector edit mode (attr editing via `attr(name, value)` — the machinery exists) — v1.1.
2. Per-sweep binding timings surfaced in the detail pane — v1.1, wants D05 counters stabilized.
3. A `--dom-dump` style CLI flag — rejected: `outerMarkup` + the harness cover it in-language; no
   compiler surface for a package concern.

## 10. Implementation log

- 2026-07-15 — design written; not started.
