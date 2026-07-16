# Atlantis Track 01 — Kernel

**Status:** draft for owner review. **Date:** 2026-07-06.
**Depends on:** `techdesign-00-overview.md` (contracts C1/C2/C4/C6/C7/C8, rulings R1–R9,
LA register), `designs/complete/techdesign-09-web-foundations.md` (Track 09: HeaderMap, incremental
parsers, chunked both ways, keep-alive, json, DateTime/Duration, encoding, digest — gate
AG-0), Track 04 DI surface for `bind`/`inject` idioms (no code dependency).
**Owns:** `Atlantis::Http`, `Atlantis::Log` (namespaces + `packages/atlantis/src/kernel/*.lev`,
`src/log/*.lev`). Seeds `Atlantis::Auth::Principal` (§2.3 — ownership transfers to Track 08).
**Consumed by:** every other track. 02 mounts the router as the terminal handler; 03 renders
into `Respond::json`; 07/08 ship middleware in the C2 shape; 09-views streams through §8.

---

## 0. Mission & scope

The kernel is the smallest thing that serves real traffic correctly: the `Context` object,
the one middleware shape and its fold, exception→status mapping with RFC 9457 bodies,
static files, structured logging, health endpoints, limits/timeouts, and streaming. It is
deliberately boring — every headline feature of Atlantis (attribute routing, DI rules,
ORM) sits *on top of* this and none of it leaks *into* this.

**In scope (this doc):**
1. `Context` (C2, frozen) and the pipeline fold (§1–2).
2. Server bootstrap over Track 09's hardened HTTP + the H-1 slip seam (§3).
3. C4 exception declarations + outermost error-mapping middleware, problem+json (§4).
4. Static file middleware: streaming, content types, traversal defense, ETag/304 (§5).
5. `Atlantis::Log`: JSON-lines logger, sinks behind DI, request-log middleware (§6).
6. Health/readiness, body-size limits, timeouts, graceful drain (§7).
7. SSE/chunked streaming + the cooperative-handler contract (§8, hurdle H-3).
8. Worker-readiness rules (§9, request-threads.md).

**Non-goals (owned elsewhere):** routing/dispatch and validation (02 — the kernel's
terminal handler is a plain function; pre-02 it throws `NotFoundException`); JSON model
binding / `@Serializable` (03); DI container conventions, config loading, `App::run(argv)`
command dispatch (04 — the composition-root *sketches* here are illustrations, 04 owns the
blessed form); auth semantics, CORS, CSRF, rate limiting (08 — kernel only carries
`Principal?` on Context); templates (09-views); anything under `src/**` (STOP rule (b)).

**vs Loom (R1, once, up front):** Loom §6 made middleware a named interface
(`interface Middleware { (Request, Handler next) => Response run; }`) and let handlers
return *either* `HttpResponse` *or* `OutStream<string>` "in the same return position".
Atlantis rejects both: middleware are **bare function values** (no wrapper type to
implement, lambdas compose directly, the pipeline is an `Array` you can read top to
bottom), and a handler returns exactly **one type** — `HttpResponse` — which *has* a
streaming body mode (§8). Two return types per route is a second endpoint model (STOP
rule (c)); Loom's "falls out for free" was the tell that it was never designed.

---

## 1. The pipeline (C2): shapes and the fold

The two shapes, verbatim from C2 — these are *function types*, first-class in the language
(`(T1, T2) => R`, reference §2.1):

```
// Handler    = (Context) => HttpResponse
// Middleware = (Context, (Context) => HttpResponse) => HttpResponse
```

There are no type aliases in the language, so these are spelled inline where needed. The
pipeline is an explicit ordered `Array` of middleware folded right-to-left over the
terminal handler, in the composition root, at boot, once.

### 1.1 The fold (kernel-sensitive code — implement exactly this)

Recursive fold, **not** a loop. Rationale: each recursion frame owns its `next` local, so
closure capture is unambiguous; a loop-based fold risks the classic captured-loop-variable
bug if lambda capture is by-reference (P-probe P1 tests both; the recursive form is
correct under either semantics).

```
namespace Atlantis::Http {
    class Pipeline {
        Array<(Context, (Context) => HttpResponse) => HttpResponse> steps;

        new () { this.steps = []; }

        Pipeline use((Context, (Context) => HttpResponse) => HttpResponse m) {
            this.steps = this.steps.add(m);      // pure Array: rebind
            return this;                          // chainable
        }

        (Context) => HttpResponse build((Context) => HttpResponse terminal) =>
            this.foldFrom(0, terminal);

        (Context) => HttpResponse foldFrom(int i, (Context) => HttpResponse terminal) {
            if (i >= this.steps.length()) { return terminal; }
            (Context, (Context) => HttpResponse) => HttpResponse m = this.steps.at(i);
            (Context) => HttpResponse next = this.foldFrom(i + 1, terminal);
            return (Context ctx) => m(ctx, next);
        }
    }
}
```

Properties: `build` is pure — the built handler closes over immutable structure only
(worker-safe, §9); index 0 is outermost (the Array reads in execution order); a middleware
that never calls `next` short-circuits (health, static-file hit, 413); one that calls it
twice is legal but pathological (documented, not prevented — same stance every mainstream
pipeline takes).

### 1.2 Composition-root sketch (illustrative; Track 04 owns the blessed form)

```
uses Atlantis;
use Atlantis::Http::Context as Ctx;

bind ILogSink => ConsoleSink();
Logger log = Logger(inject ILogSink, Log::info);

Pipeline p = Pipeline()
    .use(AccessLog(log).mw())          // §6.3 — observation only, outermost
    .use(ErrorMapper(log).mw())        // §4.3 — C4 mapping; outermost *error-handling* mw
    .use(BodyLimit(1048576).mw())      // §7.2 — 1 MiB default
    .use(Health([]).mw())              // §7.1
    .use(StaticFiles("public", "/").mw());   // §5

(Ctx) => HttpResponse app = p.build((Ctx c) => throw NotFoundException(c.request.path));
Server s = Server(8080, app);
s.start();
```

Kernel middleware are small classes (config + per-pipeline state on the instance —
constructed in the root, worker-local later) exposing `mw()` which returns the C2 lambda:

```
(Context, (Context) => HttpResponse) => HttpResponse mw() =>
    (Context c, (Context) => HttpResponse next) => this.handle(c, next);
```

This is the *idiom*, not a required base class — any bare lambda of the right shape is a
first-class citizen of the pipeline. (vs Loom: no `interface Middleware` to implement.)

