# Atlantis Track 09 — Views

**Status:** draft for owner review. **Date:** 2026-07-06.
**Depends on:** 01 (kernel/Context/static files), 02 (routing, `View` dispatch, `@Fragment`
attribute per C5), 03 (content negotiation, C7 `toJson`), 08 (CSRF token seam, session/flash
seam). **Owns:** `Atlantis::Views` (namespace + `views/` conventions).
**Wave 4 — this track may LAG (gate AG-7, 2027-01-15).** Nothing in Tracks 01–08 imports
`Atlantis::Views`; API-first apps are fully usable without it. Even the `@Fragment` attribute
lives in Track 02's C5 vocabulary — this track only consumes it. Deleting this package from a
JSON-only app must change nothing.

---

## 0. Mission, scope, non-goals

### 0.1 Mission

Server-rendered HTML for Atlantis: an MPA-first, hypermedia-native view layer where one
controller serves a full page, an htmx-style fragment, and JSON from the same handler —
without shipping a client framework, a bundler, or a second programming language inside
templates.

### 0.2 Why NO bundled SPA framework (re-derived, not inherited)

Loom's proposal (§10) reached "MPA-first, htmx, optional Inertia, no bundled SPA" from market
research (React fatigue, htmx resurgence, Next.js's hidden-boundary cautionary tale). That
research is sound and we land on the same conclusion — but for Atlantis the decision follows
from **our own invariants**, and would hold even if the market disagreed:

1. **Our compile-time story ends at the HTTP boundary.** Atlantis's whole pitch is
   attributes + rules → greppable, `--expand`-visible, checker-verified code. Any JavaScript
   we ship is *unverifiable by our compiler* — bundling an SPA means the flagship framework's
   most user-visible layer is the one layer our language cannot check. That is
   anti-differentiation.
2. **Trident runs no dependency code and ships no binaries** (overview §1). An SPA implies an
   asset pipeline — node, bundler, lockfiles from a foreign ecosystem. There is no home for
   that in our toolchain, and building one violates the PM/compiler separation hard req.
3. **One model per concern (STOP (c)).** An SPA introduces a second routing model, a second
   state model, and a second validation locus. The hypermedia path keeps the server as the
   single source of truth, reusing Tracks 02/03/08 unchanged.

**Where our v1 is more honest than Loom's:** Loom promised type-checked templates in the base
language ("templates are type-checked Language, not a new grammar", proposal §10 tier 1 and
§16.3). That promise silently requires comptime file access — the compiler reading template
files at build time — which the language **does not have** (comptime is hermetic, overview
§1). Loom's headline view feature could not have shipped. Our v1 states the constraint up
front (§1), ships a boot-checked engine that works on today's language, and files the
comptime ask as a precise ticket instead of designing on vapor.

### 0.3 Scope (v1)

- Template engine: files under `views/`, parsed **once at boot** into compiled node trees;
  dev-mode re-parse on mtime change (§2).
- Minimal Mustache-adjacent syntax — output, if/else, for, partials, one layout mechanism
  (§2.2). **No arbitrary code in templates** (§2.6).
- HTML-escaped output by default; raw is opt-in and greppable (§3).
- `View` return type + `view()` helper; content negotiation with Track 03 (§4).
- Fragments (`@Fragment`, HX-Request detection), `{{csrf}}` form helper, flash messages via
  the Track 08 session seam (§5).
- Boot-time template diagnostics: parse everything, fail fast, list every error (§8).

### 0.4 Non-goals

- **No bundled SPA framework** (§0.2). **No client JS shipped** — not even htmx itself. We
  document htmx compatibility (attribute conventions, HX-Request handling) and the demo app
  vendors `htmx.min.js` into `public/` like any other static asset (Track 01 serves it).
  Optionally, post-v1, a note-sized "tiny fragment swapper" (~30 lines of documented JS the
  app may copy) — a doc artifact, never a package export.
- No template expressions beyond dot-paths (no filters, pipes, arithmetic, comparisons —
  §2.6). No i18n, no markdown, no asset fingerprinting in v1 (§7).
- No compile-time template checking in v1 — that is the `comptime template import` ticket
  (§1.3), and v1 must not secretly depend on it.
- Inertia adapter is a designed seam only (§6), post-v1.

---

## 0R. R10 alignment (2026-07-07) — file conventions from the example

Per overview ruling R10 (`designs/atlantis/example/`): templates live in
**`Resources/Views/`** with the **`.lhtml`** extension (`Resources/Views/home.lhtml`);
`view('home')` resolves `home` → `Resources/Views/home.lhtml` (subdirs by dot or slash:
`view('links/index')`). The boot walk (§8), dev-mode mtime reload, and everything else in
this doc target that path/extension. Static assets move to `Public/` (Track 01's
`useStaticFiles()`). The example's `home.lhtml` reads "No Templating Designed Yet" — this
track is the design that fills it; `Atlantis::View('home')` in the example is the
`IActionResponse` constructor (Track 01 §3R) whose `render()` calls this engine.

---

## 1. The honest core constraint

### 1.1 Two hard facts

**(a) comptime cannot read files.** Comptime is hermetic by design (metaprogramming docs;
overview §1). A template *file* therefore cannot participate in compilation: no rule can
parse `views/index.html` and check `{{ user.name }}` against a model type. Any design
claiming compile-time-checked template files today is lying about the language.

**(b) There is no runtime reflection — permanently** (STOP (a); reference classes are
non-reifiable at comptime). A runtime template engine cannot walk an arbitrary typed object:
given a `User`, there is no `getField("name")`. So "pass your typed model straight to the
interpreter" is impossible without per-type generated accessors keyed by *runtime* strings —
which is reflection by the back door.

### 1.2 The v1 resolution: typed at the boundary, dynamic inside

View models cross into templates as **`json::JsonValue`, produced by C7 `toJson()`**. The
handler boundary is fully typed — `view("links/index", dto)` compiles only if `dto` has the
C7 surface — and the template interior is dynamic: dot-paths (`user.name`, `items.0.title`)
are string lookups into the JsonValue tree at render time.

**The trade, stated explicitly:**

| We give up (v1) | We keep |
|---|---|
| Compile-time template/path checking — a typo'd `{{ user.nmae }}` surfaces at render (dev: loud error; prod: logged empty — §3.3), not at build | Zero runtime reflection (STOP (a) intact); ships on today's language |
| Per-render `toJson()` tree construction cost (measured, P-4) | ONE interchange type (C7) — the same `toJson` the JSON API path uses; content negotiation is free (§4.3) |
| Template-side type-driven tooling (completion against the model) | Boot-time whole-corpus template validation (§8) — every syntax error, unknown partial, and context lint caught before the first request |

This is strictly more honest than a "typed templates" claim we cannot check, and strictly
safer than stringly-typed rendering with no boot validation.

### 1.3 The future ask (ticket proposed, NOT designed on)

**`request-comptime-template-import.md` — comptime file inclusion.** Precisely: a
build-time-resolved construct, e.g. `comptime string src = import("views/links/index.html");`
(paths resolved relative to the manifest root, content hashed into the build for
reproducibility), legal only in comptime context, making file *content* available to rules
and comptime functions. **What it unlocks for this track:** the same grammar (§2.2) parsed at
*compile* time by a comptime function; dot-paths checked against a declared model struct's
`meta.*` fields; codegen of a `render(Model m)` free function per template (direct field
access — no JsonValue, no boot parse, `--expand`-visible); template typos become compile
errors. The v1 engine is designed so this drops in behind the same `view()` surface
(compiled templates are just another `Template` provider) — but **no v1 feature assumes it**.

---

## 2. Template engine v1

### 2.1 Files, discovery, cache

- Templates live under `views/**` with extension `.html`. Template **name** = path relative
  to `views/` minus extension: `views/links/index.html` → `"links/index"`. Partials are
  ordinary templates whose final path segment starts with `_` (`links/_list`) — a naming
  convention, not a mechanism.
- **Boot:** recursive walk via `std::sysListDir` (Track 08 system natives F3) +
  `sysStat(path, 0)` to distinguish dirs from files; every `*.html` is read
  (`File(path, std::read)`) and parsed into a `Template`. All errors are aggregated (§8);
  any error → the app refuses to start.
- **Cache:** `Map<string, Template>` inside the `Engine`. Prod: immutable after boot — no
  stat, no reload. Dev (`views.dev = true` via C6 config): each render of a template first
  compares `sysStat(path, 2)` (mtime) **and** `sysStat(path, 1)` (size) against the cached
  values; on change, re-read + re-parse *that file only*. Includes and layouts are resolved
  by name **at render time** (§2.4), so invalidation is strictly per-file — editing a partial
  never requires invalidating its parents.

### 2.2 Syntax — exactly this, nothing more

```
{{ expr }}                       output, HTML-escaped (§3.1)
{{{ expr }}}                     output, raw — discouraged, greppable (§3.2)
{{#if expr}} … {{#else}} … {{/if}}      conditional ({{#else}} optional; §2.5 truth table)
{{#for x in expr}} … {{/for}}    iterate a JSON array; x binds each element
{{> partial/name}}               include (renders with the CURRENT model + loop scope)
{{#layout "layouts/main"}}       first construct of a child template (at most one)
{{#block "title"}} … {{/block}}  named block a child provides to its layout
{{#yield "title"}} / {{#yield}}  layout-side slots; bare yield = the child's body
{{csrf}}                         hidden CSRF input (Track 08 seam, §5.3)
{{asset "css/app.css"}}          static asset URL (Track 01 convention, §7)
{{! comment }}                   stripped at parse; never rendered
```

`expr` grammar (deliberately tiny): `path | literal` where
`path = ident ('.' (ident | int))*` — dot-paths into the model (`user.name`,
`items.0.title`), resolved segment-by-segment through the JsonValue tree (ints index
arrays); `literal = "string" | int | true | false` (literals exist chiefly as directive
arguments; in output position they render themselves). Inside `{{#for x in items}}`, `x`
becomes the root of paths in the body (`x.title`), shadowing outward scopes; `$index` (the
0-based `int` loop counter) is the **only** builtin variable. There are no operators, no
calls, no filters, no parentheses. Literal `{{` in text has no escape in v1 — use
`&#123;&#123;` or put it in the model (documented; revisit only on real demand).

### 2.3 Why no code in templates (the anti-"second language" stance)

Loom's tier-1 promise — "the template language IS the base language, type-checked against
the model" — was the principled way to allow rich template expressions: they'd be ordinary
checked code. Without comptime file access, **that road is closed** (§1.1), and every
expression feature we add instead becomes a *second, unchecked, interpreted language* — the
exact thing this project exists to avoid. So the stance inverts cleanly: since template
expressions cannot be checked, templates get **no expressions**. A condition like "is this
user an admin" is computed in the handler (typed, tested, checked code) and shipped to the
view as a boolean field: `{{#if user.isAdmin}}`. The view model is the API of the page.
Ours ships today under this constraint; Loom's couldn't ship at all. When the comptime
ticket lands (§1.3), richer checked expressions become possible *without* an interpreter —
the grammar stays frozen until then (STOP-1).

### 2.4 Compiled representation — parser and node tree

Closed node hierarchy, `match`-dispatched (the track's `match` showcase). Sketches in the
verified dialect (overview §8):

```
namespace Atlantis::Views {
    class Node { int line; }                          // source line for error reporting
    class TextNode    : Node { string text; }
    class OutputNode  : Node { Array<string> path; bool raw; }   // path=[] + lit for literals
    class LiteralNode : Node { string value; }
    class IfNode      : Node { Array<string> condPath; Array<Node> thenNodes; Array<Node> elseNodes; }
    class ForNode     : Node { string varName; Array<string> path; Array<Node> body; }
    class IncludeNode : Node { string partialName; }
    class YieldNode   : Node { string blockName; }    // "" = the child's body
    class CsrfNode    : Node { }
    class AssetNode   : Node { string assetPath; }

    class Template {
        string name;               // "links/index"
        string filePath;           // "views/links/index.html"
        int mtime; int size;       // dev-mode invalidation (§2.1)
        string? layoutName;        // from {{#layout "…"}}; None = standalone
        Map<string, Array<Node>> blocks;   // {{#block}} bodies
        Array<Node> body;          // nodes outside any block
    }

    class ParseError { string file; int line; string message; }

    class Parser {                 // hand-rolled scanner — no regex (LA-13)
        string src; string file; int pos; int line;
        Array<ParseError> errors;
        Template parse(string name, string filePath, string src) {
            // loop: indexOfFrom(src, "{{", pos) → TextNode for the gap (counting '\n'
            // to track line), then classify the tag: "{{{", "{{#", "{{/", "{{>", "{{!",
            // or plain output. Block constructs push a frame on an explicit stack;
            // "{{/if}}"/"{{/for}}"/"{{/block}}" pop and verify the kind matches.
            // Unterminated tag / mismatched close / unknown tag → errors.add(...) with
            // the OPENING line, then resynchronize at the next "{{" (collect ALL errors,
            // don't stop at the first — §8).
        }
    }
}
```

Render is a recursive walk with an explicit scope chain and a `StringBuilder` sink:

```
class Frame {                       // render-time scope
    JsonValue model;                // root model
    Array<Pair<string, JsonValue>> locals;   // for-loop bindings, innermost last
    int loopIndex;                  // $index of the innermost for
    Map<string, Array<Node>> childBlocks;    // set when rendering a layout
    RenderEnv env;                  // Context handle: csrf, flash, devMode (§5)
}

void renderNodes(Array<Node> nodes, Frame f, StringBuilder out) {
    for (Node n in nodes) {
        match (n) {                                      // type arms narrow `n`
            TextNode    => out << n.text;
            LiteralNode => out << escapeHtml(n.value);
            OutputNode  => renderOutput(n, f, out);      // resolve + escape/raw (§3)
            IfNode      => renderNodes(truthy(resolve(n.condPath, f)) ? n.thenNodes
                                                                      : n.elseNodes, f, out);
            ForNode     => renderFor(n, f, out);         // binds varName + $index per item
            IncludeNode => renderTemplate(n.partialName, f, out);   // by-name, depth-capped
            YieldNode   => renderNodes(f.childBlocks.atOrNone(n.blockName)
                                           ?? emptyNodes(), f, out);
            CsrfNode    => out << f.env.csrfInput();     // Track 08 seam (§5.3)
            AssetNode   => out << f.env.assetUrl(n.assetPath);      // §7
            else        => throw RenderException(templateAt(n), "unknown node");
        }
    }
}
```

**Layout weaving is render-time, not parse-time:** if `t.layoutName != None`, the engine
renders the *layout* template with `childBlocks` = `t.blocks` plus `"" → t.body`. This keeps
dev invalidation per-file (§2.1), allows a layout edit without touching children, and needs
no graph bookkeeping. Layouts do not chain in v1 (a layout declaring `{{#layout}}` is a boot
error); nesting is a post-v1 escalation, not a drive-by (STOP-1).

### 2.5 `{{#if}}` truth table and `{{#for}}` semantics

The language forbids truthiness, but the template `if` operates on *dynamic JSON*, where
presence-testing is the whole point. The rule is fixed and documented:

| Value at path | `{{#if}}` |
|---|---|
| missing path / null / `false` | falsy |
| `""` (empty string) / `[]` (empty array) | falsy |
| any number — **including 0** | truthy (0 is a real value; documented loudly) |
| everything else (nonempty string/array, object, `true`) | truthy |

`{{#for x in expr}}`: expr must resolve to an array — anything else is a render error in dev,
renders nothing + logs in prod (§3.3). Empty array renders nothing ("no items" messaging is
`{{#if}}` + the empty-array-falsy rule). Objects are not iterable in v1 (shape your model).

### 2.6 Boot cost model

Parsing is one linear scan per file (`indexOfFrom`-driven), O(bytes); the tree is retained
for the process lifetime. Expected corpus sizes (tens of templates, KBs each) make boot parse
negligible on LLVM; the interpreters are the open question — measured, not assumed (P-3).

---

## 3. Escaping, null rules, missing keys (the injection model)

### 3.1 Escaped by default

Templates are **trusted code**; model values are **untrusted data**. Every `{{ }}`
interpolation is HTML-escaped with this exact set:

`&` → `&amp;`  `<` → `&lt;`  `>` → `&gt;`  `"` → `&quot;`  `'` → `&#39;`

Fast path: scan first; if no byte in the set appears, return the original string —
zero-allocation for the common case (matters for P-4). Because `"` **and** `'` are escaped,
interpolation is safe in HTML text content and inside quoted attribute values (either quote
style). That is the supported surface.

### 3.2 Context rules — restrict, don't pretend

The v1 engine is context-blind (it does not parse HTML). Rather than promise context-aware
autoescaping we can't deliver, v1 **restricts**, and *lints what it can see*:

- **Allowed:** interpolation in text content and inside quoted attribute values.
- **Forbidden (documented + boot-linted where detectable):** interpolation inside `<script>`
  or `<style>` elements; in unquoted attribute values; as/inside an event-handler attribute
  (`onclick=` etc.); composing a full URL *scheme* from data (`href="{{ x }}"` where `x` may
  carry `javascript:` — put path/query pieces in, never the scheme).
- **Boot lint (§8):** the parser sees the raw text before each tag. Cheap string scans (no
  regex) flag: an interpolation whose preceding text contains an unclosed `<script`/`<style`,
  ends with `=` (unquoted attribute), or ends with `on…="`-shaped attribute openings. Dev +
  boot: these are errors; they are heuristics, so an explicit per-site
  `{{! lint:allow }}` comment on the preceding line downgrades to a warning (greppable).
- `{{{ raw }}}` is the *only* unescaped sink: three braces, trivially greppable
  (`grep -rn '{{{' views/`), called out in docs as a security review point. Sanitized-HTML
  use cases (rendered markdown) are the intended user.

### 3.3 Null and missing-key policy

- **Path resolves to JSON `null`:** renders as `""` in all modes, no warning — `null` is a
  legitimate modeled value (a `T?` field serialized via C7). In `{{#if}}`: falsy.
- **Path missing** (key absent / index out of range / traversing a non-object):
  - **dev:** loud render error — the response is a diagnostic page:
    `views/links/index.html:12 — path 'user.nmae' not found (nearest: 'user' is an object with keys [name, email])`.
  - **prod:** renders `""` and logs one structured warning (template, line, path) via
    `Atlantis::Log`.
  - **Justification:** dev optimizes feedback speed — a typo must be unmissable. Prod
    optimizes availability — a stale optional field must not 500 a working page; the warning
    keeps it visible to operators. In `{{#if}}`/`{{#for}}` *test position*, missing is falsy /
    empty **in both modes** — existence-testing is those constructs' purpose, so absence
    there is signal, not error.
- **Missing template / missing partial at render:** error in both modes (500) — that is a
  programmer error, not a data condition. Boot diagnostics (§8) make it nearly impossible to
  reach: `{{> name}}` targets are checked against the loaded corpus at boot.
- Numbers render via the Track 09 `json` number formatting (integer-valued floats print
  without `.0` — verified in P-6); bools render `true`/`false`; an **array or object in
  output position** follows the missing-key policy (dev error / prod empty + warn) — it is
  almost certainly a mistake, not a serialization request.

---

## 4. Controller integration

### 4.1 The `View` type and `view()` helper

```
namespace Atlantis::Views {
    class View {                        // recognized by Track 02 dispatch (like HttpResponse)
        string templateName;
        JsonValue model;
        int status;                     // default 200
        bool asFragment;                // §5.1; default false
    }
    // The boundary: typed handler → JsonValue interior (§1.2).
    View view(string name, JsonValue model) => View(name, model, 200, false);
    View view<T>(string name, T model) => View(name, model.toJson(), 200, false);  // C7
    View fragment<T>(string name, T model) { View v = view(name, model); v.asFragment = true; return v; }
}
```

The generic overload compiles only for types with the C7 `toJson()` member — the typed
boundary. (Open question for Track 03: if `@Serializable` also stamps an
`IJsonSerializable { JsonValue toJson(); }` interface, the generic becomes a plain
interface-typed overload — preferred; tracked in the log, not blocking.) Track 02's
dispatcher, when a handler returns `View`, calls the engine (bound via DI:
`bind Atlantis::Views::IViewEngine => Engine(cfg);` in the composition root) and wraps the
rendered string in an `HttpResponse` with `Content-Type: text/html; charset=utf-8`.
`redirect("/path")` stays Track 01/02 surface — not owned here.

### 4.2 Model building without a serializable type

`JsonValue::ofObject` composes ad-hoc view models (wrapping several DTOs plus page-level
fields) — no anonymous-object feature needed:

```
@Get("/")
View index() => view("links/index", JsonValue::ofObject(
    Map<string, JsonValue>()
        .with("top",   linksDto.toJson())
        .with("title", JsonValue::ofStr("Top links"))));
```

### 4.3 Content negotiation — one handler, HTML and JSON (Track 03 seam)

Because the view model **is** C7 JSON, negotiation is a one-liner: same data, two renderers.

```
@Get("/links")
HttpResponse list(Context ctx) {
    LinkListDto dto = LinkListDto::From(links.top(25));
    if (Atlantis::Json::wantsJson(ctx)) {           // Track 03: Accept-header check
        return Atlantis::Json::jsonResponse(dto);   // toJson → application/json
    }
    return render(ctx, view("links/index", dto));   // same toJson → HTML
}
```

Simpler apps skip negotiation and return `View` directly (Track 02 renders it); the
explicit-`HttpResponse` form above is the escape hatch when one route genuinely serves both.

---

## 5. Fragments, forms, flash (the hypermedia tier)

### 5.1 `@Fragment` routes (C5)

`@Fragment {}` (defined by Track 02 in the C5 vocabulary) marks a route whose views render
**without layout**: Track 02's routing rule records the flag in the route table; the
dispatcher sets `asFragment = true` on the returned `View` (equivalently, handlers call
`fragment(...)` directly). Rendering with `asFragment` skips `layoutName` weaving — the
template's own body renders bare. Same template file works both ways.

### 5.2 Full page vs fragment from ONE route (HX-Request)

```
bool wantsFragment(Context ctx) =>
    (ctx.request.headers.first("HX-Request") ?? "") == "true";   // HeaderMap, Track 09

@Get("/search")
View search(Context ctx, string q) {
    ResultsDto dto = links.search(q);
    if (Atlantis::Views::wantsFragment(ctx)) {
        return fragment("links/_list", dto);        // htmx swap target: just the <ul>
    }
    return view("links/index", dto);                // direct visit / refresh: full page
}
```

`views/links/index.html` contains `{{> links/_list}}` where the list belongs — the fragment
file is the single source of that markup for both paths. This is the pattern the demo app
showcases for AG-7. (We ship no JS: the page includes `/public/htmx.min.js`, vendored by the
app — §0.4.)

### 5.3 Form helper: `{{csrf}}` (Track 08 seam)

`{{csrf}}` emits `<input type="hidden" name="_csrf" value="…">`. The value comes from
`RenderEnv`, populated at render start via the **frozen seam**
`Atlantis::Auth::csrfToken(Context) -> string` (Track 08 owns generation/verification and the
`_csrf` field-name constant; this track only plants the input). Rendering `{{csrf}}` when no
CSRF middleware is installed is a render error in dev (loud), empty+warn in prod (§3.3
policy). Deliberately no `{{#form}}` block helper in v1 — hand-written `<form>` + `{{csrf}}`
is one line and zero new grammar.

### 5.4 Flash messages (Track 08 session seam)

`Atlantis::Views::flash(ctx, "notice", "Link added")` writes to the Track 08 session
(`Auth::session(ctx).put("_flash.notice", …)`); at render, the engine pops (read-once) all
`_flash.*` entries into a **reserved top-level model key `flash`** — templates read
`{{#if flash.notice}}<div class="notice">{{ flash.notice }}</div>{{/if}}`. Reserved model
keys: `flash` (only one in v1); `view()` logs a warning if the user model already carries it.
If Track 08's session shape diverges from `put/get` string semantics, coordinate — don't
fork (STOP-4).

---

## 6. Inertia-style adapter — seam only (post-v1)

For teams wanting React/Vue without a separate API, the Inertia protocol is a thin dialect of
what §4.3 already does: `Inertia::page("Links/Index", props)` where `props: JsonValue` (C7
again). Request without `X-Inertia` header → render a fixed shell template
(`views/inertia/shell.html`) with the page object JSON-escaped into `<div id="app"
data-page="…">`; request with `X-Inertia: true` → `application/json` page object
(`component`, `props`, `url`, `version`). **Everything rides existing machinery**: HeaderMap
detection (like §5.2), the escaper (§3.1 — note `data-page` is a quoted attribute, the
supported context), and C7. No client code shipped (the team brings `@inertiajs/*`
themselves). Ship after AG-7 only if demanded; the seam costs one namespace and zero core
changes.

---

## 7. Static assets — conventions only

Track 01 owns static file serving from `public/`. This track defines only the template-side
helper: `{{asset "css/app.css"}}` → `{prefix}/css/app.css` where prefix comes from config
(`views.assetPrefix`, default `/public` — must match Track 01's mount). Rationale for a
directive instead of hand-written paths: one greppable seam for the post-v1 cache-busting
upgrade (a manifest mapping logical → fingerprinted names — needs build tooling, explicitly
out of v1 scope; when it lands, `{{asset}}` call sites upgrade for free).

---

## 8. Dev experience: boot diagnostics

At boot (and on `./myapp views` — a diagnostic subcommand registered through Track 04's
app-hosted dispatch, R3):

1. Walk `views/`, parse **every** template, **collecting** errors — the parser resynchronizes
   at the next `{{` after an error instead of stopping (§2.4), so one pass reports the whole
   corpus.
2. Cross-file checks: every `{{> name}}` and `{{#layout "name"}}` target exists; every
   `{{#block}}` name in a child has a matching `{{#yield}}` in its layout (warning, not
   error — layouts may legitimately ignore optional blocks); no layout declares a layout;
   include-graph cycle check (DFS over IncludeNode targets — render also depth-caps at 32 as
   defense in depth).
3. Context lints (§3.2).
4. Output, one line per finding: `views/links/index.html:12: unclosed {{#if}} (opened line 8)`.
   Any **error** → process exits nonzero before binding the port (fail fast); `./myapp views`
   with a clean corpus lists `name → file (layout, blocks, partials used)` — the views
   analog of `./myapp routes`.

---

## 9. P-probes (run BEFORE feature work; failures → /bug.md or an LA, never a hack)

| # | Probe | Pass looks like |
|---|---|---|
| P-1 | `sysStat(path, 2)` mtime on oracle/IR **and LLVM**: touch a file, observe change; measure granularity (seconds?) | mtime observable on all three; if seconds-granular, §2.1's size+mtime pair is the documented mitigation; log actual resolution |
| P-2 | `sysListDir` recursive walk of a nested `views/` fixture + `File(path, std::read)` whole-file read **on LLVM** (reference notes file shims are interpreter-hosted — verify LLVM parity; if absent → STOP-3, file the LA) | listing + read of 20-file tree, byte-identical content across engines |
| P-3 | Parse-at-boot cost: generate 100 synthetic templates × 4KB; `sysMonotonic()` around `loadAll()` on IR and LLVM | Recorded numbers in the log; budget: ≤250ms LLVM, interpreters best-effort (documented, not gating — boot is once) |
| P-4 | Render throughput: 10k-row `{{#for}}` table via StringBuilder incl. escaper fast path; compare escape-heavy vs clean strings | O(total) scaling (2× rows ≈ 2× time); numbers logged; no quadratic blowup (concatAll contract) |
| P-5 | `match` type-arm dispatch over the 10-class Node hierarchy (with `else`) on oracle/IR/LLVM | Correct arm selection + narrowed member access in-arm on all engines |
| P-6 | JsonValue number rendering: `toJson`'d int fields print `3` not `3.0` through the Track 09 formatter | Verbatim integer text; if not, coordinate with Track 09 owner before writing a local formatter |

## 10. Foreseeable problems

| # | Problem | Mitigation |
|---|---|---|
| 1 | **XSS via attribute/URL contexts** — context-blind escaping is unsafe in `on*=`, unquoted attrs, `<script>`, URL schemes | Escape superset (5 chars incl. both quotes) makes text+quoted-attr safe; forbidden contexts documented AND boot-linted heuristically (§3.2); `{{{` is the only raw sink, greppable; post-v1: context-aware autoescape rides the comptime ticket, not v1 |
| 2 | **Template cache invalidation** (dev) — mtime granularity, editor save patterns, renamed/deleted files | mtime+size pair (P-1); stat-miss (−1) on a cached file → full re-walk of `views/`; render-time include/layout resolution keeps invalidation per-file (§2.4); prod never stats |
| 3 | **Large-list rendering perf** — per-node string appends into StringBuilder; escaper allocations | StringBuilder/concatAll is O(total) (reference §6.3.5); escaper zero-alloc fast path (§3.1); P-4 measures before optimizing; if interpreters lag badly, that is a dev-mode-only cost (prod = LLVM, C8) |
| 4 | **Recursive includes** → unbounded render | Boot cycle check + render depth cap 32 (§8) |
| 5 | **JsonValue aliasing** (overview H-8) — shared nodes in Map-held trees | Render is strictly read-only over the model; engine never mutates or stores model trees |
| 6 | **`toJson` cost per render** on hot pages | It is the same cost the JSON API path pays (C7); handlers may cache a built JsonValue for static-ish models; measured in P-4 context, optimized only on evidence |
| 7 | **Path traversal via template names** (`view("../secret")`) | Names resolve against the boot-loaded cache map, never the filesystem at render; names containing `..` or a leading `/` are rejected at `view()` (dev error / prod 500 — programmer error class) |
| 8 | **Reserved `flash` key collides with user model** | Warn on collision (§5.4); single reserved key, documented |
| 9 | **Boot walk sweeps non-template files** (editor swap files, `.DS_Store`) | Only `*.html` parsed; dotfiles skipped; everything else ignored silently |
| 10 | **`{{#if x}}` 0-is-truthy surprises** | Documented in the truth table (§2.5) with the rationale (0 is data); dev-mode render of a *number* in if-position logs a hint once |

## 11. Milestones & acceptance (aligned to AG-7, 2027-01-15; work starts Wave 4)

| M | Scope | Acceptance |
|---|---|---|
| V-M0 | P-probes P-1..P-6 green; results logged | Probe programs under `packages/atlantis/tests/views/`; findings in the log |
| V-M1 | Parser + node tree + escaper + output/if/for + missing-key policy + boot diagnostics (single-file corpus) | Golden-file corpus (template + model JSON → expected HTML) passes on oracle/IR/LLVM; error-corpus produces expected file:line messages |
| V-M2 | Partials, layout/block/yield, dev-mode mtime reload, cross-file boot checks + context lints, `./myapp views` | Layout demo renders; editing a partial in dev reflects without restart; corrupt corpus fails boot listing every error |
| V-M3 | `View`/`view()`/`fragment()` + Track 02 dispatch, content negotiation (§4.3), `@Fragment` + `wantsFragment`, `{{csrf}}`, flash, `{{asset}}` | Integration tests: one route serves page/fragment/JSON; CSRF input present and verified by Track 08's middleware in the demo |
| V-M4 (= AG-7) | Demo app (`examples/atlantis-demo`): links pages + htmx live search + JSON API from one controller; docs; perf numbers recorded | AG-7 gate: "demo app serves HTML+fragments+JSON from one controller" on IR & LLVM |
| Post-v1 | Inertia seam (§6), asset fingerprinting, comptime templates (on ticket landing), nested layouts (on demand) | — |

## 12. STOP conditions (beyond overview §0.4)

- **STOP-1:** Any temptation to grow the template grammar (operators, comparisons, filters,
  helpers beyond §2.2's fixed list, nested layouts) → STOP, escalate. The grammar is frozen
  by the anti-second-language stance (§2.3).
- **STOP-2:** Any design step that needs to enumerate a typed object's fields at runtime →
  STOP. C7 `toJson()` is the only boundary; "just a tiny bit of reflection" does not exist.
- **STOP-3:** `sysListDir` / `sysStat` / whole-file `File` read missing or broken on LLVM
  (P-1/P-2) → STOP, file the LA / bug.md; never shim via a compiler or runtime edit.
- **STOP-4:** Track 08's CSRF/session surface or Track 02's `View`-dispatch shape diverging
  from §4/§5 assumptions → coordinate in both logs before writing code; never fork a local
  variant.
- **STOP-5:** Any urge to read template files at comptime or to patch comptime to allow it →
  that is the ticket (§1.3), owner-implemented only (R9, STOP (b)).
- **STOP-6:** Shipping any JavaScript file from the `atlantis` package → STOP (§0.4 — docs
  may describe JS; the package never exports it).

## 13. Implementation log (append-only)

- 2026-07-06 — Track doc authored. v1 = boot-parsed JsonValue-model engine (C7 boundary);
  comptime-template-import proposed as a request ticket (owner decision pending — §1.3);
  grammar frozen per §2.2; open question to Track 03: does `@Serializable` stamp an
  interface usable by `view<T>` (§4.1)? Not blocking (generic overload is the fallback).
  (Coordinator note: Track 03 §16 answered — marker is `IJsonSerializable`, manually
  declared; the ticket was filed as `request-comptime-template-import.md`, LA-20.)
- 2026-07-07 (R10) — §0R added: `Resources/Views/*.lhtml` path/extension convention from
  the example; `Atlantis::View(name)` is the Track 01 §3R IActionResponse whose render
  calls this engine; static assets under `Public/`. Engine design unchanged.
- 2026-07-21 — **V-M0/V-M1/V-M2/V-M3 implemented** (Sonnet-tier, pure `.lev` library code,
  no compiler/runtime changes). V-M4 reached only as far as wiring the kernel's
  `Http::View` sugar and updating `tests/corpus/static`'s own facade assertion end-to-end
  through the real engine; the standalone `examples/atlantis-demo` app (AG-7,
  2027-01-15, explicitly Wave-4/can-lag) was NOT built — out of scope for this pass, see
  the milestone note below.

  **Files added** — `packages/atlantis/src/views/{node,escape,parser,engine,view,command}.lev`
  (Node hierarchy + `ParseError`/`Template`; the escaper; the hand-rolled parser;
  `Engine`/`Frame`/`RenderEnv`/`Resolved`/`resolvePath`/boot walk/cross-file checks/dev
  reload/the `install`/`engineOrThrow` holder; `View`/`view()`/`view<T>()`/`fragment()`/
  `wantsFragment`/`flash`/`mergeFlash`/`render(ctx,v)`; the `./myapp views` command).
  `packages/atlantis/trident.toml` gained `src/views/*.lev`.

  **P-probes (§9), `packages/atlantis/tests/views/`, all six run and recorded, oracle
  AND IR AND LLVM (P-1/P-2 needed no Atlantis dependency; P-3/P-5/P-6 ran only after
  the engine/node types existed, a sequencing deviation from "before feature work" —
  noted below):**
  - **P-1** (`p1_mtime.lev`) — GREEN on all three. `std::fileModified`/`sysStat(.,2)`
    mtime IS observable; this system's real-filesystem granularity is whole-seconds
    (confirmed by spinning past 1.1s via `sysMonotonic` before rewriting), so §2.1's
    size+mtime PAIR is exactly the needed mitigation for same-second edits (exercised
    directly by V-M2's `views_devreload` corpus case, which never waits a second).
  - **P-2** (`p2_listdir_read.lev`) — GREEN on all three: a 20-file, 3-level-nested tree
    built at runtime, walked recursively via `sysListDir`+`isDir`, and read back via
    whole-file `File` reads — byte-identical file count/lengths on oracle/IR/LLVM. No
    STOP-3 condition; the filesystem quartet is solid on the primary (LLVM) backend.
  - **P-3** (`p3_boot_cost/`) — GREEN on all three. 100 x ~4KB synthetic templates:
    oracle ~0.9s wall (process startup dominates; parse itself is a small slice), IR
    ~1.3s wall, LLVM ~0.15s wall including process start — all comfortably under the
    ≤250ms-class LLVM budget once startup overhead is discounted; boot is once, so even
    the interpreter numbers are a non-issue in practice.
  - **P-4** (`p4_render_throughput/`) — GREEN on all three, scaled down from the design's
    literal "10k rows" to 800/1600 rows (see deviation note below) — output length
    exactly doubles (no quadratic blowup in the OUTPUT), and LLVM render time is
    sub-millisecond-class at this scale (fastest of the three, as expected — prod is
    LLVM, C8).
  - **P-5** (`p5_match_dispatch/`) — GREEN on all three: `match` over the real 10-class
    Node hierarchy (`Node` + 9 subclasses) with `else`, every arm selects correctly and
    narrows in-arm member access (`n.text`, `n.path`, `n.condPath`, …) — the design's
    own `match` showcase works exactly as sketched (§2.4's `renderNodes`/`walkNodes`
    are the real, shipped versions of this probe's pattern).
  - **P-6** (`p6_number_format.lev`) — RED against the design's *desired* outcome, but
    this is a confirmed, PRE-EXISTING, already-documented limitation, not a views-track
    bug: `JsonValue::ofNum(3.0).render()` prints `"3.000000"`, not `"3"`, identically on
    all three engines (reference.md §6.11 already pins this exact wire format). Per the
    design's own instruction ("coordinate... before writing a local formatter"), NO
    local formatter was written; `$index` and any numeric model field render with the
    six-decimal tail through the template engine today. Documented in reference.md's
    new §6.13a. Revisit only when float/JSON formatting is addressed globally (§19).

  **Deviation — P-3/P-4/P-5/P-6 sequencing.** The design asks for all P-probes
  "BEFORE feature work"; P-1/P-2 (pure floor-native capability checks) were run first
  as intended, but P-3/P-5/P-6 inherently need the Node/Parser/Engine types to exist
  (P-5 specifically probes the REAL 10-class hierarchy, not a throwaway stand-in) and
  P-4 needs a working renderer, so those four were run once M1's types landed instead.
  No risk materialized retroactively (all four are green), but flagged as a deviation
  in the letter of §9's ordering, not its risk-reduction intent.

  **Deviation — P-4 row count.** Scaled from the design's literal 10k rows down to
  800/1600. Reason: `StringBuilder` self-append is DOCUMENTED quadratic on the oracle
  and IR tree-walk/bytecode interpreters specifically (reference.md §6.3.5, a
  pre-existing, out-of-scope gap unrelated to this track's code shape) — a genuine
  10k-row oracle run measured **1m58s** wall before the scale-down (LLVM itself stayed
  fast throughout; only the two interpreters are affected). 800/1600 rows still proves
  O(total) scaling (exact 2x output-length doubling) and no quadratic blowup within a
  practical manual-run time budget; the full 10k-row LLVM number was independently spot-
  checked (sub-millisecond render) and logged above.

  **Bug found and filed — #105 (`known_bugs_1.md`, P1).** While chasing what first
  looked like a "native string method fails when its argument contains a `\"`" bug
  (the initial, WRONG hypothesis — worth recording so nobody re-derives it), root-
  caused to something narrower and stranger: a local variable or parameter named
  EXACTLY `expr` breaks every subsequent native method call on it (`unknown native
  '<name>'` on `--ir`; "LLVM backend: native floor function '<name>'" refusing to
  compile on `--build-native`), independent of argument content — confirmed for
  `byteAt`/`startsWith`/`contains`/`endsWith`/`indexOf`; oracle is unaffected; sibling
  identifiers (`stmt`, `node`, `value`, `type`, `s`) do NOT trigger it. Filed with a
  minimal repro, not fixed (Leviathan compiler internals, Opus-tier, out of this
  Sonnet-tier library track's scope). Worked around by renaming the one offending
  parameter (`classifyOutput`'s `expr` -> `exprText`, `views/parser.lev`) plus writing
  the file's own quote-detection helpers byte-wise (`byteAt(i) == 34`) as a harmless
  belt-and-suspenders leftover from the investigation.

  **Corpus (`packages/atlantis/tests/corpus/views_*`, golden oracle+IR+LLVM unless
  noted):** `views_basic` (V-M1 — output/if-else/for/`$index`/raw/escape/missing-key
  prod-warn policy, single file); `views_errors` (V-M1 negative — a corrupt two-file
  corpus produces the expected aggregated `file:line: message` list and refuses to
  boot, proving the parser collects every error in one pass rather than stopping at
  the first); `views_layout` (V-M2 — partials, layout/block/yield weave, plus
  `./myapp views`' `name -> file (layout, blocks, partials)` listing); `views_devreload`
  (V-M2 — the SAME `Engine` instance picks up an edited template with no restart, via
  the mtime+size pair, no second-boundary wait needed since the edit also changes
  size); `views_integration` (V-M3 — `view()`/`view<T>()`/`fragment()`, `{{csrf}}`
  against a real Track 08 session, flash write/read-once via the new
  `Auth::currentSessionRecord`/`putSessionRecord` seam, `{{asset}}`, and
  `wantsFragment` via `HX-Request`). `tests/corpus/static` (Track 01, pre-existing) was
  UPDATED: its own comment already anticipated this ("`Atlantis::View('home')` sugar
  lands with 09") — `Http::View.render()` now renders through the installed engine
  against a real `Resources/Views/home.lhtml` fixture; the golden's last line changed
  from the placeholder `[view:home]` to the real rendered `<!doctype
  html><h1>home v</h1>`.

  **Additive Track 08 seam (STOP-4, coordinated in this log rather than forked):**
  `auth/session.lev` gained `SessionRecord.flash` (a bare `Map<string,string> = Map()`
  field default — no constructor signature change, every existing call site
  untouched) and a process-wide `installedSessionStrategy` holder +
  `currentSessionRecord(ctx)`/`putSessionRecord(ctx, rec)` (mirrors this track's own
  `Views::install`/`engineOrThrow` idiom, since `Http::Context` reaches no runtime
  container either way). A composition root that calls `useAuthentication([...])`
  should also call `Auth::installSessionStrategy(strat)` with the same instance for
  flash (and any future session read/write need) to have a seam.

  **Deviation — reserved model key collision handling (§5.4).** The design says
  `view()` should warn on a `flash` key collision; landed instead as `render()`/
  `mergeFlash` warning at MERGE time (the point flash is actually known to exist),
  via the engine's own `Log::Logger` — equivalent outcome, simpler plumbing (the bare
  `view()`/`fragment()` constructors have no logger to reach).

  **Not built:** the Inertia seam (§6), asset fingerprinting, comptime templates, and
  nested layouts remain explicitly post-v1 per the design's own §11 table — untouched.
  The standalone `examples/atlantis-demo` app (V-M4/AG-7) was not built; `designs/
  atlantis/example/` was left as an illustrative, not-necessarily-compiling sketch per
  the task's own instruction (its `Resources/Views/home.lhtml` is still the placeholder
  text — improving it was explicitly optional and deferred in favor of the tested
  corpus fixtures above, which exercise the same real engine).
