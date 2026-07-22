# editor-dom — Sonar DOM flagship example

The target-feel program from `designs/sonar_v2/dom/techdesign-00-dom-overview.md` §2,
corrected and running. This is the suite's acceptance test: if this program compiles and
produces correct output, every DOM track (D01–D08) is working together.

## Fidelity map (sketch line → mechanism → owning design doc)

| sketch line | mechanism | design |
|---|---|---|
| `SonarApp app = SonarApp();` | `class SonarApp : App` — owns the Document, `start()` alias | D01 |
| `string \| None filename;` (global) | ordinary union global captured in `{{filename}}` hole | D01/D05 |
| `dom!(\`…\`)` | comptime macro — zero runtime parse cost, expression holes, drift-tested | D02 |
| lowercase tags `<contentbar>`, `<menu>`, `<menuitem>`, `<textarea>`, `<span>` | frozen tag registry D-C1 | D02 |
| `<menuitem id="file-menu" hotkey="^f">Open</menuitem>` | `id` → Document index; `hotkey` → pending-shortcut binding (R7 seam); label = text child | D01/D04 |
| `<textarea id="text" flex="1">` | new component `TextArea` (D06) | D06 |
| `<span id="status">{{filename}}</span>` | text binding pull-sweep + diff; bare global write → updates span on next frame | D05 |
| `app.add(mainUI)` | `Container.add` + Document walk-index | D01 |
| `fileMenu.position().method = Dom::PositionMethod::Absolute` | `Position` reference-class with live get/set views | D03 |
| `fileMenu.position().y = 1` | desired origin, consumed when Absolute | D03 |
| `FileDialog openDialog = FileDialog("open")` | `Sonar::Dialogs::FileDialog` over `sysListDir` | D07 |
| `fileMenu.actions.add("open-file", …)` | `ActionRegistry` + responder-chain dispatch | D04 |
| `await openDialog.show()` | Promise-based dialog, resolve-once guarded | D07 |
| `app.query("#text").value()` | `DomNode.value()` uniform accessor (narrowing ladder) | D01 |
| `app.start()` | alias for `run()` on `SonarApp` | D01 |

## Two deltas from the sketch

1. **`dom!(\`…\`)`** instead of a bare backtick constructor argument — raw strings are
   legal only in macro-argument position (reference §6.9); `dom!` is the comptime tier.
2. **`uses Sonar::Dialogs`** instead of `uses MyApp::Dialogs` — the framework ships it.

## Running

```sh
# Interactive (real terminal):
trident run

# Scripted golden test (used by the test runner):
SONAR_SCRIPT=1 trident run
```

## Scripted session (SONAR_SCRIPT=1)

1. Initial DOM queries — verifies the tree is correctly built
2. `outerMarkup` of the toolbar — round-trip snapshot
3. Tab to focus the textarea, type "Hello, Sonar!" — verifies the input path
4. Send ^f hotkey — verifies the hotkey binding is live

The FileDialog interaction (open a file, cancel, save-as) is tested separately in
`tests/dom-dialogs/` where the fs sandbox is deterministic. The scripted session here
focuses on the core layout and binding machinery.
