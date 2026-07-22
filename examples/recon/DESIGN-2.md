# Recon v0.2 — UX Modernization Technical Design (DESIGN-2)

**Making Recon a polished, feature-rich, intuitive terminal REST client.**

**Status:** design, ready to implement. **Date:** 2026-07-15. **Kickoff:** 2026-07-15.
**Supersedes:** `DESIGN.md` §9 (UI architecture) and parts of §12 (coding standard, per §13 below).
`DESIGN.md` remains normative for the headless core (§4–§8), the async model (§2.1), and
everything this document does not explicitly amend. `RESEARCH.md` facts are superseded where
noted — Moby has shipped substantially since it was written (see §2.1).

> **Why this document exists.** Recon v0.1 proved the architecture: the headless core is
> corpus-tested and correct, and the UI shell runs end-to-end. But the shell is **unclear and
> unpolished** as a product: keybinding hints advertise commands that were never bound, the HTTP
> method cannot be changed from the UI at all, headers cannot be edited, the theme file ships but
> is never loaded, errors flash by in a corner of the top bar, and a first-time user sees three
> empty boxes and no guidance. This design turns the working skeleton into a product, with the
> user experience as the explicit first-class deliverable.

---

## 0. Reading guide

| § | What it covers |
|---|---|
| 1 | The UX audit — every concrete complaint, numbered, each mapped to its fix |
| 2 | What changed under us (Moby/compiler deltas since v0.1) and what they unlock |
| 3 | UX principles (normative) |
| 4 | Visual design system: palette, color coding, glyphs, theme v2, a11y |
| 5 | Information architecture: zones, screens, the new main layout |
| 6 | The command system: one registry → keymap, menu bar, palette, hints, help |
| 7 | Request editing, complete: method, params, headers, auth, body, tests |
| 8 | The response experience: status line, JSON tree view, search, assert-from-response |
| 9 | Feedback: notices, error taxonomy, unresolved-variable flagging, the send experience |
| 10 | Workspace: history, environments, sessions/cookies, import/export, save/dirty, settings |
| 11 | Architecture & state changes; wiring fixes; reactivity decision |
| 12 | Engine lanes: promoting LLVM |
| 13 | Coding-standard delta (retiring dead bug workarounds — verified, not assumed) |
| 14 | Testing strategy |
| 15 | Implementation tracks, file ownership, milestones, risks, STOP protocol |
| 16 | Non-goals (v0.2 boundary) |

---

## 1. The UX audit

Every finding below was verified against the v0.1 source (commit `3d06800` lineage). Severity:
**A** = actively misleading or blocks a core workflow; **B** = missing table-stakes capability;
**C** = polish. Each finding names the section that fixes it. This table is the acceptance
checklist for the whole effort: **v0.2 ships when every A and B row is closed.**

| # | Finding | Sev | Fix |
|---|---------|-----|-----|
| F1 | **The bottom bar advertises `^O import`, `^E env`, `^H history` — none are bound.** `wireKeymap()` binds only `^S`/`^Q`/`^P`/`Escape`. The primary discoverability surface lies to the user. | A | §6 (hints generated from the registry — structurally cannot lie) |
| F2 | **The HTTP method cannot be changed.** `methodLabel` is a static `Text("GET ")`. A REST client that can only GET from the UI. | A | §7.1 MethodSelector |
| F3 | **Headers cannot be added or edited** — the Headers tab is a read-only `TableView`; the row-edit dialogs in DESIGN §9.2 were never built. | A | §7.3 KeyValueEditor |
| F4 | **No Params, Auth, or Tests tabs** — DESIGN §9.1 promised five request tabs; two exist (Headers, Body). Auth and the identity-feature test layer are headless-only. | A | §7.2/§7.4/§7.6 |
| F5 | **No environment editor and no way to switch environments** — `activeEnvName` can never be set from the UI, so `{{var}}` interpolation (a headline feature) is unreachable interactively. | A | §10.2 |
| F6 | **No import UI.** Collections load only via CLI argument. `^O` is advertised (F1) but does nothing. | A | §10.4 |
| F7 | **No history pane.** `historyLabels()` exists and is tested; nothing displays it, nothing re-opens an entry. | B | §10.1 |
| F8 | **The theme file is dead.** `themes/recon.toml` ships; `loadReconTheme()` ignores it and returns `Theme::Dark()`. None of the panels set the theme keys the file defines. | A | §4.4 |
| F9 | **Errors are easy to miss**: `lastError` renders as `"! …"` in the top-bar right slot, in default style, overwritten by the next refresh. A failed send looks identical to an idle app at a glance. | A | §9.1 NoticeLine + §9.2 taxonomy |
| F10 | **No response status line.** Status/elapsed/size hide in the bottom-bar right corner; the response panel itself gives no verdict at a glance (no color, no placement). | B | §8.1 |
| F11 | **`next-tab` is wrong**: `nextRespTab()` runs `tabs.select(0)` — it always selects the *first* tab. | B | §11.3 |
| F12 | **The in-flight spinner is dead code.** `BottomBar.spinner` is constructed and never added to the bar; "sending..." is plain text and elapsed time is not shown until settle. | B | §9.4 |
| F13 | **The command bar has no completion or listing.** `CommandRegistry.matching()` and `labels()` exist and are unused; the user must already know a command's exact name to run it. | B | §6.4 palette |
| F14 | **No help surface.** No key reference, no `F1`, nothing lists what the app can do. | B | §6.5 |
| F15 | **No unresolved-variable flagging** (DESIGN §1.1 promised it): a URL with a typo'd `{{tokn}}` sends the literal braces with no warning. | B | §9.3 |
| F16 | **No empty states.** First launch shows a blank tree, a blank URL row, and an empty response box. Nothing tells the user what to do first. | B | §4.6 |
| F17 | **Cookies/sessions are view-only**: no session switching, no new session, no cookie delete/clear. | B | §10.3 |
| F18 | **No save story.** Edits exist only in memory until a send appends history; there is no dirty indicator, no save command, no quit guard. Users lose work silently. | A | §10.5 |
| F19 | **Body mode cannot be selected** (raw only); urlencoded/formdata/GraphQL bodies import fine but cannot be edited or even seen as their kind. | B | §7.5 |
| F20 | **No JSON navigation.** Response bodies pretty-print as a flat wrapped `Text` — no folding, no search, no path readout, no way to build an assertion from what you see. | B | §8.2/§8.3 |
| F21 | **No settings UI** (timeout, redirects, theme, session policy are model-only). | B | §10.6 |
| F22 | **No visual method/status color coding** anywhere (tree glyphs are plain text; status is plain text). | C | §4.2/§4.3 |
| F23 | **No menu bar.** Keyboard-first is right, but menus are the *discoverability* backbone a TUI can now afford — Moby ships `BarMenu`/`Menu` (post-RESEARCH). | B | §6.3 |
| F24 | **Focus is hard to see**: default framework focus styling only; the theme's `focused.border` keys are never wired. | C | §4.5 |
| F25 | **New/rename/delete for tree nodes missing** (`F2`/`Delete` in DESIGN §9.6's table — never wired). | B | §10.1 |
| F26 | **No "new request" path at all** — you cannot compose a request without importing a collection first. | A | §10.5 scratch collection |

The v0.1 README's honest status section already conceded most of this ("the dialog *screens* …
are the open work"). This design is that work, plus the standard it should have been held to.

---

## 2. What changed under us — and what it unlocks

### 2.1 Moby deltas since RESEARCH.md (verified against `moby/src` at HEAD)

RESEARCH.md §3.2 listed as *missing*: `Modal`, `BarMenu`/`Menu`, `@Shortcut`, theming DI. **All
of these have since shipped**, plus more:

| Now shipped | Surface (verified) | Recon v0.2 use |
|---|---|---|
| `Modal` | `title()/border()/pad()/dismissWith()/open()/close()/onDismiss()`; scrim; overlay groups | All dialogs (§7.3, §10) |
| `Moby::alert` / `Moby::confirm` | `confirm -> Promise<bool>` | Quit guard, destructive confirms |
| `Menu` / `MenuItem` / `MenuSeparator` / `BarMenu` | `BarMenu.menu(Menu)`, `Menu.item(label, cb)`, `openAt(x,y,group)`, arrow/mouse nav | The menu bar (§6.3) |
| Overlay extensions | `pushOverlay(c, dismissOnOutsidePress, inputTransparent, group)`, `newOverlayGroup`, `popOverlayGroup/Component`, focus save/restore | Palette, help, notices |
| Theming (T08) | TOML themes, dotted keys, `Style.over`, `setTheme()` live switch, 4 built-ins | Theme v2 (§4.4) |
| `@Moby::Shortcut/@Timer/@Validator`, `validateTree` | attach/detach-scoped registration | Considered §11.4 |
| `@Moby::Reactive` | set-view injection, global fan-out | Considered & rejected §11.4 |
| `moby!` templates | comptime tag grammar | Not adopted v0.2 (§16) |
| TestRenderer harness (T10) | `TestRenderer` (DI-selected), `ScriptedInput`, chord encoder, `MobyTest` (`eq/snap/snapText/harness/pump/reset`), `runtests.sh` matrix | All UI acceptance tests (§14) |
| `Input` validators & masks | `addValidator(()=>bool, msg)`, `mask(char)` | Secret fields, inline validation |
| `Tabs` polish | digit-select 1–9, arrow nav, `select()` clamps + fires on change | Response/request tabs |
| `Spinner` | braille frames by default, `setInterval` | In-flight ticker (§9.4) |
| `DebugOverlay` | frame stats overlay | `F12` (§6.2) |

**Design consequence:** v0.1's two "deviations" (compose `Modal`; owns-a-`Container` builder
classes) are ratified as the house pattern and extended — nothing in v0.2 hand-rolls what Moby
now ships.

