# Sonar bugs — live reports

Working log of user-reported Sonar/Recon runtime issues. One entry per report:
raw symptom as told to the agent, then a quick (not deep-dive) investigation,
and a ruling on which layer is at fault (Recon app code / Sonar framework /
Leviathan compiler-runtime).

---

## #1 — Recon window only 10 rows tall; width is correct; Tab cursor misplaced

**Reported (2026-07-14):** Running the native `recon` binary (`~/code/recon`),
the app renders in a window that is always only ~10 rows high regardless of
the real terminal size. Width tracks the terminal correctly. Pressing Tab
does move a cursor, but it lands in the wrong place. `^Q` behavior/quit-flow
was reported separately and is not covered by this entry.

**Investigation (quick pass, not exhaustive):**

- Terminal-size floor itself is fine: `sysWinSize`/`ioctl(TIOCGWINSZ)` →
  `term::size()` (`src/Resolver.cpp:2810`) → `AnsiRenderer.size()`
  (`sonar/src/ansi_renderer.lev:46-49`) correctly maps `WinSize{rows,cols}`
  to Sonar's `Size(w=cols, h=rows)` — this is exactly the axis mapping the
  terminal-floor design calls out as "the consumer's job" and warns not to
  "fix" (`designs/complete/techdesign-terminal-floor.md:15`), and it's done
  correctly here.
- `App.startSession()` (`sonar/src/app.lev:172-180`) takes that real `Size`
  and calls `arrange(Rect(Point(0,0), Size(sz.w, sz.h)))` against the full
  detected terminal size — so the top-level arrange call does receive the
  real height every time. The bug is downstream, in how the middle child
  consumes that height.
- Root cause: **`Recon.ReconApp.build()`** (`examples/recon/src/ui/reconapp.lev`)
  composes the root as `FlexLayout(Axis::Vertical)` with three children —
  `topBar` (`setHeight(Constraint::Fixed(1))`), the middle `outer SplitBox`,
  and `bottomBar` (`setHeight(Constraint::Fixed(1))`) — but **never calls
  `outer.setHeight(Constraint::Flex(1))`**. `Component`'s default
  `Constraint()` (`sonar/src/geometry.lev:107`) has `flex = 0`, so on a
  Vertical `FlexLayout`, height is the *main* axis and a child with no
  concrete size and no flex share falls through to
  `flex.lev:117` — `clampAxis(mc, pass1Main[i])` — i.e. it's sized to its
  own **measured/intrinsic desired height**, not stretched to fill the
  remaining space. Width, by contrast, is the *cross* axis for a Vertical
  layout, and `align_` defaults to `Align::Stretch`, which unconditionally
  fills cross-axis size (`flex.lev:145`) — exactly why width "just works"
  while height doesn't.
  - This matches Sonar's own convention elsewhere: every place in the
    codebase (tests, the C11 template golden, `RequestPanel`'s own
    `urlInput.setWidth(Constraint::Flex(1))`) that wants a child to fill its
    axis sets `Constraint::Flex(1)` on it explicitly — it's opt-in by
    design, and `ReconApp.build()` simply never opts `outer` in on height.
  - A ~10-row total (1 topBar + 1 bottomBar + ~8 rows of SplitBox intrinsic
    content) is consistent with this: the SplitBox's own children
    (sidebar tree / request+response panels) have some small nonzero
    desired height from their own content, and that's what leaks through
    as the "fixed" 10-row window instead of "whatever's left."
  - Fix shape (not applied — reporting only, per instructions): add
    `outer.setHeight(Constraint::Flex(1));` alongside the existing
    `outer.setAxis(...)`/`outer.setRatio(...)` calls in
    `examples/recon/src/ui/reconapp.lev`'s `build()`.
- **Tab/cursor-placement issue:** not independently root-caused in this
  quick pass. Plausibly a downstream symptom of the same layout bug — every
  widget in the squished middle SplitBox is already sitting in the wrong
  rect, so focus/cursor placement computed against those rects would also
  look wrong — but that's a hypothesis, not confirmed. Worth re-checking
  after the height fix lands before treating it as a separate bug.

**Ruling: Recon-side bug**, in `examples/recon/src/ui/reconapp.lev`
(`build()`), not Sonar or Leviathan. Sonar's flex/stretch machinery and the
terminal-size floor are both behaving exactly per their documented contract;
Recon just didn't opt the middle panel into filling the vertical axis.