---

## 2. `Context` — the frozen C2 shape

### 2.1 Declaration (fields are frozen; methods are kernel-additive)

```
namespace Atlantis::Http {
    class Context {
        HttpRequest request;              // Track 09 type (HeaderMap headers)
        Map<string, string> routeParams;  // filled by the router (Track 02)
        Principal? user;                  // None until auth middleware runs (Track 08)
        Map<string, string> items;        // per-request bag (request id, trace id, …)

        new (HttpRequest req) {
            this.request = req;
            this.routeParams = Map();
            this.user = None;
            this.items = Map();
        }

        // Additive convenience METHODS only — no state beyond the four frozen fields.
        string id() => this.items.at("requestId");        // set by AccessLog (§6.3)
        string? item(string k) => this.items.atOrNone(k);
    }
}
```

Adding a *field* to Context is a C2 contract change (escalation). Anything a middleware
wants to hand downstream goes in `items` (string values; encode ints via `toString`).
Per hurdle H-5, `Context` **is** the request scope — "request-scoped services" take `Context`
as a parameter; there is no runtime container.

### 2.2 One Context per request

The server (§3) constructs a fresh `Context` per parsed request — including per keep-alive
request on one connection. Contexts are never pooled or reused (identity is per-request;
reuse would leak `items` across requests).

### 2.3 The `Principal` seed (coordination, flagged)

C2 types the field `Principal?`; Track 08 owns the real type in `Atlantis::Auth`. So the
kernel can compile in Wave 1, Track 01 ships a **seed file** `src/kernel/auth_seed.lev`:

```
namespace Atlantis::Auth {
    // SEED — ownership transfers to Track 08 on its first commit; kernel never reads members.
    class Principal { string id; string name; Array<string> roles;
        new (string id, string name, Array<string> roles) {
            this.id = id; this.name = name; this.roles = roles; } }
}
```

The kernel only ever assigns `None` and passes the field through. Track 08 replaces the
seed (same name, same namespace) — flagged in the final handoff as their first action.

---

## 3. Server bootstrap and the H-1 seam

### 3.1 Design position

The kernel codes against **Track 09's documented surface** (HeaderMap, incremental
parsers, keep-alive, chunked encoder, handler-throw→500). Track 09 rewrites the prelude
HTTP types *under the same names*, so most kernel code needs no adapter at all. The risk
(hurdle H-1) is a Track 09 slip past AG-0 (2026-08-21) — today's prelude HTTP is
demo-grade (`Map<string,string>` headers, no keep-alive, no chunked).

### 3.2 The seam: one file, `src/kernel/seam.lev`

**Rule: no kernel file except `seam.lev` may touch a prelude HTTP member that Track 09
changes.** The seam is a set of free functions + one class; everything else in the kernel
calls these:

```
namespace Atlantis::Http {
    // -- request side --
    string? reqHeader(HttpRequest r, string name);       // case-insensitive first-value
    Array<string> reqHeaders(HttpRequest r, string name);
    int reqContentLength(HttpRequest r);                 // -1 when absent/unparseable
    // -- response side --
    HttpResponse mkResponse(int status, string body, Array<Pair<string,string>> headers);
    HttpResponse mkStreaming(int status, Array<Pair<string,string>> headers,
                             (ChunkedBody) => void writer);   // §8
    // -- server + clock --
    // class Server (below); int nowMs();  string isoNow();
}
```

- **When Track 09 has landed:** each function is a one-liner over HeaderMap
  (`r.headers.first(name)`), `mkStreaming` binds to Track 09's chunked encoder,
  `Server` wraps the hardened `HttpServer` (keep-alive, 500-path), `nowMs()` is
  `DateTime::now().epochMs`.
- **If Track 09 slips:** the seam file also contains the **stub tier** — a minimal
  request-line+headers parser over `TcpListener`/`TcpStream` (ported shape from today's
  prelude HTTP), `reqHeader` over the demo `Map` with a `toLower` scan, `mkStreaming`
  writing chunked framing directly to the `TcpStream` (the encoder is 6 lines:
  `size.toHex() + "\r\n" + chunk + "\r\n"`, terminal `0\r\n\r\n`), no keep-alive
  (`Connection: close` on every response — correct, slow, honest). Swap = delete the stub
  tier from the one file. Nothing outside `seam.lev` changes.

### 3.3 `Server`

```
class Server {
    int port; (Context) => HttpResponse app; bool draining;
    new (int port, (Context) => HttpResponse app) {
        this.port = port; this.app = app; this.draining = false; }
    void start();     // bind, accept loop; per request: Context(req) -> app(ctx) -> write
    void stop();      // §7.4 graceful drain
}
```

`start()` installs the app as the handler; the *server-level* try/catch (Track 09 M6's
handler-throw→500) remains the last-resort net **below** the kernel's ErrorMapper — three
layers total (§4.4). One `Server` per event loop; `HttpServer(port, workers: N)` is the
LA-1 flip (§9).

### 3.4 A named ask to Track 09 (coordination, not an LA)

C2 promises streaming response bodies; Track 09's doc streams *request* bodies but does
not explicitly expose a **server-side streaming response hook** (headers out immediately,
`Transfer-Encoding: chunked`, then app-driven writes, §8). Kernel needs one of:
`HttpResponse::ofStream(int status, (OutStream<string>) => void)` or a server-level
"take the connection" escape. Filed with the Track 09 owner alongside a **request-parser
body cap knob** (`maxBodyBytes`, §7.2). Interim: the seam's stub tier implements both
directly over `TcpStream` — so the kernel is never blocked, only un-hardened.

---

## 3R. R10 addendum (2026-07-07) — Builder/App facade + `IActionResponse`

Overview ruling R10 (the owner's `designs/atlantis/example/`) adds two conveniences ON TOP
of this track's machinery. Nothing in §1/§2/§3 changes shape — the facade compiles down to
the same fold; the pipeline stays an explicit ordered list.

```
namespace Atlantis {
    class Builder {
        // pre-Build knobs: config source, server limits (§7), log sink — all optional
        App Build();                          // reads Config (C6), constructs App
    }
    class App {
        App use(Middleware mw);               // appends; CALL ORDER IS PIPELINE ORDER
        App useAuthentication();              // named stage = Track 08's authn middleware
        App useAuthorization();               //   " guard/authz middleware (after authn)
        App useStaticFiles();                 // §5, from Public/ (R10 scaffold)
        void AddRoute(Atlantis::Routing::Route r);   // → RouteRec (Track 02 §0R.2)
        void finalize(string authDefault);    // R6 boot checks (delegates to router)
        void serve();                         // fold §1 + listen (H-1 seam)
    }
}
```

The kernel's own ordering invariants (§1: AccessLog outside ErrorMapper; CORS preflight
before auth — Track 08) are enforced at `Build()`/`serve()` time regardless of `useX`
call order for the framework-owned stages; app `use(mw)` middleware slots in call order
between the framework stages. `useX` methods make the common order READABLE; they do not
introduce hidden stages — `./app routes`-style introspection lists the effective pipeline.

