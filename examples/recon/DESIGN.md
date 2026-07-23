# Recon — Technical Design

**A terminal REST API client (Postman-in-the-terminal), written in Leviathan on the Moby TUI framework.**

**Status:** design, ready to implement. **Date:** 2026-07-14.
**Companion:** `examples/recon/RESEARCH.md` (the research dossier — every language/framework/library
fact this design relies on is verified there; section references like *(R §4.1)* point into it).

> **This is the first end-to-end Leviathan application** (excluding compiler/framework development).
> It is therefore also the **reference standard** for how Leviathan applications are structured,
> named, tested, and packaged. §12 ("Coding standard") is normative: later apps should copy its
> conventions, not re-derive them. Where this document makes a stylistic choice, it makes it *once*
> and applies it everywhere.

---

## 0. Reading guide

| § | What it covers |
|---|---|
| 1 | Product scope, v1 boundary, non-goals |
| 2 | Architecture at a glance (layers, data flow, the async model) |
| 3 | Package layout, manifest, namespaces, file ownership |
| 4 | Domain model (the core `class`es) |
| 5 | Pure engine: URL parser, variable resolver, importer/exporter, JSON path |
| 6 | Network pipeline: auth, header/body assembly, cookies, redirects, timeouts |
| 7 | The script-substitute: native assertions & extractions |
| 8 | Persistence & config |
| 9 | UI architecture: component tree, custom components, dialogs, keymap |
| 10 | Application state & event flow (the run loop integration) |
| 11 | The seven open questions from research — **resolved** |
| 12 | **Coding standard** (normative) |
| 13 | Testing strategy |
| 14 | Implementation tracks, file ownership, timeline |

---

## 1. Product scope

### 1.1 v1 delivers

- **Import & browse** Postman Collections (v2.1 and v2.0) as a navigable folder/request tree, plus
  Postman environment/globals files.
- **Build & send** HTTP/HTTPS requests: method, URL with `{{variable}}` interpolation, query params,
  headers, and bodies (raw / JSON / `x-www-form-urlencoded` / GraphQL / multipart-text).
- **Environments & variables**: a scope chain (local → environment → collection → global) with
  `{{var}}` substitution across URL, headers, body, and auth values; the common dynamic variables
  (`{{$guid}}`, `{{$timestamp}}`, `{{$isoTimestamp}}`, `{{$randomInt}}`).
- **Response inspector**: status + reason + elapsed + size, pretty-printed JSON body, headers table,
  cookies table.
- **Cookie jar & sessions**: capture `Set-Cookie`, re-attach matching cookies (domain/path/expiry/
  secure aware), named sessions each with their own jar + active environment, persisted to disk.
- **Auth**: `noauth`, Basic, Bearer, API-Key (header or query). Digest is a stretch (§6.2).
- **Redirects**: follow 3xx with a cap, method/body rules per status, cross-host auth drop.
- **Timeouts**: per-request + global default, soft (stop waiting) with an Esc hard-cancel.
- **Native test layer** (the JS-script substitute): declarative assertions + variable extractions,
  run after each response, with a pass/fail summary. This is Recon's identity feature (§7).
- **History**: every send logged, persisted, re-openable.
- **Persistence & round-trip**: collections, environments, sessions, history saved as JSON;
  collections/environments can be **exported** back to Postman v2.1 JSON (lossless-ish — §5.4).

### 1.2 v1 boundary (stated honestly, surfaced in-app, never silently dropped)

- **No JavaScript execution.** Postman `event` scripts are imported, preserved verbatim for export,
  and shown read-only. The native test layer (§7) is the substitute. Recon never claims to run
  `pm.test(...)`.
- **Text bodies only.** Binary/file request bodies and binary response downloads are out of scope
  (the HTTP client is text-only, R §1.4). `formdata` `file` parts and `body.mode == "file"` import
  as an explicit **"unsupported (binary)"** marker.
- **No decompression.** Recon always sends `Accept-Encoding: identity` (R §1.5). A response that
  still arrives compressed is shown as raw bytes with a note.
- **Always-verified TLS.** HTTPS uses `HttpClient.requestTls` with full chain + hostname
  verification. A per-request "insecure / custom-CA" path is a stretch goal (§11 Q5).
