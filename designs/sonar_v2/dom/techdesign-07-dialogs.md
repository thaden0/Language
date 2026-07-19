# Sonar DOM — Tech Design 07: Dialogs (`Sonar::Dialogs` — FileDialog, PromptDialog)

**Status:** implemented (§8). **Date:** 2026-07-15, landed 2026-07-19. **Track:** D07.
**Owns:** `sonar_v2/src/dom/dialogs.lev`. `request-stat-isdir.md` had already landed by
implementation time (§8's §3 deviation note) and files
`designs/requests/request-llvm-fs-dir-natives.md` instead (LLVM lowering for
`sysListDir`/`sysRemove`/`sysRename`).
**Depends on:** T05 Modal + `Sonar::confirm` (the Promise-dialog precedent, reused verbatim),
ListView + Input + Button, the landed filesystem floor (`sysListDir(path) -> Array<string>?`,
`sysStat`, `fileExists`, `sysMkdir`), tasks/await, D05 (`__scheduleSweepFrame` on resolve — the
D-P6 contract). Probes: D-P5.
**Gates:** G-D4. **Difficulty:** M/L. **Risk:** MED — filesystem realities (permissions, huge
dirs, races) are the risk; the dialog never trusts a cached listing.

The target sketch: `FileDialog openDialog = FileDialog('open'); … filename = await openDialog.show();`
— a modal file picker whose `show()` is an awaitable `Promise<string | None>` (path, or None on
cancel). The sketch's `uses MyApp::Dialogs` becomes `uses Sonar::Dialogs` — the framework ships it.

---

## 1. Surface

```lev
namespace Sonar { namespace Dialogs {

class FileDialog : Modal {
    new FileDialog(string mode);          // "open" | "save" — anything else throws SonarDomException
    // fluent config (pre-show)
    FileDialog startDir(string path);     // default "." at first show; REMEMBERED across shows
    FileDialog title(string t);           // default "Open File" / "Save File"
    FileDialog filter((string name) => bool pred);   // file-name predicate (dirs always listed)
    FileDialog fileName(string initial);  // save mode: pre-filled name

    Promise<string | None> show();        // D-P5; open + focus; resolve-once guarded
    Promise<string | None> show(string dir);
}

class PromptDialog : Modal {
    new PromptDialog(string title, string label);
    PromptDialog value(string initial);  PromptDialog validator((string) => bool, string message);
    Promise<string | None> show();        // Enter/OK → value; Esc/Cancel → None
}

// re-exports so `uses Sonar::Dialogs` is one-stop:
Modal alert(string title, string message)          => Sonar::alert(title, message);
Promise<bool> confirm(string title, string message) => Sonar::confirm(title, message);
}}
```

`show()` builds the UI lazily on first call (a dialog constructed at program top-level allocates
almost nothing), pushes via the landed `Modal.open()` path (OverlayHost + scrim, centered, focus
trap, **no** outside-dismiss — the T05 Modal/Menu distinction, kept), and returns a fresh pending
`Promise` per show. Resolve-once guard = the `Sonar::confirm` pattern verbatim (buttons/Enter
resolve, `onDismiss` resolves `None` if not already settled — Esc can never hang an `await`).
Every resolution path ends with `Sonar::Dom::document().__scheduleSweepFrame()` when a Document
exists (D05 §4.2) — the awaiting continuation's writes land in the frame this schedules; plain-App
users (no Document) skip it silently.

## 2. FileDialog anatomy (built from the framework's own parts)

```
┌ Open File ────────────────────────────────┐
│ Path: [ /home/len/code            ] (Input, editable — Enter navigates)
│ ┌────────────────────────────────────────┐│
│ │ ../            ListView (dirs first,   ││
│ │ src/           "name/" suffix on dirs, ││
│ │ trident.toml   sorted case-insensitive)││
│ └────────────────────────────────────────┘│
│ Name: [ …save mode only… ]  (Input)       │
│           [ Open ] [ Cancel ]  (Buttons)  │
└───────────────────────────────────────────┘
```

Layout: a `FlexContainer(Axis::Vertical)` of path Input / ListView(flex 1) / (save: name Input) /
button Bar — the DOM suite eating its own dogfood (built in code, not markup — dialogs predate any
app's registry customization by design). Sizing: `Constraint::Bounded` ~ 60×18 clamped to screen.

**Listing (`refreshListing(dir)`):** `sysListDir(dir)` — `None` ⇒ not a directory ⇒ error row +
log (never a throw mid-UI); entries partitioned dirs/files via the **isDir probe** (§3), dirs
first, each group sorted case-insensitively, `../` prepended unless at filesystem root; files
filtered by the predicate (dirs never filtered). The listing is re-read on every navigation — never
cached across user actions (fs races: the dialog shows what IS, tolerating disappearing entries by
re-listing on activation failure).

**Interaction:** ListView `on:activate` (Enter/double-press, the landed T05 surface): directory →
descend (path Input updates); file → open mode: resolve(fullPath); save mode: name Input fills.
Path Input `on:submit`: normalize + jump (missing path → error row). Save mode Open/Save button:
`dir + "/" + name`; empty name → validation message; existing file → `await confirm("Overwrite?",
…)` inside the handler (a dialog awaiting a dialog — the task machinery showcase; the confirm's
overlay stacks above per R13). Cancel button + Esc (Modal dismissKey) → resolve(None). Keyboard:
the focus ring covers path/list/name/buttons (Tab order = build order); the list owns Up/Down/Page
keys (landed).

Path strings: `"/"`-joined, `normalizePath` collapses `.`/`..` segments lexically (no realpath
native — documented); root detection = `dir == "/"` (POSIX-only v1, matching the threads/TLS
platform stance).

## 3. The isDir probe & the language request

Today the only directory test is `sysListDir(child) != None` — one listdir syscall **per entry**
per listing (O(N) listdirs; a 1k-entry dir = 1k syscalls ≈ visible lag on network filesystems).
v1 ships with this probe (correct, portable, slow-tolerable locally) plus a mitigation: probe
lazily — entries render unclassified-then-classified in one pass ONLY when the dir exceeds 256
entries (a `Sonar::log` line notes the degradation).

Filed alongside this doc: **`designs/requests/request-stat-isdir.md`** — extend `sysStat(path,
field)` with `field 3 = isDir` (0/1; -1 missing), one `stat(2)` word already sitting in the
buffer the native reads. Acceptance: FileDialog's per-entry probe collapses to one stat per entry
(and could batch later). Interim fallback (shipping): the listdir probe above. When the request
lands, `isDir(path)` swaps implementations in one helper — nothing else changes.

## 4. Milestones

| M | contents | difficulty |
|---|---|---|
| M1 | D-P5 probe; PromptDialog (the Promise/validate/focus skeleton, smallest end-to-end) | S/M |
| M2 | FileDialog open mode: listing/probe/navigation/resolve + sandbox test harness (below) | M |
| M3 | save mode: name input, overwrite confirm-chain, validation | M |
| M4 | filters, startDir memory, degradation path, request filed, polish + docs | S |

**Sandbox harness:** dialog tests build a real temp tree via `sysMkdir`/`File` writes under the
test's cwd (the corpus runs with cwd = test dir — the T10 runner rule), so listings are
deterministic; teardown via `sysRemove` walk. No mocking layer — the natives ARE the seam.

## 5. Potential issues & mitigations

1. **`Promise<string | None>` (D-P5)** — probe first (construct/resolve-string/resolve-None/await
   from an event task). Red ⇒ fallback: `class FileResult { string path; bool cancelled; }`,
   `Promise<FileResult>` — the sketch's `string | None filename = await …` becomes two lines;
   anchor-logged fidelity delta. (Expectation: green — unions are fixed-size values and `Promise`
   is erased-generic.)
2. **Huge directories** — the >256-entry degradation (§3) + ListView virtualization (landed) keep
   paint O(viewport); the probe cost is the listing's, not the frame's.
3. **Permissions/races** — every fs call tolerates failure (None/`-1` → error row + log, never an
   uncaught throw from a UI path); activation on a vanished entry re-lists.
4. **Modal-in-modal focus** (overwrite confirm above a FileDialog) — R13 focus save/restore is
   per-overlay (landed, T05-tested); the corpus re-pins the nested case.
5. **`await` inside button handlers** — handlers park as tasks (landed); the dialog's own state
   mutations post-await are covered by the resolve-time sweep-frame (D05).
6. **Windows paths** — out of scope v1 (POSIX-only, the standing platform stance); path handling
   isolated in `normalizePath`/`joinPath` helpers for the eventual port.

## 6. Testing plan

Scripted end-to-end per mode over the sandbox tree: navigate (activate dirs, `..`, path-Input
jumps), select/resolve, cancel/Esc → None, filter application, save-mode name flow + overwrite
confirm both branches, missing-path error row, vanished-entry re-list, >256 degradation log,
startDir memory across two shows, nested-confirm focus restore, resolve-once under
double-Enter races. PromptDialog validator matrix. All headless (harness + `key()`/`text()`
scripts), snapshots via TestRenderer, differential oracle/IR/LLVM.

## 7. Open questions

1. Multi-select open mode (`Promise<Array<string> | None>`) — v1.1.
2. New-folder button (sysMkdir exists) — v1.1, trivial once PromptDialog lands.
3. Recents/bookmarks rail — v2 (wants a config-file story first).
4. A `<filedialog>` markup tag — deliberately NOT registered v1 (dialogs are imperative objects
   with Promise lifecycles; markup is for retained trees) — recorded so nobody "helpfully" adds it.

## 8. Implementation log

- 2026-07-15 — design written; not started.
- 2026-07-19 — implemented (M1–M4). `sonar_v2/src/dom/dialogs.lev` is the new
  leaf: `namespace Sonar::Dialogs` (a new nested namespace, sibling of
  `Sonar::Dom`), `PromptDialog`/`FileDialog : Modal`, `FileEntry`/
  `DirListSource` (an `IListSource`), `joinPath`/`normalizePath`/`parentOf`
  path helpers, and the `alert`/`confirm` re-exports. Differential test
  `sonar_v2/tests/dom-dialogs/` (prompt basic/cancel/Escape/validator; open
  listing/nav/path-jump/select-resolve/cancel/no-selection/filter; save new-
  name/empty-name/activate-fills-name/overwrite-yes/overwrite-no; vanished-
  entry re-list; startDir memory across shows; resolve-once under a double-
  Enter) passes byte-identical on **oracle + IR**. **LLVM native**: blocked,
  pre-existing — `sysListDir`/`sysRemove`/`sysRename` have no LLVM lowering
  today (`LlvmGen.cpp`'s native dispatch has cases for `sysStat`/`sysMkdir`
  only; confirmed by isolating a PromptDialog-only probe, which — touching no
  fs natives — builds and runs correctly on LLVM, so the gap is specifically
  the Track 08 fs family, not this file's Promise/Modal/union/covariant-
  override mechanics). Filed **`designs/requests/request-llvm-fs-dir-
  natives.md`** (the interpreter-side implementations in
  `src/RuntimeNatives.cpp` are the exact spec to port; `sysMkdir`'s own LLVM
  case, one function away in the same files, is the direct template) —
  `runtests.sh` reports `FAIL dom-dialogs (llvm codegen)` deliberately left
  in that state rather than trimmed around (the whole corpus otherwise passes
  oracle+IR+LLVM clean; `dom-dialogs` is the first test to need this native
  at all). emit-C++ not attempted (expected to take the same DOM async-gap
  skip as every other D0x-track leaf with `await`, D06 included).

  **§3 deviation (anchor-logged):** the design's own premise — "today the
  only directory test is the O(N)-listdir probe" — was already stale by
  implementation time. `docs/reference.md` §6.6.5/§6.6.58 documents `sysStat`
  field 3 = isDir and `std::isDir(path)` landed, citing
  `request-stat-isdir.md` as its origin; that request file no longer exists
  in `designs/requests/` (the accepted-and-fulfilled convention deletes
  rather than archives). This file uses `std::isDir` directly — one stat(2)
  per entry — so §3's >256-entry degradation-logging mitigation is moot and
  was not implemented; no new request filed (nothing left to ask for).

  **Upstream fix, `sonar_v2/src/components/modal.lev`:** `Modal.open()` did
  not focus anything, so `dispatchKey`'s `focused_` stayed pinned to whatever
  was focused before the Modal opened — invisible until this track's own
  scripted tests were the first to drive `Sonar::confirm`/`Modal` through
  real key dispatch. Concretely: the save-mode overwrite confirm (a Modal
  opened *above* an already-open FileDialog) left Enter/Escape routed to the
  FileDialog's stale focus, never reaching the confirm's Yes/No at all. Fixed
  by focusing the first tab stop found among `children()` (not `this` —
  Modal is itself a Focusable tab stop for the Escape catch-all, so a
  document-order walk from `this` would return the Modal before any real
  control) on open, mirroring `Menu.focusFirst()`'s existing precedent
  (menu.lev). Verified against a minimal bare-Modal probe before touching
  dialogs.lev itself; alert()/confirm() gain auto-focus as a side effect
  (strictly more usable, no behavior change for their single-OK / Yes-No
  shape beyond "Enter now works without a preceding Tab").

  **Two mechanisms the test corpus had to discover the hard way** (both
  pre-existing platform behavior, not dialogs.lev bugs):
  1. A bare Escape byte is genuinely ambiguous (Alt-combo vs. CSI prefix)
     until either more bytes arrive or `input.lev`'s 50ms `EscTimeoutMs`
     elapses (`app.lev`'s `armEscTimeout`/`onEscTimeout`); a scripted test
     observing a real Escape-dismiss must let that real time pass — done via
     `await`-ing a short `sysTimerStart`-backed Promise (`settleTicks`), not
     a busy-loop.
  2. A handler that `await`s a nested `Sonar::confirm(...)` (the save-mode
     overwrite chain) PARKS the entire synchronous dispatch call — a second
     top-level `pump()` issued *after* that call returns can never reach it,
     since nothing resumes a parked `await` except work the same single-
     threaded spin notices (a due timer; `tests/corpus/tasks/
     order_two_awaits.lev`'s shape). The corpus answers the nested confirm
     from a `std::sysTimerStart` callback armed *before* the triggering key,
     through real `ScriptedInput` key dispatch rather than a bare Promise.

  **Two dialogs.lev-specific findings, fixed during testing (not spec
  gaps):** (a) `ListView.setSource`'s own `refresh()` clamps the OLD numeric
  `selected` into the new count rather than clearing it, so a stale index
  silently pointed at an unrelated row after navigating to a new directory
  (caught by the descend-then-".."-up test: a second Enter meant for ".."
  instead activated whatever file inherited that index) — `refreshListing`
  now calls `list_.setSelected(-1)` unconditionally, forcing a deliberate
  Down/click after every navigation. (b) the vanished-entry status message
  was being set *before* the re-list call, whose success path unconditionally
  clears the status row — reordered so the message survives.

  **Judgment calls (anchor-logged, not left as gaps):** the primary
  ("Open"/"Save") button's OPEN-mode behavior isn't spelled out by §2's own
  interaction table (only activation and the save-mode button are) —
  implemented as "act on the current list selection" (directory descends,
  file resolves, nothing selected asks the user to pick one), the
  conventional file-picker shape. The path Input is address-bar semantics
  (the typed text replaces the shown location outright, resolved relative to
  cwd if not `/`-absolute) rather than joined against the OLD current
  directory — the latter was tried first and double-joins a path typed to
  fix a typo in what's already shown.

  **Two base-language findings (not dialogs.lev-specific, worth recording
  for the next Modal/Container subclass):** base constructors never auto-
  run (reference §4.5) — `PromptDialog`/`FileDialog` must open with
  `Modal::Modal();` or Modal's own ctor body (the tab stop, the border, and
  critically the dismissKey capture handler) silently never executes; missing
  this made Escape a no-op until caught by the `prompt-escape` test. And
  cross-statement narrowing does not survive an early-return guard clause
  (`if (x == None) { ...; return; } T y = x;` fails to compile) — every
  optional-unwrap in this file and its test uses the `if (x != None) {
  ... } else { ... }` shape instead (the helm `FileTree.ensureLoaded`
  precedent, cited there for the same reason).

  Deviations from §1's surface sketch: none in signature; `FileDialog`/
  `PromptDialog` each redeclare their inherited `title(string)` with a
  covariant return type (verified this resolves correctly, including through
  a base-typed reference, via a standalone probe) rather than accepting
  Modal's own `Modal`-returning version, matching the landed `ContentBox.
  title`/`border` precedent for the identical situation.
