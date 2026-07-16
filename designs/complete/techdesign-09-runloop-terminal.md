# Sonar — Tech Design 09: App, Run Loop, Terminal Integration

**Status:** design, pre-implementation. **Date:** 2026-07-12. **Track:** T09.
**Owns:** `sonar/src/{app,runloop,terminal,ansi_renderer,cursor,log}.lev`.
**Depends on:** T01 (core/damage/Surface), T03 (decoder/dispatch/focus). Language: **F2** (winsize + SIGWINCH — resize; a polling interim is specified), **F1** (`Block.mismatch` — diff performance; a per-cell loop interim works), landed floor (raw mode `term::enableRaw`, `sysWatch`/`sysRead` byte-clean stdin, `sysTimerStart`, tasks/await, `using`/IDisposable, `env.setExitCode`).
**Gates:** G-S1 (minimal loop) → G-v1 (polish). **Difficulty:** L, risk MED (the SGR-minimizing emitter and teardown-on-every-path are the risk).

Conforms to anchor: C9 (App/Keymap surfaces + frozen frame phases), C5 (IRenderer/IInputSource), R3 (namespace global app), R8 (renderer owns the diff), R13 (overlay stack), R16 (log ring), C6 (cell format — the renderer is its only external reader).

**Engine lane matrix (stated per anchor):** run lanes = oracle/IR/LLVM; **emit-C++ compiles the whole package but `App.run()` is unavailable there** (no event loop in that lane) — the package's non-loop surface stays emit-C++-clean so libraries built on Sonar widgets can still target it.

---

## 1. `App` — construction, configuration, global

```lev
class App : Container {
    new App() {
        // R3: registers itself — bare write inside the namespace
        currentApp = this;      // throws SonarException if one already exists (single-app rule)
        setLayout(FlexLayout(Axis::Vertical));
    }
    // fluent config (pre-run only; post-run reconfig throws): title, altScreen(true),
    // mouse(false), bracketedPaste(true), fpsCap(60), wideGlyphs(Standard), onResize(h)
}
```

Single-app rule: a second `App()` while one exists throws (tests use `__sonarResetApp()` — a package-internal teardown helper T10 sanctions). `App` existing ≠ running: components may attach (and flush `@Shortcut`/`@Timer` registrations, T07 §5.5) before `run()`.

## 2. `run()` — the loop, phase by phase

`run()` blocks the calling task until `quit()`; internally:

```
acquire:  using TerminalSession session = TerminalSession(renderer, altScreen, mouse, paste);
          // ctor: enableRaw, alt-screen on, mouse on, paste on, hide cursor
          // close(): reverse order, UNCONDITIONALLY — 'using' runs on every exit edge
size:     surface = Surface(screen()); root box = full screen; full invalidateLayout
subscribe: inputSource.start(bytes => decoder.feed(bytes));       // sysWatch(0) inside StdinSource
          resize = signal::on(signal::WINCH) → onWinch()          // F2; interim: 200ms poll timer
          frame scheduling per §3
loop:     the language event loop IS the loop — run() awaits a quit Promise;
          everything else is callbacks (watches/timers) on the same single-threaded dispatcher
quit:     resolve the quit promise → run()'s await resumes → 'using' teardown → return
```

**Terminal teardown is the non-negotiable:** `TerminalSession : IDisposable` and `using` guarantee restore on fallthrough, return, throw-unwind (uncaught handler exceptions are caught at dispatch per T03; a genuinely uncaught escape from run() still unwinds through `using`). Sequence on close: cursor show → mouse off → paste off → alt-screen off → raw restore (`term` restore is ALSO wired at the language level on hard exits — the F2 safety net for SIGTERM/HUP; both layers stated so neither is "someone else's job"). A crashed TUI that wrecks the shell is a framework bug — this section is the fix's spec.

**Ctrl-C policy:** raw mode delivers `0x03` as a key (Ctrl+C KeyEvent). Default: unhandled Ctrl+C quits (an App-level fallback-tier keymap bind installed by default, removable via `keymap().unbind`). External `kill -INT` (F2 `signal::on(INT)`) also quits cleanly. Both spelled so "how do I make Ctrl-C not quit" has a documented answer.

## 3. Frame scheduling (damage coalescing)