**`IActionResponse`** (return-position convenience; the terminal dispatcher renders it):

```
namespace Atlantis::Http {
    interface IActionResponse { HttpResponse render(Context ctx); }
    // Provided implementors: View(name, model) [Track 09 renders], Json(v),
    // Redirect(to), Status(code, message). Atlantis::View('home') is sugar for View.
}
```

Handler return positions now accept `HttpResponse` | `IActionResponse` | a typed
`@Serializable` DTO (serialized per Track 03) — one dispatcher branch, no change to the
C2 function shapes (middleware still sees only `HttpResponse`).

---

## 4. Errors: C4 declarations + the mapping middleware

### 4.1 Declarations (kernel-owned, `src/kernel/errors.lev`) — kernel-sensitive code

```
namespace Atlantis::Http {
    interface IHttpException : IException { int status; }

    struct FieldError { string field; string message; }
    interface IValidationException : IHttpException { Array<FieldError> errors; }

    class HttpException : Exception, IHttpException {
        int status;
        new (int status, string message) {
            Exception::Exception(message);       // base-ctor form: P-probe P2 pins exact syntax
            this.status = status;
        }
    }
    class NotFoundException : HttpException {
        new (string what) { HttpException::HttpException(404, "not found: ${what}"); } }
    class BadRequestException : HttpException {
        new (string why)  { HttpException::HttpException(400, why); } }
    class UnauthorizedException : HttpException {
        new () { HttpException::HttpException(401, "unauthorized"); } }
    class ForbiddenException : HttpException {
        new () { HttpException::HttpException(403, "forbidden"); } }
    class PayloadTooLargeException : HttpException {
        new (int limit) { HttpException::HttpException(413, "body exceeds ${limit} bytes"); } }
    class ValidationException : HttpException, IValidationException {
        Array<FieldError> errors;
        new (Array<FieldError> errors) {
            HttpException::HttpException(422, "validation failed");
            this.errors = errors;
        }
    }
}
```

Any user/framework class becomes status-mapped by implementing `IHttpException` — catch by
contract, not base class (MI earning its keep, C4). Track 02 *throws* `ValidationException`;
it does not redeclare it.

### 4.2 Problem bodies (RFC 9457, built on Track 09 `json::JsonValue`)

```
namespace Atlantis::Http::Respond {
    HttpResponse text(int status, string body);            // text/plain; charset=utf-8
    HttpResponse json(int status, JsonValue v);            // application/json
    HttpResponse problem(int status, string detail, Context c) {
        Map<string, JsonValue> m = Map();
        m["type"] = JsonValue::ofStr("about:blank");
        m["title"] = JsonValue::ofStr(statusText(status));
        m["status"] = JsonValue::ofNum(status.toFloat());
        m["detail"] = JsonValue::ofStr(detail);
        m["instance"] = JsonValue::ofStr(c.request.path);
        m["requestId"] = JsonValue::ofStr(c.items.atOrNone("requestId") ?? "-");
        return mkResponse(status, JsonValue::ofObject(m).render(),
            [Pair::Of("Content-Type", "application/problem+json")]);
    }
    // validationProblem(e, c): problem(422, …) + "errors": [ {field, message}, … ]
}
```

`string statusText(int)` is a kernel free function (`match` over the common codes, else
`"Error"`). Interim before Track 09 M4 (json): a 12-line escaped-string builder inside
`Respond` marked `// INTERIM — delete at M6`; the public signatures don't change.

### 4.3 The error-mapping middleware

```
class ErrorMapper {
    Logger log;
    new (Logger log) { this.log = log; }
    HttpResponse handle(Context c, (Context) => HttpResponse next) {
        try { return next(c); }
        catch (IValidationException e) { return Respond::validationProblem(e, c); }
        catch (IHttpException e)       { return Respond::problem(e.status, e.message, c); }
        catch (IException e) {
            this.log.error("unhandled exception",
                [LogField("requestId", c.items.atOrNone("requestId") ?? "-"),
                 LogField("error", e.toString())]);
            return Respond::problem(500, "internal server error", c);  // NEVER e.message
        }
    }
}
```

Arm order is significance order: most-capable interface first. The 500 arm logs the full
`toString()` (message + type) server-side and returns only the generic detail — internals
never cross the wire. (vs Loom §6: `Response(422, e.toJson())` ad hoc; no pinned body
format, no leak rule. Atlantis pins RFC 9457 and the never-leak invariant.)

### 4.4 Ordering vs the access log (decided)

Default order is `[AccessLog, ErrorMapper, …]`. AccessLog **observes only** — it never
maps an exception to a status, so C4's "the error-mapping middleware owns the mapping"
holds; placing it outside ErrorMapper means it always sees the *final* status (including
mapped 500s) and can record duration for error paths. Defense in depth: AccessLog wraps
`next` in `catch (IException e) { log crash; throw e; }` purely so a bug in ErrorMapper
itself still gets a log line; below everything, Track 09's server-level 500 net keeps the
loop alive.

---

## 5. Static files

`StaticFiles(string root, string prefix)` middleware; GET/HEAD only, everything else →
`next(c)`. Miss (no such file) → `next(c)` (fall-through lets app routes claim the path;
a `strict` flag flips to 404 for dedicated asset mounts).

### 5.1 Path resolution — the traversal defense, spelled out

Order matters; deviations here are CVEs. Given the raw request path:

1. **Prefix gate.** If path does not start with `prefix`, `next(c)`. Strip the prefix.
2. **Decode exactly once.** `encoding::percentDecode(rest)`; `None` (malformed escape) →
   404. Never decode twice: `%252e%252e` decodes to the *literal* filename `%2e%2e`,
   which is a harmless miss, not a dot-dot.
3. **NUL scan.** Strings are byte-clean; scan `byteAt(i) == 0` → 404 (defeats
   `%00`-truncation tricks against any downstream C-ish layer).
