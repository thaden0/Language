# Sonar — Tech Design 10: Testing, Examples, Delivery

**Status:** LANDED IN FULL 2026-07-14 (M1–M6). **Date:** 2026-07-12. **Track:** T10.
**Owns:** `sonar/src/test_renderer.lev`, `sonar/tests/**`, `sonar/examples/**`, `sonar/trident.toml`, `sonar/README.md`, `sonar/CHANGELOG.md`.
**Depends on:** every track (it tests them); specifically T01 (Surface/cell format), T03 (IInputSource seam), T09 (App.pumpOnce, single-app reset helper). No unlanded language features for the harness itself.
**Gates:** grows with every gate; the harness core lands WITH G-S1 (probes P1–P11 run first). **Difficulty:** M, risk LOW.

Conforms to anchor: C12 (testing seams — TestRenderer/ScriptedInput/pumpOnce/goldens/differential doctrine), C13 (package shape), §8 (probe register — this track executes it), §9 (testing doctrine).

---

## 1. The probe suite (runs FIRST — before T01 implementation)

The anchor's P1–P11 are language-behavior probes gating design assumptions (interface field satisfaction, diamond collapse, distinct methods, struct labeled ctors, negative enum carriers, ...). Each is a ≤15-line standalone `.lev` program with an `.expected` output, living in `sonar/tests/probes/`, run against oracle + IR + LLVM. **A red probe triggers the anchor's named fallback, not improvisation.** T01's design doc carries the probe program sketches; this track owns running them and recording results in a `PROBES.md` results table (probe, engine, green/red, date, fallback-invoked?). Probe results are the first implementation-log entries of the whole framework.

## 2. `TestRenderer` (`sonar/src/test_renderer.lev` — shipped in the package, DI-selected)

```lev
class TestRenderer : IRenderer {
    new TestRenderer(int w, int h);
    // IRenderer: size() => configured; acquire/release record calls; present copies the
    // Surface cells into an internal grid; setCursor records; bell counts.
    string snapshot();            // the two-channel format below
    string textOnly();            // channel 1 only
    Point? cursor();  bool acquired();  int bells();
    void setSize(int w, int h);   // + a resize-notification hook for resize tests
}
```

**Snapshot format (frozen — goldens depend on byte stability):**

```
# 20x4                                  <- header: WxH
┌ channel 1: text ────────────────────
Hello  World........................    <- one line per row; '.' = space cell (visible blanks)
....................................
└─
┌ channel 2: style ───────────────────
AAAAAAABBBBB........................    <- one letter per cell, keyed below; '.' = default style
....................................
└─
A = fg:brightWhite bg:blue attrs:bold
B = fg:default bg:default attrs:reverse
@cursor 6,0                             <- omitted when hidden
```

Rules: wide-glyph continuation cells render `▏` in channel 1 (visible, stable); style letters assigned in first-use order (deterministic given deterministic paint order — which the frame phases guarantee); trailing-blank rows are NOT elided (fixed shape = mechanical diffs). Two channels so text-only assertions (`textOnly()`) survive theme tweaks — the anti-brittleness measure.

## 3. `ScriptedInput` (`sonar/tests/harness/scripted_input.lev`)

```lev
class ScriptedInput : IInputSource {
    ScriptedInput feed(string bytes);         // queue raw bytes (escape sequences verbatim)
    ScriptedInput key(string chord);          // convenience: chord -> encoded bytes ("^S", "Tab", "Down")
    ScriptedInput text(string s);             // printable run
    ScriptedInput splitNext(int at);          // deliver the next queued chunk split at byte N
                                              // (the decoder chunk-boundary fuzz hook, T03's matrix)
    void start((string) => void onBytes);     // delivers one queued chunk per pump
    void stop();
}
```

Delivery model: **one queued chunk per `App.pumpOnce()`** — deterministic interleaving of input and frames. The chord encoder reuses T03's tables in reverse (owned here to keep T03 pure; drift between encode/decode is itself a test: encode→decode round-trip over the full table).

## 4. The harness pattern (no test framework exists — corpus style)

House corpus style: a test is a program whose stdout matches an `.expected` file, exit code 0. The package adds a tiny assertion vocabulary (`sonar/tests/harness/assert.lev`):

```lev
namespace SonarTest {
    void eq(string got, string want, string label);   // prints "ok label" or "FAIL label" + diff, sets exit code
    void snap(App app, TestRenderer r, string label); // prints the snapshot under a label banner
    App harness(int w, int h);                        // binds TestRenderer+ScriptedInput+Default theme,
                                                      // constructs App, returns it (composition root for tests)
    void reset();                                     // tears down the single-app global between cases
}
```