### 2.2 Compiler/runtime deltas

- **Bugs #67 (LLVM component-paint segfault) and #68 (`env::get` LLVM codegen) are fixed**
  (commit `1cd07bb`). These were the *only* reasons Recon was pinned to the oracle/IR lanes.
  §12 promotes the LLVM lane behind explicit gates.
- **The MI-dispatch bug family (#64/#65/#59/#61/#41/#40/#49/#36) is fixed at source**
  (`054e159` et al., 2026-07-14) and the whole Moby composite set now passes on LLVM.
- **The current open register is exactly:** #72 [P2] (misleading TLS error text — the message
  fix landed `d994ab8`; the underlying "why did `sysTcpConnect` return −1 in that environment"
  question remains open), #73 [P2] (a **global** `Array<T>` grown by `xs = xs.add(...)` leaks
  every intermediate on LLVM native — locals are clean; workaround: grow a local, assign once),
  and **#74 [P0.3, stop-the-line]** (repeated **`Array<Struct>`**.add on native corrupts the
  allocator — struct-element arrays only; class-element arrays are not the shape).
- **Consequences bound into this design:** (a) no new code grows a *namespace-global* array
  element-by-element (§13 rule N2); (b) no new code architects on repeated `Array<Struct>.add`
  loops (§13 rule N1) — Recon's rows are already reference classes throughout, and stay so;
  (c) the LLVM lane gate (§12) explicitly audits both shapes before promotion.