4. **Lexical canonicalization** (no filesystem calls, no realpath dependency):
   split on `/`; process segments left-to-right with a stack — `""` and `"."` are
   dropped; `".."` pops the stack, and **popping an empty stack → 404** (an escape
   attempt, by construction); anything else pushes. Result = `stack.joinToString("/")`.
   Post-condition: the canonical path contains no `..` and cannot denote anything above
   the mount root — the join `root + "/" + canonical` is safe *by construction*.
5. **Serve.** `std::fileExists(full)`? No → `next(c)`. Trailing-slash request or the
   empty canonical path → try `index.html` under it. No directory listings, ever.

```
string? canonicalize(string decoded) {
    Array<string> stack = [];
    for (string seg in decoded.split("/")) {
        if (seg == "..") {
            if (stack.isEmpty()) { return None; }
            stack = stack.removeAt(stack.length() - 1);
        } else {
            if (seg != "" && seg != ".") { stack = stack.add(seg); }
        }
    }
    return stack.joinToString("/");
}
```

**Documented ops constraint:** the language has no `lstat`/`realpath`, so a symlink
*inside* the docroot pointing outside it cannot be detected. v1 rule: the docroot must not
contain such symlinks (deploy-time discipline). A `sysStat` symlink/realpath field is
registered as a P2 language ask (§ final notes). (vs Loom §9: "resolves a path and streams
the file" — no traversal algorithm at all. This section *is* the difference between a
design and a wish.)

### 5.2 Content types

`Map<string,string>` built in the constructor, keyed by lowercase extension: html, css,
js, mjs, json, txt, md, xml, svg, png, jpg/jpeg, gif, webp, ico, woff2, wasm, pdf.
Unknown → `application/octet-stream`. Text types get `; charset=utf-8`. User additions via
a constructor overload taking extra pairs.

### 5.3 Conditional GET: ETag + 304

- **Small files** (`size <= 262144`, 256 KiB): read fully; strong
  `ETag: "md5hex"` via `encoding::hexEncode(digest::md5(bytes))` (Track 09 digest —
  cache validation, not security); body served from the buffer (one read, no double I/O).
- **Large files:** weak `ETag: W/"${size}-${mtime}"` from `std::fileSize/fileModified`
  (no read needed) + streamed body (§5.4).
- `If-None-Match`: split on `,`, trim each, compare against the computed ETag (a `*`
  entry matches anything) → `304` with `ETag` + `Cache-Control`, empty body.