Golden workflow: `snap` output IS the expected file's content — regenerating goldens = re-running with output redirected (a `regen.sh` in tests/; goldens reviewed in diffs like any code). Layout/unit tests (geometry, constraint math, decoder tables, theme chains, TOML errors) are plain print-and-expect programs — table-driven via arrays of case structs.

**Differential doctrine:** every test runs on oracle + IR + LLVM with ONE shared `.expected` (byte-identical across engines — the ecosystem's standing rule); emit-C++ gets a compile-only lane (the package minus `run()`/pump paths compiles). A `runtests.sh` mirrors the compiler repo's corpus-runner conventions (engine matrix × test list, fail-fast off, summary table).

## 5. Test inventory by track (the coverage contract)

| area | tests (each a golden or table program) |
|---|---|
| T01 | geometry tables; cell encode/decode; clip algebra; damage bubble/sweep traces; wide-glyph put invariants (no half-glyphs); detach-discipline walk (R7) |
| T02 | every worked example from T02 §3 as a table test; flex remainder/clamp fixpoint; dock walk; grid spans; overflow boxes |
| T03 | decode table goldens; split-chunk fuzz (every sequence × every split via `splitNext`); dispatch order traces; focus ring; keymap chords + duplicate-throw |
| T04 | per-component × per-state snapshots; Input editing scripts (byte script → value+cursor); wrap tables; validation matrix |
| T05 | virtualization proof (counting source: itemAt calls == viewport); scroll/select scripts; sort/expand events; menu focus save/restore; Modal open/close ×1000 detach assertions; alert/confirm promise |
| T06 | M0 expansion-string goldens (pre-F4!); post-F4 --expand round-trip + built-tree snapshots; error catalog E1–E9 offsets |
| T07 | shortcut fire/detach-stop; timer tick counts; validator ordering; inherited-override dispatch |
| T08 | resolution-chain tables; TOML goldens + error line/col; theme-switch snapshot; tracing-theme key-drift test |
| T09 | phase-order trace; coalescing; idle (no frames); resize script; teardown torture (mid-frame throw → released); escape-stream goldens (SGR minimize, wide boundary, run coalescing); Ctrl-C; EOF |
| soak | mount/unmount churn (Modal/Tabs) N=10k asserting parent-link nulling + stable log ring; post-F5: the cycle-collection churn program joins the compiler-side corpus (cross-repo note) |

## 6. Example apps (`sonar/examples/` — integration tests AND documentation)

Each: own `trident.toml` (`[[dep]] path = "../.."`), a README, runnable via `trident run`, and a scripted-mode flag (env `SONAR_SCRIPT=1` binds ScriptedInput+TestRenderer and prints snapshots — every example is ALSO a differential test).

1. **`hello`** — App + Text + quit bind. The 15-liner on the README.
2. **`form-wizard`** — Input/CheckBox/RadioGroup/Button + @Validator + Modal confirm; Tab order; the forms showcase.
3. **`file-manager`** — SplitBox: TreeView (lazy dir source over `File`/sys natives) + TableView; BarMenu; @Shortcut; ContentBar status. The enterprise centerpiece.
4. **`log-viewer`** — ListView over a growing source + `App.every` tail polling + follow-mode toggle; ProgressBar/Spinner. The animation/timer showcase.
5. **`dashboard`** — GridBox of ContentBoxes; theme toggle (`Theme::Dark()`/`Light()` at runtime); DebugOverlay on. The theming showcase.
6. **`editor-lite`** (post-G-S4) — the `.sonar` template + `sonar!` + reactivity-ready layout. The template showcase; until G-S4 it exists builder-form.

## 7. Delivery & versioning

`trident.toml` per anchor C13 (sources globs, `themes/**` assets, version `0.1.0`). Semver policy: pre-1.0, minor = anything; the CHANGELOG discipline starts at first tag. README structure: quickstart (hello), concepts (retained tree/damage/mixins in one page each), the component gallery (snapshot blocks generated from examples — goldens reused as docs, one source of truth), theming guide, testing-your-app guide (harness pattern exported: `SonarTest` ships in the package so downstream apps snapshot-test THEIR UIs the same way — an enterprise selling point, stated in README). Docs live in the README + per-doc headers v1; a docs site is out of scope.

## 8. Potential issues & mitigations

1. **Golden brittleness** — one style tweak invalidating 40 snapshots. Mitigations: two-channel format + `textOnly` for behavior tests; style-channel goldens only where the style IS the subject; `regen.sh` + reviewed diffs.
2. **Nondeterminism** (timers, double-click clocks) — pumpOnce-driven tests never free-run; time-dependent behavior tested via injected ticks (`App.every` callbacks invoked by pump in scripted mode — T09 exposes the scripted-clock hook `__sonarAdvanceMs(int)`; flagged as a small T09 addition).
3. **Single-app global vs many test cases per program** — `SonarTest::reset()` (T09's sanctioned teardown); each case builds a fresh harness.
4. **Engine-differential flakes from map ordering** — style-letter assignment and any iteration over Maps must use insertion-order-stable structures (arrays of pairs where order matters) — a standing rule for ALL tracks, recorded here because the snapshot format depends on it.
5. **Example rot** — examples run in CI via scripted mode; a broken example fails the suite, by construction.

## 9. Milestones

| M | contents | difficulty |
|---|---|---|
| M1 | probe suite + PROBES.md (pre-T01!) | S |
| M2 | TestRenderer + snapshot format + assert/harness/reset | M |
| M3 | ScriptedInput + chord encoder + runtests.sh engine matrix | M |
| M4 | per-track inventories as tracks land (rolling) | M (rolling) |
| M5 | examples 1–5 + scripted-mode CI wiring | M |
| M6 | README/CHANGELOG/package polish + 0.1.0 tag checklist | S |

## 10. Open questions

1. Coverage tooling (none exists for .lev) — the inventory table is the manual coverage contract; revisit if the toolchain grows coverage.
2. Perf regression harness (frame-time budgets on the examples) — v1.1; needs stable timing surface first.

## 11. Implementation log

- 2026-07-12 — design written; not started. M1 (probes) is the first executable step of the whole framework.
- 2026-07-14 — **M1–M6 landed.** The framework was already built across T01–T11, so
  this pass delivered the *harness, runner, examples, and package polish* and
  wired the whole thing into one differential runner.
  - **M1** — probe suite + `PROBES.md` were already green (14 probes, all engines).
  - **M2** — `src/test_renderer.lev`: `TestRenderer : IRenderer` (shipped, DI-selected)
    with the frozen two-channel snapshot format (`snapshot()`/`textOnly()`, style
    letters in first-use order, wide-glyph `▏` continuation, fixed-shape rows).
    `tests/harness/assert.lev`: the `SonarTest` vocabulary — `eq`/`snap`/`snapText`/
    `harness`/`pump`/`reset`. `harness()` is the composition root (binds
    `TestRenderer`+`ScriptedInput`+Default theme, returns the App; collaborators
    reached via `renderer()`/`input()`). `reset()` calls `stopSession()` before
    dropping the single-app global — a live repeating resize-poll timer otherwise
    keeps the runtime loop alive past `main()` and hangs a never-`run()` test at exit.
  - **M3** — `tests/harness/scripted_input.lev`: `ScriptedInput : IInputSource`
    (queues chunks, delivers ONE per pump) + the chord encoder (the reverse of
    keymap.lev's `parseChord` + input.lev's decoder tables; `encode→decode`
    round-trips over the full table, proven in `tests/harness_smoke`).
    `tests/runtests.sh`: the engine-matrix runner — oracle + IR + LLVM against one
    shared `.expected`, emit-C++ compile-only (SKIP on the documented async/native
    gap), **LLVM stdout compared alone** (its `[heap]` meter is on stderr, expected).
    Each engine runs with cwd = the test dir (plans hold dir-relative paths).
    `tests/regen.sh` regenerates goldens from the oracle.
  - **M4** — the per-track golden corpus already existed (each track landed with its
    own suite); this pass added `runtests.sh` as the one rolling runner over all of
    them + the examples.
  - **M5** — `examples/{hello,form-wizard,file-manager,log-viewer,dashboard}`, each
    with its own `trident.toml` (dep on `../..`), a README, a `main.lev` that runs
    real via `trident run` and, under `SONAR_SCRIPT=1`, binds the harness and prints
    a snapshot — so **every example is also a differential test** (all 5
    byte-identical on oracle/IR/LLVM). `runtests.sh` exports `SONAR_SCRIPT=1` and
    scans `examples/*` too.
  - **M6** — `README.md` (quickstart, concepts, gallery reusing example goldens as
    docs, theming, the "testing your app" guide) + `CHANGELOG.md` (0.1.0) +
    per-example READMEs. `trident.toml` already carried the sources/assets/version.
  - **Found + filed `sonar-bugs.md #4`** (Sonar framework, not fixed here per the
    standing "T10 doesn't patch `src/**` internals" rule): a leaf's in-place update
    to a *shorter* string leaves stale glyphs under the default/empty theme
    (`paintBackground` fills only for a themed non-default bg). Worked around in
    `file-manager` with fixed-width fields; `docs/footguns.md` debt row added.
  - **One collision fixed:** shipping `Sonar::TestRenderer` in `src/*.lev` collided
    with the local `TestRenderer` that pre-existing `frame`/`loop`/`runloop` tests
    hand-rolled before it existed (under `uses Sonar` the import won, breaking them).
    Renamed those three tests' local helper to `RecordRenderer` — goldens unchanged.
