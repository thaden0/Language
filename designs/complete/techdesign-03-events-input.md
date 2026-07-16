# Sonar — Tech Design 03: Events, Input Decoding, Focus, Keymap

**Status:** implemented. **Date:** 2026-07-12. **Track:** T03.
**Owns:** `sonar/src/{events,input,focus,keymap}.lev`.
**Depends on:** T01 (Component handler lists, tree). **NO language gates** — raw mode, byte-clean `sysRead`, enums, match, narrowing are all landed. (F3 improves USER handler spelling only.)
**Gates:** G-S1. **Difficulty:** M/L. **Risk:** MED — decoder correctness against terminal reality is the risk; the mitigation is the table-driven design + split-chunk fuzz.

Implements anchor C4 (event classes, R1), C5 (`IInputSource`, `IFocusPolicy`), C9 Keymap + chord grammar, R11 (keymap tiers), R12 (tokens), R13 (top-overlay input exclusivity). Byte-clean fact relied on: strings preserve `0x1b`/controls/NULs verbatim (landed `sysRead`).

---

## 1. Event classes (`events.lev`)

Per C4 verbatim (classes — mutable `handled` is the point, R1) plus convenience predicates:
`bool KeyEvent.isChar(char c)`, `bool isCtrl(char c)` (`code==Char && mods==Mod::Ctrl && ch==c`), `bool matches(string chord)` (via the keymap chord parser — the primitive `@Shortcut` reduces to).

## 2. The decoder (`input.lev`) — an incremental, table-driven state machine

**API:** `class InputDecoder { Array<...> feed(string chunk); int pendingTimeoutMs(); Array<...> flushPending(); }` — feed returns completed events (a heterogeneous stream delivered as three typed callbacks in practice: `onKey/onMouse/onPaste` closures set at construction — avoids a union-of-classes return); `pendingTimeoutMs` > 0 when a bare ESC (or ESC-prefix) is buffered and needs a timeout decision; the RUN LOOP owns the timer and calls `flushPending` on expiry (the seam frozen here; T09 wires it; ScriptedInput tests drive it manually).

