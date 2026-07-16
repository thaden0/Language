# Sonar DOM — Tech Design 07: Dialogs (`Sonar::Dialogs` — FileDialog, PromptDialog)

**Status:** design, pre-implementation. **Date:** 2026-07-15. **Track:** D07.
**Owns:** `sonar/src/dom/dialogs.lev`. Files `designs/requests/request-stat-isdir.md`.
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
