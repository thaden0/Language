# Atlantis Track 07 — Interop: MCP, OpenAPI, Service Clients

**Status:** draft for owner review. **Date:** 2026-07-06.
**Depends on:** 01 (kernel/Context/problem+json), 02 (route metadata — doc not yet written;
§5.1 states the exact interface this track needs), 03 (`@Serializable` FromJson/toJson, C7),
04 (`App::run(argv)` command dispatch, R3), Track 09 (JsonValue §1, HttpClient §4.5).
**Owns:** `Atlantis::Mcp`, `Atlantis::OpenApi`, `Atlantis::Client`; files
`packages/atlantis/src/mcp/**`, `src/openapi/**`, `src/client/**` (+ the C5 attribute
declarations `Tool`/`Summary` in `src/mcp/attributes.lev`, declared in namespace `Atlantis`
per C1).
**Gate:** AG-5 — 2026-11-25.

---

## 0. Mission, scope, non-goals

**Mission (the R5 headline):** *every Atlantis app is an MCP server if it wants to be.*
One attribute (`@Tool`) on a method of an ordinary service class produces, at compile time,
a registered MCP tool: name, description, JSON-Schema input schema, and a typed dispatch
adapter — zero runtime reflection, greppable via `--expand`. The **same metadata source**
(attributes + generated schema members + Track 02's route records) feeds an OpenAPI 3.1
document and an `/llms.txt` API map. One generator, several emitters.

**vs Loom (R1):** Loom had **no MCP story, no agent-adjacent story, and no service-client
story at all**. This track is pure differentiation — nothing here is re-derived from Loom
because Loom never attempted it. Where ASP.NET needs Swashbuckle + a separate MCP SDK +
Refit, Atlantis ships all three surfaces from the same attribute vocabulary in the core
`atlantis` package (R8).

**Scope (v1):**
1. MCP server over streamable HTTP: single endpoint (default `/mcp`), POST JSON-RPC 2.0,
   spec revision pinned to `"2025-06-18"`, tools capability only.
2. `@Tool` attribute → compile-time tool registration (rule-generated), typed dispatch,
   schemas from typed params.
3. OpenAPI 3.1 generation from route records + DTO schemas; `./myapp openapi`.
4. Typed service clients: hand-written thin clients over `ApiClient` helpers (the honest v1).
5. `/llms.txt` + problem+json noted as the agent-friendly error surface.

**Non-goals (explicit, R5):** no LLM client, no chains, no agent runtime, no prompt
templates — nothing that *calls* a model. Also out of v1, with seams named: SSE streaming
side of streamable HTTP (§2.6), stdio transport (§2.7), MCP resources/prompts capabilities
(§2.2), structured tool output (`outputSchema`/`structuredContent`, §3.6), attribute-driven
client *generation* (§6.3 — analyzed, ask filed, not designed on), WebSockets.

---

## 1. Architecture: one generator, N emitters

```
  @Serializable structs ──rule──▶ schemaJson() member      (Track 07 rule, §4.1)
  @Tool methods        ──rule──▶ McpTool registrations     (§3)
  @Get/@Post methods   ──rule──▶ RouteRecords              (Track 02; interface §5.1)
  @Summary methods     ──rule──▶ summary registry          (§5.4)
                                      │
                     boot-time assembly (plain code, zero reflection)
                                      │
        ┌─────────────────┬───────────┴────────────┬──────────────┐
        ▼                 ▼                        ▼              ▼
   MCP tools/list    OpenAPI 3.1 doc          /llms.txt      boot validation
   (inputSchema)     (components/schemas)     (markdown)     (fail-fast, R6-style)
```

The **only** schema generator is the `schemaJson()` member (§4.1) plus the shared
type-string mapper `Atlantis::OpenApi::SchemaMap` (§4.2). MCP's `inputSchema` and OpenAPI's
`components/schemas` are two *emitters* over the same values — schema drift is structurally
impossible (and corpus-tested anyway, §9 #1).

**Ownership split with Track 03 (precise, to avoid collision):**
- Track 03 owns `Atlantis::Json`, the `@Serializable` attribute, and the rule(s) generating
  `JsonValue toJson()` + `new FromJson(JsonValue v)` (C7). Files: `src/json/**`.
- Track 07 owns the *additional* rule over `@Serializable` that generates
  `JsonValue schemaJson()` (and `new Empty()`, §4.1). It lives in **`src/openapi/schema.lev`**
  (Track 07's file), declared inside `namespace Atlantis { … }` per C1 so the app's single
  `uses Atlantis;` fires it. Runtime types it references live in `Atlantis::OpenApi`.
- Reserved generated-member names: `schemaJson`, `Empty` are Track 07's; `toJson`,
  `FromJson` are Track 03's. Neither track generates the other's names. (Two rules matching
  the same `@Serializable` attribute must both fire — probe T7-P6.)

---

## 2. MCP server (`Atlantis::Mcp`)

### 2.1 Transport: streamable HTTP, single JSON response per POST

The MCP endpoint is an ordinary C2 handler — **not** a second endpoint model (STOP (c)).
`McpServer.handler()` returns `(Context) => HttpResponse`; the app mounts it at `/mcp` in
the composition root (via Track 02's router or a path-match middleware). HTTP behavior:

| Request | Response |
|---|---|
| `POST /mcp`, JSON-RPC request object | `200`, `application/json`, single JSON-RPC response object |
| `POST /mcp`, JSON-RPC *notification* (no `id`) | `202 Accepted`, empty body |
| `POST /mcp`, unparseable body | `200`, JSON-RPC error `-32700` (id: null) |
| `POST /mcp`, JSON **array** (batch) | `200`, error `-32600` — the 2025-06-18 revision **removed** JSON-RPC batching; we pin that revision and reject batches (§9 #3) |
| `GET /mcp` | `405 Method Not Allowed` — v1 offers no server-initiated SSE stream (allowed by spec) |
| `MCP-Protocol-Version` header present but not `"2025-06-18"` | `400` problem+json |
| other verbs | `405` |

Client `Accept` headers are treated leniently in v1 (we always answer
`application/json`); logged at debug if `text/event-stream`-only.

**Sessions:** v1 is **stateless** — the server does not emit `Mcp-Session-Id` on
`initialize` and ignores one if sent (the spec makes sessions optional; a server that
never issues the header opts out). Documented choice; the seam for stateful sessions is a
`SessionStore` field on `McpServer`, unused in v1.

### 2.2 JSON-RPC methods

| Method | Handling |
|---|---|
| `initialize` | Respond `{protocolVersion: "2025-06-18", capabilities: {tools: {}}, serverInfo: {name, version}}` (name/version from typed config, C6). If the client requests a different `protocolVersion`, respond with ours — client decides whether to proceed (spec behavior). |
| `notifications/initialized` | Notification → `202`, no body, no state kept (stateless). |
| `ping` | `{}` result. |
| `tools/list` | `{tools: [{name, description, inputSchema}]}` — single page, `nextCursor` omitted (v1; pagination seam: accept and ignore `cursor`). |
| `tools/call` | `{name, arguments}` → run adapter → `{content: [{type: "text", text}], isError}` (§3.4). Unknown tool → error `-32602` with `"Unknown tool: <name>"`. |
| anything else | `-32601` method not found. |

Error codes used: `-32700` parse, `-32600` invalid request (incl. batch), `-32601` unknown
method, `-32602` invalid params / unknown tool, `-32603` internal (dispatcher bug only —
tool *execution* failures are `isError: true` results, not JSON-RPC errors, per MCP spec).
`id` is echoed verbatim (string | number | null).

### 2.3 Core types (Leviathan sketch)

```
namespace Atlantis::Mcp {
    struct ParamSpec { string name; string type; }        // compile-time-injected descriptors

    class McpResult { Array<JsonValue> content; bool isError; }   // content items pre-built

    class McpTool {
        string name; string description;
        JsonValue inputSchema;                            // built once at registration
        (JsonValue) => McpResult run;                     // uniform envelope; may await inside
    }

    interface IToolSource { Array<McpTool> mcpTools(); }  // provided by ToolSource base (§3.2)

    class McpServer {
        Array<McpTool> tools;                             // flattened at construction
        new McpServer(Array<IToolSource> sources) { /* flatten, validate: unique names, resolvable schemas — throw at boot otherwise */ }
        (Context) => HttpResponse handler();              // §2.1 table
        JsonValue dispatch(JsonValue rpc);                // testable without HTTP
    }
}
```

### 2.4 Mounting (explicit, greppable — no scanning)

```
// main.lev (composition root)
UserService users   = UserService(inject IDbConnection);
MathService math    = MathService();
McpServer mcp       = McpServer([users, math]);           // tool sources listed by hand
App app = App(pipeline, router);
app.mcp(mcp);                                             // mounts handler at /mcp; registers `mcp` command
app.run(argv);                                            // R3: serve | routes | openapi | mcp | …
```

`./myapp mcp` (Track 04 command dispatch) serves **only** the `/mcp` endpoint (fresh
pipeline: `[AccessLog, ErrorMapper, mcp.handler-as-terminal]`) — same process, same binds,
no other routes. `./myapp serve` includes `/mcp` alongside the app when mounted.

**Auth posture (R6):** `/mcp` participates in `authDefault` like any route. With the
shipped default (`auth`), a remote MCP client must present credentials (Track 08 bearer
tokens are the fit; TLS posture rides LA-2 — until then, remote MCP = behind a
TLS-terminating proxy, same as all HTTP). Local/dev: mark the mount `@NoAuth`-equivalent
via the mount API flag `app.mcp(mcp, open: …)` — no, flags-as-bools are unreadable:
`app.mcpOpen(mcp)` vs `app.mcp(mcp)`. Documented in the demo app.

### 2.5 Dispatch skeleton (the sensitive core — implementation agent ports this shape)

```
JsonValue dispatch(JsonValue rpc) {
    if (rpc.isArray()) { return err(JsonValue::ofNull(), -32600, "batching not supported (2025-06-18)"); }
    JsonValue? idv = rpc.atOrNone("id");                 // None => notification
    string? method = rpc.atOrNone("method")?.asStr();
    if (method == None) { return err(idv ?? JsonValue::ofNull(), -32600, "missing method"); }
    return match (method) {
        "initialize"     => ok(idv, initializeResult());
        "ping"           => ok(idv, JsonValue::ofObject(Map<string, JsonValue>()));
        "tools/list"     => ok(idv, listResult());
        "tools/call"     => callTool(idv, rpc.atOrNone("params"));
        else             => err(idv, -32601, "method not found: ${method}");
    };
}
```
(Notifications short-circuit in the HTTP layer to 202 before `dispatch`; `dispatch` stays
total for tests.)

### 2.6 SSE seam (deferred)

Streamable HTTP allows the server to answer a POST with an SSE stream (progress, server
requests). v1 always answers a single JSON body. The seam: `McpTool.run` already returns
via the uniform envelope; a v2 `runStreaming` variant plus Track 01's
`HttpResponse::ofStream` hook is additive. Nothing in v1's shapes blocks it.

### 2.7 stdio transport (design note only — deferred)

`sysReadLine(fd)` exists on all engines but is **blocking** — a stdio JSON-RPC loop would
starve timers/sockets between requests (db keep-alives, `std::after`). Serial
read→dispatch→write is *correct* for pure-compute tools but wrong the moment a tool awaits
I/O with background state. **Seam named:** an event-loop-integrable stdin watch
(`std::onStdinLine((string) => void)`-shaped, fd-watch under the hood) — belongs beside
LA-10's sys surface; will be raised as a Language Ask when stdio MCP is scheduled. Until
then: `./myapp mcp` (HTTP) is the story; MCP clients that only speak stdio can bridge via
the standard `mcp-remote`-style adapters.

---

## 3. `@Tool`: attribute → rule → registration → round trip

### 3.1 The attribute (C5, frozen)

```
attribute Tool { string name; string description; }       // in namespace Atlantis (C1)
```

Valid on methods of **any** class (services, not just controllers — R5). Positional args
(LA-7): `@Tool("add", "Adds two integers")`.

### 3.2 The v1 parameter contract (what v1 refuses loudly)

MCP tool arguments are **one JSON object** by protocol. v1 embraces that instead of
fighting the metaprogramming layer:

- **0 params** → `inputSchema: {type: "object", properties: {}}`.
- **1 param of a `@Serializable` struct type** (the *command object*) → `inputSchema` is
  that struct's `schemaJson()`. This is the blessed shape.
- **1 param of type `JsonValue`** → escape hatch; `inputSchema: {type: "object"}`.
- **Anything else** (2+ params, primitive param, reference-class param, generics beyond
  the mapper's table) → **boot-time error** from `McpServer`'s constructor, naming the
  class, method, and offending type ("v1 tools take one @Serializable command struct —
  wrap your params"). Boot-fail-fast is the R6-established posture; the descriptors the
  rule injects (`ParamSpec` name+type strings) make the check cheap. Compile-time refusal
  upgrades when the metaprog layer grows a diagnostics hook (not asked yet — low value).

Multi-primitive-param sugar (`@Tool int add(int a, int b)`) is **v1.1**, gated on the
codegen asks in §6.3 (per-param conversion at call position needs statement-position
`$for` (LA-6) + type-name splice). The command-object shape is idiomatic MCP anyway.

Return types: `string`, `int`, `float`, `bool`, `JsonValue`, any type with `toJson()`
(C7), or `Promise<any of those>` (adapter awaits — no coloring).

### 3.3 The rule + adapters

```
// src/mcp/rules.lev — namespace Atlantis (fires under the app's `uses Atlantis;`)
rule mcpTool {
    match @Tool(t) on method m in class C
    inject `this.__mcpTools = this.__mcpTools.push(
        Atlantis::Mcp::makeTool($t.name, $t.description,
            [ $for p in m.params : Atlantis::Mcp::ParamSpec("$p.name", "$p.type") ],
            ($_params) => this.$m($_args)));`
        at bottom of C.constructor
}
```

The `__mcpTools` field comes from inheriting `Atlantis::Mcp::ToolSource` (which implements
`IToolSource`) — MI makes this a one-word cost: `class MathService : ToolSource { … }`.
If the author forgets the base, the injected statement fails to compile with
"no member `__mcpTools`" — loud, and documented verbatim in the error-messages appendix of
the demo app. (A `class C : ToolSource` match constraint would fail *silently* — rejected.)

`makeTool` carries the type work, per arity/shape, **generic bodies duck-typed at
instantiation** (reference §2.5 — the C++-template model):

```
namespace Atlantis::Mcp {
    McpTool makeTool<R>(string n, string d, Array<ParamSpec> ps, () => R fn)
        => McpTool(n, d, OpenApi::emptyObjectSchema(),
                   (JsonValue args) => guard(n, () => content(fn())));

    McpTool makeTool<A, R>(string n, string d, Array<ParamSpec> ps, (A) => R fn)
        => McpTool(n, d, A::Empty().schemaJson(),          // T7-P2: Empty + schemaJson (§4.1)
                   (JsonValue args) => guard(n, () => content(fn(A::FromJson(args)))));  // T7-P1

    // content(): overloads for string/int/float/bool/JsonValue + generic fallback
    McpResult content<R>(R r) => contentText(json::render(r.toJson()));   // duck-typed
    McpResult content(string s) => contentText(s);                        // most-specific wins (T7-P3)
    // Promise-returning fns: await inside the adapter body — no separate overload needed
    // if `content(await …)` unifies; else makeTool overloads on (A) => Promise<R> (T7-P3b).
}
```

**The two load-bearing language behaviors** (both P-probed *before* any feature work, §8):
- **T7-P1:** `A::FromJson(args)` — a labeled-constructor call on a *generic type
  parameter*, resolved at instantiation like any duck-typed generic body. If the checker
  refuses static-shaped member resolution on type params, this is a Language Ask (small:
  monomorphization has the concrete type in hand) — filed immediately, designed-assuming
  (R4). Fallback ladder in §3.5.
- **T7-P2:** `A::Empty()` — a rule-generated no-arg labeled ctor (§4.1) giving an instance
  to call `schemaJson()` on (there is no static-method surface on structs; labeled ctors
  are the static idiom, and `schemaJson` needs *some* receiver).

### 3.4 Error mapping inside `guard`

`guard(name, thunk)`: `try { thunk() } catch`:
- `IValidationException` / malformed-arguments (`FromJson` throw) → `isError: true`,
  text = field errors (agents can self-correct — this is the useful case).
- `IHttpException` → `isError: true`, text = its message (already user-safe by C4 convention).
- any other `IException` → `isError: true`, text = `"tool '<name>' failed"`, full error
  **logged** via Track 01's logger — internals never leak (same posture as ErrorMapper).
Tool failures are **results**, not JSON-RPC errors (MCP spec: the model should see them).

### 3.5 Fallback ladder (if probes fail — no redesign, just less rule)

1. T7-P1+P2 pass → full story: attribute only, everything generated.
2. P1 passes, P2 fails → registration passes schema explicitly *in app code* where the
   type name is statically written: `mcp.tool("add", "…", AddArgs::schemaJson0(), fn)`
   — with `schemaJson0` a rule-generated *labeled-ctor-free* variant… **no**: without an
   instance there is no member call; the P2-fail fallback is a hand-kept schema literal or
   a throwaway instance the app constructs. Ugly, honest, temporary.
3. P1 fails → typed adapters written by hand in the composition root
   (`(JsonValue a) => svc.add(AddArgs::FromJson(a))` — app code names the type
   statically, so nothing exotic is needed), rule still generates name/description/
   descriptors. Language Ask filed; rule upgrades in place when it lands.
4. Always available: `McpServer.addRaw(name, desc, schemaJsonValue, (JsonValue) => McpResult)`.

### 3.6 Full round trip (the doc-example the demo app ships)

```
// app/services/math.lev
uses Atlantis;
@Serializable struct AddArgs { int a; int b; }

class MathService : ToolSource {
    @Tool("add", "Adds two integers and returns the sum")
    int add(AddArgs args) => args.a + args.b;
}
```

`--expand` shows (abridged): in `AddArgs` — `toJson`/`FromJson` (Track 03) +
`schemaJson`/`Empty` (this track, §4.1); in `MathService`'s constructor —

```
this.__mcpTools = this.__mcpTools.push(Atlantis::Mcp::makeTool(
    "add", "Adds two integers and returns the sum",
    [ Atlantis::Mcp::ParamSpec("args", "AddArgs") ],
    (AddArgs args) => this.add(args)));
```

`tools/list` → `{"jsonrpc":"2.0","id":1,"result":{"tools":[{"name":"add",
"description":"Adds two integers and returns the sum","inputSchema":{"type":"object",
"properties":{"a":{"type":"integer"},"b":{"type":"integer"}},"required":["a","b"]}}]}}`

`tools/call` `{"name":"add","arguments":{"a":2,"b":3}}` →
`{"jsonrpc":"2.0","id":2,"result":{"content":[{"type":"text","text":"5"}],"isError":false}}`

v1 note: `structuredContent`/`outputSchema` (2025-06-18 optional) deferred; the seam is a
second field pair on `McpTool` — additive.

---

## 4. Schema generation (`Atlantis::OpenApi`)

### 4.1 The `schemaJson()` rule (Track 07-owned, lives beside Track 03's rules)

```
// src/openapi/schema.lev — namespace Atlantis
rule serializableSchema {
    match @Serializable(s) on struct S
    inject `JsonValue schemaJson() => Atlantis::OpenApi::objectSchema(
        [ $for f in S.fields.where((x) => !x.hasAttr("JsonIgnore"))
              : Atlantis::OpenApi::FieldSpec("$f.name", "$f.type") ]);`
        at member of S
    // second injection, same match:
    inject `new Empty() { }` at member of S                // T7-P2: zero-value fields
}
```

Field names = JSON keys verbatim (C7; renames wait on LA-4). `objectSchema` runs at
runtime-boot over compile-time-injected `FieldSpec`s — zero reflection. `Empty()` relies
on struct fields default-initializing (probe T7-P2; if fields require ctor init, the rule
instead emits `new Empty() { this.f = <zero literal per type>; … }` via per-field
statements — which needs LA-6, so P2's outcome decides between "free" and "ask").

### 4.2 `SchemaMap`: the one type-string → JSON Schema table (both emitters call this)

| Leviathan type (canonical string) | JSON Schema (3.1 / draft 2020-12 — same emit) |
|---|---|
| `int` | `{"type":"integer"}` |
| `float` | `{"type":"number"}` |
| `bool` | `{"type":"boolean"}` |
| `string` | `{"type":"string"}` |
| `T?` | base schema with `"type":[<base>,"null"]`; dropped from `required` |
| `Array<T>` | `{"type":"array","items":<map T>}` |
| `Map<string,T>` | `{"type":"object","additionalProperties":<map T>}` |
| `JsonValue` | `{}` (any) |
| `DateTime` | `{"type":"string","format":"date-time"}` |
| other name | `SchemaRegistry` lookup (§4.3); miss → **boot error** |

Refused loudly (boot error, never silent): reference classes, `Map<K≠string,·>`, unions
other than `T?`, function types, unregistered struct names.

### 4.3 Nested structs: `SchemaRegistry`

A DTO field typed as another struct surfaces as a bare name string in `FieldSpec.type` —
unresolvable at runtime without a registry (runtime string→type lookup would be
reflection; forbidden). v1: the composition root registers nested DTOs explicitly:

```
OpenApi::schemas().register("Address", Address::Empty().schemaJson());
```

Top-level command objects and body DTOs need no registration (their schema is reached via
the typed adapter, §3.3/§5.2). Miss at boot → error listing known keys + the exact
`register` line to add. Key = the canonical spelling `meta` produces for the field type
(probe T7-P5 pins the spelling). MCP inlines nested schemas; OpenAPI emits them as
`components/schemas/<Name>` + `$ref` — same values, two emitters.

### 4.4 JsonValue aliasing (H-8)

Schemas are built once at boot and treated read-only; every emitter **renders** (string)
or builds fresh trees via `ofObject`/`ofArray` — no shared-node mutation. Same discipline
as Track 09 §6#3.

---

## 5. OpenAPI 3.1 (`Atlantis::OpenApi`)

### 5.1 The interface this track needs from Track 02 (doc not yet written — this is the ask)

```
namespace Atlantis::Routing {
    class RouteRecord {
        string verb;             // "GET" …
        string path;             // "/users/:id" (router syntax)
        string controller;       // "UserController"  ($C.name at registration)
        string action;           // "show"            ($m.name)
        Array<ParamSpec> params; // path/query params: name + canonical type string
        string bodyType;         // "" | canonical DTO type name (POST/PUT/PATCH body)
        JsonValue? bodySchema;   // captured at registration via OpenApi helper (typed context — same T7-P1/P2 trick or Track 02's own binding adapter)
        string returnType; JsonValue? returnSchema;   // best-effort; None → default response
        string authMode;         // "auth" | "noauth" | "" (R6 default applies)
        string summary;          // "" here; joined from @Summary registry at boot (§5.4)
    }
    // Router exposes: Array<RouteRecord> records();   — frozen for this track
}
```

If Track 02 lands a different shape, *this* section is updated in the same commit — the
OpenAPI assembler is the only consumer. Until Track 02 exists, M3 tests against a
hand-built `Array<RouteRecord>` fixture (no dependency inversion needed).

### 5.2 Assembly (boot-time, plain code)

`OpenApiDoc::build(records, schemas(), appInfo, authSchemes)`:
- `openapi: "3.1.0"`; `info` from typed config (C6: name, version).
- Paths: router `:id` → OpenAPI `{id}`; group records by path; `operationId` =
  `<controller>.<action>`; `summary` from §5.4.
- Parameters: each path/query `ParamSpec` → `{name, in, required: true (path) / not-optional
  (query), schema: SchemaMap(type)}`.
- `requestBody`: `bodyType != ""` → `$ref: #/components/schemas/<Name>`; schema value from
  `bodySchema` (or registry). `content: application/json` only (v1).
- Responses: `returnSchema` present → `200` with `$ref`; else `200` description-only.
  Every operation also gets `default` → `$ref: #/components/schemas/Problem` (problem+json,
  Track 01) — agents get machine-readable failure shapes everywhere.
- Security: `authSchemes` supplied by Track 08 (bearer + session-cookie scheme names are
  theirs to define); per-operation `security` from `authMode` + the app's `authDefault`
  (R6) — `noauth` routes emit `security: []`.

`./myapp openapi` (R3/Track 04 command): boots the composition root (no listen), builds,
writes `openapi.json` (pretty-rendered) to cwd, exits 0. Also mountable at
`GET /openapi.json` if the app opts in (`app.openApi()`).

### 5.3 Validation duty

M3's corpus includes a spot-check script asserting: document parses, `openapi == 3.1.0`,
every `$ref` resolves, every route in `./myapp routes` appears exactly once. (Full
meta-schema validation is a nice-to-have; noted, not gated.)

### 5.4 `@Summary` (C5) and `/llms.txt`

```
rule opSummary {
    match @Summary(s) on method m in class C : IController
    inject `this.__noteSummary("$m.name", $s.text);` at bottom of C.constructor
}
```

`__noteSummary` accumulates on the `Controller` base (Track 02 owns the base; the
one-field + one-method addition is agreed here — coordination line, not a collision;
alternatively Track 02 exposes it via an `Atlantis::OpenApi::ISummarySource` it inherits).
LA-4 is *why* this is a separate rule: the `@Get` rule cannot read `@Summary`'s args.

`GET /llms.txt` (opt-in mount, `app.llmsTxt()`): markdown built at boot from the same
records — app name/version, one line per route (`- GET /users/{id} — <summary>`), a Tools
section (`- add — Adds two integers…`), a pointer to `/openapi.json` and `/mcp`. Errors
note: all failures are `application/problem+json` (Track 01) — stated in the file so
agents know to parse them.

---

## 6. Typed service clients (`Atlantis::Client`)

### 6.1 The honest v1: hand-written thin clients over helpers

```
namespace Atlantis::Client {
    class ApiException : Exception, IHttpException {      // catch-by-interface, C4
        int status; string code; string detail;           // parsed from problem+json when present
    }
    class ApiClient {
        HttpClient http; string baseUrl; HeaderMap headers; Duration timeout;
        new ApiClient(HttpClient http, string baseUrl) { … }
        void bearer(string token);                        // sets Authorization
        Promise<JsonValue> getJson(string path);
        Promise<JsonValue> postJson(string path, JsonValue body);
        Promise<JsonValue> putJson(string path, JsonValue body);
        Promise<JsonValue> deleteJson(string path);
        // all: baseUrl+path, JSON headers, timeout; non-2xx → throw ApiException
        // (problem+json parsed for code/detail; else status + raw body snippet)
    }
}

// app code — the convention the docs teach:
class UsersClient : ApiClient {
    new UsersClient(HttpClient http, string baseUrl) { /* forward */ }
    Promise<UserDto> get(int id) => UserDto::FromJson(await this.getJson("/users/${id}"));
    Promise<UserDto> create(CreateUser cmd) => UserDto::FromJson(await this.postJson("/users", cmd.toJson()));
}
```

Path params are percent-encoded via Track 09 `encoding` when non-numeric. Retries:
**not** in v1 (retry policy without idempotency awareness is a footgun; roadmap note).

**Production-grade asks (already registered):** LA-9 (HttpClient keep-alive/pooling —
connection-per-request is the honest v1 cost) and LA-11 (DNS — until then, service targets
are IPs/localhost or `/etc/hosts`-style config entries, C6). These two are what turn this
from "correct" into "fast"; nothing in the client surface changes when they land.

### 6.2 DI shape

`bind HttpClient => HttpClient();` once; clients are ordinary injectables:
`bind UsersClient => UsersClient(inject HttpClient, cfg.usersBaseUrl);`. Tests bind a
`FakeUsersClient` — no interface required unless the app wants one (structs-in,
structs-out makes fakes trivial).

### 6.3 The aspiration: `@Get` on interface methods → generated client class (NOT designed here)

Refit-style: `interface IUsersApi { @Get("/users/:id") Promise<UserDto> get(int id); }` →
a generated `UsersApiClient : IUsersApi`. Analysis of what the rule layer lacks:
**(a)** synthesizing a *new class* from an interface match (rules today inject members
into *existing* decls — there is no `emit class` production); **(b)** per-method body
generation = statement-position iteration over methods (LA-6 is fields/statements, this is
decl-position); **(c)** splicing param values into URL templates + per-param conversions
needs type-name splice in type/ctor position (the same capability T7-P1's Language Ask
covers from the generic side). This is a **precise ask, escalated in the final handoff**,
not designed on (per track brief). v1's `ApiClient` conventions are forward-compatible:
generated clients would target the same helpers.

---

## 7. P-probes (run before M2 feature work; failures with metaprog-shaped causes → /bug.md or LA, never hack)

| # | Probe | Proves |
|---|---|---|
| T7-P1 | Generic fn body calls `A::FromJson(v)`; instantiate with a struct having that labeled ctor | static-shaped call on type param (duck-typed generics) — gates §3.3 |
| T7-P2 | Rule injects `new Empty() { }` member into a struct; construct; read fields | injectable labeled ctor + zero-default struct fields — gates schema-via-type-param |
| T7-P3 | `content(string)` + `content<R>(R)`; call with string / with a toJson-bearing struct / with `await promise` arg | most-specific overload beats generic; Promise unification (else split names) |
| T7-P4 | `[ $for p in m.params : F("$p.name", "$p.type") ]` in a method-match rule | `$for` iterates `m.params` (docs show fields; params are the same Array shape) |
| T7-P5 | Splices `"$m.name"`, `"$C.name"`, `"$p.type"` as string values; record canonical spelling of a nested-namespace struct type | name/type strings in templates + the SchemaRegistry key spelling (§4.3) |
| T7-P6 | Two rules matching `@Serializable` on one struct (stub twin of Track 03's + mine) | independent rules stack on one attribute |
| T7-P7 | Three `@Tool` methods in one class; assert `__mcpTools.length() == 3` and declaration order | multiple ctor-bottom injections accumulate, deterministic order |

Each probe: ≤30-line `.lev` program under `packages/atlantis/tests/probes/`, run with
`--expand` + executed on oracle + IR + LLVM (C8).

---

## 8. Foreseeable problems

| # | Problem | Strategy |
|---|---|---|
| 1 | **Schema drift** between MCP inputSchema and OpenAPI components | Structural: one generator (`schemaJson` + `SchemaMap`), two emitters. Plus a corpus test rendering the same DTO through both paths and diffing |
| 2 | **Large tool results** blow up clients/context windows | `maxToolResultBytes` config (default 64KB); truncate with a `"…[truncated N bytes]"` marker, `isError` stays false; SSE/resource-links are the v2 seams |
| 3 | **JSON-RPC batch requests** from older clients | Pinned revision 2025-06-18 *removed* batching → `-32600` with explanatory message naming the revision. Revisit only if a real client demands 2025-03-26 |
| 4 | Nested struct fields unresolvable at boot | `SchemaRegistry` + fail-fast boot error printing the exact `register(...)` line (§4.3) |
| 5 | Silent tool loss (missing `uses Atlantis;` in the service file → rule never fires) | Boot check: `McpServer` with zero tools from a listed source logs a loud warning naming the class; docs lead with the `uses` requirement |
| 6 | JsonValue aliasing corrupting cached schemas (H-8) | Boot-built, read-only, fresh trees per emit (§4.4) |
| 7 | Long-running tools starve the loop (H-3) | Cooperative contract documented on `@Tool`; tools that compute >x ms should be split; LA-1 threads is the structural fix; dev-mode slow-tool log line |
| 8 | Duplicate tool names across sources | Boot error listing both owning classes |
| 9 | `/mcp` under `authDefault=auth` surprises first-time users | §2.4 posture + demo app shows both `mcp` (guarded) and `mcpOpen` (dev) mounts; remote auth = Track 08 bearer, TLS via proxy until LA-2 |
| 10 | Generic overload ambiguity (`content`, `makeTool` w/ Promise returns) | T7-P3 decides; fallback: distinct names (`contentJson`, `makeToolAsync`) — ugly but unambiguous |
| 11 | Spec revision churn (MCP is young) | Version pinned as one comptime const; negotiation isolated in `initializeResult()`; revision bump = one-file change + corpus rerun |
| 12 | Track 02 lands a different RouteRecord | §5.1 is the contract-of-record until 02 writes theirs; assembler is the only consumer; M3 runs on fixtures either way |

---

## 9. Milestones & acceptance (AG-5: 2026-11-25; Wave 3 start after 02's C2 surface)

| M | Scope | Acceptance | Window |
|---|---|---|---|
| M1 | `Atlantis::Mcp` JSON-RPC core + `McpServer` + `addRaw` + `/mcp` handler + `./app mcp` | initialize / initialized / ping / tools/list / tools/call round trip vs a scripted client, hand-registered tools; §2.1 HTTP table exercised (batch→-32600, GET→405, notif→202); oracle+IR+LLVM | Oct 12–23 |
| M2 | Probes T7-P1..P7, then `@Tool` rule + `makeTool` adapters + `schemaJson`/`Empty` rule + `SchemaMap` + boot validation | §3.6 round trip works from attributes alone; `--expand` output matches a hand-written twin; refusal cases error at boot with the documented messages | Oct 26–Nov 6 |
| M3 | OpenAPI assembly + `./app openapi` + `/llms.txt` + `@Summary` rule + security wiring | openapi.json passes §5.3 checks against the demo app (RouteRecord fixtures if 02 not landed); drift test (#1) green | Nov 9–17 |
| M4 | `Atlantis::Client` (`ApiClient`, `ApiException`) + demo `UsersClient` hitting the demo app | client corpus: 2xx path, problem+json → typed exception, timeout → exception; **AG-5 acceptance run**: MCP list+call over HTTP + openapi emit from the same attributes, IR+LLVM | Nov 18–25 |

---

## 10. STOP conditions (per overview §0.4, plus track-specific)

- Any T7 probe fails for a metaprog-shaped reason → STOP: file `/bug.md` or the Language
  Ask (T7-P1's is pre-drafted in §3.3), commit the probe, escalate. Never hack around.
- Any design step that wants runtime string→type lookup or runtime attribute reading →
  STOP — the design is wrong or the data must be compile-time-injected (overview (a)).
- Tempted to auto-discover tool sources (scanning) instead of the explicit `McpServer([…])`
  list → STOP; explicit mounting is a ruling-level ergonomic here.
- Changing C5 shapes (`Tool`/`Summary` fields), C2, C7, or §5.1 after Track 02 consumes it
  → escalation, not edit.
- Compiler/toolchain edits, `.ext` files, second endpoint/middleware model → never (§0.4 b/c/f).
- MCP spec-revision bump or batching support demanded by a client → escalate (owner call:
  compat matrix vs pin).

---

## 11. Implementation log (append-only)

- 2026-07-06 — Track 07 design authored. Key decisions: v1 tool params = one
  @Serializable command object (protocol-idiomatic, metaprog-honest); one schema
  generator/two emitters; stateless streamable-HTTP MCP pinned to 2025-06-18 (no batching,
  no SSE, no sessions — seams named); `schemaJson`/`Empty` rule owned here, beside Track
  03's rules, member names reserved; RouteRecord contract-of-record staked in §5.1 pending
  Track 02; typed clients = conventions over `ApiClient`, generation deferred to a named
  ask. Probes T7-P1..P7 gate M2.
- 2026-07-07 (coordinator) — Track 02 landed after this doc's check: its `RouteRec` is
  canonical (02 owns routing); §5.1 defers to it. The two fields this track needs were
  adopted into RouteRec additively (`bodySchema`, `summary` — overview log 2026-07-07),
  so §5.1's shape survives as the *requirements list*, not the definition. The generic-
  static-members ask was FILED as `designs/request-generic-static-members.md` (LA-18);
  stdin-watch and pause/resume joined LA-23 (small natives batch); the client-generation
  triple is recorded in the overview LA register alongside `request-metaprog-splices.md`.
  Auth security-scheme naming: map Track 08's GuardSpec strings verbatim into OpenAPI
  securitySchemes (session cookie + bearer), per overview ruling.
- 2026-07-19 — **Track 07 implemented in full (M1–M4)**, all four milestones green on
  oracle/IR/LLVM (`packages/atlantis/tests/corpus/{mcp_core,mcp_tool,openapi,client}`,
  probes `tests/probes/mcp_p1..p7_*.lev`). Key deviations from this doc, each logged in
  detail at its call site:
  - **C1 placement, applied**: all new code lives in `Atlantis::Mcp`/`Atlantis::OpenApi`/
    `Atlantis::Client` (the amended C1, not this doc's original flat-`Atlantis` sketch) —
    **except** `schema.lev`'s `serializableSchema` rule, which stays in flat `Atlantis`
    (matching `@Serializable`'s own un-migrated home) because of a real, filed compiler
    bug: a rule cannot match an attribute declared in a different namespace than the rule
    itself, confirmed with a minimal sibling-namespace repro independent of nesting depth
    or direction (**known_bugs_2.md #98**, P1). `@Tool`/`@Summary` are Track 07's own
    attributes, co-declared with their matching rules in the correct C1-amended namespace,
    so they're unaffected.
  - **§3.3's `mcpTool` rule generates METADATA ONLY, not a working `McpTool`.** The
    sketch's typed dispatch closure needs LA-16 (identifier/type-position splicing —
    `$p.type::FromJson` in call-target/type position) — filed, **not landed**. Confirmed
    by three independent probes before writing any rule code (§7's T7-P1, and two more
    beyond the design's own table): (1) a generic type param does not infer from an
    explicitly-typed lambda parameter OR a bound method reference — `A::FromJson(v)`
    itself works fine once `A` is pinned via explicit turbofish (LA-32, landed same day);
    (2) `$p.type` cannot be spliced into a turbofish type-argument position (parse error);
    (3) `$p.type::FromJson(v)` parses as a call-target splice but fails at RUNTIME
    ("cannot resolve call target") — the hole is a string VALUE, never a type/identifier
    splice. Shipped per the design's own **§3.5 fallback ladder, rung 3**: `@Tool` +
    `mcpToolMeta` generates `Array<ToolMeta>` (name/description/param name+type strings —
    everything reachable as a plain string value-hole); the app hand-writes the typed
    closure via the new `ToolSource.registerRun(name, desc, schema, run)`, naming the
    concrete param type statically (`AddArgs::FromJson(a)`, `AddArgs::Empty().schemaJson()`
    — exactly the design's own sketch text). A boot-time `warnUnwiredTools` loudly flags
    any `@Tool` method whose name never got a matching `registerRun` (or `addRaw`) call.
    `content()`/`guard()` (§3.3/§3.4) ARE fully attribute-independent and ship as designed
    (T7-P3 confirmed most-specific-overload-wins + duck-typed generic + `content(await …)`
    unification all work).
  - **§4.1's `$for` holes are BARE, never quoted** — `PSpec($p.name, $p.type)`, not
    `PSpec("$p.name", "$p.type")` as this doc's own §3.3/§4.1 sketches show. A quoted hole
    is a string LITERAL (`"$p.name"` renders as the four characters), not interpolation —
    already found and documented by Track 06's ORM (P2); this doc's sketches predate that
    finding and are simply wrong on this one syntax point, confirmed again by T7-P4/P5.
  - **§4.3 `SchemaRegistry`/`OpenApi::schemas()`** ships as a namespace-level singleton
    (verified legal and byte-identical across oracle/IR/LLVM before use) — the only reach
    available to `schemaJson()`, a fixed-signature generated member with no injection point
    for a registry parameter.
  - **§5.1 `RouteRec` extension**: `bodySchema: JsonValue?`, `summary: string` added to the
    real `RouteRec` (`routing/route_rec.lev`) as the LAST two constructor params (one call
    site in `kernel/facade.lev` + two in Track 02's own `routing.lev` corpus updated). M3's
    corpus tests against a hand-built `Array<RouteRec>` fixture (this doc's own sanctioned
    fallback) rather than a live `Router`, since Era-A's `AddRoute` hardcodes
    `controller = "Route"` (not the real controller class name) — real Era-A integration
    (and therefore live `@Summary` joining through an actual mounted app) is a follow-up
    once Track 02 exposes real controller names through the fluent builder.
  - **§5.4 `@Summary`**: does NOT add a field/method to Track 02's `Controller` base (the
    design's own primary sketch) — uses the design's OWN named alternative instead (a
    plain `Atlantis::OpenApi::noteSummary` free function, no base-class dependency), a
    smaller cross-track footprint. Nested-struct schemas emit INLINE everywhere (not
    `$ref`) — a simplification from §4.3's "MCP inlines, OpenAPI $refs" split emission;
    only the fixed `Problem` schema is `$ref`'d. Logged as reduced v1 scope, not a defect.
  - **§6 `Atlantis::Client`**: `ApiClient` takes `host`/`port`, not a `baseUrl` string — the
    real Track 09 `HttpClient` (`src/Resolver.cpp`) has no raw-URL constructor
    (`request(method,host,port,path,headers,body,cb)`, `fetch(host,port,path)` GET-only;
    URL-string parsing is an explicit, still-open deferral). `getJson`/`postJson`/etc. wrap
    `HttpClient.request`'s callback into a `Promise<JsonValue>` the same way `fetch` itself
    does, confirmed `await`-able directly from a corpus program's top-level `main()`.
    Timeout → exception (M4's acceptance table) is not tested: no request-timeout knob
    exists on `HttpClient` yet.
  - **Compiler findings**: filed `known_bugs_2.md #98` (above). Also hit, and RESOLVED
    WITHOUT filing (stale local build, not a real regression): `build/leviathan` was ~20
    minutes older than the commit that fixed bugs #91/#92 (2-level nested-namespace
    rule/attribute matching) — every probe that looked like a #91 regression, including
    ORM's OWN regression-floor probe, passed clean after a plain rebuild. No compiler code
    was touched by this track (overview §0.4(b)/(h) honored throughout).
  - Full `packages/atlantis/tests/runtests.sh` stays green apart from the pre-existing,
    independently-verified (via `git stash` A/B) `routing (llvm)` failure (bug #95,
    unrelated to this track).