- Also emitted: `Last-Modified: DateTime::ofEpochMs(mtime*1000).httpDate()`,
  `Cache-Control` (config, default `public, max-age=3600`). `Range` is explicitly
  deferred (roadmap; needs seek, which waits on Block — honest, unlike Loom's "range
  requests fall out").
- HEAD: identical headers, no body.

### 5.4 Streaming large files (cooperative — H-3)

```
return mkStreaming(200, headers, (ChunkedBody out) => {
    using File f = File(full, std::read);
    string chunk = f.read(65536);
    while (chunk != "") {
        out.write(chunk);
        await nextTick();          // yield the loop between chunks — MANDATORY
        chunk = f.read(65536);
    }
    out.end();
});
```

**Binary-asset risk (flagged loud):** bodies are byte-clean strings (LA-3 fallback) and
`File` binary mode is inert until Block; P-probe P4 verifies `File.read` round-trips all
256 byte values on IR **and LLVM**. If it does not, static files ship text-assets-only and
binary assets STOP on LA-3 — logged, not hacked around.

---

## 6. `Atlantis::Log` — structured JSON-lines

### 6.1 Surface

```
namespace Atlantis::Log {
    struct LogField { string key; string value; }
    interface ILogSink { void write(string line); }       // one complete line, no newline
    class ConsoleSink : ILogSink {
        void write(string line) { console.writeln(line); } }   // single call = interleave-safe

    // levels: int until enum lands (LA-8): 0 debug, 1 info, 2 warn, 3 error
    const int debug = 0; const int info = 1; const int warn = 2; const int error = 3;

    class Logger {
        ILogSink sink; int minLevel;
        new (ILogSink sink, int minLevel) { this.sink = sink; this.minLevel = minLevel; }
        void debug(string msg, Array<LogField> f) { this.emit(0, "debug", msg, f); }
        void info (string msg, Array<LogField> f) { this.emit(1, "info",  msg, f); }
        void warn (string msg, Array<LogField> f) { this.emit(2, "warn",  msg, f); }
        void error(string msg, Array<LogField> f) { this.emit(3, "error", msg, f); }
        void emit(int lvl, string name, string msg, Array<LogField> fields) {
            if (lvl < this.minLevel) { return; }
            Map<string, JsonValue> m = Map();
            m["ts"] = JsonValue::ofStr(isoNow());          // seam clock (§3.2)
            m["level"] = JsonValue::ofStr(name);
            m["msg"] = JsonValue::ofStr(msg);
            for (LogField f in fields) { m[f.key] = JsonValue::ofStr(f.value); }
            this.sink.write(JsonValue::ofObject(m).render());
        }
    }
}
```

One JSON object per line; rendering via `JsonValue` guarantees escaping is never
hand-rolled (fresh tree per line — H-8 aliasing discipline). Field values are strings in
the generic API; the request-log line (§6.3) builds its `JsonValue` directly so `status`
and `durMs` are JSON *numbers*. Sink binding is DI: `bind ILogSink => ConsoleSink();` —
tests bind a `CaptureSink`; a file/buffered sink is a later drop-in behind the same
interface. Log lines are capped at 8 KiB (truncate `msg`/values with `…"[truncated]"`) so
one bad value can't flood the sink.

### 6.2 Request IDs

`AccessLog` holds `int seq` (instance state, per-pipeline → per-worker later) and stamps
`ctx.items["requestId"] = "${bootMs.toHex()}-${this.seq.toHex()}"`, incrementing `seq`.
Unique per process; when LA-1 workers land, a worker index prefixes it (`w0-…`). No global
counter anywhere (§9).

### 6.3 The request-logging middleware

```
class AccessLog {
    Logger log; int seq; int bootMs; int slowMs;      // slowMs: dev-mode slow-handler warning (H-3)
    HttpResponse handle(Context c, (Context) => HttpResponse next) {
        int t0 = nowMs();
        this.seq = this.seq + 1;
        c.items["requestId"] = "${this.bootMs.toHex()}-${this.seq.toHex()}";
        try {
            HttpResponse resp = next(c);
            this.line(c, resp.status, nowMs() - t0);
            return resp;
        } catch (IException e) {           // ErrorMapper bug net only — observe, rethrow
            this.line(c, 500, nowMs() - t0);
            throw e;
        }
    }
    // line(): {"ts","level":"info","msg":"request","requestId",method,path,status:num,durMs:num}
    //         + level "warn" with "slow":true when durMs > slowMs
}
```

(vs Loom §11: "request IDs threaded as a stream context value" — no mechanism given.
Here: `items` on Context, stamped by one middleware, read by all, schema pinned.)

---

## 7. Operational endpoints and guards

### 7.1 Health & readiness (middleware, not routes — works pre-Track-02)

```
interface IReadyCheck { string name(); bool ready(); }    // may await internally (no coloring)

class Health {
    Array<IReadyCheck> checks;
    HttpResponse handle(Context c, (Context) => HttpResponse next) {
        if (c.request.path == "/healthz") { return Respond::text(200, "ok"); }   // liveness: no deps
        if (c.request.path == "/readyz") {
            Array<FieldError> failing = [];   // reuse shape: {name, "not ready"}
            for (IReadyCheck ch in this.checks) {
                if (!ch.ready()) { failing = failing.add(FieldError(ch.name(), "not ready")); } }
            if (failing.isEmpty()) { return Respond::text(200, "ready"); }
            return Respond::text(503, "not ready");        // body lists failing check names
        }
        return next(c);
    }
}
```

Track 05's pool registers an `IReadyCheck` ("mysql: can acquire"); the app adds its own.
During drain (§7.4) `/readyz` returns 503 so load balancers stop routing before the stop.

### 7.2 Request body size limit

`BodyLimit(int maxBytes)` sits directly under ErrorMapper: if
`reqContentLength(c.request) > maxBytes` → `throw PayloadTooLargeException(maxBytes)`
(mapped to 413) **and** the response carries `Connection: close` (the client is
mid-upload; don't reuse the connection). Chunked request bodies have no Content-Length —
the real cap belongs in Track 09's parser (`maxBodyBytes` knob, asked in §3.4); interim,
the seam counts buffered body bytes and aborts the connection at the cap.

### 7.3 Timeouts — what is honestly possible pre-LA-1

On one cooperative loop, **nothing can preempt a compute-bound handler** — a per-request
wall-clock kill switch is impossible until true workers (LA-1). Pretending otherwise is
how frameworks lie. The kernel ships the three real levers:

1. **Slow-client timeouts** — idle/read timers per connection: Track 09 keep-alive
   already owns these (its idle timer + `maxRequestsPerConn`).
2. **Awaited-work timeouts** — the kernel utility, usable around any `Promise`:

```
namespace Atlantis::Http {
    class RaceState { bool settled; new () { this.settled = false; } }

    T? awaitTimeout<T>(Promise<T> work, Duration d) {
        Promise<bool> gate = Promise();
        RaceState s = RaceState();
        Timer t = std::after(d.ms);
        t.subscribe((int tick) => {
            if (!s.settled) { s.settled = true; gate.resolve(false); } });
        work.then((T v) => {
            if (!s.settled) { s.settled = true; t.cancel(); gate.resolve(true); } });
        if (await gate) { return work.get(); }
        return None;                       // timed out; work is NOT cancelled (documented)
    }
}
```

   `None` → the caller throws (e.g. `HttpException(504, "upstream timeout")`). The
   abandoned promise keeps running to completion — cancellation is a future story
   (deferred exactly as Loom §3.4 deferred it; we say so instead of hand-waving).
3. **Deadline propagation** — a `Deadline` middleware stamps
   `c.items["deadlineMs"] = (nowMs() + budget.ms).toString()`; downstream awaits size
   their `awaitTimeout` from `deadlineMs - nowMs()`. Convention, enforced by review.

### 7.4 Graceful shutdown (LA-10 fallback, documented drain)

`Server.stop()`: set `draining = true` (flips `/readyz` to 503), `listener.stop()` (no
new connections), mark live keep-alive connections close-after-current-response (seam
flag; Track 09's per-connection state machine already owns the close path). In-flight
requests complete normally. **The loop then exits on its own when work drains** — no
timers or watched fds remain (kernel discipline: every repeating timer the kernel arms is
cancelled on drain; SSE handlers must cancel their pingers on connection close, §8).
Trigger today: process managers drain at the proxy and then kill — there are no signals
yet; when LA-10 (`sysSpawn` + signals, Track 08 scope) lands, SIGTERM → `stop()` is a
5-line addition to `Server.start()`. No admin shutdown endpoint in v1: `TcpStream` exposes
no peer address, so it cannot even be loopback-gated (peer-address is a new ask, § final).

---

## 8. SSE and streaming responses

### 8.1 One response type, two body modes

A handler always returns `HttpResponse`. `mkStreaming(status, headers, writer)` builds the
streaming mode: the server sends the status line + headers + `Transfer-Encoding: chunked`
immediately, then invokes `writer(ChunkedBody)`.

```
class ChunkedBody : OutStream<string> {      // conforms to C2's "body writes an OutStream<string>"
    void write(string chunk);                 // one HTTP chunk out, immediately
    // (<<) delegates to write — chainable
    void end();                               // terminal 0-chunk; connection may keep-alive after
    void onClose(() => void cb);              // peer went away — stop producing
}
```

The writer callback **may return before the body is done**: `ChunkedBody` stays valid when
captured by timers/subscriptions — that is precisely the SSE shape. Subclassing
`OutStream<string>` is P-probe P5; fallback if prelude typed views aren't subclassable:
`ChunkedBody` is a standalone class with the same members (C2's wording is satisfied by
the `<<`-writable body object; noted as a micro-deviation in the log if taken).

### 8.2 SSE helper

```
class SseStream {
    ChunkedBody out;
    new (ChunkedBody out) { this.out = out; }
    void event(string data) {
        // multi-line data → one "data:" line per \n-segment, then blank line
        for (string ln in data.split("\n")) { this.out.write("data: ${ln}\n"); }
        this.out.write("\n");
    }
    void namedEvent(string name, string data) { this.out.write("event: ${name}\n"); this.event(data); }
    void ping() { this.out.write(": ping\n\n"); }     // comment — keeps proxies awake
    void close() { this.out.end(); }
}

// usage: headers Content-Type: text/event-stream, Cache-Control: no-cache
return mkStreaming(200, sseHeaders(), (ChunkedBody out) => {
    SseStream sse = SseStream(out);
    Timer t = std::every(1000);
    t.subscribe((int n) => { sse.event("tick ${n}"); });
    out.onClose(() => { t.cancel(); });               // MANDATORY — else the loop never drains
});
```

### 8.3 THE COOPERATIVE-HANDLER CONTRACT (H-3 — read this twice)

The runtime is a single-threaded event loop. **A handler that computes without yielding
stalls every other request on the loop.** The kernel's rules, stated here and in every
piece of user-facing doc:

1. All I/O through promises/streams — `await` them; never spin-wait.
2. Long CPU work does not belong in a handler (pre-LA-1 there is nowhere to put it —
   this is exactly what true workers fix; the pipeline needs zero changes then).
3. Streaming producers chunk ≤ 64 KiB and `await nextTick()` between chunks (§5.4).
4. Every timer/subscription created for a connection is cancelled on `onClose` —
   otherwise drain (§7.4) never completes and the process never exits.
5. Dev aid: AccessLog's `slowMs` warning (§6.3) names the offending route.

`nextTick` — the kernel yield primitive (3 lines, over the event loop timer):

```
Promise<int> nextTick() {
    Promise<int> p = Promise();
    std::after(0).subscribe((int t) => { p.resolve(t); });
    return p;
}
```

### 8.4 Handlers may await (the shape that makes C2 work)

`Handler = (Context) => HttpResponse` is not `Promise<HttpResponse>` **because `await` is
legal anywhere** — a handler awaits a DB call and the loop keeps serving other requests
while it is parked. This is the no-coloring collapse doing real work, and it is the
kernel's single riskiest runtime assumption → P-probe P3 verifies overlap behavior on IR
and LLVM *before* M1. If parked callbacks do not multiplex, C2 itself is wrong and this
track STOPs (escalation, not improvisation).

---

## 9. Worker-readiness (request-threads.md)

Rules enforced across the kernel, so LA-1 adoption is `HttpServer(port, workers: N)` —
a config flip, not a rewrite:

1. **No ambient mutable state.** No namespace-level `var` anywhere in `Atlantis::Http` or
   `Atlantis::Log` (namespace `const`s are fine). Grep-auditable; the corpus includes a
   `grep -n "^\s*var" src/kernel src/log` check note.
2. **Per-request state lives on `Context`** — nowhere else (H-5).
3. **Per-pipeline state lives on middleware instances** built in the composition root;
   under workers, each worker builds its own pipeline from the same pure config → zero
   shared mutable objects.
4. **Sinks write one line per call** (§6.1) — interleave-safe when N workers share stdout.
5. Planned flip: N event loops each accepting on a shared port (SO_REUSEPORT per
   request-threads.md R-5); request ids gain a worker prefix; nothing else changes.

---

## 10. P-probes (run BEFORE implementing; `build/leviathan --run`, `--ir`, LLVM lane)

| # | Probe program | Verifies | On failure |
|---|---|---|---|
| P1 | Store `(int) => int` in a var, a class field, and an `Array<(int) => int>`; call stored values; build a 3-step fold both recursively and via a while-loop; each "middleware" appends its tag to a string — expect `"a(b(c(term)))"` ordering from both | Function types are first-class in all three positions (the Pipeline class depends on it); loop-capture semantics | STOP — C2's fold is unimplementable as designed; escalate (likely a bug.md or an LA) |
| P2 | Declare `IHttpException : IException { int status; }`, `HttpException : Exception, IHttpException` with base-ctor call, two subclasses; throw/catch by `IValidationException` → `IHttpException` → `IException` order; rethrow from a catch arm | Interface fields on exceptions, base-ctor syntax (`Base::Base(args)` exact form), arm selection, rethrow | Adjust §4.1 syntax to what the probe pins; semantic failure → bug.md |
| P3 | `HttpServer` handler that `await`s a 200 ms timer-promise; loopback client fires two overlapping requests; assert both complete and total wall time ≈ 200 ms, not 400 | Await-anywhere multiplexes inside server callbacks (§8.4) — the C2 keystone | STOP — escalate to owner; C2 may need `Promise<HttpResponse>` (contract change) |
| P4 | Write a 256-byte file containing every byte value via `sysWrite`; read it back with `File.read(65536)`; compare `byteAt` for all i — on IR **and** LLVM. Also: `std::fileExists` on a directory path | Byte-clean binary file round-trip (static assets, §5.4); dir-detection behavior | Static files ship text-only; binary waits on LA-3 (logged); fileExists-on-dir result shapes §5.1 step 5 |
| P5 | Subclass `OutStream<string>` overriding `<<`; loopback TcpStream: handler returns, then a `std::every` timer writes chunked frames; client asserts framing; cancel on close, assert loop exits (+0B discipline) | `ChunkedBody : OutStream<string>` viability; write-after-return (SSE shape); drain (§7.4) | Standalone ChunkedBody fallback (§8.1); write-after-return failure → redesign §8 with Track 09 owner |
| P6 | `awaitTimeout` race both ways (work first / timer first), 200 iterations, assert `settled` guard holds and no double-resolve throw; `t.cancel()` after a one-shot already fired | §7.3 utility correctness; Timer.cancel-after-fire semantics | Guard rework; cancel-after-fire throw → wrap in settled check (already present) or bug.md |
| P7 | Traversal fixture table through §5.1 canonicalize: `/a/../../x` → None, `/%2e%2e/x` → None (after decode), `/%252e%252e/x` → literal-miss, `/a/./b//c` → `a/b/c`, NUL byte → None, `..%2fx` → None | The defense algorithm, byte-for-byte, before it guards anything | Fix algorithm; any surprise → add fixture permanently |

Probe sources live in `packages/atlantis/tests/probes/` and stay in-tree as regression
corpus (C8 discipline).

---

## 11. Foreseeable problems & solution strategies

| # | Problem | Strategy |
|---|---|---|
| 1 | Function-typed fields/array elements unproven at this scale (whole design rides on them) | P1 first, before any kernel code; failure is a STOP, not a workaround (C2 is frozen) |
| 2 | Loop-variable capture corrupts the fold (every layer sees the last `next`) | Recursive fold is the primary design (§1.1) — correct under by-value or by-reference capture; loop form exists only inside P1 as evidence |
| 3 | Await inside a server callback might not multiplex (loop parks whole accept path) | P3 before M1; failure escalates a C2 change (`Promise<HttpResponse>`) to the owner — kernel does not decide this alone |
| 4 | Binary assets corrupt through string bodies / File shims on LLVM | P4 gate; fallback text-assets-only + LA-3 dependency logged; never ship silently-corrupting bytes |
| 5 | Track 09 slips AG-0 (H-1) | Everything prelude-HTTP-touching lives in `seam.lev` (§3.2) with a stub tier; swap = delete stub; M6 is the swap milestone with buffer to AG-1 |
| 6 | Track 09 lacks a streaming-response hook (C2 promises one) | Named ask filed with Track 09 owner (§3.4); interim the seam stub writes chunked frames to TcpStream directly — kernel API (`mkStreaming`) identical either way |
| 7 | SSE/timer leaks keep the loop alive → process never drains (§7.4) | `onClose` cancellation contract (§8.2, mandatory); P5 asserts loop exit; drain corpus in M5 acceptance |
| 8 | Compute-bound handler stalls the loop (H-3) and users blame the framework | Contract documented in three places (§8.3, user docs, AccessLog `slowMs` warning); structural fix is LA-1 — already ticketed, pipeline shape ready |
| 9 | Symlink escape from docroot undetectable (no lstat) | Documented ops constraint (§5.1); registered P2 ask for a `sysStat` symlink/realpath field; not a silent hole — a written rule |
| 10 | ETag double-read cost on large files | Two-tier scheme (§5.3): strong md5 ≤ 256 KiB from the single serving read; weak size-mtime above — never read a file twice, never digest a gigabyte |
| 11 | `Principal` type ownership straddles 01/08 (C2 names it before 08 exists) | Seed file with explicit transfer note (§2.3); kernel never touches members; flagged to owner + Track 08 in handoff |
| 12 | problem+json needs `json` before Track 09 M4 lands | 12-line interim escaped builder behind the same `Respond` signatures, deleted at M6; public surface never changes |
| 13 | Error-mapper/access-log ordering ambiguity (who logs mapped 500s?) | Pinned in §4.4: AccessLog outside, observe-and-rethrow only; ErrorMapper owns all mapping (C4 intact); server-level net below both |
| 14 | Request parser accepts unbounded chunked bodies (no Content-Length to gate on) | Cap knob asked of Track 09 (§3.4); interim seam-side byte counting with connection abort at the limit |

---

## 12. Milestones & acceptance (gate AG-1: 2026-09-15)

| M | Deliverable | Accept | Target |
|---|---|---|---|
| M0 | P-probes P1–P4 (P5–P7 by M3) | Probe corpus green on oracle+IR+LLVM; results logged; any STOP raised now, not later | Jul 7–10 |
| M1 | `Context`, C4 exceptions, `Pipeline` fold, `Respond`, seam with stub tier, `Server` on stub | Hello-app: 3 middleware + terminal handler; curl sees ordered middleware effects; throw → mapped status | Jul 13–17 |
| M2 | `Atlantis::Log` + AccessLog + ErrorMapper + problem+json (interim builder) | JSON-lines validated by parsing each line with a checker script; 500 path logs full error, body generic; request ids unique across 1k requests | Jul 20–24 |
| M3 | Health/readiness, BodyLimit, `awaitTimeout`, `nextTick`, Deadline convention | 413 with Connection: close; /readyz flips under a failing check; P6 race corpus green | Jul 27–31 |
| M4 | StaticFiles: canonicalization, types, ETag/304, streaming (binary per P4 verdict) | P7 traversal table green; curl conditional-GET round-trip (200 w/ ETag → 304); 100 MB file streams without loop stall (interleaved second request stays fast) | Aug 3–10 |
| M5 | SSE + ChunkedBody on stub seam; drain behavior | SSE client receives timer events; disconnect cancels producer; `stop()` drains and process exits (+0B discipline) | Aug 11–18 |
| M6 | **Seam swap** to landed Track 09 (HeaderMap, keep-alive, chunked encoder, streaming hook, parser cap); delete stub tier + interim JSON builder | Full kernel corpus green on real substrate; keep-alive: 3 requests one connection through the whole pipeline; lcurl-style loopback fixtures | Aug 24–Sep 4 |
| M7 | AG-1 demo hello-app (`examples/atlantis-demo` seed: static site + /healthz + one SSE route + JSON errors), docs, supersession note on `designs/proposal-web-framework.md` (§7 overview duty) | **AG-1:** pipeline + Context + error mapping + static files + logging serve real traffic on IR & LLVM; demo runs; wall-time of `trident build` logged (H-7) | Sep 7–14 |

Buffer: Aug 19–21 and Sep 5–6 float. If Track 09 slips past Sep 1, AG-1 demos on the stub
tier (demo-grade transport, framework-grade kernel) and M6 re-schedules — the gate's
spirit (kernel serves real traffic) is met; the swap is additive.

---

## 13. STOP conditions

- **P1 fails** (function values not storable in fields/arrays) — C2's composition model is
  unimplementable as specified. STOP, escalate.
- **P3 fails** (await in a server callback does not multiplex) — C2's sync `Handler` shape
  is wrong; changing it is a frozen-contract amendment. STOP, escalate to owner.
- **P4 fails on LLVM** (binary bytes corrupt through string bodies) *and* text-only assets
  are deemed insufficient for AG-1 — coordinate LA-3 priority with owner. STOP on binary,
  proceed on text.
- Any fix that would require touching `src/**`, `tools/**`, `runtime/**` — rule (b);
  file to `/bug.md` or the LA register instead.
- The seam file grows beyond ~200 lines or a second file starts naming prelude HTTP
  internals — the H-1 containment has failed; stop and re-scope before continuing.
- Any temptation to add a field to `Context`, a second middleware shape, or a second error
  channel — rules (c)/(e).

---

## 14. Implementation log (append-only)

*(implementer appends dated entries here: probe results, deviations + justifications,
"not a STOP because…" notes, build wall-times per H-7)*

- 2026-07-07 (R10) — §3R added per overview ruling R10: `Atlantis::Builder`/`App` facade
  (named stages over the same fold — call order is pipeline order, framework invariants
  enforced at Build/serve) and `IActionResponse` (View/Json/Redirect/Status rendered by
  the terminal dispatcher). C2 function shapes, fold, error mapping, seam: unchanged.
  Static files now serve from `Public/` per the R10 scaffold.

- 2026-07-12 — **LANDED IN FULL** on oracle + IR + LLVM. Package
  `packages/atlantis/` (`src/kernel/*.lev`, `src/log/*.lev`), ~1015 lines. Test
  corpus **15/15 green** across all three engines (`packages/atlantis/tests/RESULTS.md`):
  probes p1 (fold) / p2 (exceptions); `corpus/kernel` (M1–M3 acceptance shape driven
  without a socket — pipeline fold + Context + 404/400/422/413/500 mapping + problem+json
  + BodyLimit + Health + Deadline + server-side-only 500 logging); `corpus/static`
  (M4 — P7 traversal + serve/ETag(md5)/304/HEAD + `%2e%2e`/`%252e%252e` defense, plus SSE
  framing + facade + IActionResponse); `corpus/loopback` (the `Server` serving **real
  traffic** through the full pipeline over an actual socket with an in-process
  `HttpClient` — exception→status end-to-end, 500 internals never leak). `trident run`
  wall-time ~0.03 s (H-7); `seam.lev` is 117 lines (H-1 containment holds, < 200).

  **P-probe verdicts (M0).** P1 ✓ (function values are first-class in `Array`/field, the
  recursive fold works, capture is correct) — *with* the idiom corrections below. P2 ✓
  (interface fields on exceptions, `Exception::Exception(msg)` base-ctor, catch ordering,
  rethrow — all engines). P3: await-in-callback multiplexing is a **landed language
  feature** (LA-30 stackful tasks, reference §6.6.67); `corpus/loopback` exercises it
  server-side rather than a standalone probe. P4 ✓ (256-byte binary round-trips through
  byte-clean strings on oracle/IR/**LLVM** — **static files can serve binary; no LA-3
  block**). P5: `ChunkedBody` implemented as the design's sanctioned **standalone-class
  fallback** (§8.1), not an `OutStream<string>` subclass. P6: `awaitTimeout` is now
  provided by the language (below). P7 ✓ (canonicalize fixtures pass verbatim).

  **Deviations forced by the landed compiler (none is a STOP; all are surface/idiom, not
  model changes):**
  1. **Namespaces (bug.md #37).** The qualified-declaration form `namespace Atlantis::Http
     {…}` does not parse, and full-path `Atlantis::Http::x()` access from an outer scope
     fails to resolve. The kernel declares the C1 namespaces via **nested braces**
     (`namespace Atlantis { namespace Http { … } }`, likewise `Log`/`Auth`) and consumers
     `uses Atlantis::Http;` (which resolves correctly). The *logical* namespaces are exactly
     `Atlantis::Http` / `Atlantis::Log` / `Atlantis::Auth` (C1 honored) — only the
     declaration spelling and the outer-scope access path differ. Filed P1 (every future
     Atlantis track needs this convention until the compiler supports the direct form).
  2. **Lambda parameters are untyped (bug.md #39).** `(c, next) => …`, not `(Context c,
     … next) => …`; the target function type supplies the parameter types. Every `mw()`
     binds `self = this` into a local first (a lambda does not close over the implicit
     receiver — the documented prelude discipline).
  3. **Middleware are lambdas, never bare named functions.** A bare free-function name is
     not a first-class value on this compiler (the checker/lowerer reject it); the design
     already mandates lambda middleware, so this is in-spirit. (`Class::method` references
     *do* work as values, LA-25.)
  4. **JSON via a hand-rolled escaped-string builder (`jsonQuote`), not `JsonValue`
     (bug.md #30).** `JsonValue` does not lower on LLVM, and AG-1 requires LLVM. This is
     the design's own §4.2 "INTERIM builder" promoted to the primary path (problem+json,
     validationProblem, and every log line build strings). `Respond::json(int, string)`
     takes a pre-rendered JSON body; a `JsonValue` overload can return when #30 is fixed.
  5. **Explicit struct constructors (bug.md #38)** for `FieldError`/`LogField` (positional
     auto-construction silently leaves fields empty).
  6. **`awaitTimeout` is reused from the prelude, not re-implemented.** The design's §7.3
     `RaceState`/`awaitTimeout` is now **provided by the language** (LA-30 B2, reference
     §6.6.68 `T? awaitTimeout<T>(Promise<T>, int ms)`), along with `TaskGroup` /
     `CancelledException`. `limits.lev` documents this and ships `Deadline` +
     `deadlineRemaining`; the kernel does not re-declare the race.
  7. **`Server` wraps the hardened `HttpServer` (design §3.2 "when Track 09 has landed").**
     Track 09 shipped, so the seam's **stub tier is omitted** — `Server` installs the app
     over `HttpServer.handle`, inheriting keep-alive and the per-connection 500 net (the
     last layer below ErrorMapper, §4.4). Fresh Context per request (incl. per keep-alive
     request).
  8. **Streaming is collect-then-send (interim).** `HttpServer` exposes no server-side
     streaming-response hook, so `mkStreaming`/`ChunkedBody` buffer a *finite* writer's
     chunks into one response (correct output; static files serve correctly on all three
     engines). **Not a STOP:** §3.4 and hurdle H-6 always treated the streaming hook as a
     Track 09 coordination ask with a documented interim, and the buffered path serves real
     traffic. Filed `designs/requests/request-http-streaming-response.md` (three candidate
     shapes; the surface is stable across the swap — only `mkStreaming`'s body changes).
     Unbounded SSE-push and true 100 MB incremental streaming (part of M4/M5 acceptance)
     land with that hook.
  9. **`Map`: bracket-sugar set (`m[k]=v`) + field-default `= Map()` init** (bug.md #18
     `Map.with` off LLVM; `Map<K,V>()` type-args don't parse and bare `Map()` infers K/V
     only from a field-default target, not a ctor-body assignment). `routeParams`/`items`
     get a fresh Map per Context via field initializers.
  10. **Narrowing:** positive `if (x != None) { … }` blocks or `?? ""` coalesce (a negative
      early-return `if (x == None) return;` does not narrow `x` afterward on this checker).
  11. **Interface arrays** are built via `.add(subtype)`, not a single-element subtype
      literal (`Array<IReadyCheck> = [flagCheck]` fails invariance; a *heterogeneous*
      literal infers the common supertype and is fine).

  **STOP conditions checked — none triggered.** P1 passed (C2's fold is implementable), P3
  multiplexing works (loopback), P4 passed on LLVM (binary OK), `seam.lev` stayed one file
  under 200 lines, no field was added to `Context`, no second middleware/error/endpoint
  model was introduced, and **no `src/**`/`tools/**`/`runtime/**` file was touched** — the
  three compiler gaps went to `/bug.md` (#37/#38/#39) and the streaming capability to a
  request ticket, per the STOP protocol (b)/(h).

  Per the task directive, this design moves to `designs/complete/` on landing.