**States:** `enum DecodeState { Ground, Esc, Csi, Ss3, Utf8, PasteBody }` — transitions via exhaustive `match` (the language's own showcase; omitting a state is a compile error).

**Normative decode tables (the artifact):**
- Ground: printable ASCII → `Char` + ch; `0x0D`→Enter, `0x09`→Tab, `0x7F`/`0x08`→Backspace, `0x1B`→Esc state; other C0 `0x01-0x1A` → `Char` + Mod::Ctrl + the letter (`0x03` = Ctrl+C — raw mode delivers it here, ISIG cleared); `0x80-0xFF` → Utf8 state (2/3/4-byte length from the lead; invalid/overlong/truncated-then-invalid → U+FFFD Char event, never a throw; bytes buffered across chunk splits).
- Esc: `[`→Csi; `O`→Ss3; another byte within the window → **Alt-prefix**: re-dispatch that byte through Ground with Mod::Alt (covers M-x and Alt+arrow's ESC ESC [ A double-escape via recursion depth 1); timeout expiry → bare Escape KeyEvent.
- Csi: accumulate params/intermediates until final byte. Finals: `A/B/C/D`→Up/Down/Left/Right; `H/F`→Home/End; `Z`→BackTab; `~` with first param 1/2/3/4/5/6→Home/Insert/Delete/End/PageUp/PageDown, 15/17/18/19/20/21/23/24→F5–F12; modifier param (second, or `1;M` form): `M-1` bitset 1=Shift 2=Alt 4=Ctrl → Sonar::Mod. `<` prefix → SGR mouse: `<b;x;y M/m` — button = low 2 bits (0/1/2 = Left/Middle/Right), +32 = motion (kind Move, or Drag when a button is held — held-state tracked in the decoder), +64 = wheel (0/1 → WheelUp/WheelDown, kind Press), `M`=Press `m`=Release; coords 1-based → 0-based. `200~`→PasteBody; unknown final → swallow + `Sonar::log` (policy: log-and-drop; garbage must not become keystrokes).
- Ss3: `P/Q/R/S`→F1–F4; `A-D` arrows (application mode).
- PasteBody: accumulate verbatim (including ESC) until the exact terminator `\x1b[201~` → one `PasteEvent`; O(n) scanning via indexOf per chunk, no per-byte allocation.

## 3. Dispatch (`events.lev`)

**Target selection:** keys/paste → focused leaf (App's `focused()`); mouse → hit test.
**Hit test:** from the TOP overlay only when overlays exist and the top overlay is not input-transparent (R13; "outside press" reported to App for dismiss-on-outside, T05 §3.8); else the root tree. Depth-first REVERSE child order (topmost wins), `visible()` + point-in-`box` + ancestor content-rect clipping (a child painted outside its parent's content rect is not hittable).
**Phases:** build the ancestor path root→target once. Capture: root→target firing `onKeyCapture`/`onMouseCapture`; **keymap capture tier** consults `App.keymap()` at the root step for modifier-bearing or special-key events (R11: `mods != 0 || code != Char`). Then target+bubble target→root (`onKey`/`onMouse`). `handled` semantics: set during capture ⇒ skip target/bubble; set during bubble ⇒ stop the walk. After an UNHANDLED bubble: the **keymap fallback tier** (unmodified-printable binds), then framework defaults (Tab/BackTab → focus next/prev; wheel → nearest Scrollable ancestor of the hit component via a parent walk).
**Handler exceptions:** caught at the dispatch layer, `Sonar::log`ged + a DebugOverlay error counter; the loop survives (enterprise resilience ruling; the trade vs loudness documented — a crashing TUI that wrecks the shell is worse than a logged handler bug).

## 4. Focus (`focus.lev`)

`FocusRing : IFocusPolicy` — document-order depth-first walk collecting `c is Focusable && c.isTabStop() && c.visible()` (P3 narrowing probe pattern); `next/prev` with wraparound; `first` for initial focus. Overlay scoping: when overlays exist, the walk covers only the top overlay's subtree (the focus trap R13 promises; Modal gets it for free). Focus MOVE mechanics (App owns the field; this file provides `moveFocus(app, from, to)`): old → `focused=false`, `onFocusChange(false)`, invalidate; new → mirror with true; App validates the target is attached (parent-walk reaches root/overlay) and throws otherwise.

## 5. Keymap (`keymap.lev`)

Chord grammar (frozen C9): `[C-][M-][S-]key`, `^X` ≡ `C-X`; key = printable char (case-normalized: `^s` ≡ `^S`) or KeyCode name (`Enter`, `F5`, ...). Parser → `struct Chord { int mods; KeyCode code; char ch; }`; parse errors throw at bind time (loud, immediate). `bind(chord, action) -> int` (duplicate chord in one keymap throws per C9); `unbind(token)`; `handle(e) -> bool` matches mods+code(+ch when Char). Component-scoped keymaps: v1 pattern = a component instantiates its own `Keymap` and consults it in its `onKey` (BarMenu/Modal do this; shown as the pattern) — a first-class per-component keymap slot is v2.

## 6. Milestones

| M | contents | difficulty |
|---|---|---|
| M1 | event classes + dispatch (capture/bubble/handled/hit test) over a hand-built tree | M |
| M2 | decoder Ground/C0/UTF-8 + chunk buffering | M |
| M3 | decoder CSI/SS3/mouse/paste + timeout seam + Alt-prefix | L |
| M4 | FocusRing + move mechanics + overlay trap | S/M |
| M5 | Keymap (parser, tiers wired into dispatch) | S/M |

## 7. Potential issues & mitigations

1. **ESC ambiguity × Alt-prefix ordering** — resolution order pinned: a byte arriving while in Esc state within the window is Alt-prefix; timeout makes it bare Escape; `[`/`O` always win as sequence intros. The split-chunk fuzz covers ESC at chunk end.
2. **Terminal diversity** (kitty protocol, urxvt variants) — v1 capability boundary: xterm-compatible VT + SGR 1006 mouse; the decode tables are DATA (arrays in `input.lev`), so extensions are additive rows, not rewrites. Documented boundary.
3. **Paste floods** — O(n) indexOf scanning, one PasteEvent per paste; a 10MB paste allocates the text once. Tested with a large scripted paste.
4. **Handler-list mutation during dispatch** — snapshot-at-fire falls out of pure arrays (dispatch iterates the array VALUE captured when firing); pinned by an offKey-during-dispatch test.
5. **UTF-8 split across reads** — the Utf8 state buffers up to 3 continuation bytes across feeds; the fuzz splits every multi-byte sequence at every position.
6. **Wheel-to-Scrollable walk on detached/hidden ancestors** — the walk checks visible(); detached targets can't be hit (hit test starts from attached roots).

## 8. Testing plan

Decoder: table-driven byte→event goldens for EVERY row of §2's tables; the split-chunk fuzz (every sequence split at every byte position via ScriptedInput.splitNext — enumerable and cheap); timeout cases driven manually (pendingTimeoutMs/flushPending). Dispatch: scripted trees asserting capture/bubble order, handled short-circuits, keymap tier ordering (a `^S` bind vs a capture handler vs a focused Input), wheel routing. Focus: ring traversal goldens incl. hidden/non-tabstop skips + overlay trap. Keymap: chord parse table (green + error), duplicate throw, unbind. All headless via T10; differential oracle/IR/LLVM.

## 9. Open questions

1. Kitty keyboard protocol (disambiguated modifiers) — v2 opt-in, additive table rows.
2. Mouse move without buttons (1003) — v1 uses 1002 (T09 enables); revisit for hover UIs.

## 10. Implementation log

- 2026-07-12 — design written; not started.
- 2026-07-12 — implemented in full across the four owned files. One
  placement deviation from this doc's own §1/§3 split, forced by a language
  rule rather than chosen: `KeyEvent`/`MouseEvent`/`PasteEvent` already live
  in `component.lev` (T01 declared them there to wire `Component`'s handler
  lists before T03 existed), and Leviathan classes cannot be reopened across
  files (only namespaces reopen and merge). `KeyEvent.isChar`/`isCtrl`/
  `matches` are therefore added as methods on the existing class declaration
  in `component.lev` rather than living in `events.lev`; everything else
  (dispatch, decoder, focus, keymap) is exactly where §1's file list says.
  `events.lev` additionally hosts a minimal `Sonar::log` ring buffer +
  `handlerErrorCount()` (R16/C1 declares `log` but no track had implemented
  it yet; T05's `DebugOverlay` is a pure reader, no ownership conflict).
  `dispatchKey`/`dispatchPaste`/`dispatchMouse` take the resolved root/
  focused/keymap/focus-policy as plain parameters rather than an `App`
  reference (T03 has no T09 dependency; T09's run loop will be the caller
  that supplies these from its own fields, the same "seam via parameters"
  shape T01 used `ILifecycleHost` for where a callback had to originate
  deep inside the tree instead).
  Two compiler bugs were found and worked around, filed as bug.md #40 and
  #41 (both P1, both emit-C++/LLVM-only — oracle and `--ir` are unaffected
  throughout): #40, a `Map` valued by a user `enum` segfaults past one
  entry (`keymap.lev` uses an if/else chain instead of
  `Map<string, KeyCode>` for the chord key-name table); #41, an
  `Array<struct>` where the struct carries an enum field goes stale after
  unrelated heap activity elsewhere in the program, corrupting later
  equality reads (`Chord` in `keymap.lev` was changed from a `struct` to a
  `class` — it is parsed once and never mutated, so this costs nothing
  semantically). Both were caught by running the differential smoke test
  (`sonar/tests/events/smoke.lev`, corpus-style print-and-expect covering
  every decoder table row incl. split-chunk cases, keymap bind/duplicate/
  unbind/case-sensitivity, `KeyEvent.isChar/isCtrl/matches`, `FocusRing`
  traversal + `moveFocus`, dispatch capture/target/bubble/handled-
  short-circuit, handler-exception containment, the R11 keymap tiers, the
  Tab/BackTab framework default, and mouse hit-test/dispatch/wheel-routing/
  outside-press) across all four active engines and diffing; oracle and
  `--ir` were byte-identical from the first pass, native (`--build-native`)
  and LLVM initially diverged, which is what surfaced both bugs. After the
  workarounds all four engines are byte-identical. T01's own differential
  suite (`sonar/tests/core`, the P1–P11 probes) was re-run unchanged as a
  regression check and stayed green throughout — also note for future
  implementers: this session's local `build/` binaries were stale (built
  before the last merged commit) and produced several false-alarm failures
  before a full rebuild; rebuild before trusting a native/LLVM divergence.