State: `bool framePending`. `invalidate`/`invalidateLayout` (via a Component→App damage notification: the root's `childDamage_` bubble terminates at App, which calls `scheduleFrame()`) sets `framePending` and arms a one-shot timer at `max(0, lastFrameStart + 1000/fpsCap − now)` — i.e. immediate when idle, coalesced to the cap under load. Idle app: no timer armed, blocked on stdin/signal watches, **zero CPU** (the anchor's idle guarantee).

Frame execution = the frozen C9 phases:

```
input   — (already happened: events ARE the wakeups)
layout  — collect highest layoutDirty_ roots (T01 damage.lev); measure+arrange each
sweep   — collect dirty components (T01)
paint   — for each dirty component: s.pushClip(itsBox ∩ ancestors); c.paint(s); popClip
          then overlays in stack order (OverlayHost subtrees)
present — renderer.present(surface)
cursor  — f = focused(); renderer.setCursor(f == None ? None : f.cursorPos() shifted to screen)
stats   — frame counters for Sonar::frameStats()
```

Re-entrancy rule (frozen): handler-driven tree mutations during a frame mark damage for the NEXT frame; `scheduleFrame` during a frame is legal and coalesces. Layout/paint code invalidating (the T02 forbidden case) is a programming error surfaced by a debug-mode flag (`__sonarInFrame` assertion counter → throw).

Timers: `App.every(intervalMs, cb) -> int` / `cancelEvery(token)` wrap `sysTimerStart` with token bookkeeping in an App registry; ALL cancelled at teardown (leak/lifecycle hygiene — T07's detach discipline handles per-component cancels; App handles process-level). `Sonar::nowMs()` (T05's double-click dependency) = the loop clock via `sysNow` — exposed here.