**Fix applied (2026-07-14):** added `outer.setHeight(Constraint::Flex(1));`
in `examples/recon/src/ui/reconapp.lev`'s `build()`. Verified: recon corpus
11/11 both lanes; native binary rebuilt and smoke-tested in a real 40-row
pty — now paints up to row 33 (previously capped near row 10). Landed on
`agent1`/`master` (`3d06800`), and applied to the vendored `~/code/recon`
copy with its binary rebuilt in place.

---

## #2 — Input cursor renders at the wrong screen position

**Reported (2026-07-14):** After the #1 layout fix, the terminal cursor is
still "off" — visibly landing away from the character being edited. The
user also reports that pressing Enter can leave you "able to type on the
side of the screen," i.e. the visible cursor (and the apparent insertion
point) shows up somewhere far from the focused widget.

**Investigation (quick pass):**

- `App.renderFrame()` (`sonar/src/app.lev:461-471`) computes the final
  on-screen cursor as `Point(leaf.box.x() + p.x, leaf.box.y() + p.y)` where
  `p = leaf.cursorPos()` — the comment right above it ("cursor: focused
  leaf's cursor, shifted into screen space") makes the contract explicit:
  `cursorPos()` is expected to return a point **relative to the widget's own
  box**, which `app.lev` then shifts into absolute screen coordinates by
  adding `leaf.box.x()/y()`.
- Recon's own hand-written `TextArea.cursorPos()`
  (`examples/recon/src/ui/textarea.lev:185-188`) follows that contract
  correctly: `return Point(this.curCol, this.curLine - this.scrollY);` —
  purely content-relative, no box offset added.
- Sonar's shipped `Input.cursorPos()` (`sonar/src/components/input.lev:175-181`)
  does not:
  ```
  Point? cursorPos() {
      if (!focused || !visible()) return None;
      Rect content = box.inset(this.chrome());
      ...
      return Point(content.x() + cellPos, content.y());
  }
  ```
  `box` here is already the widget's **absolute** on-screen rect (the same
  `box` that `app.lev` reads via `leaf.box.x()`), so `content.x()` is already
  an absolute screen column. `Input.cursorPos()` returns that absolute point
  directly, violating the box-relative contract. `app.lev` then adds
  `leaf.box.x()/y()` **again** on top of it — a double-offset that pushes the
  reported cursor an extra `(box.x(), box.y())` to the right/down of where it
  should be. For a widget like the URL `Input` sitting well to the right of
  the sidebar, that's easily enough to land the cursor off the edge of the
  widget or even off-screen — matching both "cursor is off" and "typing
  appears to happen on the side of the screen."

**Ruling: Sonar bug**, in `sonar/src/components/input.lev`'s `cursorPos()`.
It returns an absolute point instead of a box-relative one, breaking the
contract `App.renderFrame()` and Recon's own `TextArea` both correctly
assume. Not Recon, not Leviathan.

**FIXED (2026-07-15, commit e02678b):** `cursorPos()` now returns
`Point(content.x() - box.x() + cellPos, content.y() - box.y())` — box-relative,
so `App.renderFrame`'s box-offset add lands the caret correctly.
`tests/components/basic` strengthened to move the cursor to a non-zero column
(box origin (2,3), cursor 3 cells in → reports x=3, y=0, not the old absolute
5,3). All 19 sonar tests pass on oracle + IR.

---

## #3 — `TLS handshake:  (host 'www.google.com', fd -1)` sending a GET over HTTPS

**Reported (2026-07-14):** Sending a request to `https://www.google.com`
from the compiled Recon app fails immediately with
`TLS handshake:  (host 'www.google.com', fd -1)` (note the double space —
the reason string is empty).

**Investigation (quick pass):**

- Recon's send path calls `std::HttpClient.requestTls(...)`
  (`examples/recon/src/net/sender.lev:94-97`) — this is Leviathan's own
  prelude `std::` HTTP client, not Recon or Sonar code.
- `HttpClient.requestTls` (`src/Resolver.cpp:2334-2356`) does:
  ```
  int fd = std::sysTcpConnect(host, port);
  ...
  std::tlsConnect(fd, host, "", "", 0, (cfd) => { ... });
  ```
  — it never checks `fd` for failure before handing it to `tlsConnect`.
  `sysTcpConnect` is documented to return `-1 on failure`
  (`src/Resolver.cpp:1417`).
