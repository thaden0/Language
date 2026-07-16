# Sonar

A retained-mode terminal UI framework for [Leviathan](../info.md). Widgets,
layout, theming, input, and a damage-driven run loop — with a first-class
testing story: every screen is a byte-stable snapshot you can assert on.

- **Retained tree.** You build a component tree once; Sonar keeps it, tracks
  damage, and repaints only what changed.
- **Safe by default.** No `null`, no truthiness, value-type geometry; the raw
  byte `Block` and terminal control are the gated surfaces.
- **Testable by construction.** The same `TestRenderer` + `ScriptedInput`
  harness the framework tests itself with ships in the package, so *your* app
  snapshot-tests the same way (see [Testing your app](#testing-your-app)).

Package version `0.1.0` (pre-1.0: minor bumps may break). Backend coverage:
oracle, IR, and LLVM run the full loop byte-identically; emit-C++ compiles the
non-loop widget surface (`App.run()`/pump paths are async and skip that lane).

## Quickstart

```lev
uses Sonar;

void main() {
    App a = App().title("hello");
    a.add(Text().text("Hello, Sonar!  Press q to quit."));
    a.keymap().bind("q", () => a.quit());
    a.run();
}
```

```toml
# trident.toml
name = "hello"
entry = "main"
sources = ["main.lev"]

[[dep]]
path = "path/to/sonar"
as = "Sonar"
```

```sh
trident run
```

The runnable version lives in [`examples/hello`](examples/hello); every example
is also a differential test (see below).

## Concepts

### The retained tree

An `App` is the root `Container`. You `add` components (or nest `Container`s:
`SplitBox`, `GridBox`, `ContentBox`, `Tabs`, `Menu`, ...). Each component owns a
`box` (its arranged `Rect`) and a `Constraint` per axis. Layout is a strategy on
the container (`FlexLayout`, `GridLayout`, `DockLayout`, `StackLayout`); the root
defaults to a vertical flex. To fill an axis, opt a child in with
`c.setHeight(Constraint::Flex(1))` — stretch is explicit, not automatic.

### Damage & the frame

A component that changes calls `invalidate()` (content) or `invalidateLayout()`
(size). The run loop coalesces damage and, each frame, re-measures the dirty
subtrees, paints only what is damaged into a `Surface` (a flat cell grid), and
hands the `Surface` to an `IRenderer` that diffs it against the previous frame
and emits the minimal escape stream. One `sysWrite` per frame.

### Mixins

Behaviour is composed from small bases via Leviathan's multiple inheritance:
`Focusable` (tab stop + cursor), `Styleable` (theme resolution), `Bordered`,
`Scrollable`. A widget is `class Button : Focusable, Styleable, ISingleLine`.
Collisions are resolved by the language's `distinct`/collapse rules, so a leaf
that mixes several bases stays one object with one slot per member.

## Component gallery

Snapshots below are the `TestRenderer` two-channel format (channel 1 = text,
`.` = a blank cell), reused verbatim from the examples' goldens — one source of
truth for docs and tests.

**file-manager** — `SplitBox` + `ListView` + preview `ContentBox` + `ContentBar`:

```
README.md....│┌─preview──────────┐
main.lev.....││name.=."demo".....│
trident.toml.││..................│
notes.txt....││..................│
.............│└──────────────────┘
file:.trident.toml..........q:quit
```

**dashboard** — `GridBox` of bordered `ContentBox` tiles:

```
┌─CPU──────┐.┌─Memory──┐
│ok........│.│ok.......│
│..........│.│.........│
└──────────┘.└─────────┘
```

See [`examples/`](examples) for the full set: `hello`, `form-wizard`,
`file-manager`, `log-viewer`, `dashboard`.

## Theming

Styles come from an `ITheme` bound via Leviathan DI, resolved most-specific
first (instance override → exact key → key-minus-state → component base →
`"default"`). Built-ins ship as TOML assets: `Theme::Default()` (empty /
terminal-native), `Theme::Dark()`, `Theme::Light()`, `Theme::HighContrast()`.
Swap at runtime with `installTheme(Theme::Dark())`. Author your own from TOML:

```toml
[input.focused]
fg = "brightWhite"
bg = "blue"
attrs = "bold"
```

Every state key must carry a non-color `attrs` channel (the a11y rule), so the
UI reads on monochrome and color-blind terminals.

## Testing your app

The harness Sonar tests itself with is shipped so downstream apps snapshot-test
the same way. `TestRenderer` (in the package) records frames; `ScriptedInput`
and the `SonarTest` vocabulary (`tests/harness/`) drive them:

```lev
uses Sonar;                     // TestRenderer ships in the package

App a = SonarTest::harness(20, 3);    // binds TestRenderer + ScriptedInput + Default theme
a.add(Text().text("hi"));
SonarTest::snapText(a, SonarTest::renderer(), "greeting");   // prints the golden
SonarTest::input().key("Tab");                                // queue a chord
SonarTest::pump(a, SonarTest::input());                       // deliver + render
SonarTest::reset();                                           // tear down between cases
```

- **Golden = program output.** A test is a program whose stdout matches an
  `.expected` file (house corpus style); `snap`'s output *is* the golden.
  Regenerate by re-running with output redirected (`tests/regen.sh`).
- **Two channels.** `snapshot()` prints text *and* style; `textOnly()` prints
  text alone so behaviour assertions survive theme tweaks.
- **Deterministic input.** `ScriptedInput` queues chunks and delivers one per
  pump, so input and frames interleave deterministically. `key("^S")` /
  `key("Down")` encode chords to the exact bytes the decoder reads back.
- **Differential.** Run everything across engines with
  [`tests/runtests.sh`](tests/runtests.sh) (oracle + IR + LLVM against one
  shared `.expected`; emit-C++ compile-only).

Include the harness in your test's manifest:

```toml
sources = ["main_test.lev", "path/to/sonar/tests/harness/scripted_input.lev",
           "path/to/sonar/tests/harness/assert.lev"]
```

## Layout of the package

```
src/            widgets, layout, theming, input, run loop, renderer
src/test_renderer.lev   the recording renderer (shipped, DI-selected)
themes/         built-in TOML themes
tests/          probes + per-track golden corpus + the harness
tests/harness/  ScriptedInput + the SonarTest assertion vocabulary
tests/runtests.sh   the differential engine-matrix runner
examples/       five runnable apps, each also a differential test
```

See [CHANGELOG.md](CHANGELOG.md) for the release log and
[`../designs/complete/`](../designs/complete) for the per-track design docs.
