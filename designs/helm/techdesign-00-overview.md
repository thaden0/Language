# Helm — Tech Design 00: A Full IDE on Sonar 2

**Status:** design, pre-implementation. **Date:** 2026-07-15.
**Scope:** an application built on the `sonar_v2` DOM framework ("Sonar 2") and the
`leviathan`/`trident` toolchain. Helm is a **trident package** (checked user code); it never
touches `src/**` or `runtime/**`. Where this doc and any future track doc disagree, this
document wins until amended here.

Ground-truth inputs: `designs/sonar_v2/techdesign-00-overview.md` (Sonar contracts/rulings),
`sonar_v2/src/dom/*.lev` (the actual DOM API — `SonarApp`, `DomNode`, `query`, actions),
`sonar_v2/src/components/*.lev`, `docs/reference.md` (language facts),
`known_bugs_1.md`/`known_bugs_2.md` (live compiler bugs and their workarounds to design around).

---

## 0. What Helm is

**Helm** is a terminal IDE for the Leviathan language, written in Leviathan, rendered by Sonar 2.
It is the developer's cockpit for a `trident` workspace: a project tree, a multi-buffer text
editor with syntax highlighting and diagnostics, build/run/test integration through `trident`
and `leviathan`, an integrated terminal, a command palette, and a quick-open switcher — the
familiar VS-Code-shaped surface, but retained-mode TUI and self-hosted.

Naming follows the house nautical set (`leviathan`, `trident`, `harpoon`, `sonar`, `atlantis`):
**Helm** is where you steer the ship.

**Design position (mirrors Sonar's own, because it inherits Sonar's cost model):**

1. **Retained UI, owned document model.** The Sonar tree is persistent and mutated in place; the
   editor's text is *not* stored in the component tree — it lives in a purpose-built buffer model
   (§4) that the `EditorView` component paints from. Leviathan's pure arrays + ARC punish
   per-keystroke tree rebuilds and reward stable identity and localized damage.
2. **The compiler is a service, not a library.** Helm shells out to `leviathan` and `trident` as
   subprocesses and consumes their output. Helm never links compiler internals (the two-binary
   boundary and the compiler/package-manager separation are frozen contracts — §9, STOP protocol).
   All "language intelligence" is a thin bridge over those processes (§6).
3. **Everything is a command.** Menus, the palette, keymaps, and buttons all fire *named actions*
   through Sonar's `ActionRegistry` responder chain (`sonar_v2/src/dom/actions.lev`). One command
   registry is the single source of truth for "what Helm can do."
4. **DOM markup for chrome, imperative for the hot path.** Panels, bars, dialogs, and menus are
   authored as Sonar backtick markup and wired with `query(...).on(...)` / `.actions()`. The
   editor's per-frame paint is hand-written `paintContent` — no markup, no allocation.
5. **Engine lanes:** oracle + IR + **LLVM** are the run lanes (LLVM is the shipping build);
   emit-C++ is compile-only for anything touching `App.run()` (no event loop in that lane), same
   as Sonar. ELF/X64Gen is frozen and is never a Helm target and never a gate.