- `tlsConnect` (`src/Resolver.cpp:1727-1734`) arms a TLS session on that fd;
  when `armed < 0` it throws
  `RuntimeException("TLS handshake: " + std::sysTlsError(fd) + " (host '" + host + "', fd " + fd + ")")`.
  With `fd == -1`, `sysTlsError(-1)` has no real handshake state to report a
  reason for, hence the empty reason string in the message. The `fd -1` in
  the error text is the same `fd` `sysTcpConnect` returned — i.e. the
  **plain TCP connect itself failed**, and that failure is being reported as
  a confusing TLS-specific error instead of a clear connect failure.
- Not chased further (house rule: framework/example work stops at
  `src/**`), but worth flagging: outbound network access works fine from
  the shell in this environment (`curl -sS https://www.google.com` → `200`
  during this investigation), so *why* `std::sysTcpConnect` itself returns
  -1 for the compiled Leviathan process is still an open question — it may
  be a real native-floor bug (DNS resolution, connect logic) distinct from
  the message-formatting issue described above.

**Ruling: Leviathan bug** (compiler prelude, `src/Resolver.cpp`'s
`std::HttpClient`), not Sonar or Recon. Filed as `known_bugs_1.md #72`.

**FIXED (2026-07-15, commit e02678b):** `requestTls` now checks `fd < 0`
before `tlsConnect` and throws a clear `TCP connect failed (host, port)`
instead of the empty-reason `TLS handshake:  (... fd -1)`. Verified on oracle
+ IR; `run_tls.sh` fully green. NOTE: the *diagnostic* is fixed; the open
question of *why* `sysTcpConnect` returns -1 for the compiled process while
the shell reaches the host fine (§ above) is a separate native-floor concern,
still tracked in `known_bugs_1.md #72`.

---

## #4 — Leaf component paint leaves STALE glyphs when its content shrinks (default/empty theme)

**Found (2026-07-14):** while building the Track 10 `file-manager` and
`log-viewer` examples. Updating a `Text`/`ContentBar` in place with a
*shorter* string (e.g. selecting a file so the preview goes from
`"# Project readme"` → `"name = \"demo\""`, or a status filename
`"README.md"` → `"main.lev"`) leaves the tail of the previous, longer
string on screen: the preview shows `void main() { }e` (a stray `e` from
`readme`), and the status shows `Rmain.lev` (the `R` from `README.md`).

**Investigation (quick pass):**

- The frame is damage-driven (`sonar/src/damage.lev`
  `paintDamagedFrom`): a `dirty()` leaf repaints *within its own box
  clip*, but the repaint is only `paintBackground(s)` + `paintContent(s)`.
- `paintContent` writes its text/segments directly and **never clears the
  cells it is about to leave blank** — e.g. `ContentBar.paintContent`
  (`sonar/src/components/contentbar.lev:18-44`) `writeText`s the left /
  center / right segments only, and `Text.paintContent` writes wrapped
  lines only. The rest of the leaf's box keeps whatever cells the previous
  (longer) content wrote.
- The one thing that WOULD clear those cells is
  `Styleable.paintBackground` (`sonar/src/mixins.lev:95-112`), but it fills
  **only** when a theme is installed AND the resolved `background` bg is
  non-`Default` (`mixins.lev:106`). Under the built-in **Default theme
  (intentionally empty) or no theme at all**, `hasFill` is false / the bg
  is `Default`, so nothing is cleared. A themed non-default background
  masks the bug (the fill repaints the whole box each frame).
- So the stale cells are a function of paint history, not current state —
  any leaf whose content can shrink or move (status bars, previews,
  spinners, live counters) is affected on the default theme.

**Ruling: Sonar framework bug**, in the leaf paint contract
(`Component`/`Styleable.paintBackground` + every `paintContent`). A
`dirty()` leaf repaint must clear its whole box to the resolved background
before painting content — e.g. `paintBackground` should fill the box with
spaces at the resolved style unconditionally (not only when the themed bg
is non-default), or each `paintContent` should `fill` its content rect
first. Not Leviathan, not the example. **Filed, not fixed here**, per the
standing rule that example/framework-delivery work (Track 10) does not
patch `sonar/src/**` component internals as a side effect — the fix
touches the shared paint path of every component and wants its own change +
full differential re-run. **Workaround in the examples:** pad mutable
fields to a fixed width so each repaint fully overwrites the previous one
(used in `sonar/examples/file-manager`), which also matches how real
status/field UIs are laid out. `docs/footguns.md` debt row added.
