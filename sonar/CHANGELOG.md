# Changelog

All notable changes to the `sonar` package. Pre-1.0, minor bumps may break;
CHANGELOG discipline starts at the first tag (Track 10 §7).

## [0.1.0] — 2026-07-14

First tagged release. The full framework across Tracks 01–11, delivered and
tested.

### Core (T01–T03)
- Geometry/style value types, the frozen 8-byte cell `Surface` with wide-glyph
  healing and clip stack, component damage/lifecycle/handlers, collapse-based
  mixins, containers, and damage sweeps.
- Layout strategies: `FlexLayout`, `GridLayout`, `DockLayout`, `StackLayout`.
- Events + input: capture/target/bubble dispatch, the ANSI/UTF-8 input decoder,
  `FocusRing` traversal, and the `Chord`/`Keymap` parser.

### Components (T04–T05)
- Basic: `Text`, `ContentBar`, `Input`, `Button`, `CheckBox`, `RadioGroup`,
  `ProgressBar`, `Spinner`.
- Composite/data: `ContentBox`, `GridBox`, `SplitBox`, `Tabs`, `ListView`,
  `TableView`, `TreeView`, `Modal` (+ `alert`/`confirm`), `Menu`/`BarMenu`,
  `DebugOverlay`.

### Templates, theming, run loop, reactivity, attributes (T06–T09, T11, T07)
- The `sonar!` compile-time template layer and `.sonar` grammar.
- Theming & DI: `Theme : ITheme`, the most-specific-first resolution chain, a
  TOML subset parser, and four built-in themes with an a11y `attrs` rule.
- `App` run loop, damage-coalesced scheduler, `AnsiRenderer` (row-diff + SGR
  minimizer, one `sysWrite`/frame), terminal session, resize, timers, overlays.
- `@Sonar::Reactive` compile-time reactivity; `@Shortcut`/`@Timer`/`@Validator`
  attribute rules.

### Testing & delivery (T10) — new in this release
- `TestRenderer` (shipped in the package): a recording `IRenderer` with the
  frozen two-channel snapshot format (`snapshot()` / `textOnly()`).
- `ScriptedInput` + a chord encoder (`tests/harness/`): queues input chunks,
  delivers one per pump, and encodes chord specs (`^S`, `Down`, `M-a`, ...) to
  the exact bytes the decoder reads back (encode→decode round-trips).
- `SonarTest` assertion vocabulary: `eq` / `snap` / `snapText` / `harness` /
  `pump` / `reset` — the composition root for corpus-style golden tests.
- `tests/runtests.sh`: the differential engine-matrix runner (oracle + IR + LLVM
  against one shared `.expected`; emit-C++ compile-only; LLVM stdout compared
  alone, its `[heap]` meter stays on stderr).
- Five example apps under `examples/`, each runnable via `trident run` and each
  a differential test in scripted mode (`SONAR_SCRIPT=1`): `hello`,
  `form-wizard`, `file-manager`, `log-viewer`, `dashboard`.
- Probe suite (`tests/probes/` + `PROBES.md`): 14 language-behaviour probes,
  all green on oracle/IR/emit-C++/LLVM.

### Known issues
- `sonar-bugs.md #4` — a leaf repaint under the default/empty theme does not
  clear stale glyphs when its content shrinks. Workaround: fixed-width fields
  (used in `examples/file-manager`).