**Explicit non-goals for v1** (recorded so nobody silently re-opens them): a graphical debugger
(step/breakpoints — deferred to v2, gated on a compiler debug protocol that does not exist),
remote/SSH workspaces, multi-root workspaces, a plugin/extension API, collaborative editing,
Git UI beyond a status readout, truecolor theming (inherits Sonar's 16-color cell format, C6),
grapheme-cluster/RTL text (inherits Sonar's scalar cell math), and Language Server Protocol
wire-compatibility (Helm's language bridge is process-local and bespoke — §6.4 notes the LSP
adapter as a v2 possibility, not a v1 shape).

---

## 1. The ground Helm stands on

### 1.1 Sonar 2 surface Helm consumes (verified against `sonar_v2/src/`)

- **`SonarApp`** (`dom/app.lev`): `start()`/`run()`, `add(...)`, `query(sel) -> DomNode`,
  `queryAll -> DomNodeList`, `queryOrNone`, per-frame binding sweep, overlay stack (inherited
  from Sonar `App`: `pushOverlay`/`popOverlay`, `keymap()`, `focus()`, `every(ms, cb)`,
  `pumpOnce()` test hook).
- **Backtick markup** (`dom/markup.lev`): `<tag attr="v">…</tag>`, `{{expr}}` binding holes,
  `id="…"`, `class="…"`. Runtime tier only; the comptime `dom!` tier (`$for`/`$if`/`on:`/`{expr}`)
  is a Sonar D02 feature Helm **may** use once it lands but does not gate on.
- **`DomNode`** (`dom/node.lev`): jQuery-style — `.on(event, () => void)` (events: `key mouse
  paste press change submit toggle select dismiss`), `.actions()` (the `ActionRegistry`),
  `.value()/.text()/.checked()/.enabled()`, class ops (`addClass/removeClass/toggleClass`,
  `.hide()/.show()` via the semantic `hidden` class), `.position()`, `.query/.queryAll`,
  `.children()/.parent()`.
- **Components** (`sonar_v2/src/components/`): `text`, `input`, `button`, `checkbox`, `radio`,
  `contentbar` (status/tool bars), `contentbox` (scroll+border frame), `splitbox` (resizable
  panes), `gridbox`, `tabs`, `listview`, `tableview`, `treeview` (virtualized — paint viewport
  only), `menu`/`menuitem`, `modal`, `progress`, `spinner`, `debugoverlay`.
- **Layout** (`sonar_v2/src/layout/`): flex, grid, dock, stack.
- **Theming** (`sonar_v2/src/theme.lev` + `themes/*.toml`): dotted keys, TOML parsed in-language
  (no JSON on LLVM — bug #30), runtime `setTheme` swap + full invalidate.

Helm adds exactly **one new low-level component** — `EditorView` (§4.3) — because no existing
Sonar component is a rope-backed, syntax-highlighted, gutter-bearing code editor. `treeview`,
`tableview`, `listview`, `tabs`, `contentbox`, `splitbox`, `modal`, `contentbar`, `input`,
`menu` are consumed as-is. If `EditorView` needs a capability the Sonar core lacks, that is a
STOP (escalate to a Sonar track), not a fork of Sonar.

### 1.2 Toolchain surface Helm drives (verified against project overview)

- **`leviathan`** — pure compiler. Run modes: `--run`, `--emit-ir`/`--ir`, `--emit-cpp`,
  `--emit-llvm`, `--expand`, `--rules`. Diagnostics go to stderr. Compiles the source set handed
  to it; does not read the manifest.
- **`trident`** — build driver / package manager. Reads `trident.toml`, builds the dependency
  graph, invokes `leviathan`. This is the front door: `trident build`, `trident run`,
  `trident test` (drives `harpoon`), and the frozen build-plan contract
  (`designs/complete/techdesign-toolchain.md`).
- **`harpoon`** — the unit-test library (`@Test` auto-discovery); Helm's test panel reads its
  output.

**Language-side dependency (a gate, not an improvisation):** machine-readable diagnostics. Today
`leviathan` prints human diagnostics to stderr. Helm v1 parses that text (§6.2) — a legitimate
but brittle contract. A clean `leviathan --diagnostics=jsonl` (one JSON object per line, or a
line-oriented `file:line:col:sev:code:msg` form to dodge bug #30's LLVM-JSON hole) is the right
long-term seam. **Adding a compiler flag is a compiler-side change → STOP/escalate.** It is
tracked here as gate **G-LANG-1** (§7) and owned by a language design doc, not by Helm.

---

## 2. Architecture at a glance

```
                    ┌──────────────────────────────────────────────┐
                    │  SonarApp  (retained UI tree, frame loop)     │
                    │  ┌────────┬───────────────────┬────────────┐  │
                    │  │Activity│  Editor group     │  Panels    │  │
                    │  │ + Tree │  (tabs + Editor-   │ (terminal/ │  │
   keys/mouse ─────▶│  │ (H05)  │   View, H04)      │  problems/ │  │
                    │  │        │                   │  output,   │  │
                    │  ├────────┴───────────────────┤  H07)      │  │
                    │  │  StatusBar (H08)            │            │  │
                    │  └────────────────────────────┴────────────┘  │
                    └───────┬───────────────┬──────────────┬────────┘
                            │ commands (H03)│ workspace(H02)│
                    ┌───────▼───────┐ ┌─────▼──────┐ ┌──────▼───────────┐
                    │ CommandRegistry│ │ Workspace/ │ │ Language Service │
                    │  + Keymap/Menu │ │  Buffers   │ │  Bridge (H06)    │
                    └───────────────┘ │  (rope)    │ └──────┬───────────┘
                                      └────────────┘        │ spawn + Channel
                                             ▲              ▼
                                             │        ┌───────────────┐
                                       file I/O       │ leviathan /   │
                                             │        │ trident procs │
                                      ┌──────▼──────┐ └───────────────┘
                                      │  Filesystem  │  (subprocess bridge, H09)
                                      └─────────────┘
```

**Process model.** Helm is one process. Compiler/build/test work and the integrated terminal run
as **child processes** driven through a subprocess bridge (H09). All blocking I/O (subprocess
stdout/stderr, file reads of large files, the PTY) runs on `spawn`ed tasks and communicates back
to the UI task over **`Channel`s** — never a bare global `Promise` awaited across `spawn`
(bug #35; Channels are the portal, cheat-sheet §15). The UI task drains channels at frame start
(a `renderFrame` hook, exactly where `SonarApp.__sweepBindings` already runs) and mutates the
document + Sonar tree; Sonar's damage model repaints only what changed.

**Threading rule (frozen H-R1):** only the UI task mutates the Sonar tree, the `Workspace`, and
buffers. Worker tasks produce **immutable messages** (diagnostics batches, process-output chunks,
completion results) onto channels; the UI task applies them. This keeps the retained tree
single-writer and sidesteps the multi-mixin dispatch and array-staleness bug families that bite
under concurrent mutation.

---

## 3. Screen layout (the shell)

The root is a `dock` layout. Authored once as Sonar markup at boot (`H01`), then driven
imperatively:

```
uses Sonar;
uses Sonar::Dom;

// H01 — shell.lev (abbreviated; real attrs per each component's Sonar table)
SonarApp app = SonarApp();
app.add(FlexContainer(`
  <contentbar id="menubar" dock="top">…</contentbar>          <!-- H01 menu bar -->
  <splitbox id="hsplit" dock="fill" axis="horizontal" split="24">
      <contentbox id="sidebar" minWidth="18" border="single">
          <tabs id="side-tabs">
              <tab label="Files"><treeview id="filetree"/></tab>
              <tab label="Search"><contentbox id="searchpane"/></tab>
          </tabs>
      </contentbox>
      <splitbox id="vsplit" axis="vertical" split="70">
          <tabs id="editor-tabs"/>                              <!-- H04 host; EditorViews mount here -->
          <tabs id="panel-tabs">                                <!-- H07 -->
              <tab label="Terminal"><contentbox id="terminal"/></tab>
              <tab label="Problems"><tableview id="problems"/></tab>
              <tab label="Output"><contentbox id="output"/></tab>
              <tab label="Test"><tableview id="testresults"/></tab>
          </tabs>
      </splitbox>
  </splitbox>
  <contentbar id="statusbar" dock="bottom">…</contentbar>       <!-- H08 -->
`));
```

- **Menu bar** (top `contentbar` + `menu`): File / Edit / View / Run / Help. Every item fires a
  named command (H03). Accelerators (`^S`, `^P`, `^`\`) are bound in `app.keymap()` and win at the
  capture phase (Sonar R11) so a focused editor never swallows `^S`.
- **Sidebar** (`contentbox` + `tabs`): the file **`treeview`** (H05) and a search pane (H12).
  Collapsible via a View command that toggles the `hidden` class on `#sidebar` and re-splits.
- **Editor group** (`tabs` of `EditorView`s, H04): the center. Tabs carry the buffer's short name
  + a dirty dot; middle-click / `^W` closes.
- **Panel** (`tabs`: Terminal / Problems / Output / Test): the bottom dock, `^`\` toggles it.
- **Status bar** (bottom `contentbar`, H08): mode indicator, cursor `Ln,Col`, file encoding,
  language (`Leviathan`), diagnostics counts (`● 2  ▲ 5`), build state (spinner while building),
  Git branch. Segments are named regions updated by `query("#statusbar")…setLeft/…`.
- **Overlays** (Sonar overlay stack, R13): the **command palette** and **quick-open** are
  `modal`-hosted overlays that own input exclusively while open; **Escape** pops them.

---

## 4. The buffer/editor model (H04 — the hard part)

No Sonar component edits code; this is where Helm earns its keep. Split into a headless model
(`H04a` buffers) and the view (`H04b` `EditorView`).

### 4.1 Why not store text in the component tree

Leviathan arrays are **pure/persistent** (`arr = arr.add(x)` copies-on-write; cheat-sheet §5) and
strings are byte-counted immutable. Storing a document as an `Array<string>` of lines and
rebuilding it per keystroke is O(n) copies per edit — unacceptable for a real editor. The model
therefore uses a **piece table** (a.k.a. piece chain) over two immutable byte spans:

- **`original`** — the file's bytes at load, a single immutable `Block`.
- **`added`** — an append-only `Block` of inserted text (grown in chunks; F1 `Block.blit` does the
  native copy).
- **pieces** — an `Array<Piece>` where `Piece { int buf; int off; int len; }` (`buf` = 0 original
  / 1 added). Edits splice the *piece array* (small: one insert = at most 3 pieces), never the
  text bytes. Undo/redo is a stack of piece-array snapshots (cheap: the array is small and
  persistent-sharing).

`Piece` and every parallel-column struct that lands in an array is declared **`class`, not
`struct`** — the Sonar tracks hit `Array<struct-with-...-field>` staleness/corruption repeatedly
(bug #41 / #74 precedent; sonar T03/T07 logs). Where columns must be primitives+closures, use
**parallel arrays**, never `Array<struct>` (the `ActionRegistry` pattern, `dom/actions.lev`).

**Line index.** A `LineIndex` maps line → byte offset, rebuilt incrementally on edit within the
touched piece range (not globally). Big-file guard: files over a size threshold open **read-only,
no highlight** with a banner (v1 honest limit; the piece table itself scales, the syntax pass and
diagnostics do not).

### 4.2 `TextBuffer` contract (frozen H-C1)

```lev
class TextBuffer {
    new FromFile(string path);            // loads bytes into `original`
    new Empty();
    string  path();     bool  isDirty();
    int     lineCount();
    string  line(int i);                  // decoded line i (no trailing newline)
    string  slice(Range r);               // r = (Pos start, Pos end); Pos = (int line, int col-in-scalars)
    void    insert(Pos at, string text);  // splices pieces; bumps version; records undo
    void    remove(Range r);
    int     version();                    // monotonic; the reparse/highlight/diagnostics key
    void    undo();   void redo();
    void    save();                       // writes bytes; clears dirty; keeps piece table
    int     onChange((ChangeEvent) => void h);   // token; offChange(t) — Sonar R12 token pattern
}
```

`col` is measured in **Unicode scalars** and rendered through `Sonar::glyphWidth` for wide-cell
math (cheat-sheet §10), matching Sonar's Surface contract. `ChangeEvent` carries the dirtied
line range so highlighting and the diagnostics debounce only touch what moved.

### 4.3 `EditorView` component (frozen H-C2)

`EditorView : Container, Scrollable, Focusable, Bordered` — composed exactly like a Sonar
composite leaf, and it **redeclares** every `Container`/`Scrollable` method it relies on
(`paint`, `arrange`, `contentDesired`, `__sonarChildren`, `contentRect`, `scrollTo`) forwarding to
the working accessors. This is not optional politeness: the still-open multi-mixin bugs (the
`Container.paint()` children-loop that renders nothing for a multi-mixin leaf; the intermediate-
mixin dispatch family) documented in the Sonar T05/T07 logs make the redeclaration **load-
bearing**. `TableColumn`/tree-row precedent: declare row/column carriers as `class`.

Responsibilities:

- Owns a `TextBuffer` + `Cursor`(s) + selection + a viewport (`scrollY/scrollX` from `Scrollable`).
- `paintContent(Surface s)`: paints **only visible lines** (`scrollY .. scrollY+height`), each as
  spans colored by the highlight cache (§6.1). Gutter (line numbers, diagnostic squiggles/marks,
  Git change bar) is a fixed-width left inset. Zero allocation in the hot loop — write directly to
  the Surface via `writeText`; reuse a scratch style. Wide glyphs use `glyphWidth`.
- Cursor: `cursorPos()` (the `Focusable` hook Sonar's App reads post-paint) returns the caret cell
  so the terminal hardware cursor lands correctly; multi-cursor secondary carets are painted as
  reverse-video cells.
- Input: `onKey` handles motion, editing, selection (Shift+motion), clipboard, indent, comment-
  toggle; printable keys insert. Bound editor commands (`^D` duplicate line, `^/` comment, `Alt-↑`
  move line, etc.) are registered as **scoped keymap** entries (Sonar R11 scoping) so they beat
  focus routing only while the editor is focused, without stealing global accelerators.
- Mouse: click-to-place, drag-to-select, wheel-scroll, click-in-gutter to select line.

**Editing correctness discipline (from the Sonar bug corpus):** every stored handler spells its
receiver explicitly (`this.insertChar(e)`, never a bare implicit-`this` call from a lambda stored
in a field — bug #53 segfaults on LLVM otherwise). Timer/handler registration structs use
explicit constructors, never positional auto-construction (bug #38).

### 4.4 Undo, clipboard, multi-cursor

Undo is buffer-level (piece-array snapshots, coalesced by a typing-run timer so a burst of
keystrokes is one undo step). Clipboard is an in-process register plus an opt-in OSC 52 terminal-
clipboard bridge (best-effort; guarded, off by default). Multi-cursor is a `Array<Cursor>` on the
view; edits apply per-cursor lowest-offset-last so offsets don't shift under each other.

---

## 5. Module / track map

Disjoint file ownership is normative (Sonar house convention): a track writes only its own files.
`helm/src/` is the package root; namespace `Helm`.

| ID | files | owns | difficulty | gates on |
|----|-------|------|-----------|----------|
| H01 | `helm/src/shell.lev`, `main.lev` | boot, root markup, dock layout, menu bar, DI composition root | M | Sonar v1 |
| H02 | `helm/src/workspace/*.lev` | `Workspace`, project root, open-buffer set, file watching hooks | M | H09 |
| H03 | `helm/src/command/*.lev` | `CommandRegistry`, `Command`, keymap binding, menu/palette wiring | M | Sonar keymap/actions |
| H04 | `helm/src/editor/*.lev` | `TextBuffer` (piece table), `LineIndex`, `Cursor`, `EditorView`, undo, clipboard | **XL** | Sonar core; F1 Block ops |
| H05 | `helm/src/explorer/*.lev` | file `treeview` model + `IListSource`-style adapter, file ops (new/rename/delete) | M | Sonar treeview, H09 |
| H06 | `helm/src/lang/*.lev` | language-service bridge: diagnostics, highlight token stream, (later) hover/goto/complete | **L** | H09; **G-LANG-1** |
| H07 | `helm/src/panels/*.lev` | Problems table, Output view, Test-results table, panel host | M | H06, H10 |
| H08 | `helm/src/status/*.lev` | status bar segments, build-state indicator | S | H03, H06, H11 |
| H09 | `helm/src/proc/*.lev` | subprocess bridge (spawn+Channel), line framing, cancellation | **L** | language `spawn`/`Channel`, process floor |
| H10 | `helm/src/terminal/*.lev` | integrated terminal: PTY host + a minimal VT parser + `TerminalView` | **XL** | H09; **G-LANG-2** (PTY floor) |
| H11 | `helm/src/build/*.lev` | trident driver: build/run/test commands, build-plan parse, error routing to Problems | M | H09, H06 |
| H12 | `helm/src/config/*.lev`, `search/*.lev` | settings/keymap TOML, theme selection, workspace search (grep) | M | Sonar theming, H09 |
| H13 | `helm/tests/**`, `helm/trident.toml`, `helm/examples/**` | golden/scripted/differential harness, packaging | M | all |

Recommended landing order: **H09 → H03 → H02 → H05 → H04 → H06 → H11 → H07 → H08 → H01 (assemble)
→ H12 → H10 → H13**. Rationale: the subprocess bridge and command system are the spine everything
plugs into; the editor is the long pole but only needs H09/H03; the integrated terminal (H10) is
the riskiest and is deliberately last so a red PTY floor doesn't block a usable IDE (Helm is fully
functional with an "external terminal" fallback and the Output/Problems panels).

---

## 6. Language intelligence (H06 — the bridge)

Helm has **no compiler in-process**. Intelligence is computed three ways, cheapest first:

### 6.1 Syntax highlighting — in-process, lexer-shaped, no subprocess

Highlighting must be instant and can't wait on a subprocess per keystroke. Helm ships a **small
in-language Leviathan lexer** in `helm/src/lang/highlight.lev` (keywords, identifiers, numbers,
strings, chars, comments, operators, punctuation, and the `@Attr`/`macro!`/quasiliteral forms) —
a *highlighter*, not the real compiler lexer, deliberately lossy and resilient to incomplete code.
It runs **per changed line range** (the `ChangeEvent` from H-C1), producing a per-line token span
cache keyed by `buffer.version()`. `EditorView.paintContent` reads the cache; a cache miss paints
the raw line uncolored and schedules the re-lex. Token → theme-key mapping (`syntax.keyword`,
`syntax.string`, …) resolves through Sonar theming (R10), so highlight colors are themeable TOML.

Keeping a *second, smaller* lexer (rather than shelling to `leviathan --emit-ir` for tokens) is a
deliberate cost/robustness call, explicitly logged: it must tolerate the half-typed code the real
lexer rejects, and it must never block a frame. If the real compiler later exposes a token stream
(`--tokens`), the highlighter can cross-check offline but not depend on it at frame time.

### 6.2 Diagnostics — subprocess, debounced, version-keyed

On buffer idle (debounce ~300 ms after the last edit, via `app.every`/a timer), H06 asks H09 to
run the compiler over the **saved-or-shadow** source set and parses diagnostics:

- v1 parses stderr text: `path:line:col: error: message` / `warning:` / `note:` lines (the
  compiler's existing human format). A tolerant parser with a golden corpus of real compiler
  output (`helm/tests/diag-corpus/`) so format drift is caught by a failing test, not silently.
- **G-LANG-1** upgrades this to a machine format (`--diagnostics=…`, §1.2). When it lands, H06
  swaps the parser and deletes the brittle path — behind an `IDiagnosticSource` DI seam so the
  swap is one bind.

Diagnostics carry the `buffer.version()` they were computed against; a result older than the
current version is **dropped** (the edit already moved on). Results fan out to: gutter marks +
inline squiggles in `EditorView`, the **Problems** table (H07, click-to-jump), and the status-bar
counts (H08).

**Shadow-file question (open):** editing an unsaved buffer means the on-disk source is stale.
v1 writes a **temp shadow copy** of dirty buffers to a scratch dir and points the compiler at the
shadow set (the compiler compiles the source set it's handed — it doesn't need the real paths).
This is honest but I/O-heavy; §8 tracks a stdin-source compiler mode as the clean fix (another
G-LANG item).

### 6.3 Navigation & completion — v1.1, subprocess or index

Goto-definition, find-references, hover types, and completion are **v1.1**, not v1. They need
semantic info the stderr diagnostics don't carry. Two candidate seams, decided when we get there:
(a) a compiler query mode (`leviathan --symbol-at file:line:col`), or (b) an in-process symbol
index built from the highlighter's parse plus a lightweight resolver. Both are real work; v1 ships
without them and the command palette greys the corresponding commands. Recorded so the v1 editor
isn't over-built to anticipate an unchosen design.

### 6.4 `IDiagnosticSource` / `ILanguageService` DI seam (frozen H-C3)

```lev
interface ILanguageService {
    int         onDiagnostics((DiagnosticBatch) => void h);   // token; batches are version-keyed
    void        requestDiagnostics(BufferSnapshot snap);       // debounced upstream; H06 dedupes by version
    TokenLine   highlight(string line, int lineNo, HighlightState carry);  // in-process, sync
    // v1.1: Location? definitionAt(...); Array<Completion> completeAt(...); string? hoverAt(...);
}
```

DI (`bind ILanguageService => LeviathanService()`) lets tests bind a `FakeLanguageService` that
returns scripted diagnostics with zero subprocesses (H13). `DiagnosticBatch` and `TokenLine` are
value carriers built from parallel columns / `class` rows, never `Array<struct>`.

---

## 7. Build, run, test (H11) + subprocess bridge (H09)

### 7.1 Subprocess bridge (H09, frozen H-C4)

```lev
class Proc {
    new Proc(string exe, Array<string> args, string cwd);
    Channel<ProcEvent> start();      // spawns a reader task; events: Stdout(line)|Stderr(line)|Exit(code)
    void write(string bytes);        // stdin (terminal / stdin-source compiler mode)
    void signal(int sig);            // cancellation (SIGINT/SIGTERM) — reuses Sonar F2 signal consts
    bool running();
}
```

A `spawn`ed reader task frames stdout/stderr into lines and pushes `ProcEvent`s onto the channel;
the UI task drains at frame start. **No bare global `Promise` crosses the `spawn` boundary** (bug
#35) — the `Channel` is the only portal (cheat-sheet §15). Cancellation is a signal, and every
long-running command is cancellable from the UI (the build spinner's `^C`). This needs a language
**process floor** (spawn a child, capture fds, deliver a signal). If the runtime lacks a piece of
it, that's **G-LANG-2** (a language-side design), not something Helm improvises in `runtime/**`.

### 7.2 trident driver (H11)

Commands (all registered in H03, so they're on the palette, menu, and keymap):

- **Build** (`^B`): `trident build` in the workspace root. Output streams to the Output panel;
  compiler diagnostics are also routed into Problems (reusing H06's parser). Status bar shows a
  spinner during, then ✓/✗.
- **Run** (`F5`): `trident run`. Stdout/stderr stream to a dedicated Output/Terminal tab. `Shift-F5`
  stops (signal).
- **Test** (`^;` or a Run submenu): `trident test` (drives `harpoon`); the Test panel is a
  `tableview` of `@Test` results (name / pass-fail / time / message), parsed from harpoon's output;
  click-to-jump to the failing assertion's source line.
- **Choose engine** (a command + status-bar affordance): pass through `--run`/`--ir`/`--emit-llvm`
  so a developer can differential-run the same program across engines — a first-class Leviathan
  workflow, and Helm should surface it because the project *lives* on differential testing.

Build-plan awareness is read-only: Helm parses trident's plan/output; it never re-implements
dependency resolution (the compiler/PM separation is frozen — §9).

---

## 8. Integrated terminal (H10)

The riskiest track, deliberately last. A real terminal needs a **PTY** (allocate, set winsize,
read/write, reap) and a **VT parser** (a subset: cursor moves, SGR colors, erase, scroll region,
the alt screen) driving a `TerminalView` backed by a Sonar `Surface`-like cell grid. Two hard
dependencies:

- **PTY floor (G-LANG-2, language-side):** openpty/forkpty-equivalent + winsize. If absent, v1
  ships **no embedded terminal** and instead offers "Run in external terminal" + the streaming
  Output panel (which covers 90% of the build/run/test need). This keeps a red PTY floor from
  sinking the IDE.
- **VT parsing** is in-package Leviathan work (an `ansi` inverse of Sonar's `ansi_renderer`).
  Bounded scope: enough to host a shell and the project's own Sonar TUIs, not xterm-completeness
  (sixel/DEC specials are out, matching Sonar's own non-goals).

Because the terminal *is* itself a Sonar surface, a fun consequence: Helm can host a Sonar app
(including Helm-built TUIs, or `recon`) inside its own terminal panel. Nice-to-have, not a v1 gate.

---

## 9. Frozen conventions, STOP protocol, footgun discipline

**House rules (binding):** `.lev` only. Helm never touches `src/**` or `runtime/**`. A language
gap found mid-implementation is a **STOP** — record it in the track doc's log + a `bug.md`-style
repro; implementers escalate, design agents record in §14 only. Never merge compiler and package-
manager concerns; never re-implement dependency resolution in Helm. X64Gen/ELF frozen; nothing
gates on ELF. Completed designs move to `designs/complete/`.

**Footgun discipline baked into every track** (from `known_bugs_1.md`/`known_bugs_2.md` + the Sonar bug corpus):

1. No truthiness — conditions are `bool`; `x == None`/`!= None` with narrowing; `?.`/`??`.
2. No `static` — namespace consts/functions + labeled constructors (`TextBuffer::FromFile`).
3. Maps write via `m[k] = v` bracket sugar; `.with/.without` are missing on LLVM (bug #18) — use
   bracket sugar or parallel arrays.
4. **Arrays are pure** — never store a per-frame-mutated `Array<struct>`; the piece table mutates a
   small persistent array, hot state is `class`-typed or parallel columns.
5. `Array<struct-with-nested-field>` corrupts on LLVM after unrelated writes — row/column carriers
   are **`class`** (Sonar `Chord`/`TableColumn`/`TreeRow` precedent).
6. Stored handlers spell the receiver explicitly (`this.method(...)`) — bare implicit-`this` from a
   field-stored lambda segfaults on LLVM (bug #53).
7. Registration structs use explicit constructors, never positional auto-construction (bug #38).
8. Multi-mixin leaves (`EditorView`) redeclare inherited `Container`/`Scrollable` methods they rely
   on (children-loop paint bug still open).
9. Inside `namespace Helm`, write globals bare (never `Helm::x = v` — lowering hazard).
10. No JSON on LLVM (bug #30) — settings/keymap/themes are in-language TOML (Sonar's `toml.lev`).
11. No `console.write` during a running app — diagnostics via `Sonar::log` + `DebugOverlay`.
12. Don't `await` a bare global `Promise` across `spawn` (bug #35) — `Channel` only.
13. Declare lambda-taking overloads before same-name string-taking overloads (bug #34).

Any Helm feature that would require *breaking* one of these is a design smell to escalate here,
not to route around silently.

---

## 10. Gates & timeline

| gate | contents | depends |
|------|----------|---------|
| G-H0 | H09 proc bridge + H03 command registry; a "run trident build, stream to Output" spike works | Sonar v1, process floor |
| G-H1 | H02 workspace + H05 file tree; open/close/switch files (read-only viewer) | G-H0 |
| G-H2 | H04 editor: piece-table buffer + `EditorView` edit/save/undo/highlight; the editor is usable | Sonar core, F1 |
| G-H3 | H06 diagnostics (stderr parse) + H07 Problems/Output + H08 status bar; edit→diagnostics loop closes | G-H2 |
| G-H4 | H11 build/run/test wired to commands + palette + menu; H01 shell fully assembled | G-H3 |
| G-H5 | H12 settings/keymap/theme + workspace search; self-host milestone: **edit Helm's own source in Helm** | G-H4 |
| G-H6 | H10 integrated terminal (or the external-terminal fallback if G-LANG-2 is red) | G-H5 |
| G-v1 | H13 full golden/scripted/differential suite + docs; tag `helm 0.1.0` | all |
| **G-LANG-1** | compiler machine-readable diagnostics flag (language-side design) | — |
| **G-LANG-2** | process/PTY floor sufficient for H09/H10 (language-side design) | — |

The **self-host milestone (G-H5)** is the honesty test: Helm is done enough when its own authors
edit, build, and test Helm inside Helm.

---

## 11. Testing doctrine (H13)

Inherits Sonar's, because it renders through Sonar:

- **Snapshot** — `TestRenderer` goldens of the shell, editor viewport (incl. highlight color
  channel), Problems table, palette overlay. Two-channel format (text grid + style annotations) so
  a color-only change doesn't churn text goldens.
- **Scripted input** — `ScriptedInput` byte scripts + `App.pumpOnce()` drive deterministic
  editing sessions (type-a-function, trigger-a-diagnostic, jump-to-error, save) with golden
  end-states. Split-escape-sequence scripts exercise the input path.
- **Differential** — byte-identical snapshots across oracle / IR / LLVM (the project doctrine).
  emit-C++ is compile-only for anything touching `run()`.
- **Unit** — piece-table edit algebra (insert/remove/undo/redo invariants: bytes reconstructed ==
  expected), `LineIndex` incremental correctness, diagnostic-parse corpus, highlighter token
  tables — all `harpoon` `@Test`, table-driven.
- **Fake services** — `FakeLanguageService` + `FakeProc` (bind via DI) make the whole edit→
  diagnostics→Problems loop testable with zero subprocesses. The subprocess bridge itself gets a
  small live integration test against a real `leviathan`/`trident`, isolated so it can be skipped
  in the hermetic differential lane.

Every track ships its own test plan in its track doc; H13 owns the harness.

---

## 12. Risk register (run before G-H2)

| # | risk | mitigation / fallback |
|---|------|----------------------|
| K1 | Piece-table edit perf under pure-array cost model | pieces are small + persistent-sharing; undo coalesced; big files open read-only/no-highlight; bench under `bench/` before G-H2 |
| K2 | Multi-mixin `EditorView` renders nothing (open Sonar children-loop bug) | redeclare inherited paint/arrange/children (§4.3); probe first, single-inheritance-chain fallback like Sonar P7 |
| K3 | `Array<struct>` staleness/corruption in hot editor state | `class` rows + parallel columns everywhere (§9.4-5); grep the corpus like Sonar T04 did |
| K4 | Diagnostic stderr-format drift breaks the parser | golden diag-corpus test; DI seam to swap to G-LANG-1 machine format |
| K5 | No process/PTY floor (G-LANG-2 red) | external-terminal + Output-panel fallback; terminal is the *last* gate, not a blocker |
| K6 | `spawn`/`Channel` async correctness (bug #35 family) | single-writer UI task (H-R1); Channels only; a focused async harness before H09 lands |
| K7 | Shadow-file I/O churn for unsaved-buffer diagnostics | debounce hard; pursue compiler stdin-source mode (G-LANG item); cache by version |
| K8 | Scope creep into a debugger/LSP/plugins | explicit v1 non-goals (§0); v1.1 seams recorded but unbuilt |

---

## 13. Open questions (decide before the owning gate)

1. **Shadow files vs stdin compiler mode** for unsaved-buffer diagnostics (§6.2) — owner H06/G-H3.
2. **Navigation/completion seam** (compiler query mode vs in-process index) — deferred to v1.1,
   don't over-build the v1 editor for it (§6.3).
3. **Clipboard**: OSC 52 only, or also an X/Wayland bridge? v1 = in-process register + opt-in OSC
   52 (§4.4).
4. **File watching**: poll vs an inotify floor for external-change detection — v1 may poll on focus.
5. **Multi-cursor in v1?** Model supports it (§4.4); ship single-cursor first, multi behind a flag.

---

## 14. Implementation log (append-only)

- 2026-07-15 — doc created. Architecture, module tracks H01–H13, contracts H-C1..H-C4 + rule
  H-R1, gates G-H0..G-v1 + language gates G-LANG-1/2 set. Two escalations pre-recorded as
  language-side gates rather than improvised: machine-readable compiler diagnostics (G-LANG-1) and
  a process/PTY floor for the subprocess bridge and integrated terminal (G-LANG-2). Grounded
  against `sonar_v2/src/dom/*` (verified `SonarApp`/`DomNode`/`query`/actions API) and the live
  compiler-bug corpus (piece table + `EditorView` designed around bugs #18/#30/#34/#35/#38/#41/
  #53/#74 and the open multi-mixin children-loop defect).
- 2026-07-15 — **G-H0 landed.** Package created at `examples/helm/` (a trident package depending on
  `sonar_v2` as `Sonar`; sibling of `examples/recon`). Implemented H09 (`src/proc/proc.lev`) and
  H03 (`src/command/command.lev`), plus a headless entry `src/main.lev`. Golden tests green on
  **oracle + IR** (`tests/{command,proc,spike}`, driven by `tests/run-tests.sh`): the spike fires a
  named command via a `^B` keymap → spawns a child → streams `ProcEvent`s into an Output sink →
  exit — the "run a build, stream to Output" loop.
  - **H-C4 refinement (design vs language reality).** The spec's `Channel<ProcEvent> start()` with a
    "`spawn`ed reader task" is **not implementable as written**: `Process` (reference §6.6.59) is a
    fd-/loop-bound carrier that the flattenability rules (§6.6.66) forbid from crossing a
    `std::spawn` boundary, and its I/O is already event-loop driven on the UI thread. So there is no
    worker task and no cross-thread Channel — the "reader" is `Process`'s own
    `onStdout`/`onStderr`/`exitCode` callbacks, which enqueue onto an in-process `ProcEventQueue`
    the UI drains synchronously at frame start (`ProcStreamer` is the headless drain-on-tick
    driver). Single-writer H-R1 holds trivially (one thread). Net: `start()` returns a
    `ProcEventQueue`, not a `Channel`; H-C4's signature is amended to match.
  - **Floor reality (G-LANG-2).** `Process`/spawn run on **oracle + IR only**; compiled backends
    defer the spawn natives. Helm's compiler/build/test/terminal features therefore run on the
    interpreter engines in v1 — the golden lane is oracle+IR, not LLVM. `cwd` has no `Process`
    chdir, so a non-empty cwd is folded into `/usr/bin/env -C <cwd> <exe> …` (callers usually pass
    an explicit workspace path to `trident`/`leviathan` instead). `signal(sig)` maps to `SIGTERM`
    (`Process.kill()` is the only exposed signal). All recorded under the existing G-LANG-2 gate;
    no new escalation.
  - **Compiler bug found and FIXED** (task explicitly authorized bug fixes; this is a resolver
    correctness fix, not a frozen-contract/architecture change, so not a STOP). A `namespace`
    reopened across files saw only the **first block's** file-level `uses` (the shared namespace
    scope was re-parented once, to the first block's import overlay). Order-dependent: because
    `proc.lev` (no `uses Sonar`) globbed as Helm's first block, every unqualified Sonar type in the
    namespace (e.g. `Keymap` in `command.lev`) failed to resolve — even in files that *did*
    `uses Sonar`. Fix in `src/Resolver.cpp`: give each reopened namespace its own aggregate import
    overlay folding in **every** block's file overlay (order-independent). Regression floor:
    `tests/corpus/project/reopen_ns_uses_order`. Verified green: 26 multi-file projects, all
    composition corpora, `sonar_v2` differential suite. With the resolver fixed, Helm standardized
    on the idiomatic `uses Sonar;` + unqualified names (matching Sonar/recon); the corpus test is
    the regression guard.
- 2026-07-16 — **G-H1 landed.** H02 (`src/workspace/{workspace,paths}.lev`) and H05
  (`src/explorer/filetree.lev`) — the read-only file viewer. Two new golden tests
  (`tests/{workspace,filetree}`, oracle+IR); the headless `main <dir>` now opens a `Workspace` and
  prints the explorer tree (5 Helm tests total, all green).
  - **H02 Workspace** — project root + open-document set (open/activate/close, active tracking,
    idempotent re-open, `openPaths`) + a v1 file-watch hook (`isStale`/`reload` by mtime, the
    §13.4 poll-on-focus stance). An open doc is a minimal immutable `OpenDoc` (path + content +
    mtime); when H04's piece-table `TextBuffer` lands it supersedes `OpenDoc` in the open-set (kept
    deliberately small so the swap is clean). Pure model + prelude `File` I/O — no Sonar dep.
  - **H05 FileTree** — implements `Sonar::ITreeSource` (listview.lev) so the virtualized `TreeView`
    can paint the viewport without the whole tree resident. Lazy per-directory listing via
    `std::sysListDir`, dirs-first + alphabetical; `TreeNodeId` wraps a stable append-only node id
    (`class FileNode` rows, never `Array<struct>`). File ops (create/rename/delete via
    `std::sysMkdir`/`sysRename`/`sysRemove`, `File(...,std::write)`) re-list the affected parent.
  - Language facts used along the way (recorded so later tracks don't relearn): early-`return`
    optional-narrowing doesn't persist — use `?? default` (footgun); `sortBy` infers its key, the
    explicit `sortBy<T>(…)` form is rejected in value position; program args arrive after `--` with
    `env::args()[0]` an engine-dependent program name, so select args by predicate, not index.
```

- **2026-07-16 — G-LANG-2 process half is GREEN on LLVM** (language-side landing,
  `designs/complete/techdesign-spawn-llvm.md`): `sysSpawn`/`sysPidfdOpen`/`sysReap`/`sysKill` now
  compile and run under `--build-native`, byte-identical to the interpreters
  (`tests/corpus/sys_spawn/`, oracle=IR=LLVM). Helm's proc-bridge (H09) features are no
  longer interpreter-pinned — the golden lane can add LLVM for proc-bridge tests. The PTY
  floor (the terminal half of G-LANG-2, needed by H10) remains open.

- 2026-07-17 — **G-H2 landed.** H04, the editor — the XL long pole. Five new `.lev` files under
  `examples/helm/src/editor/` and two golden tests (`tests/{buffer,editor}`); 7 Helm tests total,
  all green on **oracle + IR** (byte-identical). Split headless-model (H04a) / Sonar-view (H04b),
  exactly per §4.
  - **H04a — the buffer/edit algebra (headless, no Sonar).**
    - `piece.lev` — `PieceTable` over two immutable **`Block`** byte stores (`original` +
      append-only `added`, doubled on growth so repeated inserts amortise O(1)). Byte-offset
      splice API: `insert` splits ≤1 piece (3 out), `remove` trims the two boundary pieces + drops
      the middle, `textRange` materialises via `Block.toString`. `Piece`/`PieceLoc` are **class**
      rows (footgun #5 / Sonar #66). Undo/redo = snapshots of the small persistent pieces array
      (the shared `added` store only grows, so old offsets stay valid forever).
    - `lineindex.lev` — `LineIndex`: line→byte-start, found by scanning **bytes** for 0x0A
      (correct for UTF-8 — multibyte bytes are all ≥0x80 — and dodges the `string.at` mid-sequence
      throw, Sonar #59). v1 rebuilds fully on edit (O(n) scan); incremental is a noted perf rider
      (K1), slots behind the same interface. Big files still open read-only per §4.1.
    - `cursor.lev` — `Pos`/`Range`/`Cursor` **class** carriers (col in **scalars**); Cursor holds
      a selection anchor + sticky `goalCol` for vertical motion.
    - `buffer.lev` — `TextBuffer`, the **frozen H-C1** contract, owning the scalar↔byte
      translation (`utf8Len` off the codepoint, no allocation) so nothing above it touches a byte.
      `FromFile`/`Empty`/`FromString`, version, dirty, undo/redo, `save`, `onChange` (R12 token,
      version-keyed `ChangeEvent` with the dirtied line range). Golden covers split inserts,
      newline inserts, remove, slice, a **wide-glyph** (世) column, undo/redo byte reconstruction,
      and change-event fan-out. (Test-side footgun logged: a closure that reassigns a captured
      *local* array doesn't write back — accumulate through a reference-type field instead.)
  - **H04b — `EditorView`, the Sonar component (frozen H-C2).** Virtualized (paints only
    `scrollY..scrollY+h` lines), gutter with right-aligned 1-based line numbers, wide-glyph-aware
    caret + horizontal/vertical auto-scroll, selection painting (Shift+motion), full editing
    (insert/enter/backspace/delete, selection-delete, Ctrl+Z/Y undo/redo, in-process
    Ctrl+C/X/V clipboard, bracketed paste). `cursorPos()` returns the box-relative caret (Input
    contract). Golden drives synthetic `KeyEvent`s → paint → `TestRenderer.snapshot()` (text +
    style channels + `@cursor`); a reverse-video selection theme makes selections visible in the
    style channel; scrolling verified with a 10-line buffer in a height-6 viewport.
    - **H-C2 refinement (design vs language reality — the H-C4 precedent).** §4.3 froze the header
      as `EditorView : Container, Scrollable, Focusable, Bordered` and made "redeclare inherited
      Container/Scrollable methods" load-bearing against the open multi-mixin children-loop paint
      bug. The direct Sonar precedent resolves this more cleanly: **TreeView** — an
      identically-shaped virtualized/scrollable/focusable leaf that paints rows, not children — is
      deliberately **NOT a Container**. An editor has no child components (it paints text spans), so
      being a Container would only re-expose the children-loop bug for zero benefit. `EditorView`
      therefore mirrors TreeView's header exactly (`Focusable, Scrollable, Bordered, Styleable`) and
      redeclares the methods the multi-mixin dispatch family can drop (`scrollTo`, `paintChrome`)
      plus its overrides (`contentDesired`, `paintContent`, `cursorPos`). H-C2's "Container" line is
      amended to match; the redeclaration discipline it mandated is honoured against the
      non-Container base the codebase's own precedent proves safe. No new escalation.
  - **Floor / lane note.** Nothing in H04 is process-pinned (pure `Block` + Sonar `TestRenderer`,
    both LLVM-supported), so the §11 three-lane oracle=IR=**LLVM** byte-identity is *expected*
    green; wiring the native lane (`tests/run_native.sh` + the runtime `.S` context-switch objects)
    is deferred to H13's harness rather than the Helm test runner (which stays oracle+IR because the
    proc/spike tests remain there). No compiler bugs hit; no `src/**`/`runtime/**` touched.
  - **Next gate G-H3** = H06 diagnostics (stderr parse) + H07 Problems/Output + H08 status bar; the
    edit→diagnostics loop. The highlight seam is ready (paintContent reads one resolved text style
    today; H06 swaps in the per-span highlight cache keyed by `buffer.version()`). `OpenDoc`
    (§G-H1) is now ready to be superseded by `TextBuffer` in the Workspace open-set.
- 2026-07-17 — **G-H3 landed.** H06 diagnostics + H07 Problems/Output/Test panels + H08 status bar
  — the edit→diagnostics loop closes. Eight new `.lev` files under `examples/helm/src/{lang,panels,
  status}/` and four golden tests (`tests/{diag,panels,status,langloop}`); **11 Helm tests total,
  all green on oracle + IR** (byte-identical). No `src/**`/`runtime/**` touched; no compiler bugs
  hit.
  - **H06 — the language-service bridge (`src/lang/`).**
    - `diagnostic.lev` — `Diagnostic` (path/line/col/severity/message) + `DiagnosticBatch`
      (version + `Array<Diagnostic>`), both **class** rows (no `Array<struct>`). Severity int
      consts + `sevGlyph` (`● ▲ ‣ ◦`); the batch is version-keyed (§6.2) with error/warning counts
      and a `forPath` per-buffer view.
    - `diagparse.lev` — `IDiagnosticSource` (the G-LANG-1 swap seam) + `StderrDiagnosticSource`,
      which parses the **exact** `src/Diagnostic.cpp` fprintf forms verified against the live
      compiler: spanned `path:line:col: sev: msg` and whole-file/prelude `path: sev: msg`, skipping
      the source-snippet, caret, and `N error(s)` summary lines (tolerant by construction — K4).
      A golden corpus (`tests/diag`) is the format-drift guard.
    - `langservice.lev` — `BufferSnapshot`; the frozen **H-C3** `ILanguageService` (diagnostics
      surface); a shared `DiagSubscribers` R12 token registry; `LeviathanService` (the live path);
      `FakeLanguageService` (the test seam); and `DiagnosticsController`, which version-keys results
      (drops a batch older than the newest request) and fans surviving batches to its sinks,
      `bindBuffer`ing a `TextBuffer` so an edit requests diagnostics without the editor knowing the
      service exists.
    - **H-C3 scope refinement (design vs gate — the H-C4/H-C2 precedent).** §6.4 froze
      `ILanguageService` with a synchronous in-process `highlight(...)` alongside the diagnostics
      methods. Highlighting (§6.1) is a **separate concern** from the diagnostics loop G-H3 names,
      and the `EditorView` highlight seam already exists (it reads one resolved text style, ready to
      swap in a per-span cache). So this gate ships the **diagnostics surface** of H-C3; the
      in-process lexer/`highlight()` lands as an H06 rider. No new escalation.
    - **Live-path floor note (G-LANG-2).** `LeviathanService` drives H09's `Proc` with
      `--emit-ir` over a shadow copy of the (possibly dirty) buffer — `Proc` runs on oracle+IR only,
      so this path is exercised by the **isolated live integration test** (§11), not the hermetic
      golden lane, which binds `FakeLanguageService`. The single-file shadow (vs a trident-driven
      cross-`uses` compile) is the honest v1 limit the shadow-file open question (§13.1) tracks;
      cross-file diagnostics are an H11 rider.
  - **H07 — the panels (`src/panels/`).** `problems.lev` (`ProblemsModel : ITableSource`, sorted
    errors→warnings→notes then path/line/col via a zero-padded composite `sortBy` key,
    `diagnosticAt` for click-to-jump), `output.lev` (`OutputModel` — a capped append-only line
    buffer + `IListSource`; `Proc`-free, H11 feeds it), `testresults.lev` (`TestResult` rows +
    `TestResultsModel : ITableSource`; harpoon-output parsing deferred to H11), and `panelhost.lev`
    (`PanelHost` owns the three models; `applyBatch` refreshes Problems and reveals its tab when a
    batch carries errors, never stealing focus on a clean batch).
  - **H08 — the status bar (`src/status/statusbar.lev`).** `StatusModel` composes mode / branch /
    diagnostics counts (`● 2  ▲ 5`) / build-state on the left and cursor `Ln,Col` / encoding /
    language on the right into `ContentBar`'s three regions (pure model — no Sonar dep; the golden
    renders it through a **real** `Sonar::ContentBar` + `TestRenderer`). Build-state int consts
    (idle/running/ok/failed); `applyBatch` folds a delivered batch into the counts — the status half
    of the loop.
  - **`langloop` golden** drives the whole loop with zero subprocesses: `FakeLanguageService` +
    `DiagnosticsController` + a live `TextBuffer`, proving edit→request→deliver→fan-out **and** the
    version-keyed drop (edit bumps to v2, a late v1 batch is discarded, Problems/counts unchanged),
    then recovery through v2 (errors→warning) and v3 (clean).
  - **Language facts (recorded so later tracks don't relearn).** A bare `version` field reference
    inside a method reads as an "ambiguous function reference" against same-named `version()`
    methods elsewhere in the program — qualify the field with `this.`. `sortBy((x) => keyString)`
    sorts ascending lexically, so a zero-padded composite key string gives deterministic
    multi-column ordering. `build/` is gitignored, so plan files stay out of the tree.
  - **Next gate G-H4** = H11 build/run/test wired to commands + palette + menu, and H01 shell fully
    assembled. The `IDiagnosticSource`/`ILanguageService` DI seams are ready for the trident-driven
    diagnostics path; `PanelHost.appendOutput` and the status build-state consts are the seams H11
    streams build output and ✓/✗ into.
- 2026-07-17 — **G-H4 landed.** H11 build/run/test driver + the H01 shell assembled. Four new
  `.lev` files (`src/build/{harpoon,builddriver}.lev`, `src/command/menu.lev`, `src/shell.lev`),
  `main.lev` rewritten to compose the shell, the package `trident.toml` sources widened to every
  `src/**` dir, and two golden tests (`tests/{build,shell}`); **13 Helm tests total, all green on
  oracle + IR** (byte-identical). No `src/**`/`runtime/**` touched; no compiler bugs hit.
  - **H11 — the trident driver (`src/build/`).**
    - `builddriver.lev` — `BuildDriver`, the single seam turning a Build/Run/Test command into a
      child process and routing its output. It reuses H09's `Proc`/`ProcStreamer` and H06's
      `StderrDiagnosticSource`/`StderrCollector`, owning only the job lifecycle + routing, driven
      through **`onEvent(ProcEvent)`** — the exact shape a live `Proc` streams, so the whole
      build→Output/Problems/Test/status loop is golden-testable with **zero subprocesses** (the
      `FakeProc` doctrine, §11). Pure argv builders (`buildArgs`/`runArgs`/`testArgs`/
      `engineRunArgs`) are shared by the live spawn and the golden. Job/engine tags are int consts
      (bug #40/#41). Build/test stderr parses into Problems + status counts (`applyBatch`); test
      stdout parses into the Test panel and reveals it; a plain `run`'s stderr is program output,
      **not** parsed as diagnostics. Exit code → `BuildOk`/`BuildFailed`. `stop()` = SIGTERM (§7.1).
    - `harpoon.lev` — `parseHarpoon(stdout)` → `Array<TestResult>`, matching
      `harpoon/src/runner.lev`'s exact report (`  Class::method … ok|FAIL|ERROR|skip`, six-space
      detail lines, `N run:` summary). Tolerant by construction (K4); the `tests/build` golden is
      the drift guard. harpoon reports no per-test timing/location in v1, so `ms`/`path`/`line` are 0.
    - **Design-vs-reality refinements (the H-C4/H-C2/H-C3 precedent; no new escalation).**
      (1) `trident` exposes **no `test` subcommand** (verified: `build|run|check|emit-llvm|plan`), so
      Helm's Test command runs a harpoon project via `trident run <testsDir>` and parses stdout.
      (2) "Choose engine" (§7.2) drives a differential run of the already-built plan through
      `leviathan --plan <ws>/build/plan.lvplan <flag>` — the exact invocation the project's own test
      runners use — surfaced as a status-bar affordance (`StatusModel.setEngine`, a new right-side
      `⚙ <engine>` segment that stays empty by default so H08's golden is unchanged).
  - **H01 — the shell assembled (`src/shell.lev`).** `HelmShell` is the single wiring point (the
    ReconApp precedent): it owns the `SonarApp`, the one `CommandRegistry` (12 commands — build/run/
    stop/test/engine×4/runEngine/togglePanel/palette/save/quit, each with its accelerator), the
    `MenuModel` (File/Edit/View/Run/Help), the `PanelHost`, `StatusModel`, and `BuildDriver`, and
    assembles them into the dock chrome (menu bar / body / panel-tab strip / status bar as real
    `ContentBar`/`ContentBox` widgets `app.add`-ed to the tree). Everything is wired in the ctor
    (accelerators `bindInto` the app keymap) so the shell is testable without `run()`. Two drivers:
    `run()` (the live `SonarApp.run()` loop) and **`renderShell(w,h)`** — a headless `TestRenderer`
    snapshot of the assembled chrome. `main.lev` composes the shell and prints its surface + a
    rendered snapshot by default (deterministic, non-blocking); `HELM_RUN=1` launches the live loop.
    - **Shell-snapshot refinement.** `renderShell` paints the top/panel/status bars directly onto one
      `Surface` (priming the default background with `surface.clear`, which the live App's root fill
      supplies) rather than driving `SonarApp.pumpOnce()` — matching the editor/status golden idiom
      and keeping the oracle+IR lane free of the frame-timer the event loop would start. The live
      EditorView/TreeView body mount, the command-palette overlay, and dropdown menus are H04/H05/H12
      riders on the live shell; the body is their (blank) mount region in the assembled snapshot.
  - **`tests/build`** drives the driver with synthetic `ProcEvent`s (build with 2 diagnostics→exit 1,
    a harpoon test report→exit 0, a run whose stderr is program output) and checks the panels/status
    routing + argv builders + engine choice. **`tests/shell`** constructs the shell and prints the
    command surface, the menu structure, a palette filter, and three `renderShell` snapshots (initial;
    after a diagnostic batch reveals Problems + folds the counts; after a `^`\`-style panel toggle).
  - **Next gate G-H5** = H12 settings/keymap/theme + workspace search, and the self-host milestone
    (edit Helm's own source in Helm). The `HelmShell` body is the EditorView mount point; the
    `CommandRegistry`/`MenuModel`/palette surface is ready for the settings + search commands.
