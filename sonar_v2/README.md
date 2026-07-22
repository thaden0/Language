# Sonar — Leviathan TUI Framework

Sonar is a retained-mode terminal UI framework for the Leviathan programming language.
Components, layout, focus, overlays, and theming are all landed in T01–T11. The DOM
layer (v0.2.0, tracks D01–D08) adds a web-developer-friendly surface on top.

## Sonar DOM

The DOM layer is **a layer, not a rewrite**. The existing component tree is the DOM;
`DomNode` is a thin jQuery-style wrapper, never a parallel tree.

### Three markup tiers, one grammar

```lev
// Tier 1 — comptime dom! macro (zero runtime parse cost, expression holes):
IComponent ui = dom!(`
    <flex id="main">
        <span>{{filename}}</span>
        <textarea id="text" flex="1"></textarea>
    </flex>
`);

// Tier 2 — comptime import() (external .sonar asset file):
IComponent ui2 = FlexContainer(import("main.sonar"));

// Tier 3 — runtime string (dynamic markup):
IComponent ui3 = buildMarkup("<flex><span>hello</span></flex>", doc);
```

All three tiers produce byte-identical serialized trees (validated by the drift corpus).

### Selectors

CSS-style selectors over the live component tree:

```lev
DomNode  node  = app.query("#text");           // throws on miss
DomNode? maybe = app.queryOrNone(".hidden");   // None on miss
DomNodeList all = app.queryAll("menuitem");
```

Grammar: tag, `#id`, `.class`, `[attr]`, `[attr=value]`, descendant (space), child (`>`),
group (`,`). A bare `#id` selector hits the Document's fast-index path.

### Classes

```lev
node.addClass("focused");
node.removeClass("hidden");
node.toggleClass("selected");
bool h = node.hasClass("error");
```

`hidden` is a **semantic class**: `node.hide()` ↔ `node.addClass("hidden")` ↔
`node.visible() == false`, both directions.

### Actions

Named commands, fired by hotkeys or code, grayed across all bound items at once:

```lev
fileMenu.actions.add("save", () => { /* ... */ });
fileMenu.actions.setEnabled("save", false);   // grays all bound items instantly
fileMenu.actions.fire("save");                // responder-chain walk
```

### Bindings (`{{…}}`)

```lev
string | None filename = None;

// In a dom! hole: a bare write to the global updates the span on the next frame.
dom!(`<span>{{filename}}</span>`)

// Runtime tier: expose() or doc.set() feeds the sweep:
doc.expose("clock", () => clock.now);
```

The sweep runs at frame start (pull, not push). `doc.refresh()` forces an immediate sweep.

### Dialogs

```lev
uses Sonar::Dialogs;

FileDialog   openDlg = FileDialog("open");
PromptDialog saveDlg = PromptDialog("Save As", "Save as:");

string | None path = await openDlg.show();
string | None name = await saveDlg.show();
```

### The DOM Inspector (DevTools)

```lev
uses Sonar::DevTools;

// Install once after building the app:
DevTools::inspector(app);           // default hotkey: F12
DevTools::inspector(app, "C-F12"); // custom hotkey
```

Press the hotkey to open an overlay showing:
- **Tree pane**: the full component hierarchy with `tag#id.class…` labels
- **Detail pane**: live box/position/classes for the selected node
- **Query bar**: CSS selector → match count → Enter to cycle

The inspector excludes its own overlay subtree from the tree to avoid infinite nesting.
The `inspector-highlight` class is added to the selected node (theme-hooked).

### Testing your app with selectors

Use `sonar_v2/tests/harness/dom_helpers.lev` in your test's `trident.toml`:

```lev
uses Sonar;
uses Sonar::Dom;

// dom_helpers.lev adds to SonarTest namespace:
SonarTest::click(app, "#ok-button");
SonarTest::type(app, "#search", "hello");
SonarTest::chord(app, "^s");                   // DOM hotkey grammar
SonarTest::expectText(app, "#status", "saved", "status after save");

string mouse = SonarTest::encodeMouse(10, 5, 0, true);  // SGR press at (10,5)
```

## Quickstart — the editor-dom example

```lev
uses Sonar;
uses Sonar::Dom;
uses Sonar::Dialogs;

string | None filename = None;

void main() {
    SonarApp app = SonarApp();
    app.add(dom!(`
        <flex id="main" flex="1">
            <contentbar dock="Top">
                <menuitem id="file-menu" hotkey="^f">Open</menuitem>
            </contentbar>
            <textarea id="text" flex="1"></textarea>
            <contentbar dock="Bottom">
                <span>{{filename}}</span>
            </contentbar>
        </flex>
    `));

    FileDialog openDialog = FileDialog("open");
    FlexContainer fileMenu = FlexContainer(
        "<menu class=\"hidden\"><menuitem hotkey=\"^o\">Open File</menuitem></menu>"
    );
    fileMenu.position().method = Dom::PositionMethod::Absolute;
    fileMenu.position().y = 1;
    app.add(fileMenu);

    fileMenu.actions.add("open-file", () => {
        fileMenu.addClass("hidden");
        string | None result = await openDialog.show();
        if (result != None) { string path = result; filename = path; }
    });

    app.start();
}
```

See `examples/editor-dom/` for the complete, runnable version with a scripted test session.

## Running the test suite

```sh
cd sonar_v2
bash tests/runtests.sh
```

The runner finds every directory under `tests/` and `examples/` that has a `trident.toml`
and a `*.expected` golden file, runs each on oracle/IR/LLVM, and compares stdout.