- **Single run lane: the tree-walk oracle (`trident run`), with the IR interpreter as the second.**
  The compiled lanes are not v1 targets (emit-C++ has no `App.run()`; LLVM segfaults on component
  paint, bug #67). Networking and the loop are engine-clean; it is only the Moby *paint* path that
  pins us to the interpreters (R §7).

### 1.3 Non-goals (v1)

Streaming / SSE / WebSockets; gRPC; connection pooling / keep-alive; a full OAuth2 authorization-code
flow; running imported JS; syntax-highlighted body editing; 256-color themes.

---

## 2. Architecture at a glance

Recon is layered so that **everything except §9 is pure, headless, and unit-testable without a
terminal**. The UI is a thin, replaceable shell over a fully-testable core.

```
                    ┌─────────────────────────────────────────────┐
   terminal ◄──────►│  UI shell (Moby)          — src/ui/*.lev    │   §9
                    │  panels, custom widgets, dialogs, keymap      │
                    └───────────────────┬─────────────────────────┘
                                        │ reads/mutates
                    ┌───────────────────▼─────────────────────────┐
                    │  Application state  (ReconApp / AppState)     │   §10
                    │  active request · last response · history     │
                    │  active session · environments · collections  │
                    └───────────────────┬─────────────────────────┘
        ┌───────────────┬───────────────┼───────────────┬───────────────┐
        ▼               ▼               ▼               ▼               ▼
   ┌─────────┐   ┌────────────┐   ┌──────────┐   ┌───────────┐   ┌───────────┐
   │ Net     │   │ Import/     │   │ Eval     │   │ Store      │   │ Model     │
   │ pipeline│   │ Export      │   │ assert/  │   │ (files,    │   │ (domain   │
   │ §6      │   │ §5.3–5.4    │   │ jsonpath │   │  config)   │   │  types)   │
   │         │   │             │   │ §7       │   │ §8         │   │ §4        │
   └────┬────┘   └─────────────┘   └──────────┘   └───────────┘   └───────────┘
        │ uses std::HttpClient / json / encoding / digest / datetime  (R §4)
        ▼
   the language event loop  (sockets · timers · the Moby run loop all ride it)
```

### 2.1 The async model (decided once, applied everywhere)

**The UI task never `await`s.** All network work is **callback + timer** driven on the single-threaded
event loop that the Moby run loop already rides (R §3.7). A send is kicked off from a key handler,
returns immediately, and its continuation fires later on the same loop:

```lev
// in the request-panel controller — note the EXPLICIT this. everywhere (bug #53)
void onSendChord() {
    PreparedRequest pr = this.state.buildPrepared();   // pure: resolve vars/auth/body/cookies
    this.state.beginInFlight();
    this.invalidate();                                  // show the spinner next frame
    this.sender.send(pr, (RunOutcome o) => this.onOutcome(o));   // continuation, no await
}
```

`RequestSender.send(pr, cb)` (§6.4) is an internal continuation state machine: it issues the request
with the callback-style `HttpClient.request` / `requestTls`, follows redirects by re-issuing, applies
the timeout with a `std::after(ms)` timer plus a **`settled` guard bool**, and finally calls `cb`
exactly once with a `RunOutcome`. No `await`, no `spawn`, no OS threads — so bug #35 and the
"does awaiting park the UI?" question never arise. `await` / `Promise` / `awaitTimeout` remain
available and are used freely in **tests** (which are not UI tasks) and would be the tool if Recon
ever grows a non-UI worker task.

This is the standard: **in a Moby app, background work is callbacks + `App.every`/`std::after`
timers; reserve `await` for headless code.**

---

## 3. Package layout & namespaces

### 3.1 Directory layout

```
examples/recon/
  DESIGN.md                     this document
  RESEARCH.md                   the dossier
  trident.toml                  manifest (§3.2)
  themes/
    recon.toml                  the shipped theme (TOML, never JSON — R §3.3 R10)
  fixtures/                     sample data for tests/demos
    sample.postman_collection.json
    sample.postman_environment.json
  src/
    main.lev                    entry: env read, DI root, launch (§10.4)
    app.lev                     ReconApp, AppState (§10)
    model/
      request.lev               RequestSpec, RequestBody, HeaderEntry, FormField, AuthSpec, Method, BodyMode
      collection.lev            Collection, CollectionItem, ScriptBlock
      environment.lev           Environment, Variable, VarScope
      cookie.lev                Cookie, CookieJar
      session.lev               Session, SessionSet
      history.lev               HistoryEntry, History
      tests.lev                 Assertion, Extraction, TestResult, TestPlan
    net/
      url.lev                   Url + parseUrl (§5.1)
      vars.lev                  VarResolver + dynamic variables (§5.2)
      auth.lev                  effectiveAuth + applyAuth (§6.2)
      body.lev                  buildBody per BodyMode (§6.3)
      sender.lev                RequestSender, RunOutcome, RedirectPolicy (§6.4)
    io/
      jsonio.lev                small JsonValue<->model read/build helpers (§5.5)
      importer.lev              Postman collection/environment import (§5.3)
      exporter.lev              model -> Postman v2.1 JSON (§5.4)
      store.lev                 config dir + load/save sessions/envs/history/settings (§8)
    eval/
      jsonpath.lev              evalJsonPath (§7.2)
      runner.lev                runTests: assertions + extractions (§7.1)
    ui/
      textarea.lev              TextArea custom component (§9.3)
      dialog.lev                Dialog + ConfirmDialog/PromptDialog/FormDialog + openDialog (§9.4)
      commandbar.lev            CommandBar (keyboard command palette, §9.5)
      sources.lev               tree/table/list source adapters (§9.2)
      sidebar.lev               collection sidebar wiring
      requestpanel.lev          request builder panel
      responsepanel.lev         response inspector panel
      statusbar.lev             top/bottom ContentBar wiring
      keymap.lev                chord bindings + command registry (§9.6)
  tests/
    <corpus tests>              *.lev + *.expected (§13)
```

### 3.2 Manifest

```toml
# examples/recon/trident.toml
name    = "recon"
entry   = "main"                       # gather everything, call Recon::main()
sources = ["src/**/*.lev"]             # recursive glob; alphabetical expansion (order irrelevant — §12.2)

[[dep]]
path    = "../../moby"
as      = "Moby"
version = "0.2.0"
```

`entry = "main"` names the free function `Recon::main()` in `src/main.lev` (R §8.1). `**` recursion
is the same glob form the Moby/Atlantis manifests use for `assets`. If a future `trident` rejects
`**` in `sources`, flatten to `src/*.lev` — namespaces are declaration-based, so directory layout is
purely organizational (§12.2).

### 3.3 Namespace policy

- **One namespace: `Recon`.** Every Recon file opens `namespace Recon { ... }`. Files merge; disk
  layout is organizational only (this mirrors Moby's single-`Moby`-namespace pattern). We do **not**
  nest sub-namespaces — a flat `Recon` keeps `::` qualification unnecessary within the app and avoids
  the phantom-dep and qualified-write hazards (§12.2).
- **Imports at the top of `main.lev` and any file that needs them:** `uses Moby;` (the UI files),
  and the prelude `std`/`json`/`encoding`/`digest`/`datetime`/`regex`/`env` namespaces are used
  qualified (`json::parse`, `encoding::base64Encode`) — qualified use is the standard for prelude
  calls so a reader always sees which subsystem a call belongs to (§12.3).
- **Namespace globals are written bare** (`activeApp = a;`, never `Recon::activeApp = a;`) — the
  qualified-write form has a lowering hazard (R §2.8).

---

## 4. Domain model

All domain types are **reference `class`es**, for three reasons that are the standard rule (§12.4):
they have identity, they are mutated across call sites, and several would otherwise be `struct`s
living inside `Array`/`Map` with `enum` fields (bug #41) or as `Map` values (bug #49). The prelude's
own value `struct`s (`Header`, `Pair`, geometry) are used where the research verifies they are safe.

### 4.1 Request model — `src/model/request.lev`

```lev
enum Method  : int { GET, HEAD, POST, PUT, PATCH, DELETE, OPTIONS }
enum BodyMode : int { NoBody, Raw, UrlEncoded, FormData, GraphQL, FileBinary }

class HeaderEntry {            // editable row: enabled flag + preserved order
    public string name  = "";
    public string value = "";
    public bool   enabled = true;
    new HeaderEntry() { }
    new Of(string n, string v) { name = n; value = v; }
}

class FormField {              // urlencoded / formdata row
    public string key   = "";
    public string value = "";
    public string kind  = "text";     // "text" | "file"  (file is import-only, unsupported to send)
    public bool   enabled = true;
    new FormField() { }
    new Of(string k, string v) { key = k; value = v; }
}

class RequestBody {            // a CLASS (has a BodyMode enum field) so #41 never applies
    public BodyMode mode = BodyMode::NoBody;
    public string raw = "";
    public string rawLanguage = "text";        // "json" | "xml" | "text" | ...
    public Array<FormField> urlencoded;
    public Array<FormField> formdata;
    public string graphqlQuery = "";
    public string graphqlVars  = "";
    new RequestBody() { }
}

class AuthSpec {               // kind + a flat string param map (values are primitives -> #49 safe)
    public string kind = "noauth";              // noauth|basic|bearer|apikey|digest|inherit
    public Map<string,string> params;           // e.g. "username","password","token","key","value","in"
    new AuthSpec() { }
    new Of(string k) { kind = k; }
}

class RequestSpec {
    public string          name   = "";
    public Method          method = Method::GET;
    public string          url    = "";         // may contain {{vars}}; the source of truth
    public Array<HeaderEntry> headers;
    public RequestBody     body;
    public AuthSpec?       auth   = None;        // None == inherit from folder/collection
    public TestPlan        tests;               // native assertions/extractions (§7)
    new RequestSpec() { body = RequestBody(); tests = TestPlan(); }
}
```

> `AuthSpec? auth = None` is legal — `T?` over a **class** is fine. (The `((T)=>R)?` bug #51 is about
> optional *function-typed* fields only; we never declare one — §12.6.)

### 4.2 Collection model — `src/model/collection.lev`

```lev
class ScriptBlock {            // preserved verbatim for round-trip export
    public string listen = "";                 // "prerequest" | "test"
    public string source = "";                 // exec[] joined with "\n"
    new ScriptBlock() { }
}

class CollectionItem {         // one node: folder OR request (isFolder discriminates)
    public string name = "";
    public bool   isFolder = false;
    public Array<CollectionItem> children;      // meaningful when isFolder
    public RequestSpec? request = None;         // meaningful when !isFolder
    public AuthSpec?    auth    = None;         // folder/request-level default
    public Array<ScriptBlock> events;           // preserved verbatim
    public JsonValue?   raw     = None;         // original node, for lossless-ish export (§5.4)
    new CollectionItem() { }
    new Folder(string n) { name = n; isFolder = true; }
    new Request(string n) { name = n; request = RequestSpec(); }
}

class Collection {
    public string name = "";
    public string postmanId = "";
    public string schemaVersion = "2.1.0";
    public CollectionItem root;                 // a synthetic folder holding top-level items
    public Array<Variable> variables;           // collection-level vars
    public AuthSpec? auth = None;               // collection-level default auth
    public JsonValue? raw = None;               // original document (export fidelity)
    public string sourcePath = "";              // where it was imported from
    new Collection() { root = CollectionItem::Folder("<root>"); }
}
```

The **folder-vs-request discriminator** follows the Postman rule (R §5.1): a node with an `item`
array is a folder; a node with a `request` is a request. The importer sets `isFolder` accordingly.

### 4.3 Environment & variables — `src/model/environment.lev`

```lev
class Variable {
    public string key = "";
    public string value = "";
    public bool   enabled = true;
    public bool   secret  = false;      // type=="secret" -> masked in the UI
    new Variable() { }
    new Of(string k, string v) { key = k; value = v; }
}

class Environment {
    public string id = "";
    public string name = "";
    public Array<Variable> values;
    new Environment() { }
    new Named(string n) { name = n; }
}

// A resolution layer. VarResolver (§5.2) walks an ordered list of these, most-specific first.
class VarScope {
    public string name = "";                    // "local" | "environment" | "collection" | "global"
    public Map<string,string> values;           // key -> value (only enabled vars, flattened)
    new VarScope() { }
    new Named(string n) { name = n; }
}
```

> **`enabled` vs `disabled` polarity** (R §5.2): environment files use `enabled: true`; collection
> variables use `disabled: false`. The importer normalizes both to `Variable.enabled` at import time
> so downstream code sees one polarity.

### 4.4 Cookies — `src/model/cookie.lev`

```lev
class Cookie {
    public string name = "";
    public string value = "";
    public string domain = "";                  // no leading dot; hostOnly distinguishes
    public string path = "/";
    public bool   secure = false;
    public bool   httpOnly = false;
    public bool   hostOnly = true;              // true = exact host match only
    public bool   session = true;               // no Expires/Max-Age -> session cookie
    public int    expiresEpochMs = 0;           // absolute expiry when !session
    public string sameSite = "";                // informational
    new Cookie() { }
}

class CookieJar {
    public Array<Cookie> cookies;
    new CookieJar() { }
    // capture Set-Cookie lines from a response (§6.5)
    void capture(Array<string> setCookieLines, string reqHost, string reqPath, int nowMs) { ... }
    // build a single "Cookie: n1=v1; n2=v2" value for an outgoing request (§6.5)
    string headerFor(string host, string path, bool isHttps, int nowMs) { ... }
    void prune(int nowMs) { ... }               // drop expired
}
```

Cookies are keyed by **(domain, path, name)**: `capture` replaces a matching triple in place and
deletes on `Max-Age=0` / past `Expires`. `headerFor` collects non-expired cookies whose domain
matches (host == domain when `hostOnly`, else host ends with `.domain`), whose `path` is a prefix of
the request path, and (`secure` ⇒ https), joined `"; "`.

### 4.5 Session, history, tests

```lev
// session.lev
class Session {
    public string name = "default";
    public CookieJar jar;
    public string activeEnvName = "";
    new Session() { jar = CookieJar(); }
    new Named(string n) { jar = CookieJar(); name = n; }
}
class SessionSet {                              // all sessions + which is active
    public Array<Session> sessions;
    public string activeName = "default";
    new SessionSet() { }
}

// history.lev
class HistoryEntry {
    public string method = "GET";
    public string url = "";
    public int    status = 0;
    public int    elapsedMs = 0;
    public int    bodySize = 0;
    public string timestamp = "";               // DateTime::now().iso8601()
    public string requestJson = "";             // serialized RequestSpec snapshot (re-openable)
    public string responsePreview = "";         // first N bytes of the body
    new HistoryEntry() { }
}
class History {
    public Array<HistoryEntry> entries;         // most-recent-first, capped (e.g. 500)
    new History() { }
    void add(HistoryEntry e) { ... }            // prepend + cap
}
```

`tests.lev` (Assertion/Extraction/TestResult/TestPlan) is specified in §7.

---

## 5. Pure engine — URL, variables, import/export, JSON path

Everything in §5 is **pure and headless**: no Moby, no `console` during a running app, deterministic,
unit-tested as print-and-expect corpus programs (§13). These are the highest-value tests.

### 5.1 URL parser — `src/net/url.lev`

The HTTP client does no URL parsing (R §6.1). Recon owns it.

```lev
class Url {
    public string scheme = "http";
    public string host = "";
    public int    port = 80;
    public string path = "/";
    public string query = "";                   // without the leading '?'
    public string fragment = "";                // client-only; never sent
    new Url() { }
    bool isHttps() => scheme == "https";
    string pathWithQuery() => query.isEmpty() ? path : path + "?" + query;  // what HttpClient wants
}

// Total: None on a malformed/relative-without-base URL. A free function (no statics — §12.5).
Url? parseUrl(string raw) { ... }
// Resolve a possibly-relative Location against a base (for redirects, §6.6).
Url? resolveUrl(Url base, string location) { ... }
```

Algorithm (R §6.1): split scheme at `"://"` (default `http` if absent); authority is up to the next
`/`, `?`, or `#`; split host:port at the **last** `:` in the authority (bracket form `[::1]:port` for
IPv6); default port 80/443 by scheme; path from first `/` to `?`/`#` (default `/`); query between `?`
and `#`; fragment after `#`. Variable interpolation happens **before** parsing (§5.2): resolve
`{{vars}}` → concrete string → `parseUrl`.

### 5.2 Variable resolver — `src/net/vars.lev`

```lev
class VarResolver {
    public Array<VarScope> scopes;              // ordered MOST-SPECIFIC FIRST
    new VarResolver() { }
    string resolve(string template) { ... }     // interpolate {{name}} and {{$dynamic}}
    string? lookup(string name) { ... }         // walk scopes; None if unbound
}
```

Interpolation scans for `{{ ... }}` (double braces — the lexer leaves these alone; `${` is the only
sequence needing an escape, R §2.1). For each hole:

1. If the name starts with `$` it is a **dynamic variable**, evaluated fresh:
   - `{{$guid}}` → RFC-4122-shaped id from `encoding::hexEncode(std::sysRandom(16))` (format the 8-4-4-4-12 groups).
   - `{{$timestamp}}` → `(std::sysNow() / 1000).toString()` (epoch seconds).
   - `{{$isoTimestamp}}` → `DateTime::now().iso8601()`.
   - `{{$randomInt}}` → an int in `0..1000` derived from `std::sysRandom`.
   - Any other `{{$...}}` → left literal (honest: we do not fabricate).
2. Else `lookup(name)` walks the scope chain; **unbound → left literal** `{{name}}` (Postman's
   behavior; the UI flags unresolved holes so the user notices).
3. **Recursive resolution with a depth cap of 8**: if a resolved value itself contains `{{...}}`,
   re-resolve, bounded, to break cycles.

`resolve` is applied to URL, every enabled header value, the body, and auth values (R §5.3).

### 5.3 Postman importer — `src/io/importer.lev`

```lev
Collection?  importCollection(JsonValue doc, string sourcePath) { ... }
Environment? importEnvironment(JsonValue doc) { ... }
```

- **Total inputs:** `json::parse` returns `None` on malformed JSON (R §4.2), so callers get `None`,
  never a throw. `atOrNone(key)` (R §4.2) is used for every optional field.
- **Version detection** via `info.schema` substring (`v2.1.0` / `v2.0.0`); **v1 legacy** (a `requests`
  array) is detected and **refused with a clear message**, not half-parsed (R §5.4).
- **URL**: trust `url.raw` as the source of truth; if `url` is a bare string, that string *is* raw
  (v2.0). The structured `host`/`path`/`query` arrays are a fallback only.
- **Body**: mode-tagged (R §5.1). `raw` sets `rawLanguage` from `options.raw.language`; `urlencoded`/
  `formdata` fill `FormField` arrays; `graphql` fills the two graphql strings; `mode == "file"` and
  `formdata` `file` parts import as `FileBinary` / `kind == "file"` and are marked **unsupported to
  send** (surfaced, not dropped).
- **Auth**: `type` + nested `{key,value}` config array → `AuthSpec`. `noauth` disables inherited auth.
- **Events**: `event[].script.exec[]` joined with `\n` into `ScriptBlock`, preserved verbatim.
- **Round-trip fidelity**: the original `JsonValue` node is stored on `CollectionItem.raw` /
  `Collection.raw` so export (§5.4) can re-emit unknown fields.

### 5.4 Exporter — `src/io/exporter.lev`

```lev
JsonValue exportCollection(Collection c) { ... }     // Postman v2.1 shape
JsonValue exportEnvironment(Environment e) { ... }
```

Builds `JsonValue::ofObject(...)` trees and the caller renders with `.renderPretty(2)` (R §4.2).
**Lossless-ish strategy (§11 Q3, decided):** where a `raw` node was preserved, export merges Recon's
edited fields *onto* the raw tree so unknown fields and `event` scripts survive the round trip; where
no raw exists (a request built fresh in Recon), export emits the canonical v2.1 shape.

### 5.5 JSON model helpers — `src/io/jsonio.lev`

Small, reused read/build helpers so no other file hand-rolls `JsonValue` walking:

```lev
string  jStr(JsonValue v, string key, string dflt) { ... }   // object[key].asStr() ?? dflt
bool    jBool(JsonValue v, string key, bool dflt) { ... }
Array<JsonValue> jArr(JsonValue v, string key) { ... }       // [] if absent/not-array
JsonValue jObj(Array<Pair<string,JsonValue>> fields) { ... } // build an object node
string  numClean(float n) { ... }   // trim JSON's "42.000000" -> "42" for display (R §4.2)
```

---

## 6. Network pipeline — `src/net/`

### 6.1 Prepared request

The pure boundary between "model + resolution" and "send". Built by `AppState.buildPrepared()`
(§10.2) so the sender receives something fully concrete.

```lev
class PreparedRequest {
    public string   method = "GET";
    public Url      url;
    public HeaderMap headers;                   // prelude std::HeaderMap
    public string   body = "";
    public int      timeoutMs = 30000;
    public RedirectPolicy redirects;
    new PreparedRequest() { url = Url(); headers = HeaderMap(); redirects = RedirectPolicy(); }
}
```

### 6.2 Auth resolution — `src/net/auth.lev`

```lev
AuthSpec effectiveAuth(RequestSpec req, CollectionItem folder, Collection col) { ... }
void     applyAuth(AuthSpec a, VarResolver vars, HeaderMap hdrs, Url url) { ... }
```

- **Cascade** (R §5.1): request auth → folder auth → collection auth; `kind == "noauth"` disables an
  inherited one; `kind == "inherit"` (or `auth == None`) falls through.
- **applyAuth** resolves `{{vars}}` in the values, then:
  - `basic` → `hdrs.set("Authorization", "Basic " + encoding::base64Encode(user + ":" + pass))`.
  - `bearer` → `hdrs.set("Authorization", "Bearer " + token)`.
  - `apikey` → header (`hdrs.set(keyName, keyVal)`) or query (append to `url.query`) per `in`.
  - `digest` → **stretch** (buildable from `digest::md5` + `hexEncode`, R §4.4); v1 surfaces it as
    "unsupported" if it cannot complete the challenge/response without a prior 401 round.
  - anything else (`oauth2`, `awsv4`, ...) → surfaced as unsupported, not silently skipped.

### 6.3 Body assembly — `src/net/body.lev`

```lev
// returns (bodyString, contentType?) — contentType None means "don't set one"
Pair<string, string?> buildBody(RequestBody b, VarResolver vars) { ... }
```

- `NoBody` → `("", None)`.
- `Raw` → resolved text; content-type from `rawLanguage` (`json` → `application/json`, `xml` →
  `application/xml`, else `text/plain`) unless the user set one explicitly (their header wins).
- `UrlEncoded` → `k=v&k2=v2` with `encoding::percentEncode` on each enabled field;
  `application/x-www-form-urlencoded`.
- `FormData` → a `multipart/form-data` body assembled with a generated boundary (from `sysRandom`
  hex); **text parts only** — a `file` field emits a clear "binary part omitted" placeholder;
  `multipart/form-data; boundary=...`.
- `GraphQL` → `{"query": ..., "variables": ...}` via `JsonValue` build + `render`;
  `application/json`.
- `FileBinary` → not sendable; the send is blocked with a surfaced error.

### 6.4 The sender — `src/net/sender.lev`

```lev
class RedirectPolicy {
    public bool follow = true;
    public int  max = 10;
    public bool dropAuthCrossHost = true;
    new RedirectPolicy() { }
}

class RunOutcome {                              // exactly one of these states
    public bool ok = false;
    public HttpResponse? response = None;       // set when ok
    public int elapsedMs = 0;
    public Array<Url> redirectChain;            // the hops taken
    public bool timedOut = false;
    public bool cancelled = false;
    public string error = "";                   // set on a thrown/verification failure
    new RunOutcome() { }
}

class RequestSender {
    public HttpClient client;
    new RequestSender() { client = HttpClient(); }
    // continuation-style; cb fires exactly once on the event loop (§2.1)
    void send(PreparedRequest pr, (RunOutcome)=>void cb) { ... }
    void cancelActive() { ... }                 // Esc hard-cancel: mark settled + close fd if held
}
```

`send` is a state machine (all continuation callbacks use **explicit `this.`** — bug #53):

1. Record `int t0 = std::sysMonotonic();` and a `settled = false` guard.
2. Arm the timeout: `std::after(pr.timeoutMs)` → if `!settled`, settle with `timedOut = true`.
3. Issue: pick `client.requestTls` iff `pr.url.isHttps()`, else `client.request`, passing
   `pr.method, host, port, pr.url.pathWithQuery(), pr.headers, pr.body, onResp`.
   - The client re-derives `Host`/`Connection`/`Content-Length` and drops any user copies (R §4.1),
     so Recon never sets those.
4. `onResp(resp)`: if `settled` return; compute `elapsed`; **capture `Set-Cookie`** into the active
   jar (§6.5); if `pr.redirects.follow` and status ∈ {301,302,303,307,308} with a `Location` and the
   hop count < `max` → apply the redirect rules (§6.6) and re-issue (recurse); else settle with the
   response.
5. TLS verification failure / unresolved host → the client throws a **loud named `RuntimeException`
   before the first byte** (R §4.1). `send` wraps the issue call in `try/catch (IRuntimeException e)`
   and settles with `error = e.message`.

Only the callback style is used — no `await`, so the UI keeps painting the spinner every frame.

### 6.5 Cookies (capture & attach) — logic on `CookieJar` (§4.4)

- **Capture** after every response: `resp.headers.all("Set-Cookie")` preserves each duplicate line in
  order (R §4.1). Each line: split on `;`; the first `name=value` pair is the cookie; the rest are
  attributes (`Domain`, `Path`, `Expires`, `Max-Age`, `Secure`, `HttpOnly`, `SameSite`). Default
  domain = request host (`hostOnly = true`); default path = the request path's directory. `Expires`
  via `datetime::parseHttpDate` (R §4.6); `Max-Age` via `DateTime::now().plus(Duration::ofSeconds(n))`;
  `Max-Age=0` or a past `Expires` **deletes** the (domain,path,name) triple.
- **Attach** before a request: `jar.headerFor(host, path, isHttps, nowMs)` → one `Cookie:` header
  (§4.4). Added to the `HeaderMap` in `buildPrepared`.

### 6.6 Redirect rules (R §6.4)

- 301/302/303 → re-issue as **GET**, drop the body (303 always; 301/302 by common practice for
  non-GET). 307/308 → **preserve** method and body.
- Resolve `Location` with `resolveUrl(currentUrl, location)` (may be relative).
- Re-attach cookies for the new host; **drop `Authorization` on a cross-host hop** when
  `dropAuthCrossHost` (the safe default).
- Cap at `max`; the full `redirectChain` is surfaced in the response inspector.

### 6.7 Timeouts & cancel (R §6.6)

Soft timeout is the `std::after` timer + `settled` guard in `send` (§6.4). A hard `Esc` cancel calls
`RequestSender.cancelActive()`, which sets `settled` and (if a lower `TcpStream`/fd is held for a
custom path) closes it. v1 uses the `HttpClient` level, so cancel = stop waiting + ignore the late
callback; the socket closes itself when the peer does.

---

## 7. The script substitute — native tests — `src/eval/`

Recon's identity feature. Since Postman JS cannot run (R §1.1), Recon ships a **declarative** layer
covering the 90% case, with its own persisted schema, preserving the original `event` scripts for
export.

### 7.1 Model & runner — `src/model/tests.lev`, `src/eval/runner.lev`

```lev
class Assertion {
    public string source = "status";  // "status" | "header" | "body" | "jsonpath"
    public string arg = "";           // header name, or a JSON path like "$.token"
    public string op = "equals";      // equals | notEquals | contains | matches | exists | inRange | isType
    public string expected = "";      // comparison target ("200", "200..299", "string", a regex, ...)
    new Assertion() { }
}
class Extraction {
    public string source = "jsonpath"; // "jsonpath" | "header" | "regexGroup"
    public string arg = "";            // path / header name / pattern
    public string group = "1";         // for regexGroup
    public string varName = "";        // destination variable
    public string scope = "environment"; // which scope to write
    new Extraction() { }
}
class TestResult {
    public string label = "";
    public bool   passed = false;
    public string detail = "";        // "expected 200, got 404"
    new TestResult() { }
}
class TestPlan {
    public Array<Assertion> assertions;
    public Array<Extraction> extractions;
    new TestPlan() { }
}
```

```lev
// runner.lev — pure over a response + resolver; no UI
Array<TestResult> runTests(TestPlan plan, HttpResponse resp, int elapsedMs, VarResolver vars) { ... }
```

`runTests` evaluates each assertion (status equals/in-range; header present/equals/matches; body
contains/matches; jsonpath equals/exists/type) into a `TestResult`, and applies each extraction
(evaluate the source, write the value into the named scope on the resolver — enabling **chained
requests**: login → extract `$.token` → env var `token` → `{{token}}` in the next request). This is
the direct replacement for `pm.test(...)` and `pm.environment.set(...)`.

### 7.2 JSON path — `src/eval/jsonpath.lev`

```lev
JsonValue? evalJsonPath(JsonValue root, string path) { ... }   // $.a.b[0].c
```

A small, total walker over `JsonValue`: `$` = root, `.key` = object field (`atOrNone`), `[n]` =
array index (bounds-checked → `None`). Returns `None` for any miss/kind-mismatch rather than throwing
— assertions turn `None` into a clean failure detail.

---

## 8. Persistence & config — `src/io/store.lev`

- **Config root**: `env::get("HOME")` + `"/.config/recon/"`. **Read `HOME` in `main()`** and pass the
  resolved root down (bug #68: `env::get` mis-codegens deep in a package on LLVM; reading it at the
  top-level entry sidesteps it and is good practice regardless — §12.7). Create with `std::sysMkdir`.
- **Files** (all JSON, built via `JsonValue` + `renderPretty(2)`):
  - `sessions/<name>.json` — a serialized `CookieJar` + `activeEnvName`.
  - `environments/<name>.json` — serialized `Environment` (also imports Postman env files).
  - `history.json` — the `History` (capped).
  - `settings.json` — theme name, default timeout, follow-redirects default, active session.
  - Imported collections are referenced by their `sourcePath` and re-read on launch; a Recon-edited
    collection is written back via the exporter (§5.4).
- **File I/O uses `using`** for guaranteed close on every exit edge (R §2.5):

```lev
string readAll(string path) {
    using File f = File(path, std::read);
    return f.read(f.size());                    // size() gives the byte length up front (R §4.8)
}
void writeAll(string path, string text) {
    using File f = File(path, std::write);
    f.write(text);
}
```

- **Session cookies vs persistent**: `Cookie.session == true` cookies are optionally cleared on exit
  (a setting); persistent cookies (with expiry) always survive.

---

## 9. UI architecture (Moby) — `src/ui/`

Moby is a **checked package** — full narrowing/`T?`, no prelude caveats (R §3.3 R15). The prelude
footguns still apply to Recon's own prelude-facing code (net/io/eval), but the UI layer enjoys the
clean checker.

### 9.1 Component tree

```
App (FlexLayout Vertical)                                     src/app.lev
 ├─ ContentBar  topBar        (active env · session · global status)   statusbar.lev
 ├─ SplitBox (Horizontal, ratio 28%)                          app.lev
 │   ├─ TreeView sidebar      (ITreeSource over the collection)   sidebar.lev + sources.lev
 │   └─ SplitBox (Vertical, ratio 45%)
 │       ├─ Container requestPanel                             requestpanel.lev
 │       │   ├─ Container   urlRow (method label + Input url + [Send])
 │       │   └─ Tabs        (Params | Headers | Body | Auth | Tests)
 │       │        ├─ TableView paramsTable   (ITableSource)    sources.lev
 │       │        ├─ TableView headersTable
 │       │        ├─ Container bodyPane (mode RadioGroup + TextArea)   textarea.lev
 │       │        ├─ Container authPane (RadioGroup + Inputs)
 │       │        └─ Container testsPane (assertion/extraction lists)
 │       └─ Tabs         responsePanel  (Body | Headers | Cookies | Tests)   responsepanel.lev
 │            ├─ ContentBox + Text  (pretty body)
 │            ├─ TableView headers
 │            ├─ TableView cookies
 │            └─ ListView  testResults (pass/fail)
 └─ ContentBar  bottomBar     (keybind hints · timing · size · Spinner when in-flight)   statusbar.lev
```

All shipped Moby components (`TreeView`, `TableView`, `ListView`, `Tabs`, `SplitBox`, `ContentBox`,
`ContentBar`, `Input`, `Button`, `RadioGroup`, `Spinner`) already work around the paint footguns (R
§7); Recon inherits those fixes by composing them.

### 9.2 Source adapters — `src/ui/sources.lev`

Recon implements the virtualization source interfaces over its model:

- `class ReconTreeSource : ITreeSource` over the `Collection` tree. `TreeNodeId { int id }` maps to a
  flat index registry the source owns; `labelAt` shows folder/request names with a method glyph.
  `on:select(TreeNodeId)` loads the request into the request panel; `on:expand` toggles folders.
- `class ParamTableSource : ITableSource`, `HeaderTableSource`, `CookieTableSource`,
  `RespHeaderTableSource` — each over the relevant `Array<...>` model, columns `TableColumn`
  (a class-shaped row per R §7, safe). Editing is via the row-edit dialog (§9.4); the source only
  reads + `refresh()`es.
- `ArrayListSource` (shipped) wraps `Array<string>` for the history and test-result lists.

### 9.3 `TextArea` — the multi-line editor — `src/ui/textarea.lev`

The single new **leaf** component (there is no `TextBox` in Moby — R §3.2). It is `Focusable +
Scrollable` (a leaf, not a Container, so it paints via `paintContent` and dodges the custom-Container
paint caveat).

```lev
class TextArea : Focusable, Scrollable {
    Array<string> lines;          // model: one string per line
    int curLine = 0;
    int curCol  = 0;              // scalar column (chars(), not bytes — R §2.9)
    bool readOnly = false;

    new TextArea() { lines = [""]; this.registerHandlers(); }

    void setText(string t) { lines = t.split("\n"); this.clampCursor(); this.invalidate(); }
    string text() => lines.joinToString("\n");

    Size contentDesired(Size avail) => Size(avail.w, lines.length());     // sizing hook
    void paintContent(Surface s) { ... }        // draw visible lines with scrollY offset, cursor cell
    Point? cursorPos() => ...;                   // report the terminal cursor when focused

    void registerHandlers() {
        // EXPLICIT this. on every sibling-method call (bug #53); field reads/writes may be bare.
        this.onKey((KeyEvent e) => this.onKeyDown(e));
    }
    void onKeyDown(KeyEvent e) { ... }           // insert/Enter-split/Backspace-join/arrows/Home/End/Page*
    void onPasteText(PasteEvent e) { ... }       // split on "\n", insert
}
```

Cursor math uses `line.chars()` for scalar-correct movement; `length()`/`subStr` stay byte-counted
so index conversions go through `chars()` (R §2.9). Editing rebuilds the affected line string (COW is
fine — the array is small). Handlers register in the constructor with explicit `this.` receivers.
`char`-typed comparisons bind a `char` local first (bug #50): `char nl = '\n'; if (e.ch == nl) ...`.

### 9.4 Dialogs over the overlay stack — `src/ui/dialog.lev`

There is no `Modal` (R §3.2). Recon builds a minimal one over `App.pushOverlay`/`popOverlay` (R §3.7
R13 — the top overlay owns input). Because it is a **custom Container subclass**, it redeclares
`paint()`/`arrange()`/`contentRect()` on the leaf (the framework's own leaves do this — R §7).

```lev
class Dialog : Container, Bordered, Focusable {
    string result = "";
    bool   confirmed = false;
    (Dialog)=>void onClose;       // non-nullable field (avoid ((T)=>R)? — bug #51)
    bool   hasOnClose = false;
    new Dialog(string title) { this.setTitle(title); this.setBorder(BorderStyle::Rounded);
                               this.registerHandlers(); }
    void arrange(Rect r) { ... } void paint(Surface s) { ... } Rect contentRect() { ... }  // R §7
    void registerHandlers() { this.onKeyCapture((KeyEvent e) => this.onDialogKey(e)); }  // Esc/Enter
    void onDialogKey(KeyEvent e) { ... }         // Enter -> confirm+close; Esc -> cancel+close
    void close(bool ok) { ... }                   // pop overlay, fire onClose if hasOnClose
}
class ConfirmDialog : Dialog { ... }              // yes/no
class PromptDialog  : Dialog { ... }              // one labeled Input -> result
class FormDialog    : Dialog { ... }              // N labeled Inputs (env editor, header edit, cookie edit)

void openDialog(App app, Dialog d) { app.pushOverlay(d); app.focus(d); }
```

Dialogs power: environment editor, add/edit header, add/edit query param, edit cookie, save-as,
confirm-delete, new request/folder.

### 9.5 `CommandBar` — the keyboard command palette — `src/ui/commandbar.lev`

There is no `BarMenu`/`Menu` (R §3.2). A keyboard-first TUI wants a command palette instead: a
single-line `Input` overlay (triggered by `:` or `Ctrl-P`) that runs named commands against a
registry — `send`, `save`, `export`, `import`, `new-request`, `switch-env`, `switch-session`,
`toggle-redirects`, `set-timeout`, `theme`, `quit`. It reuses the overlay stack (§9.4) and the command
registry (§9.6).

### 9.6 Keymap & command registry — `src/ui/keymap.lev`

Chords bind through `App.keymap().bind(chord, action)` (R §3.7; no `@Shortcut` attribute exists —
R §3.2). Global chords win at capture over a focused `Input` (R §3.3 R11).

| Chord | Action |
|---|---|
| `^S` / `Enter` (URL focused) | send the active request |
| `^P` / `:` | open the command bar |
| `^O` | import a collection/environment (prompt for path) |
| `^E` | environment editor |
| `^H` | toggle history pane |
| `Tab` / `S-Tab` | focus traversal (FocusRing) |
| `F2` | rename node · `Delete` delete node (with confirm) |
| `Esc` | cancel in-flight send / dismiss overlay |
| `^Q` | quit |

Commands are `()=>void` closures stored in a `Map<string, ...>`-style registry; since method
references have no identity (R §3.3 R12), removal is by the int token `keymap().bind` returns, not by
value.

### 9.7 Theming — `src/ui/theme.lev` + `themes/recon.toml`

One shipped theme in **TOML** (never JSON — R §3.3 R10), plus the four Moby built-ins selectable via
settings/command bar. Bound at the composition root (`bind ITheme => ...`, §10.4). Theme keys are
dotted strings (`"request.url.border"`, `"response.status.ok"`, `"test.pass"`, `"test.fail"`). Secret
variables and masked auth values render with a `"field.secret"` style.

---

## 10. Application state & event flow

### 10.1 `AppState` — the mutable hub — `src/app.lev`

```lev
class AppState {
    public Array<Collection> collections;
    public Array<Environment> environments;
    public string activeEnvName = "";
    public SessionSet sessions;
    public History history;
    public RequestSpec active;            // the request currently being edited
    public CollectionItem? activeNode = None;
    public HttpResponse? lastResponse = None;
    public RunOutcome? lastOutcome = None;
    public Array<TestResult> lastTests;
    public bool inFlight = false;
    public string configRoot = "";        // resolved in main(), injected here (§8)

    new AppState() { active = RequestSpec(); sessions = SessionSet(); history = History(); }

    VarResolver resolver() { ... }        // build the scope chain: env > active collection > global
    PreparedRequest buildPrepared() { ... } // resolve vars/auth/body, attach cookies, choose scheme
    void beginInFlight() { inFlight = true; }
    void onOutcome(RunOutcome o) { ... }  // record response, run tests, capture cookies, append history
}
```

`AppState` is a plain reference `class` — the single source of truth. Widgets read from it and mutate
it; a mutation is followed by `invalidate()` on the affected pane so the next frame repaints (Moby is
retained-mode: mutate-in-place + damage repaint — R §3).

### 10.2 The send flow (end to end)

1. User edits URL/params/headers/body/auth via widgets → the widgets mutate `state.active`.
2. `^S` fires `RequestPanel.onSendChord()` (§2.1) → `state.buildPrepared()`:
   - `resolver().resolve(...)` over URL/headers/body/auth values (§5.2),
   - `parseUrl` the resolved URL (§5.1), choose `request`/`requestTls` by scheme,
   - `effectiveAuth` + `applyAuth` (§6.2), `buildBody` (§6.3),
   - `Accept-Encoding: identity`, `jar.headerFor(...)` cookie header, user headers (enabled only).
3. `sender.send(pr, cb)` runs the redirect/timeout state machine (§6.4); the UI shows the `Spinner`.
4. `cb(outcome)` → `state.onOutcome`:
   - capture `Set-Cookie` into the active session's jar (§6.5),
   - `runTests(active.tests, resp, elapsed, resolver())` (§7) and write extractions back into scopes,
   - append a `HistoryEntry`, persist history + session jar,
   - set `lastResponse`/`lastTests`, `inFlight = false`, and `invalidate()` the response panel +
     bottom bar.

Everything after step 3 happens later on the event loop; the UI never blocked.

### 10.3 Diagnostics during a run

`console.write` is forbidden while the app runs (R §3.3 R16 — it corrupts the screen). All Recon
diagnostics go through `Moby::log(string)` (a 200-entry ring buffer); `MOBY_LOG_STDERR=1` tees it
for headless debugging.

### 10.4 Composition root — `src/main.lev`

```lev
uses Moby;

void main() {
    // env reads at the TOP LEVEL (bug #68), then pass values down.
    string home = env::get("HOME") ?? ".";
    string cfg = home + "/.config/recon";
    std::sysMkdir(cfg);

    Array<string> args = env::args();          // optional: a collection path to open on launch

    // DI composition root (follow the shipped Moby examples' setter/bind mix — R §3.8)
    bind ITheme     => loadTheme(cfg);         // Recon theme or a built-in
    bind IRenderer  => AnsiRenderer();
    bind IInputSource => StdinSource();

    ReconApp recon = ReconApp(cfg, args);      // builds AppState, panels, keymap, loads persisted data
    recon.run();                               // App.run() — blocks until quit(); teardown via using
}
```

`ReconApp` owns the Moby `App`, wires the tree (§9.1), binds the keymap (§9.6), loads persisted
sessions/environments/history/settings, and opens any collection named on the command line.

---

## 11. The seven open questions — resolved

The research (R §9.1) left seven decisions. This design makes them:

1. **Body editor → build `TextArea`** (§9.3). A real multi-line editor is the Postman-like choice and
   the reusable, standard-setting artifact; it is a bounded leaf component, not a rabbit hole.
2. **Script substitute → ship the native declarative test layer in v1** (§7), with its own persisted
   schema, preserving Postman `event` scripts verbatim for export. This is Recon's identity.
3. **Model fidelity → keep the raw `JsonValue` alongside the parsed model** (§4.2, §5.4) for
   lossless-ish export; export merges edits onto the raw tree.
4. **Dialogs → a minimal `Dialog`/overlay layer + a `CommandBar`** (§9.4, §9.5). Keyboard-first;
   in-pane where a dialog would be heavyweight.
5. **Insecure TLS → v1 always verifies** (`HttpClient`). A custom-CA / skip-verify path over
   `std::tlsConnect(..., verifyMode, caFile)` is a **stretch** (a per-request toggle + a custom send
   path); surfaced honestly, not silently insecure.
6. **Binary bodies → out of scope in v1**, surfaced explicitly (§1.2, §5.3, §6.3). Text responses
   only; a binary response shows size + a note.
7. **Streaming/SSE/WebSockets → out of scope**, stated (§1.3).

---

## 12. Coding standard (normative)

This section is the deliverable's most durable output: **how Leviathan applications are written.**
Later apps copy it.

### 12.1 Files, modules, formatting

- One concern per file; files grouped in `src/<area>/`. File names are lowercase, no separators
  matching the primary type where natural (`sender.lev`, `textarea.lev`).
- 4-space indent, no tabs. One statement per line. A method body is one statement (block or `=>`).
- Public API of a file goes near the top; helpers below.

### 12.2 Namespaces

- One app namespace (`Recon`); files reopen and merge it. **Disk layout is organizational only** —
  namespaces are declaration-based, so `sources` glob order never matters for a function-`entry`
  program. Do not nest sub-namespaces without a reason (avoids `::` noise, phantom-dep friction, and
  the qualified-write hazard).
- **Namespace globals are written bare** inside the namespace (`x = v;`), never `NS::x = v;`.

### 12.3 Imports

- `uses Moby;` at the top of UI files; prelude subsystems are called **qualified**
  (`json::parse`, `encoding::percentEncode`, `digest::md5`, `datetime::parseHttpDate`, `std::after`)
  so the subsystem is legible at the call site.
- A file may only `uses`/`use` a namespace from the project or a **direct** dep.

### 12.4 `class` vs `struct`

- **Default to `class`.** Use `struct` only for small, copy-semantics value bundles with no identity
  and no mutation-across-call-sites (and never one holding an `enum` field if it will live in an
  `Array`, nor one used as a `Map` value on a class field).
- Concretely: geometry/config bundles → `struct` (and Moby's are); model rows, trees, jars, state,
  components → `class`.

### 12.5 No statics; construction

- **There is no `static` keyword.** A "static factory" is either a **labeled constructor**
  (`new Of(...)` returning the instance) or a **free function** in the namespace (when it must return
  `T?` or otherwise not an instance — e.g. `parseUrl(string) -> Url?`).
- Construction has no `new` at the call site: `Url u = Url();`, `Url u = Url::Parse(...)` (labeled).

### 12.6 Errors, optionals, no truthiness

- **Total functions return `T?`** and never throw for expected absence (`parseUrl`, `json::parse`,
  `toInt`, `atOrNone`, `evalJsonPath`). Callers **narrow before use** (`if (x != None) { ... }` or
  `x ?? dflt`).
- **Throw only for programmer error / unrecoverable conditions**; catch by the interface contract
  (`catch (IRuntimeException e)`).
- **No truthiness** — conditions are `bool`. Write `if (x != None)`, never `if (x)`.
- **Never declare an optional function-typed field** (`((T)=>R)?` — bug #51). Use a non-nullable
  handler field + a `bool has...` flag (the `Dialog.onClose`/`hasOnClose` pattern, §9.4).

### 12.7 Collections & the map/array rules

- **Map writes use bracket sugar** `m[k] = v` (never `.with`/`.without` — bug #18).
- **Array append** is `a = a.add(x)`; `arr[i] = v` is rebind-sugar (fine when uniquely owned).
- **Never `Map<K, Struct>` as a class field** (bug #49) — use a `class` value or `Map<K,int>` indexing
  parallel arrays.
- **Array rows that carry an `enum` field are `class`es, not `struct`s** (bug #41). (This is why
  `RequestBody`, `CollectionItem`, etc. are classes.)
- Read `env::get(...)` at the top-level `main()` and pass values down (bug #68).

### 12.8 Lambdas & handlers

- **Always call sibling instance methods with an explicit `this.method()`** inside any lambda/handler
  (bug #53 — bare `this` silently drops on IR, segfaults on native/LLVM). Field reads/writes may be
  bare.
- **Bind a `char` local before comparing/passing** (`char nl = '\n';`) — a bare `'x'` in
  argument/ternary position does not retype (bug #50).
- **Bind an indexed callable before calling it** (`var f = arr[i]; f();`) — bug #52.

### 12.9 Moby UI rules

- Custom **leaf** components: subclass `Component`/mixins, override `contentDesired` + `paintContent`,
  register handlers in the constructor with explicit `this.`, call `invalidate()` on state change.
- Custom **Container** subclasses redeclare `paint()`/`arrange()`/`contentRect()` on the leaf (R §7).
- Follow the R-series (R §3.3): detach discipline (R7), leaf-typed fluent chains (R6), base-list order
  core-first (R5), overlay-owns-input (R13), no `console` during a run (R16), dotted theme keys +
  TOML (R10), keymap wins at capture (R11), remove-by-token (R12).
- **Overloaded methods that get overridden stay single-arity per name** (a same-arity overridden
  overload is a compile error — R §2.3).

### 12.10 Async

- UI code never `await`s: **callbacks + `App.every`/`std::after` timers** (§2.1). `await`/`Promise`/
  `awaitTimeout` are for headless code and tests only. Stay single-threaded; no `spawn` in v1.

---

## 13. Testing strategy

House style is **corpus tests**: a program whose stdout matches an `.expected` file, exit 0, run
byte-identical on the **oracle and IR** (LLVM excluded for UI paint per bug #67). Layers:

1. **Pure-logic corpus (highest value, no UI):** URL parser, `resolveUrl`, variable interpolation
   (incl. dynamic vars with a seeded/mocked clock+random where determinism is needed), the Postman
   importer (over the `fixtures/` samples), the exporter round-trip, the JSON-path evaluator, the
   assertion/extraction runner, the cookie jar (capture/attach/expire). These are print-and-expect
   programs and cover the bulk of Recon's correctness.
2. **Networking (hermetic):** use the in-language `HttpServer` (R §8.3) to run a mock server + Recon's
   sender in one event loop — test redirects (chain + cap + cross-host auth drop), timeouts, cookie
   round-trips, chunked decode, auth headers. No real network.
3. **UI (headless):** bind `TestRenderer` (records a cell grid, `snapshot()`/`textOnly()`) instead of
   `AnsiRenderer` and `ScriptedInput` instead of `StdinSource`; drive frames with `App.pumpOnce()`.
   Test focus traversal, the tree/table sources, dialog open/confirm/cancel, the command bar, and the
   `TextArea` editing operations (type/enter/backspace/arrows/paste → assert on `text()` and the
   rendered snapshot).
4. **Determinism:** clock/random are the only nondeterminism; tests inject fixed values (dynamic vars,
   cookie expiry, history timestamps) so output is stable.

A `fixtures/` collection + environment ship with the repo so the importer/exporter/end-to-end tests
are self-contained.

---

## 14. Implementation tracks

Tracks have **disjoint file ownership** so they can proceed in parallel where dependencies allow.
Each names its files, its key deliverable, and its acceptance test. Timeline is indicative
(kickoff **2026-07-14**); a single implementer would do them in dependency order, but the ownership
split lets independent work fan out.

| Track | Owns | Deliverable | Depends on | Acceptance |
|---|---|---|---|---|
| **T0 Skeleton & standard** | `trident.toml`, `src/main.lev` (stub), `themes/recon.toml`, this §12 wired into a README | Package builds & runs an empty `App` on the oracle; DI root; config dir created | — | `trident run` shows an empty framed app; `trident check` clean |
| **T1 Model** | `src/model/*.lev` | All domain classes (§4) compile; JSON (de)serialize helpers (§5.5) | T0 | corpus: build a `RequestSpec`, round-trip it through jsonio |
| **T2 URL + vars** | `src/net/url.lev`, `src/net/vars.lev` | `parseUrl`/`resolveUrl`, `VarResolver` + dynamic vars | T0 | corpus: URL parse table + interpolation cases |
| **T3 Import/Export** | `src/io/importer.lev`, `src/io/exporter.lev` | Postman v2.0/v2.1 import; v2.1 export round-trip | T1, T2 | corpus over `fixtures/`; export re-imports equal |
| **T4 Net pipeline** | `src/net/auth.lev`, `src/net/body.lev`, `src/net/sender.lev`, `PreparedRequest` | auth apply, body assembly, sender (redirect+timeout state machine) | T1, T2 | hermetic `HttpServer`: redirects, timeout, auth headers |
| **T5 Cookies & store** | `src/model/cookie.lev` logic, `src/model/session.lev`, `src/io/store.lev`, `src/model/history.lev` | cookie jar capture/attach/expire; persistence; sessions | T1, T4 | hermetic cookie round-trip; save/load corpus |
| **T6 Eval / tests** | `src/eval/jsonpath.lev`, `src/eval/runner.lev`, `src/model/tests.lev` | JSON path; assertion/extraction runner | T1 | corpus: assertions pass/fail + extraction writes a var |
| **T7 Custom widgets** | `src/ui/textarea.lev`, `src/ui/dialog.lev`, `src/ui/commandbar.lev` | `TextArea`, `Dialog` family, `CommandBar` | T0 | headless: TextArea edit ops; dialog confirm/cancel |
| **T8 Panels & sources** | `src/ui/sources.lev`, `sidebar.lev`, `requestpanel.lev`, `responsepanel.lev`, `statusbar.lev` | source adapters + the four panels wired to `AppState` | T1, T7 | headless snapshot of each panel over a fixture request |
| **T9 App orchestration** | `src/app.lev` (`ReconApp`, `AppState`), `src/ui/keymap.lev`, finalize `main.lev` | state hub, send flow (§10.2), keymap/commands, launch | T4, T5, T6, T8 | headless end-to-end: load fixture → send (mock) → response + tests render |
| **T10 History & export UI** | history pane + export command (wires T3/T5 into the UI) | history browse/re-open; export-collection command | T3, T5, T9 | headless: send → history entry appears → re-open |
| **T11 Polish** | theme finalization, unresolved-variable flagging, in-flight/cancel UX, unsupported-feature surfacing | UX niceties per §1.2/§9.7 | T9 | manual + snapshot review |

**Critical path:** T0 → T1 → {T2, T6, T7} → {T3, T4} → {T5, T8} → T9 → {T10, T11}. T2/T6/T7 can start
as soon as T1 lands; T4 and T3 are parallel; the UI (T7/T8) is parallel to the net/eval work and
converges at T9.

**Milestones:**
- **M1 — headless core (T1–T6):** the entire non-UI product works and is corpus-tested. This is the
  proof that Recon is correct; the UI is then a shell over a trusted core. Target ~2026-07-21.
- **M2 — interactive shell (T7–T9):** the app is usable end-to-end on the oracle/IR lane. Target
  ~2026-07-28.
- **M3 — v1 (T10–T11):** history, export, polish, and the honest surfacing of every §1.2 boundary.
  Target ~2026-07-31.

Every track builds and differential-tests on the **oracle (`trident run`)** as it goes, with the IR
interpreter (`--ir`) as the second lane, and rebuilds before trusting any cross-engine divergence
(stale `build/` binaries have caused false alarms — R §7).

---

*This design, together with `RESEARCH.md`, is self-contained: an implementer needs no other file to
build Recon. Where it sets a convention (§12), that convention is the Leviathan application standard.*
