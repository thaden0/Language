# Sonar — Tech Design 08: Theming & Dependency Injection

**Status:** LANDED (M1–M3 in full; M4 registry shipped, drift test T10-gated). **Date:** 2026-07-12, landed 2026-07-13. **Track:** T08.
**Owns:** `sonar/src/{theme,toml}.lev`, `sonar/themes/*.toml` (built-in theme assets).
**Depends on:** T01 (Style/Styleable, enums). No unlanded language features (bind/inject DI is landed and verified; value-struct binds rejected by the checker — designed around).
**Gates:** G-S3. **Difficulty:** M, risk LOW/MED (the TOML subset parser is the only moving part).

Conforms to anchor: C10 (Theme surface + composition root), R10 (dotted keys, fallback chain, in-language TOML — bug #30 forbids the JSON path on LLVM, runtime switching mutates the instance), C1 (namespace), cheat-sheet §7 (bracket-sugar map writes — bug #18; no statics ⇒ labeled ctors).

---

## 1. Design position

Theming is three layers, resolved most-specific-first:

1. **Instance overrides** — `Styleable.setStyle(key, style)` on one component.
2. **The bound theme** — `inject ITheme`, resolved at construction (DI).
3. **Component defaults** — hard fallbacks in paint code (`Style()` = Default/Default/0 → the terminal's own colors), so an EMPTY theme still renders legibly. This is the "no theme required" guarantee.

DI is the landed `bind`/`inject`: the composition root binds `ITheme`; components' constructors take an injected trailing `ITheme` parameter filled implicitly. A `Modal` subtree with its own look uses a lexical `bind ITheme => darkVariant;` in its construction scope — scoped rebinding for free (landed nearest-wins). Binds must be interfaces or reference classes (checker rejects struct binds) — `Theme` is a class, `Style` is a struct that is *passed*, never bound. Frozen in the anchor; restated because it shapes every signature here.

## 2. Key grammar and resolution (normative)

**Grammar:** `component[.state].part` — lowercase component name, optional state, part. States (from T04's convention, canonicalized here): `focused`, `disabled`, `selected`, plus the compound `focused.selected` (data views). Parts are per-component (`text`, `border`, `title`, `scrollbar`, `thumb`, `item`, `header`, `divider`, `scrim`, `chord`, ...).

**Resolution chain** for a requested key, first hit wins:

```
1. instance override:  exact key
2. theme:              exact key                     "input.focused.text"
3. theme:              key minus state               "input.text"
4. theme:              component + part elided → component base   "input"
5. theme:              "default"
6. Style()             (Default/Default/0)
```

Each hit **layers over** the next via `Style.over` (fg/bg `Default` = transparent, attrs OR — anchor C2), so a theme may specify only `input.focused.text = { attrs = "reverse" }` and inherit colors from `input`. Resolution is therefore a fold from step 5 up to step 1, not a single lookup — `resolve` composes the chain. Cost: ≤5 map probes per style fetch, done per damaged-component repaint only; a per-component memo (`Map<string, Style>` cleared on theme change) is the stated optimization if profiling demands (not built by default — issue §6.3).

`Styleable.resolve(key, theme)` (anchor C8) implements this; every T04/T05 paint path fetches through it.

## 3. `Theme`

```lev
class Theme : ITheme {
    // storage: Map<string, Style> — bracket-sugar writes only (bug #18)
    new Theme();                              // empty
    new FromToml(string tomlText);            // parses; throws SonarException with line/col on error
    Theme set(string key, Style s);           // fluent (leaf rule R6)
    Style style(string key);                  // raw single-level lookup; missing => Style()
    bool has(string key);
    void setTheme(Theme other);               // runtime switch: replace contents, then
                                              // Sonar::app() full invalidate (R10)
    Array<string> keys();                     // introspection/tooling
}
```

Built-in themes as **labeled constructors** (no statics): `Theme::Default()` (terminal-native: near-empty map — the defaults guarantee does the work), `Theme::Dark()`, `Theme::Light()`, `Theme::HighContrast()` (enterprise/accessibility: bold+reverse focus indicators, no color-only signaling — the a11y ruling: every state distinction must include a non-color channel (attrs); this constraint applies to ALL built-ins and is the documented bar for contributed themes).

Built-ins are authored as TOML **assets** (`sonar/themes/*.toml`, declared in the package manifest) loaded via comptime `import()` inside labeled ctors: `new Dark() { loadToml(import("themes/dark.toml")); }` — the file is a hashed build input (LA-20), the parse cost is runtime-at-construction (comptime holds only the string). One-time cost, negligible.

## 4. The TOML subset (normative — `toml.lev`)

Exactly what themes need, nothing more (dependency-free, matching the toolchain's own hand-rolled-parser philosophy):

```toml
[input.focused]                # table header = key prefix
fg    = "brightWhite"          # Color by name (camelCase of the enum member) or int 0..15 or "default"
bg    = "blue"
attrs = "bold|underline"       # '|'-joined Attr names, or an int

[menu.item]
fg = 7
```

Supported: table headers (dotted), `key = value` with string/int values, `#` comments, blank lines. NOT supported (parse error, not silent skip): arrays, inline tables, multiline strings, floats, booleans, quoted keys. Errors throw `SonarException("theme.toml:12:5: expected '=' ...")` — line/col mandatory.

Parser shape: line-oriented scan (split on `\n`, trim, classify), a current-prefix register from the last table header, `fg/bg/attrs` folded into a `Style` per prefix written at table close. ~150 lines of `.lev`, pure string/stdlib code. Color-name table = a `match`-free `Map<string,int>` built once; attr names likewise.

## 5. Composition root & recipes (documentation-grade, shipped in the doc + examples)

```lev
bind ITheme => Theme::Dark();
bind IRenderer => AnsiRenderer();
bind IFocusPolicy => FocusRing();
bind IInputSource => StdinSource();
App app = App();
```

Recipes to include in the package docs (each a tested example): user-config theme (`Theme::FromToml(File.readAll(path))` with a try/catch fallback to `Theme::Default()`); scoped Modal rebind; runtime dark/light toggle (`boundTheme.setTheme(Theme::Light())` from a `@Shortcut`); brand palette via `Theme::Dark()` + `.set` overrides.

## 6. Potential issues & mitigations

1. **Key drift** between T04/T05 paint code and theme files. Mitigation: this doc's appendix is the **canonical key registry** (aggregating every key T04/T05 declare); a T10 test constructs every component, repaints under a tracing ITheme (records requested keys), and diffs against the registry — drift fails CI.
2. **TOML parse fragility.** Mitigation: the subset is tiny and closed; table-driven parser tests including every error message; malformed built-in assets are impossible to ship (their ctor test loads them).
3. **Resolution cost** (≤5 probes × layered `over` per fetch). Mitigation: acceptable at damage-repaint frequency by design; the memo optimization is specified (§2) and gated on a profile, not speculation.
4. **`setTheme` invalidate ordering** — switching themes from inside a paint would corrupt the frame. Mitigation: `setTheme` calls `App.requestFrame()` semantics (mutations take effect next frame — the C9 frame rule); never repaints synchronously.
5. **Injected-theme capture** — components capture the ITheme reference at construction; a LATER `bind` shadow doesn't retro-affect constructed components. Correct per DI semantics; documented so nobody expects container-style re-resolution.
6. **Color-blind/mono terminals** — the a11y attrs-channel rule (§3) plus `Theme::HighContrast()`; `NO_COLOR` env respect is a T09 open question (renderer strips SGR colors), cross-referenced.

## 7. Milestones

| M | contents | difficulty |
|---|---|---|
| M1 | Theme + resolution chain + Styleable.resolve wiring | M |
| M2 | TOML subset parser + error taxonomy | M |
| M3 | built-in themes (4 × .toml assets + ctors) + a11y pass | S |
| M4 | key registry appendix + tracing-theme drift test (with T10) | S |

## 8. Testing plan

Unit: resolution-chain table tests (every fallback step + layering algebra, incl. transparent-fg-over-bg cases); TOML parser goldens (valid + every error with line/col asserted); theme-switch invalidation (snapshot before/after under TestRenderer); scoped-rebind Modal test (two themes in one tree, one frame); the drift test (§6.1). Differential oracle/IR/LLVM.

## 9. Open questions

1. `NO_COLOR` / `TERM=dumb` handling — T09 renderer question, registry cross-ref here.
2. Theme hot-reload from disk in dev (needs file-watch natives that don't exist) — v2, would be a `designs/requests/` ticket if pursued.
3. The `theme` template attribute (T06 E9 deferral) — resolves together with a Styleable prefix ruling; v1.1.

## Appendix A — Canonical key registry (M4)

The vocabulary the built-in themes declare and paint code fetches through
`Styleable.resolve`. States (`focused`/`disabled`/`selected`) append per the §2
grammar `component[.state].part`; only the state keys the built-ins actually
carry are listed. The a11y rule (§3) holds for **every** state key here — each
carries a non-color `attrs` channel in all three coloured built-ins.

| component | base | parts / states declared |
|---|---|---|
| (root) | `default` | — (colours the whole surface; the fold's step-5 anchor) |
| Text | `text` | — |
| Input | `input` | `input.focused`, `input.disabled` |
| Button | `button` | `button.focused`, `button.disabled` |
| CheckBox | `checkbox` | `checkbox.focused` |
| RadioGroup | `radio` | `radio.focused`, `radio.selected` |
| ProgressBar | `progress` | `progress.bar` |
| Spinner | `spinner` | — |
| ContentBar | `contentbar` | — |
| Bordered (mixin) | `border` | `border.focused`, `title` |
| Scrollable (mixin) | `scrollbar` | `scrollbar.thumb` |
| Menu (T05, provisional) | `menu.item` | `menu.item.selected` |

**Drift test (deferred to T10, per §6.1):** the automated guard — construct
every component, repaint under a tracing `ITheme` that records requested keys,
and diff against this registry so any drift fails CI — needs the T09 `App`/paint
loop to drive a real repaint, so it lands with T10 (T08 ships the registry and
the resolution machinery it will check). Until then this table is the
maintained-by-hand contract; a new component adds its rows here and to the
coloured built-in assets in the same commit.

## 10. Implementation log

- 2026-07-12 — design written; not started.
- 2026-07-13 — **M1–M3 landed in full; M4 registry appendix shipped, its
  automated drift test deferred to T10 (§6.1, gated on the App paint loop).**
  Files: `sonar/src/{theme,toml}.lev`, `sonar/themes/{default,dark,light,
  highContrast}.toml`, `Styleable.resolve` (mixins.lev) expanded to the full
  §2 six-step layered fold, `assets = ["themes/*.toml"]` added to the package
  manifest. Acceptance corpus `sonar/tests/theme/` (resolution chain + layering,
  TOML valid + every error with line:col asserted, all four built-ins + a11y
  audit, in-place theme switch, DI inject + scoped rebind) is **byte-identical
  on oracle / IR / emit-C++ / LLVM**. Existing T01–T06 suites stay green on all
  four engines.

  **Two forced deviations from this doc, both principled and filed:**

  1. **Theme storage is parallel primitive columns, not `Map<string, Style>`
     (§3).** Two P0 footguns escalated *after* this design was written both bite
     the literal encoding on the primary backend: **#49** (`Map<K, Struct>`
     class field silently corrupts on LLVM at 3+ entries — every real theme has
     far more) and **#41** (`Array<struct>` whose struct has an enum field —
     `Style` has `Color fg`/`Color bg` — goes stale after unrelated heap
     activity on emit-C++/LLVM, and paint runs heavy allocation between a
     theme's construction and its reads). The `Map<K,int>`-into-parallel-array
     workaround (docs/footguns.md) is taken one step further to dodge **both**:
     a `Map<string,int> keyIndex_` indexes three parallel `Array<int>` carrier
     columns (fg code / bg code / attrs); `Style` is reconstructed by value on
     read. No struct ever enters a `Map` value or an `Array` element, so neither
     bug is reachable. Debt rows added to footguns.md (#41, #49) — reverting to
     `Map<string, Style>` is sanctioned once either bug is fixed. The `ITheme`
     surface (`style`/`has`) and `Theme`'s public API are exactly as specified.
  2. **Built-in assets load via a top-level `comptime` global, not inline
     `loadToml(import("..."))` (§3).** `import()` only folds in a **comptime
     context**; called in a runtime ctor body it throws
     "import() is compile-time-only" (LA-20). The landed shape is one
     `comptime string themeXToml = import("themes/x.toml");` per asset at
     namespace top level, spliced into the labeled ctor — functionally identical
     (comptime holds the string, the parse runs at construction), and verified
     working **cross-package** (a dependency resolving `import()` against its own
     `assets`) on oracle and LLVM native.

  **DI (§1, §5):** landed `bind`/`inject` used as-is — factory-form bind
  (`bind ITheme => Theme::Dark();`, footgun #56's sanctioned shape) with a
  scoped block rebind for the Modal/subtree case (nearest-wins). Retrofitting an
  injected trailing-`ITheme` ctor parameter onto every T04 component is **not**
  in T08 (it would rewrite T04's landed ctors); components keep T01's
  `activeTheme_`/`installTheme` seam, and `Styleable.resolve` is the fold every
  paint path calls. The composition-root recipes (§5) ship as documentation.