- **Trident glob `src/**` (v0.1 bug #71)**: the explicit per-directory `sources` list stays; new
  UI files must be added to the manifest list when created (checklist item in every track).

### 2.3 Open Moby bugs that constrain the visual work

- **moby-bugs #4 (open):** a leaf repainting a *shorter* string leaves stale glyphs when the
  resolved background is the default/empty theme (`paintBackground` fills only for a themed
  non-default bg). **Mitigation, normative:** every Recon-authored leaf paints its own
  full-content-width background line (or pads with spaces to the content width) in
  `paintContent`; and theme v2 gives every Recon surface a themed bg so the framework fill
  engages. Both belts. (Watchpoint W1 in §15.4.)
- **moby-bugs #1 (Recon side fixed in `3d06800`):** vertical flex must be opted into
  (`setHeight(Constraint::Flex(1))`) — retained knowledge, now a standard rule (§13 N3).

---

## 3. UX principles (normative)

Every screen, dialog, and keybinding in this document was checked against these eight rules;
every future Recon change should be too.

- **P1 — Hints never lie.** Every advertised chord/command is generated from the one command
  registry (§6.1). It is *structurally impossible* to show a hint for an unbound command,
  because the hint text is derived from the binding. A corpus test enforces it (§14.2).
- **P2 — Every state is visible.** In-flight, error, dirty, active env, active session, cookie
  count, unresolved variables: each has a fixed home on screen (§5.1) and a themed style. If
  the app knows it, the user can see it.
- **P3 — Three ways to every command.** Chord (fast), menu (discoverable), palette (searchable).
  All three render from the same registry entry, so they cannot drift apart.
- **P4 — Never steal focus.** Background completion (a send settling, a toast, history
  appending) must not move keyboard focus. Only a *user-initiated* open (dialog, palette,
  menu) may move focus, and closing it restores the previous focus (Moby's overlay stack
  already saves/restores).
- **P5 — Escape always retreats, in order.** Top overlay → in-flight cancel → nothing. Never
  quit, never destructive. One rule, everywhere (§6.2).
- **P6 — Destructive means confirm; absent means explain.** Deletes confirm with the thing
  named ("Delete request 'Login'?"). Unsupported/absent features say so in place (binary
  bodies, compressed responses — v0.1 already did this well; keep it).
- **P7 — Color is never the only channel.** Status text accompanies status color; method names
  accompany method colors; `✓/✗` accompany pass/fail green/red; the secret mask is dots, not a
  color. (This is Moby's own theme a11y rule, applied to every Recon key — §4.4.)
- **P8 — Keyboard-first, mouse-welcome.** Every flow completes without a mouse. Everything
  clickable (tabs, tree rows, menu titles, buttons) already handles mouse via Moby; nothing
  in v0.2 may *require* it.

---

## 4. Visual design system

### 4.1 Palette discipline

Moby's `Color` is the 16-color terminal set plus `Default`, with an 8-bit attr field
(bold/dim/italic/underline/blink/reverse/hidden/strike as the renderer supports). **v0.2 stays
inside 16 colors** — no 256-color request to Moby (out of scope, §16). The discipline that
makes 16 colors look designed rather than accidental:

- **One accent.** Cyan is Recon's accent: focused borders, selected tab underline text, the
  active menu title, palette selection. Nothing else is cyan.
- **Semantic colors are reserved.** Green/red/yellow speak *only* for success/failure/warning
  (status classes, test results, notice severities, method badges per §4.2). They are never
  decoration.
- **Structure is monochrome.** Borders, separators, gutters, disabled rows: `BrightBlack`
  (dim). Content is `White`/`BrightWhite`. The screen should read as a grayscale document with
  a few deliberate signals.
- **Backgrounds are quiet.** Panel bg = terminal default (theme `default.bg`); the *only*
  filled backgrounds are the two bars, the menu/palette selection row, the modal scrim, and
  the notice line — the places that must pop.

### 4.2 Method color coding

Applied wherever a method appears (selector §7.1, tree rows are text-only — see the honest
limit below — history rows, status line):

| Method | Style | Badge text |
|---|---|---|
| GET | `BrightGreen` bold | `GET` |
| POST | `BrightYellow` bold | `POST` |
| PUT | `BrightBlue` bold | `PUT` |
| PATCH | `BrightMagenta` bold | `PATCH` |
| DELETE | `BrightRed` bold | `DEL` |
| HEAD | `BrightCyan` | `HEAD` |
| OPTIONS | `Cyan` | `OPT` |

The badge *text* is the non-color channel (P7). Badges are width-5, left-aligned, so columns of
them align in history and (future) multi-request views.

**Honest limit:** `ITreeSource.labelAt` returns a plain string — the shipped `TreeView` has no
per-row style hook, so the sidebar tree keeps *text* method badges without color in v0.2. The
color coding lives in the method selector, history pane, and status line. (Filing a Moby
enhancement request for a styled-label tree source is a v0.3 candidate; we do not fork or
patch the framework for it.)

### 4.3 Status color coding

| Class | Style | Rendering |
|---|---|---|
| 2xx | `BrightGreen` bold | `● 200 OK` |
| 3xx | `BrightYellow` bold | `● 302 Found` |
| 4xx | `BrightRed` bold | `● 404 Not Found` |
| 5xx | `Red` bold | `● 500 Internal Server Error` |
| timeout / cancel | `BrightBlack` + `Yellow` text | `◌ timed out after 30.0 s` / `◌ cancelled` |
| transport error | `BrightRed` | `✗ connect failed: <reason>` |

`fmt.lev` (new, §11.2) owns `statusClass(int) -> string` (the theme-key suffix), `fmtMs(int)`
(`"143 ms"`, `"2.4 s"`), `fmtBytes(int)` (`"1.2 KB"`, `"340 B"`), and `fmtAgo(nowMs, thenIso)`
(`"2m ago"`) — all pure, all corpus-tested (they take `now` as a parameter; nothing reads the
clock inside).

### 4.4 Theme v2

**The shipped theme becomes real.** Three changes:

1. **`themes/recon.toml` is actually loaded**, via the T08 idiom: `trident.toml` gains
   `assets = ["themes/recon.toml"]`, and `src/ui/theme.lev` declares a top-level
   `comptime string reconThemeToml = import("themes/recon.toml");` global (the *only* legal
   `import()` position — a runtime call throws, LA-20), parsed once by Moby's TOML loader at
   startup. `Settings.themeName` selects among `recon | dark | light | highContrast` (the
   Moby built-ins for the latter three); unknown names fall back to `recon` with a notice.
2. **Runtime switching**: the settings dialog / `theme` command calls `setTheme(...)` (T08
   supports live in-place switch) and repaints; the choice persists in `settings.json`.
3. **The full key taxonomy** — the file grows from 12 keys to the complete set. Every key
   below carries the a11y rule (any state distinction has a non-color channel *in the UI
   element itself*, per P7). Component base keys (`input`, `button`, `tabs`, …) follow Moby's
   most-specific-first `resolve` fold, so shipped components pick them up without code.

```
default                         # terminal-default fg/bg
bar.top / bar.bottom            # the two chrome bars (filled bg)
bar.top.dirty                   # the dirty dot + name segment
menu / menu.focused / menu.open # BarMenu titles
notice.info|success|warn|error  # the notice line severities (§9.1)
method.get|post|put|patch|delete|head|options
status.ok|redirect|clienterr|servererr|neutral
request.url.border / request.url.focused.border
request.url.hole.resolved / request.url.hole.unresolved   # §9.3 preview strip
editor.gutter / editor.cursorline                          # TextArea v2
json.key|string|number|bool|null|punct|fold                # JsonView + body highlight
json.match / json.match.current                            # search hits
table.header / table.row.disabled                          # KV editor
test.pass / test.fail / test.summary
sidebar.folder / sidebar.request / sidebar.selected
history.row / history.row.status
dialog.title / dialog.hint                                 # modal footer hints
field.secret / field.invalid
palette.match                                              # filtered-substring highlight
statusline.inflight / statusline.meta
```

### 4.5 Focus, selection, borders

- **Focused component**: border `Cyan` (theme `*.focused.border`), title bold. Unfocused:
  `BrightBlack`. Every bordered pane (sidebar, request tabs area, response box, every modal)
  participates — this is the single biggest "where am I?" fix and it is pure theme wiring.
- **Borders**: `Single` for the four main panes; `Rounded` for every overlay (modals, palette,
  menus already rounded via their own chrome). No `Double`/`Heavy` anywhere (visual noise).
- **Selection rows** (tree, lists, tables, menu, palette): reverse-video via the component's
  shipped selected style, plus a `▶` lead glyph in Recon-authored lists (non-color channel).

### 4.6 Empty states

Every zone renders guidance when it has nothing real to show (all static `Text`, zero cost):

- **Sidebar, no collection:** `No collection open.` / `^O  import a Postman collection` /
  `^N  create a scratch request`.
- **Response panel, never sent:** a centered quick-start card: `Recon — terminal REST client`
  / `^S send · ^P commands · F1 help` / `Import a collection with ^O, or type a URL and ^S.`
- **History, empty:** `Sends will appear here. Enter re-opens one.`
- **Tests tab, empty plan:** `No assertions yet. Press a to add one — or open a response and
  press a on any JSON value.`

### 4.7 Glyph vocabulary

Fixed set, all narrow (width-1) BMP glyphs — Moby's wide-glyph healing exists, but staying
narrow avoids ragged columns on constrained terminals: `▸ ▾` (tree, shipped), `▶` (selection),
`●` (status dot / dirty), `◌` (timeout/cancel), `✓ ✗` (pass/fail), `…` (truncation), `⠋…`
(braille spinner, shipped default), `∙` (separator in meta lines). ASCII fallback is *not*
attempted in v0.2 (the decoder/renderer are already UTF-8; a `RECON_ASCII=1` toggle is listed
as a v0.3 idea only).

---

## 5. Information architecture

### 5.1 The main screen (after)

```
┌ 1 menu bar ─────────────────────────────────────────────────────────────────────┐
│ File  Request  View  Environment  Session  Help                                 │
├ 2 top bar ───────────────────────────────────────────────────────────────────────┤
│ Recon ∙ ● acme-api            env: staging ✓   session: default   cookies: 7    │
├──────────────┬───────────────────────────────────────────────────────────────────┤
│ 3 sidebar    │ 4 request panel                                                   │
│ Collections│H│ ┌─────┐ ┌───────────────────────────────────────────────┐ ┌──────┐│
│ ▾ acme-api   │ │ GET▾│ │ https://{{host}}/v1/users?page=2              │ │ Send ││
│   ▸ auth     │ └─────┘ └───────────────────────────────────────────────┘ └──────┘│
│  ▶GET  users │ {{host}} → api.acme.dev                                            │
│   POST login │ ─ Params ─ Headers ─ Auth ─ Body ─ Tests ──────────────────────────│
│   DEL  user  │  [x] page        2                                                 │
│              │  [x] limit       50                                                │
│              │  [ ] debug       true                                              │
│              │  a add   e edit   d delete   space toggle                          │
├──────────────┴───────────────────────────────────────────────────────────────────┤
│ 5 response panel                                                                  │
│ ● 200 OK ∙ 143 ms ∙ 1.2 KB ∙ https://api.acme.dev/v1/users?page=2                │
│ ─ Body ─ Headers ─ Cookies ─ Tests ✓3 ─────────────────────────────────────────── │
│  ▾ $                                                                              │
│    users: [ … ] 3 items                                                           │
│    ▾ meta                                                                         │
│        page: 2                                                                    │
│        total: 61                                                                  │
│  $.meta.page                                              / search  a assert      │
├ 6 notice line ────────────────────────────────────────────────────────────────────┤
│ ✓ extracted token → environment 'staging'                                         │
├ 7 bottom bar ─────────────────────────────────────────────────────────────────────┤
│ ^S send  ^P palette  F1 help  ^O import  ^E env  ^H history  Tab focus  ^Q quit  │
└───────────────────────────────────────────────────────────────────────────────────┘
```

Zone changes vs v0.1: **(1)** menu bar is new (one row, `BarMenu`); **(2)** top bar gains the
dirty dot + collection name, env checkmark, cookie count; **(3)** the sidebar header gains a
`Collections | History` two-tab strip (§10.1); **(4)** the request panel gains the method
selector, the variable-preview strip (only when holes exist, §9.3), and the full five-tab set;
**(5)** the response panel gains the status line and the JSON tree with a path/actions footer;
**(6)** the notice line is new (one row, auto-hides when empty — the layout collapses it);
**(7)** the bottom bar hints are now *generated* (P1).

Vertical budget on a 24-row terminal: 1 menu + 1 top + 1 notice (usually collapsed) + 1 bottom
= 3–4 chrome rows, unchanged from v0.1's 2 plus the two new single-row strips. The
request:response split stays the user-adjustable `SplitBox` ratio (default 45%).

### 5.2 Screen inventory

| Surface | Kind | Section |
|---|---|---|
| Main screen | root layout | §5.1 |
| Command palette | Modal (input + list) | §6.4 |
| Help / key reference | Modal (generated) | §6.5 |
| Menu drop-downs | shipped `Menu` overlays | §6.3 |
| Row editor (header/param/form/variable) | FormDialog | §7.3 |
| Assertion / extraction editors | FormDialog | §7.6 |
| Environment editor | large Modal | §10.2 |
| Sessions & cookie manager | Modal ×2 | §10.3 |
| Import prompt / export prompt / save-as | PromptDialog | §10.4/§10.5 |
| Quit guard (save/discard/cancel) | Modal, 3 buttons | §10.5 |
| Settings & themes | Modal | §10.6 |
| Notices log (recent 50) | Modal | §9.1 |
| Debug overlay | shipped `DebugOverlay` | `F12` |

---

## 6. The command system — one registry, four surfaces

### 6.1 The Command model (`src/ui/commands.lev`, rewritten)

```lev
class Command {
    public string id = "";            // "request.send" — dotted, category prefix
    public string title = "";         // "Send request"
    public string category = "";      // File | Request | View | Environment | Session | Help
    public string chord = "";         // display + binding source of truth: "^S", "F1", ""
    public () => void action;         // the handler
    public bool hasAction = false;
    public () => bool enabledWhen;    // optional gate (e.g. "collection open")
    public bool hasEnabled = false;
    new Command() { }
}

class CommandRegistry {
    Array<Command> all_;              // insertion-ordered; classes, so .add is lane-safe
    new CommandRegistry() { }
    void register(Command c) { ... }
    bool run(string id) { ... }                      // respects enabledWhen; notice if disabled
    Array<Command> inCategory(string cat) { ... }
    Array<Command> matching(string needle) { ... }   // case-insensitive substring on id+title
    Array<Command> bound() { ... }                   // chord != ""
}
```

**The invariant (P1):** `wireKeymap()` iterates `registry.bound()` and binds exactly those
chords; the bottom bar renders from a curated id list but *reads each chord from the registry*;
menus and the palette render titles+chords from the same entries. There is one authoring site
per command. The `hints_never_lie` corpus test (§14.2) walks the registry and asserts every
rendered hint's chord round-trips through the chord encoder to a bound action.

### 6.2 Keymap v2 (complete global table)

**Chord discipline (normative):** global chords are Ctrl-letters and F-keys **only**. Moby
rules global chords win at capture over a focused `Input` (R11), so a bare printable global
chord (`?`, `:`, `[`) would steal typing from the URL field — v0.1's design tables suggesting
`:` and `?` are rescinded. Component-local keys (only when that component is focused) may be
printable.

| Chord | Command id | Notes |
|---|---|---|
| `^S` | `request.send` | disabled while in flight (enabledWhen) |
| `Escape` | — (not a command) | P5 cascade: top overlay closes → else cancel in-flight → else no-op |
| `^P` | `view.palette` | |
| `F1` | `help.keys` | |
| `F10` | `view.menu` | focus the menu bar (also reachable by Tab) |
| `F12` | `view.debug` | shipped DebugOverlay toggle |
| `^O` | `file.import` | collection *or* environment — auto-detected (§10.4) |
| `^W` | `file.save` | save active collection (§10.5) |
| `^N` | `request.new` | scratch request (§10.5) |
| `^E` | `env.edit` | environment editor |
| `^H` | `view.history` | toggle sidebar History tab |
| `^B` | `view.sidebar` | focus the sidebar tree |
| `^Q` | `app.quit` | dirty-guarded (§10.5); `^C` remains Moby's hard quit |
| `Tab` / `S-Tab` | — | shipped FocusRing traversal (unchanged) |

Component-local (documented in help, bound in the component): tree `F2` rename, `Delete`
delete, `Enter` open; KV editors `a/e/d/Space/Enter`; JsonView `arrows/Enter/Space` fold,
`/` search, `n/N`, `a` assert, `x` extract; TextArea `^F` format-JSON (component-level so the
chord only applies while editing a body); tabs `1–9`/arrows (shipped).

### 6.3 Menu bar (`src/ui/menubar.lev`, new)

One `BarMenu` row built from the registry (each `MenuItem` label = `title` padded + chord
right-aligned; `Menu.item(label, () => registry.run(id))`):

```
File        Request        View            Environment      Session          Help
─ Import…   ─ Send         ─ Palette…      ─ Edit…          ─ Switch…        ─ Key reference
─ Save      ─ Cancel       ─ History       ─ Switch…        ─ New session…   ─ Notices log
─ Export…   ─ New request  ─ Sidebar       ─ Manage globals ─ Cookie manager ─ About Recon
─ Save response body…      ─ Next resp tab ────────────────  ─ Clear cookies…
─ Settings… ─ Rename (F2)  ─ Theme ▸
─ Quit      ─ Delete       ─ Debug overlay
```

`Theme ▸` is a submenu (`Menu` inside `Menu` — the overlay-group machinery closes the chain).
Menus never hold state; they are a rendering of the registry (P3).

### 6.4 Command palette v2 (`src/ui/palette.lev`, replacing `commandbar.lev`)

A `Modal` containing an `Input` (filter) over a `ListView` (matches). Typing refilters via
`onChange` → `registry.matching(...)`; rows render `title  —  id` with the chord right-aligned
and the matched substring styled `palette.match`; `Up/Down` move, `Enter` runs, `Escape`
closes. Disabled commands render dim with their reason (`"send — in flight"`). The old
bare-name command bar behavior (type exact name, Enter) still works — an exact-id match sorts
first. Recent commands (last 5, session-local) sort above the fold when the filter is empty.

### 6.5 Help overlay (`src/ui/help.lev`, new)

`F1`. A read-only Modal, **generated**: section per category, `chord  title` rows from the
registry, then a static "While focused" block per component class (tree/KV/JsonView/TextArea
local keys, §6.2). Footer: `Esc close ∙ ^P run any command by name`. Because it is generated,
adding a command updates help automatically — help cannot go stale (P1's sibling).

---

## 7. Request editing, complete

### 7.1 MethodSelector (`src/ui/methodselector.lev`, new)

A focusable width-7 badge (`GET ▾`), styled per §4.2. `Space`/`Enter` opens a `Menu` overlay at
its position (`openAt`, own overlay group) listing the seven methods, each item styled; arrows
also cycle directly without opening (fast path). Selection writes `state.active.method`, marks
dirty, repaints the badge. Mouse press opens the menu (shipped `Menu` handles its own mouse).

### 7.2 Params tab (`src/ui/paramspane.lev`, new)

A `KeyValueEditor` (§7.3) over the request's query parameters, with the URL two-way sync:

- **Model change:** `RequestSpec` gains `public Array<FormField> query;` — the full row set
  *including disabled rows* (a raw URL string cannot carry a disabled param). This mirrors
  Postman's own v2.1 shape (`url.raw` holds enabled ones; `url.query[]` carries all with
  `disabled` flags), so the importer/exporter round-trip is natural: importer prefers
  `url.query[]` when present, else parses `url.raw`; exporter emits both.
- **Invariant (normative, corpus-tested):** *the query substring of `RequestSpec.url` always
  equals the join of the enabled `query` rows*, re-established at exactly four sync points:
  `loadFrom` (URL → table), table edit accept (table → URL), URL `onSubmit`/blur commit
  (URL → table), and `commitTo` (final table → URL before send). The table shows raw text;
  percent-encoding happens only in `buildPrepared` (unchanged).
- New pure helpers in `net/url.lev`: `parseQueryPairs(string) -> Array<FormField>` and
  `joinQueryPairs(Array<FormField>) -> string` (enabled rows only) — corpus-tested alongside
  `parseUrl`.

### 7.3 KeyValueEditor (`src/ui/kveditor.lev`, new) + FormDialog (`src/ui/dialog.lev`, extended)

The one editing pattern for headers, params, urlencoded/formdata fields, and environment
variables (F3's fix, reused four times):

- A focusable list (built on `ListView` + an `IListSource` over the row array) rendering
  `[x] name    value` (disabled rows dim, `table.row.disabled`), `▶` on the selection.
- Keys (local, shown in a one-row footer): `a` add, `e`/`Enter` edit, `d` delete (confirm),
  `Space` toggle enabled. All mutations mark dirty and `refresh()`.
- Add/edit opens a **FormDialog** — the DESIGN §9.4 component finally built: a `Modal` holding
  N labeled `Input`s (label column + input column via a two-column `GridBox`), `Enter` on the
  last input (or the OK button) accepts, `Escape` cancels, `onAccept(Array<string>)` returns
  the values. Fields can be masked (`Input.mask('•')`) for secrets and carry validators
  (`addValidator`) for inline `field.invalid` styling — e.g. header names reject `:`/spaces.
- Variants are configuration, not subclasses: column labels, mask flags, validator set.

### 7.4 Auth tab (`src/ui/authpane.lev`, new)

- A `RadioGroup`: `Inherit ∙ None ∙ Basic ∙ Bearer ∙ API Key` (Digest remains surfaced-
  unsupported per DESIGN §6.2 — visible, explained, disabled).
- Per-kind field rows (FormDialog-style inline, not a popup — they are few): Basic →
  user + password (masked); Bearer → token (masked); API Key → key, value (masked),
  `in: header|query` radio.
- **Inherit shows its resolution** (the cascade made visible): `Inherited: bearer — from
  collection 'acme-api'`, computed by a new pure `authSourceLabel(req, folder, col) -> string`
  beside `effectiveAuth` in `net/auth.lev` (corpus-tested). `None` shows `No auth (overrides
  inherited bearer)` when it is actually overriding — the difference between "none" and
  "inherit-nothing" is exactly the kind of invisible state P2 exists for.
- Values accept `{{vars}}`; the unresolved-flagging strip (§9.3) covers them at send time.

### 7.5 Body tab (rebuilt inside `src/ui/requestpanel.lev`)

- A mode `RadioGroup` across the top: `None ∙ Raw ∙ Form (urlencoded) ∙ Multipart ∙ GraphQL`
  (`FileBinary` renders as a disabled explanation row when imported — unchanged v1 boundary).
- **Raw** → TextArea v2 + a small `rawLanguage` radio (`json ∙ xml ∙ text`) that drives the
  content-type default (existing `buildBody` logic) and the highlighter.
- **Form/urlencoded and Multipart** → a `KeyValueEditor` each (multipart file rows render
  disabled with the "binary part omitted at send" note — existing §6.3 behavior surfaced).
- **GraphQL** → two labeled TextAreas stacked (query, variables).
- **TextArea v2** (`src/ui/textarea.lev`, extended): line-number gutter (`editor.gutter`,
  width = digits+1, toggleable in settings), a host-rendered footer segment `Ln 3, Col 14`,
  `^F` (component-local) pretty-print when the buffer parses as JSON (else notice "not valid
  JSON — nothing changed"), and **minimal JSON token highlighting** when `rawLanguage == json`:
  a per-line, escape-aware scanner (JSON strings cannot span lines) painting
  key/string/number/bool/null/punct with the `json.*` theme keys. No general syntax-highlight
  framework — one language, one scanner, corpus-tested as a pure `tokenizeJsonLine` function.
- Body edits mark dirty on change (`onKeyDown` sets a flag consumed by `commitTo`).

### 7.6 Tests tab (`src/ui/testspane.lev`, new)

The identity feature finally gets a UI:

- Two sections (labels + KV-style lists): **Assertions** (`status equals 200`,
  `jsonpath $.token exists`, …) and **Extractions** (`$.token → env.token`).
- `a/e/d` per section via FormDialogs: assertion editor = source radio
  (`status|header|body|jsonpath`) + arg + op radio (`equals|notEquals|contains|matches|exists|
  inRange|isType`) + expected; extraction editor = source radio + arg + group + varName +
  scope radio (`environment|collection`). Validators: op/arg combinations checked inline
  (e.g. `inRange` expects `a..b`).
- **After a send, rows go live**: each assertion row prefixes `✓`/`✗` (+ detail on the failed
  row, dim) by joining `state.lastTests` by label; the tab label itself becomes
  `Tests ✓3` / `Tests ✗1` (string label — cheap, glanceable).
- Rows created from the response inspector (§8.3) land here pre-filled.

---

## 8. The response experience

### 8.1 The status line (`src/ui/statusline.lev`, new)

One row pinned above the response tabs (zone 5 header):

- **Idle, never sent:** hidden (the empty-state card owns the space, §4.6).
- **In flight:** `⠹ sending… 1.3 s ∙ Esc to cancel` — braille `Spinner` + elapsed, updated by
  an `App.every(100)` ticker armed in `beginInFlight` and cancelled on settle (`cancelEvery`;
  the token lives on ReconApp — §9.4).
- **Settled OK:** `● 200 OK ∙ 143 ms ∙ 1.2 KB ∙ https://api.acme.dev/v1/users?page=2` styled
  per §4.3 (`fmtMs`/`fmtBytes`), plus `∙ 2 redirects ▸` when the chain is non-empty —
  focusing/clicking it expands the hop list into the Headers tab's top rows (each hop:
  `302 https://… → https://…`).
- **Settled failed:** the taxonomy line (§9.2), e.g. `✗ connect failed: connection refused ∙
  api.acme.dev:443 ∙ 12 ms`.

### 8.2 JsonView (`src/ui/jsonview.lev`, new) — the Body tab's default

A focusable, scrollable **leaf** (paints via `paintContent`, per the house pattern) over the
parsed `JsonValue`:

- **Model:** a fold-tree flattened to visible rows. Row = indent + styled key + `: ` + styled
  leaf value, or a fold summary for containers: `users: [ … ] 3 items` / `meta: { … } 2 keys`.
  Root row `▾ $`. Fold state is a `Map<int,bool>` keyed by row registry id (the TreeView
  pattern, Recon-owned).
- **Keys:** `Up/Down/PageUp/PageDown/Home/End` move the cursor row; `Enter`/`Space` toggle
  fold; `Left` folds (or jumps to parent when already folded — the tree-nav idiom); `Right`
  unfolds. Mouse press selects; press on a fold glyph toggles.
- **Painting:** per-row, token-styled (`json.*` keys), cursor row on `editor.cursorline` bg,
  **full-width bg fill every row** (moby-bugs #4 belt, §2.3). Long values ellipsize with `…`
  (full value visible via the footer path + a future copy story, §16).
- **Footer** (one row inside the pane): the selected row's JSONPath in the exact
  `eval/jsonpath.lev` dialect (`$.meta.page`), right-aligned hints `/ search  a assert  x extract`.
- **Search:** `/` opens an inline footer `Input`; matches (case-insensitive substring over
  key+rendered value) style `json.match`, `n/N` cycle (auto-unfolding ancestors of the target
  — fold state is data, so this is a pure operation), current match `json.match.current`,
  `Esc` clears.
- **Fallbacks:** non-JSON bodies render in the v0.1 wrapped `Text` (raw view); a `raw ∙ tree`
  toggle (component-local `t`) is remembered per session. Bodies > 512 KB open in raw view
  with a notice (fold-tree cost guard).

### 8.3 Assert / extract from the response — the flagship flow

With a row selected in JsonView: **`a`** opens the assertion FormDialog pre-filled
(`source=jsonpath, arg=<footer path>, op=equals, expected=<current value>`); **`x`** opens the
extraction editor pre-filled (`source=jsonpath, arg=<path>, varName=<last path segment>,
scope=environment`). Accepting appends to `state.active.tests`, marks dirty, notices
`✓ assertion added — Tests tab`, and the Tests tab badge updates. Header rows in the Headers
tab get the same `a`/`x` (source=header, arg=name). This closes the loop that makes the native
test layer *discoverable*: users build tests by pointing at reality instead of authoring
syntax from memory (F20, and the single largest "intuitive UX" win in this design).

---

## 9. Feedback

### 9.1 NoticeLine + notices log (`src/ui/notices.lev`, new)

**Decision:** feedback is a **docked one-row notice line** (zone 6), not floating toasts.
Rationale, recorded: floating overlays interact with focus save/restore and repaint-on-pop
during active typing, and stale-shrink (#4) risk multiplies on frequently-resized floaters;
a docked line has none of these failure modes, is one afternoon of work, and is the
established TUI idiom (vim/emacs/helix). *(Floating `inputTransparent` toasts were prototyped
on paper against `pushOverlay(c, false, true)` and remain a v0.3 option if the line proves too
quiet — the API supports them; the risk/benefit doesn't, yet.)*

- `class Notice { level (info|success|warn|error), text, atMs }` (reference class); AppState
  holds `Array<Notice> notices` (capped 50, newest last — instance field, #73-safe).
- The line renders the newest non-expired notice, styled `notice.<level>` with its glyph
  (`∙ ✓ ! ✗`), `+N more` when several arrived within its window. Auto-expiry: info/success
  4 s, warn 8 s, error sticky until any keypress (a P2 exception — errors must be *seen*).
  Expiry rides one `App.every(500)` house ticker (armed only while something is displayed).
  When nothing is live the row collapses (`Constraint::Fixed(0)` — layout reflows).
- **Notices log:** `Help ▸ Notices log` / palette `view.notices` opens a Modal `ListView` of
  the 50 retained rows with `fmtAgo` timestamps — the answer to "what did that flash say?".
- `Moby::log` keeps receiving everything notices receive (headless debugging unchanged).

### 9.2 Error taxonomy (normative)

| Failure | Surface | Example |
|---|---|---|
| Transport (connect/TLS/DNS) | status line `✗` + **error notice** | `✗ connect failed: connection refused` |
| Timeout / cancel | status line `◌` + warn notice | `◌ timed out after 30.0 s` |
| HTTP 4xx/5xx | status line color only — **not an app error** (the request *worked*) | `● 404 Not Found` |
| Import/parse failures | error notice + sidebar unchanged | `✗ import failed: not a Postman collection (v1 format)` |
| Persistence failures | error notice | `✗ cannot write history.json: …` |
| Command disabled/unknown | info notice | `∙ send — already in flight` |
| Programmer errors | uncaught → loud exit (unchanged §12.6 policy) | — |

One rule: **every failed user intention produces exactly one notice**, at the moment of
failure, naming the thing that failed. No more silent `lastError` overwrites (F9).

### 9.3 Unresolved-variable flagging

- New pure API in `net/vars.lev`:

```lev
class VarHole {                       // one {{...}} occurrence
    public string name = "";
    public bool resolved = false;
    public bool dynamic = false;      // {{$guid}} family
    public string scope = "";         // which scope resolved it ("environment", …)
    new VarHole() { }
}
class ResolveReport {
    public string result = "";
    public Array<VarHole> holes;
    new ResolveReport() { }
    bool clean() { ... }              // no unresolved holes
}
ResolveReport resolveReport(string template, VarResolver vars) { ... }
```

  `resolve()` becomes a thin wrapper over it (one scanner, no drift). Corpus tests cover
  nesting, depth-cap, dynamics, and unknowns.
- **The preview strip** (request panel, under the URL row; hidden when `clean()` and no holes):
  the resolved URL with each hole's *source* rendered inline — resolved holes show their value
  styled `request.url.hole.resolved` (`{{host}} → api.acme.dev` collapses to the value,
  underlined), unresolved ones keep the literal braces styled `request.url.hole.unresolved`
  (BrightRed bold). Painted by a small span leaf (`src/ui/richtext.lev`, new: a `Component`
  painting `Array<Span{text, styleKey}>` — also reused by the status line and notice line).
- **At send:** if any URL/header/auth/body hole is unresolved and
  `settings.warnUnresolved` (default true), a `Moby::confirm` intercepts:
  `"2 unresolved variables ({{tokn}}, {{userid}}) will be sent literally. Send anyway?"`.
  Decline focuses the preview strip. The report rows also land as a warn notice.

### 9.4 The send experience (the fixes wired together)

`beginInFlight` → Send button disables + relabels `…`, status line ticker starts (§8.1),
method/URL rows stay fully editable-looking but `request.send` is gated (enabledWhen), bottom
bar unchanged (P4 — nothing moves). Settle → ticker cancelled, status line renders the
verdict, Tests badge updates, history row appends (visible if the History tab is showing),
cookies count in the top bar updates, notice only on failure or extraction writes
(`✓ extracted token → environment 'staging'`). The v0.1 dead `Spinner` field is deleted; the
status line owns the one live spinner (F12).

---

## 10. Workspace features

### 10.1 Sidebar: Collections | History (`src/ui/sidebar.lev` + `src/ui/historypane.lev`)

The left pane becomes a two-tab `Tabs` (labels `Collections`, `History`) — discoverable,
digit/arrow-navigable, and `^H` jumps straight to History (F7).

- **Collections tab** = the existing `TreeView`, plus node operations (F25): `F2` rename
  (PromptDialog prefilled), `Delete` delete (confirm, names the node), both marking dirty;
  `Enter`/select loads the request (unchanged). Multi-collection: additional imports append
  as sibling roots (already modeled — `state.collections`; the tree source gains a
  `buildAll(Array<Collection>)`).
- **History tab** (`historypane.lev`, new): a `ListView` over `History.entries`, rows
  `GET   200  /v1/users            143 ms   2m ago` (method badge width-5, status, truncated
  path, `fmtMs`, `fmtAgo`), newest first (already the model order). `Enter` re-opens the
  entry: deserializes the stored `requestJson` snapshot into `state.active` (the v0.1 model
  already persists it), loads the panel, notices `∙ opened from history`. `d` deletes an
  entry, `D` clears (confirm). Selection never auto-sends (P6).

### 10.2 Environment editor (`src/ui/enveditor.lev`, new) — `^E`

A large Modal (`pad`ded, ~80×20 target):

- **Left:** `ListView` of environments (`✓` marks active); footer keys `Enter activate ∙
  n new ∙ r rename ∙ d delete`. Activating writes `state.activeEnvName`, updates the top bar,
  re-resolves the preview strip, notices `✓ environment 'staging' active`. A synthetic
  `Globals` row pins to the bottom (the DESIGN §10.1 convention made visible).
- **Right:** a `KeyValueEditor` over the selected environment's `values` with a third
  toggle column: `Space` enable/disable, `s` toggle secret (secret values render masked
  `••••` via `field.secret`; the row editor masks its value `Input`).
- **Persistence:** every accepted mutation saves that environment file immediately
  (`environments/<name>.json` — already implemented in store.lev); no dirty state to manage,
  and a crash loses nothing (envs are settings-like, not document-like).
- Postman environment files imported via `^O` (auto-detected, §10.4) land here.

### 10.3 Sessions & cookies (`src/ui/sessions.lev`, new)

- **Sessions modal** (menu `Session ▸ Switch…` / palette): `ListView` of sessions (`✓`
  active), `Enter` switch (swaps jar + persists `settings.activeSession`, updates top bar),
  `n` new (PromptDialog for name), `d` delete (confirm; cannot delete the active one —
  disabled with reason). Switching notices the cookie count: `✓ session 'work' — 12 cookies`.
- **Cookie manager modal**: rows `name=value  domain  path  expires|session` over the active
  jar (ListView; the read-only `TableView` stays for the response panel, but management needs
  selection+actions). `d` delete cookie, `D` clear all (confirm), `h` clear for the selected
  cookie's domain. Mutations persist the session immediately.
- The top bar's `cookies: 7` count (P2) reads the active jar each refresh.

### 10.4 Import & export UX

- **`^O` / `file.import`**: PromptDialog for a path (prefilled with the last-used directory,
  persisted in settings as `recentImportDir`). On accept: read → parse → **auto-detect**:
  a Postman *environment* file (`values[]` + no `info.schema`) imports as an environment
  (notice `✓ environment 'staging' imported — ^E to activate`); a collection imports as a
  collection (tree appends, first request auto-selected, notice with item count); Postman v1
  refuses with the existing clear message as an error notice (F6, honest-boundary preserved).
  Failures never half-import (existing total importer).
- **`file.export`**: unchanged mechanics (merge-onto-raw exporter), now with a PromptDialog
  for the target path (prefilled `sourcePath`), success notice with the written path, error
  notice on write failure. Menu `File ▸ Export…`.
- **`file.saveResponseBody`**: new — writes `lastResponse.body` to a prompted path (the
  cheap, high-value sibling; text bodies only, per the v1 boundary).

### 10.5 Save, dirty, scratch, quit guard

- **Dirty tracking:** `AppState.dirty` (bool) + `markDirty()`; set by every mutation named in
  §7/§10.1 (method, URL commit, KV edits, body edits, tests edits, node rename/delete,
  scratch adds). Cleared by `file.save`. Rendered as `● <collection>` in the top bar
  (`bar.top.dirty`) — the universal "unsaved" idiom (F18).
- **`^W` / `file.save`:** exports the active collection onto `sourcePath` (merge-onto-raw —
  round-trip fidelity is already built); if `sourcePath` is empty (scratch/new), prompts
  save-as. Success notice with path; dirty clears.
- **`^N` / `request.new`:** appends a request to the **scratch collection** — auto-created on
  first use (`name = "scratch"`, `sourcePath = <configRoot>/scratch.json`), shown in the tree
  like any collection, autosaved on quit. First-run users get a working editor in one
  keystroke (F26).
- **Quit guard:** `^Q` with `dirty` opens a three-button Modal (`Save`, `Discard`, `Cancel` —
  `Button`s; default focus Save): Save → `file.save` then quit (save-as flow can cancel the
  quit); Discard → quit; Cancel/Esc → back. `^C` remains the shipped hard quit (documented in
  help as such).

### 10.6 Settings & themes (`src/ui/settingsmodal.lev`, new)

Menu `File ▸ Settings…` / palette. A Modal form over `Settings` (all rows are existing or new
model fields; accepted values persist via the existing `saveSettings`):

`theme` (radio: recon/dark/light/highContrast — selection applies **live** via `setTheme` so
the user previews before closing; cancel restores), `default timeout` (validated int ms),
`follow redirects` (checkbox), `warn on unresolved variables` (checkbox, §9.3), `clear session
cookies on exit` (checkbox, existing), `editor line numbers` (checkbox, §7.5). `Settings`
gains `warnUnresolved`, `editorLineNumbers`, `recentImportDir` fields (loader defaults keep
old settings.json files valid — additive JSON).

---

## 11. Architecture & state changes

### 11.1 Composition (ReconApp — integration-track owned)

```
App (FlexLayout Vertical)
 ├─ MenuBar          (BarMenu, Fixed(1))                    §6.3
 ├─ TopBar           (ContentBar, Fixed(1))                 §5.1
 ├─ SplitBox outer   (Horizontal 28%, Flex(1))              ← moby-bugs #1 opt-in retained
 │   ├─ Tabs sidebar (Collections | History)                §10.1
 │   └─ SplitBox inner (Vertical 45%)
 │       ├─ RequestPanel.root  (method ∙ url ∙ send / preview strip / 5 tabs)
 │       └─ Container response (StatusLine Fixed(1) + Tabs body/headers/cookies/tests)
 ├─ NoticeLine       (Fixed(1), collapses to 0)             §9.1
 └─ BottomBar        (ContentBar, Fixed(1))                 hints from registry
```

Overlays (palette/help/menus/dialogs/settings/log) ride the shipped stack with groups; no
Recon-authored Container subclasses anywhere (house pattern re-affirmed — even though the MI
family is fixed, §13).

### 11.2 New shared modules

- `src/ui/fmt.lev` — `fmtMs/fmtBytes/fmtAgo/statusClass/truncateMiddle` (pure; `now` is
  always a parameter).
- `src/ui/richtext.lev` — the span leaf (§9.3), used by preview strip, status line, notice
  line. Paints full-width bg (the #4 belt).
- `AppState` additions: `dirty`, `notices`, `scratch` handle, `tickerToken`; `Settings`
  additions per §10.6. (`AppState` stays the single hub; widgets keep reading it directly.)

### 11.3 Wiring fixes (v0.1 defects closed in passing)

`nextRespTab` becomes `select((tabs.selected + 1) % 4)` (F11); the dead `BottomBar.spinner`
field is deleted (F12); `commandbar.lev` is replaced by `palette.lev` (F13); `keymap.lev`'s
misplaced `CommandRegistry` moves to `commands.lev` and `keymap.lev` keeps only chord wiring
(naming honesty); `openCollection` failures route through notices (they previously set
`lastError` silently mid-refresh).

### 11.4 Reactivity decision (recorded)

**Keep the explicit `refresh()` hub; do not adopt `@Moby::Reactive` for app state.** The
landed reactivity is component-field-scoped (int/string/bool/float set-views with global
fan-out per host) — excellent for a self-contained widget, wrong for a cross-panel hub where
one send touches seven surfaces in a deliberate order (state → statusline → tests badge →
history → cookies → topbar → frame). The explicit hub is *why* v0.1's update flow is easy to
reason about; v0.2 keeps it and simply routes every mutation through the (now more granular)
`refresh(...)` helpers. `@Moby::Shortcut/@Timer` attributes are likewise skipped: Recon's
chords come from the registry (P1), not per-class annotations. Revisit only if a v0.3 widget
is genuinely self-contained.

---

## 12. Engine lanes: promoting LLVM

v0.1 pinned Recon to oracle+IR because of #67/#68 — **both fixed** (§2.2). Promotion is a
gated verification track (U7), not an assumption:

| Gate | Check | Pass condition |
|---|---|---|
| G1 | Full Recon corpus (all tests incl. new UI snapshots) on `--build-native` | byte-identical stdout vs `.expected` (LLVM `[heap]` meter is stderr — compare stdout only, the T10 rule) |
| G2 | Interactive smoke on a real terminal: launch, import fixture, edit, send against the hermetic server, quit | no crash, no visual corruption, teardown clean |
| G3 | Append-shape audit | no namespace-global array growth loops (#73); no `Array<Struct>.add` loops (#74) in Recon code — rows are classes (verified in review); prelude-internal struct arrays (`HeaderMap.entries()`) noted as **watchpoint W2**, small-N per frame |
| G4 | `[heap]` meter on the e2e test | `live-at-exit` flat across 3 consecutive runs |

**Outcome A (all green):** README + `tests/run-tests.sh` add the LLVM lane to the matrix
(oracle+IR remain the differential reference pair); the "interpreter-only" language is deleted
from README/DESIGN. **Outcome B (any red):** file the minimal repro in the bug register
(house rule: Recon never patches `src/**`), keep the lane *experimental* behind
`RECON_LANE=llvm` in run-tests.sh, ship v0.2 on oracle/IR unchanged, and re-gate after the
fix. #74 is P0.3 stop-the-line and heads the compiler fix queue regardless — U7 must not
block any UX track (it has no downstream dependents in §15).

emit-C++ stays compile-only forever (no `App.run()` there — unchanged platform fact).

---

## 13. Coding-standard delta (amends DESIGN §12)

The v0.1 standard hard-mandates workarounds for bugs that have since been fixed. **Rule:** a
workaround mandate is retired only when a *probe corpus test* demonstrating the original
failure shape passes on every target lane (oracle, IR, LLVM). Track U0 lands the probes; U8
flips the text. Until U8 merges, the old rules stay in force for new code (no churn mid-flight).

| v0.1 rule | Bug | Status | Action |
|---|---|---|---|
| Explicit `this.` on sibling calls in lambdas | #53 | fixed (register-clear) | **Retire the mandate**, keep as *permitted* style; probe `probe_this_lambda.lev` |
| No `struct` with enum field in `Array` / no `Map<K,Struct>` field | #41/#49 | fixed (`054e159`/`904bcbd`) | **Retire the prohibition**; keep "default to `class` for model rows" as guidance (identity/mutation reasons stand); probes |
| Bind a `char` local before compare/pass | #50 | fixed | Retire; probe |
| Never declare `((T)=>R)?` field | #51 | fixed | Retire; the `hasOnX` flag pattern remains *permitted* (it reads fine); probe |
| Bind indexed callable before calling | #52 | fixed | Retire; probe |
| `env::get` only at top-level `main()` | #68 | fixed | Retire the mandate; keep "read env at the root, pass values down" as architecture guidance (it is simply good design) |
| Map bracket-sugar writes only | #18 | fixed per register | Retire after probe |
| Compose-don't-subclass `Container` | #65 family | fixed | **Keep as house pattern** (simpler, testable, no framework-internals coupling) — reworded from "workaround" to "style" |
| Vertical children opt into `Flex(1)` | moby #1 | framework behavior | **Keep** (N3): every child of a vertical flex that should fill declares it |
| **N1 (new):** no repeated `Array<Struct>.add` loops in native-lane code until #74 closes | #74 open | — | Mandate |
| **N2 (new):** never grow a namespace-global array element-wise; grow a local, assign once | #73 open | — | Mandate |
| **N3 (new):** Recon leaves paint full-width row backgrounds | moby #4 open | — | Mandate |
| **N4 (new):** global chords are Ctrl/F-keys only (R11 capture) | — | — | Mandate (§6.2) |
| **N5 (new):** every user-facing failure produces exactly one notice (§9.2) | — | — | Mandate |
| **N6 (new):** new UI files are added to the manifest `sources` list (explicit dirs; `**` excluded top-level files, v0.1 #71) | — | — | Checklist |

README's standard section is regenerated from the post-U8 state (one source: DESIGN-2 §13).

---

## 14. Testing strategy

House style unchanged — corpus programs, byte-identical across lanes (oracle+IR; +LLVM after
U7 Outcome A). New layers:

1. **Pure additions** (highest value, no UI): `fmt` (all formatters, boundary values),
   `resolveReport` (holes/scopes/dynamics/depth-cap), `parseQueryPairs`/`joinQueryPairs` +
   the §7.2 sync invariant (property-style: URL→table→URL round-trips), `tokenizeJsonLine`
   (strings/escapes/numbers/punct), `authSourceLabel` (cascade table), notice expiry logic
   (injected clock), history label/`fmtAgo` (injected now).
2. **The registry invariants** (§6.1): `hints_never_lie.lev` — walks `registry.bound()`,
   asserts every bottom-bar/menu/help-rendered chord encodes (chord-encoder round-trip) and
   dispatches; asserts every category renders in help; asserts disabled commands refuse with
   a notice. This test is the P1 principle, executable.
3. **Snapshot corpus per surface** (TestRenderer + ScriptedInput, one test dir each):
   main-screen empty states; main screen with fixture loaded; method selector open+cycle;
   each request tab (params sync, header add/edit/toggle via scripted chords, auth kinds,
   body modes + `^F` format, tests add/live-results); JsonView (fold/unfold/search/`a`
   prefill); status line (in-flight tick ×3 frames with scripted clock, settled ok, settled
   error); notice line (each severity, expiry, log modal); env editor (activate/secret mask);
   sessions/cookie manager; import auto-detect both kinds + v1 refusal; history re-open;
   dirty dot + quit guard (all three buttons); settings (theme live-switch snapshot ×2
   themes); palette (filter/run); help overlay; menu bar open/navigate.
   Every scripted test ends `quit()` + `stopSession()` (the T10 hang rule).
4. **Hermetic network flows** (existing server): unchanged suites + cancel-during-send
   (ticker stops, `◌` line), extraction-write notice, redirect-chain line.
5. **Determinism:** clock/random injected everywhere new (`fmtAgo(now,…)`, notice expiry
   ticks driven by scripted pumps, `{{$guid}}` untouched — existing seams).
6. **Lane discipline:** `run-tests.sh` keeps oracle+IR as the differential pair; U7 adds the
   LLVM column per §12. Rebuild before trusting any divergence (the stale-binary rule).

---

## 15. Implementation tracks

Disjoint file ownership; `reconapp.lev`, `app.lev` (AppState), `main.lev`, and `trident.toml`
are owned **exclusively by the integration track UI-INT**, which lands in three passes so
feature tracks never contend on the hub files. Kickoff **2026-07-15**.

### 15.1 Tracks

| Track | Owns (creates/rewrites) | Deliverable | Depends | Acceptance |
|---|---|---|---|---|
| **U0 Probes** | `tests/probes/*` (new dir), §13 probe programs | M0 gate: theme `import()` via assets; BarMenu-in-layout probe; NoticeLine collapse probe; retired-bug probes (§13); LLVM smoke build | — | all probes green on oracle+IR; probe report committed |
| **U1 Visual system** | `themes/recon.toml` (v2), `src/ui/theme.lev`, `src/ui/fmt.lev`, `src/ui/richtext.lev`, `src/ui/statusline.lev`, `src/ui/statusbar.lev` (rewrite) | theme loaded for real + full taxonomy; fmt lib; status line (idle/ok/fail forms); top/bottom bars v2; empty-state texts | U0 | pure fmt corpus; statusline snapshots ×3 states; theme-switch snapshot |
| **U2 Command surface** | `src/ui/commands.lev`, `src/ui/keymap.lev` (rewrite), `src/ui/menubar.lev`, `src/ui/palette.lev` (replaces `commandbar.lev`), `src/ui/help.lev` | registry v2 + full §6.2 keymap + menu bar + palette + generated help + generated hints | U0 | `hints_never_lie`; palette/help/menu snapshots |
| **U3 Request editing** | `src/ui/methodselector.lev`, `src/ui/kveditor.lev`, `src/ui/dialog.lev` (FormDialog), `src/ui/paramspane.lev`, `src/ui/authpane.lev`, `src/ui/testspane.lev`, `src/ui/textarea.lev` (v2), `src/ui/requestpanel.lev` (rewrite), `src/model/request.lev` (query field), `src/net/url.lev` (+query helpers), `src/net/auth.lev` (+authSourceLabel), `src/io/importer.lev`+`exporter.lev` (query round-trip) | the five tabs, complete editing | U1, U2 | §7.2 invariant corpus; per-tab scripted snapshots; import/export round-trip incl. `query[]` |
| **U4 Response experience** | `src/ui/jsonview.lev`, `src/ui/responsepanel.lev` (rewrite) | JsonView + search + assert/extract prefill + redirect chain + raw toggle | U1 | JsonView fold/search/`a` snapshots; big-body fallback test |
| **U5 Workspace** | `src/ui/sidebar.lev` (tabs rewrite), `src/ui/historypane.lev`, `src/ui/enveditor.lev`, `src/ui/sessions.lev`, `src/ui/settingsmodal.lev`, `src/io/store.lev` (settings v2, scratch) | history/env/sessions/cookies/settings/import-export UX, scratch collection | U2, U3(dialogs) | scripted snapshots per modal; settings persistence corpus; auto-detect import corpus |
| **U6 Feedback** | `src/ui/notices.lev`, `src/net/vars.lev` (resolveReport) | notice line + log + taxonomy wiring + unresolved-var strip & send-guard | U1 | resolveReport corpus; notice snapshots; send-guard scripted test |
| **UI-INT** | `src/ui/reconapp.lev`, `src/app.lev`, `src/main.lev`, `trident.toml` | I1 (post U1+U2): new chrome zones wired; I2 (post U3+U4): panels swapped in; I3 (post U5+U6): workspace+feedback wired; each pass fixes §11.3 items in scope | as listed | e2e snapshot after each pass; full suite green |
| **U7 Lane promotion** | `tests/run-tests.sh`, README lane text | §12 gates G1–G4 executed; Outcome A or B recorded | I2 | gate table filled in; matrix updated or `RECON_LANE=llvm` fallback documented |
| **U8 Standards delta** | `README.md`, `DESIGN.md` §12 pointer note, this doc's §13 checkboxes | retire/flip per §13, regenerate README standard | U0 probes, I3 | probes green on all promoted lanes; README regenerated |
| **U9 Release** | `CHANGELOG.md`, `examples`-facing README polish, final snapshot sweep | v0.2.0 | all | full matrix green; F-table (§1) all A/B rows checked off |

### 15.2 Critical path & parallelism

```
U0 ─┬─ U1 ─┬─ U3 ─┐
    │      ├─ U4 ─┤
    ├─ U2 ─┘      ├─ I2 ─ U7
    │             │
    └─(probes)    I1 ──── U5 ─┬─ I3 ─ U8 ─ U9
                       U6 ─┘
```

U1∥U2 after U0; U3∥U4 after U1/U2+I1; U5∥U6 after I1 (U5's dialogs need U3's FormDialog —
sequence U3 first within the owner's queue or split FormDialog into U2; decided: **FormDialog
moves to U2's `dialog.lev` ownership** so U5 and U3 both consume it — table above reflects
final ownership except this line, which overrides: `dialog.lev` → **U2**).

### 15.3 Milestones

| Milestone | Contains | Target |
|---|---|---|
| **M0 — probes green** | U0 | 2026-07-16 |
| **M1 — "looks like a product"** | U1, U2, I1 (menu, palette, help, honest hints, theme live, status line, empty states) | 2026-07-19 |
| **M2 — "editing complete"** | U3, U4, I2 (five tabs, JsonView, assert-from-response) | 2026-07-23 |
| **M3 — "workspace complete"** | U5, U6, I3 (history/env/sessions/settings, notices, var flagging, dirty/save) | 2026-07-26 |
| **M4 — v0.2.0** | U7, U8, U9 | 2026-07-29 |

### 15.4 Risk register & watchpoints

| # | Risk | Mitigation |
|---|---|---|
| W1 | moby #4 stale glyphs on any shrinking leaf | N3 full-width bg painting; themed default bg; snapshot tests shrink content deliberately |
| W2 | #74 surfaces via prelude struct arrays (`HeaderMap.entries()`) on the LLVM lane | small-N per frame; gate G3 watches; Outcome B fallback costs nothing UX-side |
| W3 | BarMenu/overlay focus interplay with palette+dialog stacking | U0 probe exercises menu→submenu→dialog→restore chain before any feature builds on it |
| W4 | NoticeLine collapse (`Fixed(0)`) reflow jitter | U0 probe; fallback = keep the row permanently with quiet bg |
| W5 | JsonView perf on large bodies | 512 KB raw-view guard (§8.2); fold-tree built once per response |
| W6 | Track contention on hub files | UI-INT exclusive ownership; three scheduled passes |
| W7 | Registry refactor breaks v0.1 palette tests | old command names kept as ids' last segment; exact-name run path preserved (§6.4) |

### 15.5 STOP-and-escalate (implementer protocol)

Stop the track and escalate (do not improvise) when: (a) a needed capability turns out to
require modifying `moby/src/**` or compiler `src/**` — file the request/bug instead; (b) any
engine divergence appears on the oracle/IR pair — rebuild first, then file; (c) a #74-shaped
crash (allocator SIGSEGV away from site) appears anywhere — stop-the-line applies; (d) an
overlay/focus behavior contradicts §2.1's verified API table; (e) a §13 probe that "should"
pass red-flags — the standards delta halts, the mandate stays.

---

## 16. Non-goals (v0.2 boundary — stated, surfaced, not silently dropped)

Everything in DESIGN §1.2/§1.3 stays out (JS execution, binary bodies, decompression,
insecure-TLS toggle, SSE/WebSockets/gRPC, OAuth2 flows, pooling). Additionally out of v0.2:
256-color/truecolor themes (16-color discipline instead, §4.1); clipboard/OSC52 copy;
mouse-hover states; a styled-label tree source (framework request candidate); floating toasts
(§9.1 decision); `moby!` template adoption; ASCII-glyph fallback mode; multi-request
tabs/workspaces; response-body diffing. Each is a candidate for a v0.3 list, none blocks the
§1 checklist.

---

*Together with DESIGN.md (core) and RESEARCH.md (verified substrate), this document is
self-contained for the v0.2 effort: §1 is the acceptance checklist, §3 the taste, §4–§10 the
specification, §15 the plan. Where it amends the v0.1 coding standard, §13's probe-gated
process is the only path a rule change may take.*