**Resize:** WINCH tick (or poll-interim detecting a size change) → `term::size()` → `surface.resize(w,h)` → root `invalidateLayout` + full-screen damage + `onResize` handlers → scheduleFrame. Multiple WINCHes coalesce (F2's stream semantics + framePending). The poll interim (F2 not landed): a 200ms `every` comparing sizes — spec'd, marked deletable at F2.

## 4. `StdinSource` (the production IInputSource)

`start(onBytes)`: `sysWatch(0, fd => { var chunk = sysRead(0, 4096); if (chunk.length() == 0) { /* EOF: quit? */ } else onBytes(chunk); })` — byte-clean strings (landed). EOF on stdin (terminal closed): treated as quit (loud log). `stop()`: `sysUnwatch(0)`. The decoder's ESC-timeout need (T03): StdinSource owns no timers; App arms the decoder-requested flush timeout (`decoder.pendingTimeoutMs()`) after each feed — the seam T03 froze.

## 5. `AnsiRenderer` (R8 — owns prev frame + diff + escape emission)

State: `Block prev` (same cell format, renderer-owned), `bool prevValid` (false ⇒ full repaint), current SGR state + cursor position registers (emission-time caches).

`present(Surface s)`:
1. `prevValid == false` → emit home+clear, write every cell, copy cells→prev, done.
2. Else scan rows; per row, find difference runs: `i = cells.mismatch(prev, rowStart)` (F1) then extend the run while cells differ (interim pre-F1: per-cell compare loop — same structure, slower; the swap is one function).
3. Per run: emit cursor move (CUP `\x1b[r;cH`) **only if** the emission cursor isn't already there (tracking register); emit SGR **only on style change** from the emission SGR register (the minimizer: diff the packed style bytes; changed fg → `38;5;n`/`39`, bg likewise, attr set changes → rebuild via `0;...` when bits turn OFF, incremental `1;4;...` when only turning ON — the classic monotonic trick, spelled in a table); emit the run's scalars UTF-8-encoded (continuation cells emit nothing — the wide-glyph rule; a run starting ON a continuation cell backs up one column to re-emit the lead cell — the boundary rule that prevents torn glyphs).
4. Copy changed runs cells→prev (F1 `blit` per run); flush the emit buffer (array-of-strings → one `joinToString` → one `sysWrite` — single syscall per frame).

Colors: 16-color SGR (30–37/90–97, 40–47/100–107) from the Color carriers; `Default` → 39/49. (Truecolor rides the C6 v2 note, not here.)

`acquire/release`: the escape set (alt `\x1b[?1049h/l`, SGR mouse `\x1b[?1006h + ?1002h`, paste `\x1b[?2004h/l`, cursor hide/show `\x1b[?25l/h`), raw mode via `term::enableRaw` + restore. `release()` idempotent (guard flag) — called by `using` teardown AND available to panic paths.

`setCursor(Point?)`: None → hide; some → show + CUP. Emitted AFTER present (frozen phase order) so the hardware cursor lands on the focused Input's cell.

`bell()`: `\x07`.

## 6. `Sonar::log` + stats (R16)

A namespace ring buffer (`Array<string>`, cap 200, rebind-rotate) + `Sonar::frameStats() -> FrameStats` (`struct FrameStats { int frameMs; int dirtyComponents; int cellsChanged; int frames; }`). `log` never touches the terminal (that's the point); DebugOverlay (T05) renders both. `console.write` during a session corrupts the screen — stated in docs; a debug-mode stderr tee (`SONAR_LOG_STDERR=1` env check at App ctor) gives headless debugging.

## 7. Milestones

| M | contents | difficulty | gates |
|---|---|---|---|
| M1 | TerminalSession + acquire/release + StdinSource + minimal loop (full repaint every frame, no diff) — "hello TUI" runs | M | T01/T03 |
| M2 | frame scheduler + damage-driven paint + phase assertions | M | M1 |
| M3 | AnsiRenderer diff + SGR minimizer + run coalescing (per-cell interim) | L | M2 |
| M4 | resize (poll interim → F2 swap), Ctrl-C policy, timers/every, nowMs | S/M | M2; F2 |
| M5 | F1 mismatch/blit swap-in + single-write flush + stats/log/DebugOverlay feed | S | F1 |
| M6 | teardown torture (throw paths, double-release, EOF, panic tee) | M | M1–M5 |

## 8. Potential issues & mitigations

1. **SGR minimizer correctness** (the classic class of bugs: stale attr assumed off). Mitigations: the attrs-turning-OFF ⇒ full `0;`-rebuild rule (never try to negate individual attrs — half the SGR-off codes are nonstandard); golden escape-stream tests (T10 asserts exact bytes for scripted damage); a `SONAR_DUMB_RENDER=1` full-repaint-no-minimize mode for bisecting visual bugs — shipped, documented.
2. **Torn wide glyphs at run boundaries** — the back-up-one-column rule (§5.3); tested with CJK-at-boundary goldens.
3. **Teardown on every path** — `using` covers run()'s frame; the language-level restore (F2) covers signals; the remaining hole is `kill -9` (unfixable, documented). Double-release guarded.
4. **Watch/timer keep-alive vs quit** — the language loop keeps the process alive while watches exist; quit must unwatch stdin, cancel all timers, close signal streams, or the process never exits. The teardown checklist is normative (§2's quit line + App timer registry) and has a dedicated test (process exits within 100ms of quit).
5. **fpsCap starvation under handler storms** — a handler that invalidates every event coalesces to fpsCap by design; but a handler LOOP (invalidate → frame → handler invalidates again) spins at cap forever. Mitigation: DebugOverlay's frames counter makes it visible; docs name the anti-pattern (mutate state in frames, not in reaction to frames).
6. **`screen()` before acquire** (size queries pre-run) — returns the F2 `term::size()` best-effort (24×80 default); documented.
7. **bug #35 adjacency** — the quit Promise is awaited on the main task, resolved from callbacks on the same thread; no spawn crossing, no exposure. Stated so nobody "optimizes" quit onto a worker.

## 9. Testing plan

TestRenderer-based: phase-order assertion harness (instrumented component records measure/arrange/paint/present sequence); coalescing test (N invalidates → 1 frame); idle test (no damage ⇒ no frames over a scripted 500ms); resize script; teardown tests (§8.3/§8.4 — incl. a throw from a handler mid-frame: session released, terminal restored, exception reported); escape-stream goldens for AnsiRenderer (scripted damage → exact byte expectations, incl. SGR minimize cases + wide-glyph boundary); Ctrl-C default + unbind; EOF quit. Differential oracle/IR/LLVM; emit-C++ compile-only lane test (package compiles, run() untested there).

## 10. Open questions

1. `NO_COLOR`/`TERM=dumb` — strip SGR colors in AnsiRenderer when set? (Leaning yes, attrs kept; needs `env` read at acquire.) With T08.
2. Synchronized-output escapes (`\x1b[?2026h` begin/end frame) on supporting terminals — v1.1 nicety, trivially additive at present() edges.
3. Mouse move (no-button) reporting — v1 uses 1002 (button-motion only); full 1003 opt-in later.

## 11. Implementation log

- 2026-07-12 — design written; not started.
- 2026-07-13 — **LANDED IN FULL (M1–M6).** `sonar/src/{app,runloop,terminal,ansi_renderer,cursor,log}.lev`:
  - **terminal.lev** — `IRenderer` (C5) + `TerminalSession : IDisposable` (the `using`
    acquire/release bracket, idempotent close).
  - **cursor.lev** — the escape vocabulary (CUP/alt/mouse/paste/cursor/clear/bell) + the
    16-colour SGR encoders + the **SGR minimizer** `sgrTransition` (attrs-turning-OFF ⇒ full
    `0;`-rebuild; incremental additions otherwise; `noColor` strips colours, keeps attrs).
  - **ansi_renderer.lev** — `AnsiRenderer : IRenderer` (R8): full-repaint + per-row mismatch-driven
    diff with run coalescing, wide-glyph continuation/back-up-one boundary rule, one SGR reset per
    frame keeping `emitStyle_` in lockstep, single `sysWrite` per frame (via a `StringBuilder`), a
    `__capture` seam for golden-byte tests. `mismatch`/`blit` (F1) used directly.
  - **runloop.lev** — `StdinSource : IInputSource` (sysWatch(0) byte-clean, EOF→quit).
  - **app.lev** — `App : Container, ILifecycleHost` with the frozen C9 frame phases, damage-coalesced
    `scheduleFrame`, decoder wiring + ESC-timeout arming, key/mouse/paste dispatch through T03,
    Tab focus (re-derived from the tree post-dispatch), default `^C`/external-INT quit, `every`/
    `cancelEvery` timer registry, overlay stack (R13), signal/poll resize, `pumpOnce` test hook,
    the R3 `currentApp`/`app()` globals, and unconditional `stopSession` teardown.
  - **log.lev** — `FrameStats` + `frameStats()` + the renderer `cellsChanged` counter (`log()`
    itself stays in events.lev; the SONAR_LOG_STDERR tee drains the ring to fd 2 from App).
  - **Engine lanes:** run lanes oracle/IR/**LLVM** all byte-identical for the run loop, input/focus/
    keymap dispatch, timers, teardown (`sonar/tests/{runloop,loop,teardown}`) and for the
    `AnsiRenderer` diff/SGR emitter over a directly-built Surface. The full frame pipeline over
    *drawing* components (`sonar/tests/frame`) is oracle/IR-only — see the two bugs below. emit-C++
    compiles the non-loop widget surface (verified against `tests/components`); a program reaching
    `App.run()` pulls loop/task natives emit-C++ lacks, exactly as §10's lane matrix states.
  - **Bugs found + filed.** **bug.md #67 [P0]** — `Surface.fill`/`put`/`writeText` segfaults on LLVM
    when the enclosing `paint()` is reached via **interface dispatch** (`Container.paint`→child); a
    nested `Size.w` read miscompiles to a dynamic member fetch. No landed test had ever painted a
    *drawing* component through the container hierarchy, so T09 is the first to hit it; it blocks the
    LLVM lane for component painting (only). The `App` **root** background fill sidesteps it with a
    fresh `Rect`; child components remain #67-blocked on LLVM until the compiler fix. **bug.md #68
    [P2]** — `env::get` (`sysEnv`) fails LLVM codegen as a non-inlined package call, so the
    `SONAR_DUMB_RENDER`/`NO_COLOR`/`TERM`/`SONAR_LOG_STDERR` toggles are exposed as **setters**
    (`AnsiRenderer.__setDumb`/`__setNoColor`, `App.logToStderr`) rather than env reads — the app's
    own `main()` reads the vars (where `env::get` inlines) and forwards them.
  - **Deviations from the spec surface:** renderer/input are lazy-defaulted and injected via test
    setters (`__useRenderer`/`__useInput`) rather than DI `bind` (avoids footgun #56). The
    `__sonarInFrame` re-entrancy throw is a recorded `inFrame()` flag only (the assertion belongs in
    T01/T02 `invalidate`, which T09 does not own). `debugOverlay(bool)` stores a flag; the actual
    overlay component is T05's. Overlay focus save/restore is minimal (push/pop + input routing +
    paint) pending T05. `wideGlyphs` takes a required arg (enum-valued default params are rejected by
    the checker).
