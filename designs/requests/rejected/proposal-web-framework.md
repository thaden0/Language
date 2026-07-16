# Proposal: A First-Class Web Framework (working name: **Loom**)

**Status:** direction-finding proposal. Depends on `designs/proposal-metaprogramming.md` (the rule
layer) and on the project/file system prerequisites listed there.
**Location note:** in `docs/` alongside `reference.md` and `proposal-metaprogramming.md` (see that
doc's location note — both proposals live under `docs/`).

> **One-line thesis.** The market's loudest unmet want is **one coherent model instead of many
> special rules** — colorless concurrency, one error path, one data-flow abstraction, ergonomics
> that don't cost analyzability. This language *already* has the hard part: **streams are the
> system boundary, `Promise` is a one-shot stream, `await` has no function coloring, DI is
> compile-time, and objects/structs are typed.** Loom is mostly *exposure* of strengths the
> language already has, tied together by the rule layer (Doc 1) so the ergonomic surface
> (`@Route`, `@Column`) compiles to hand-written code with zero runtime reflection.

The framework's job is **not** to invent a web programming model. It is to collapse the five or six
models every other framework accreted (callbacks, promises, async/await, observables, streams,
sync) into the **one** the language was designed around, and to make the professional batteries
(routing, data, validation, auth, uploads, front end) fall out of that one model plus the rule
layer.

---

## 0. Table of contents
1. What "professional / first-class" actually means (grounded)
2. The one big bet: consistency — collapsing many rules into one
3. The concurrency/data-flow model: **everything is a stream** (Streams vs Async vs Promises vs Callbacks, resolved)
4. How the rule layer (Doc 1) powers the ergonomics
5. Routing & Controllers
6. Request / Response (as streams)
7. Models & Database Access (structs, type-safe queries, migrations, N+1)
8. Validation, DI, Config
9. Static files & File Uploads
10. Front end — SPA vs MPA, template engine vs Inertia (recommendation)
11. Auth, OpenAPI, Observability
12. What Loom explicitly does NOT include (and why)
13. Directory structure & setup (easy default, advanced not locked out)
14. Worked example — a real application walkthrough
15. The consistency scorecard (many rules → one)
16. Prerequisites, phasing, risks
17. Headline recommendations

---

## 1. What "professional / first-class" actually means (grounded)

From the research (ASP.NET Core, Symfony, NestJS, Django, FastAPI converge here), "professional"
decomposes into a concrete checklist — and each item maps to something the language already leans
toward:

| "Professional" means… | Loom's answer (language feature it rests on) |
|---|---|
| First-class DI, testable, overridable in tests | `bind`/`inject` — **compile-time DI already exists** (§12.5) |
| One canonical way per concern (kills "team-invented framework") | §1 "one rule over many special cases" is the language's whole ethos |
| Middleware/observability as composable, ordered stages | **stream transforms** (§13) — one composition model |
| Migrations, first-class & conflict-aware | value-`struct` models + generated schema (Doc 1 §10.2) |
| Auth / validation / OpenAPI in the box (no "build half of Django yourself") | rule-generated wiring + typed attributes |
| Structure/conventions, predictable (the Next.js anti-lesson: nothing hidden) | explicit routing, `--expand` shows all magic (Doc 1 §6.4) |

The two anti-patterns the research is loudest about, and Loom's stance:

- **Laravel-style "magic" that breaks static analysis** (Eloquent `__call`, facades as static
  service locators, global helpers "pollute the namespace," properties "injected magically"). →
  Loom gets Laravel/FastAPI-level DX using **real typed language features** (attributes + rules the
  compiler and IDE understand), never runtime `__call`. The magic is *at compile time and
  greppable* (Doc 1 §5.3), not *at runtime and opaque*.
- **Next.js-style invisible execution boundaries** ("you thought you made an API call but read a
  cache"; "compiles but fails at runtime"). → Loom's rule that **where code runs and what is cached
  is never implicit**. `--expand` (Doc 1) makes every generated line visible; there is no
  server/client bifurcation of one function.

---

## 2. The one big bet: consistency

The brief is explicit that **consistency is critical — collapse existing patterns into fewer,
broadly-applicable rules.** This is also, per the research, the single largest differentiation
opportunity ("the loudest unmet want across the entire corpus"). So it is the organizing principle,
not a section.

Loom commits to **five collapses**. Each takes a place where mainstream frameworks have *many*
special rules and reduces it to *one* rule that applies everywhere. They are stated up front and
then realized through the rest of the doc.

1. **One data-flow model: the stream.** Callbacks, promises, async/await, observables, and streams
   collapse to `StreamBuffer` + typed `In/Out/IOStream`, with `Promise` = a one-shot stream and
   `await` = stream-pull (§3). No function coloring.
2. **One composition model: the stream transform.** Middleware, pipelines, interceptors, filters,
   and reactive operators collapse to "a function from a stream to a stream" (§6). Request handling,
   middleware, SSE, websockets, and file streaming are the *same* shape.
3. **One error model: exceptions primary, `Result` for expected outcomes.** No third
   framework-specific error channel (Axum's *two* incompatible error-to-response paths is the
   anti-pattern). Catch-by-type/interface maps directly to HTTP status (§6.4).
4. **One wiring model: `bind`/`inject` + rules.** DI, route registration, ORM schema, validation,
   and serialization all wire the same way — typed attributes read by namespace-scoped rules that
   emit ordinary `bind`/calls (§4). No DI container object, no reflection, no service locator.
5. **One data model: the value `struct`.** Request DTOs, DB rows, API responses, and form models are
   all `struct`s (§7). Dense/columnar storage, field-wise equality, copy semantics, and
   compile-time schema generation all fall out of the one value-type rule (`info.md` §9).

The scorecard in §15 tallies exactly how many special rules each collapse eliminates.

---

## 3. The concurrency / data-flow model: **everything is a stream**

This is the heart of the proposal and the brief's explicit demand ("recommend ONE coherent model
across all scopes: Streams vs Async vs Promises vs Callbacks"). The language already made this
decision (`info.md` §13); Loom adopts it wholesale and refuses to add a second model.

### 3.1 The problem, per the research
"What color is your function?" — async/await splits every codebase into red/blue functions with
painful rules; color "bleeds all over your codebase." Node teams juggle callbacks *and* promises
*and* streams *and* (via RxJS) observables — "feel similar, different mental models, picking wrong
makes the codebase harder." Go is repeatedly praised because it "does not color its functions at
all." FastAPI's async footguns (holding a DB connection across a request lifecycle) show the cost of
a half-thought concurrency model.

### 3.2 The language already solved coloring
From `info.md` §6.6.54 / §13 / §14, verbatim capabilities Loom builds on:
- **No `async` keyword, no function coloring.** "await is permitted anywhere. A function returning
  `Promise<T>` is the async form." `await` pumps the event loop rather than doing stackful
  suspension — so *any* function may await, exactly the Go-goroutine property the research wants.
- **`Promise<T>` is a one-shot stream.** "await and stream-pull are the same suspension, two
  surfaces." There is no separate promise runtime.
- **Streams are THE system boundary.** "Nothing crosses the process boundary except through a
  stream." Files, sockets, timers, stdin — all stream endpoints, one API (`pull`/`<<`/`subscribe`).
- **Callbacks reshape into promises/streams trivially** ("HttpClient.fetch = get + a resolve
  callback, turning callback pyramids into linear code").

### 3.3 Loom's single rule
> **Every asynchronous or incremental value in Loom is a stream. A single value that arrives later
> is a one-shot stream (`Promise`). You consume it with `await` (one value) or `subscribe`/`pull`
> (many). There is no other async primitive, and no function is "colored."**

Concretely, one model covers every scope a web framework touches:

| Scope | Other frameworks | Loom (one model) |
|---|---|---|
| A request handler returning a value | async fn / Promise / Result wrapper | ordinary function; may `await`; returns `HttpResponse` |
| A body/file being read incrementally | Node Readable / async iterator | `InStream<string>` (`pull`/`subscribe`) |
| A response being written | `res.write()` / callbacks | `OutStream<string>` (`<<`, chainable) |
| Server-Sent Events / long poll | a separate SSE API | an `OutStream` you keep writing to |
| WebSocket | a separate ws API | an `IOStream` (both ends, §13 diamond) |
| A DB query result set | driver-specific cursor/promise | `InStream<Row>` (stream of rows) |
| Timers / scheduled work | setTimeout / cron lib | `std::every(ms)` → a stream of ticks (§6.6.55) |
| Fan-out to N subscribers | EventEmitter / RxJS Subject | a library reshaping (broadcast is explicit, §13) |

The payoff the research asked for: **one mental model, no coloring, no "streams vs observables vs
promises vs callbacks" decision.** A handler that awaits a DB row, streams a file, and pushes an SSE
event uses *the same three verbs* (`await`, `<<`, `subscribe`) throughout.

### 3.4 Cancellation and errors across the boundary (designed early, per `info.md` §14/§19)
- **Cancellation is a stream signal**, not a parallel mechanism: closing the consumer end (or a
  `cancel()` on a timer/subscription, which already exist) propagates as ordinary stream close.
  Loom threads a single `close`/EOF convention (`sysRecv → None` = peer closed, §6.6.57) rather than
  a bespoke `CancellationToken` on every signature.
- **Errors** use collapse #3: a failing handler *throws*; the framework's outermost stream transform
  (§6) catches by type/interface and maps to a status. Expected domain outcomes use `Result` unions
  (`int | string` is already Result-shaped). One path, chosen by whether the outcome is *exceptional*
  or *expected* — the same distinction `info.md` §12.6 already draws.

---

## 4. How the rule layer (Doc 1) powers the ergonomics

Every ergonomic in Loom is a **typed attribute read by a namespace-scoped rule that emits ordinary
code** (Doc 1 §4). This is the concrete answer to "ergonomics without analyzability loss": the
`@Route` you write is terse, but it expands (visibly, via `--expand`) into the exact `router.add`
call you'd write by hand, checked by the normal checker, compiled to the fixed-offset fast path
(Doc 1 §8, P1). No reflection, no `__call`, no container scanning at boot.

The framework ships **one namespace, `Web`** (plus sub-namespaces `Web::Data`, `Web::View`), whose
rules you opt into with `uses Web;`. Everything below is powered by that namespace's rules:

| You write | Rule in `Web` emits | Runtime cost |
|---|---|---|
| `@Route("GET","/users")` on a method | `router.add(...)` in the controller ctor | identical to hand-written |
| `@Table("users")` / `@Column` on a struct | `static schema()` + column map | compile-time constant |
| `@Inject` on a ctor param | resolves via existing `bind` (§12.5) | compile-time direct call |
| `@Validate` on a DTO field | validation code in the extractor | inline checks |
| `@Serialize` on a struct | `toJson`/`fromJson` over the fields | inline field access |
| `@Auth("admin")` on a method | a guard stream-transform prologue | inline check |

Because rules are **namespace-scoped** (Doc 1 §5), a file that never `uses Web` is untouched — the
framework cannot reach into code that didn't opt in. "What magic is on this file?" = "read its
`uses`."

---

## 5. Routing & Controllers

**Decision (from research §3):** attribute-on-handler routing (ASP.NET `[HttpGet]`, NestJS `@Get`,
FastAPI `@app.get`) is the loved sweet spot *when the language has real analyzable annotations* —
which the rule layer provides. **Avoid file-based routing** (Next.js's invisible execution model is
the cautionary tale) and avoid forcing an explicit route table (Express/Axum "50 endpoints answered
differently by different devs"). Route declaration *is* the validation + OpenAPI source of truth
(Axum/FastAPI lesson).

```
uses Web;
namespace App {
    class UserController : Controller {          // Controller provides router, request, response
        @Route("GET",  "/users")        Array<User> list()               => users.all();
        @Route("GET",  "/users/:id")    User        show(int id)         => users.get(id);
        @Route("POST", "/users")        User        create(NewUser body) => users.add(body);
        @Route("DELETE","/users/:id")   void        remove(int id)       => users.delete(id);
    }
}
```

- **One endpoint model, not two.** ASP.NET's minimal-APIs-vs-controllers split is itself a "which do
  I use" tax (research §2). Loom ships **one** — attributed methods on a `Controller`. (A bare
  `@Route` free function is the same rule with `in namespace` scope for the trivial case, so there is
  no *second model*, just the controller omitted.)
- **Type-driven extraction (Axum/FastAPI's most-loved property).** A handler parameter is filled
  from the request *by its type and name*, decided at compile time by an extraction rule:
  - `int id` where the route has `:id` → path segment, parsed & validated to `int`.
  - `NewUser body` (a `struct`) → deserialized from the request body, validated against the struct's
    `@Validate` rules.
  - `Query<Filter> q` / `Header h` / injected services → by wrapper type or `@Inject`.
  The extraction is generated code (visible via `--expand`), not runtime reflection — so a missing
  or mistyped parameter is a **compile error**, not a 500 at runtime (kills FastAPI's "runtime-only"
  class and Django's stringly-typed-lookup class).
- **Sub-routing / prefixes** are class-level attributes: `@RoutePrefix("/api/v1")` on the controller
  composes with method routes. Versioning is an attribute, not a convention scattered across files.

---

## 6. Request / Response (as streams — collapse #2)

Request and response are the same `In/Out` stream types the language already has (`info.md` §13,
§6.6.57). Middleware is **not a special API** — it is collapse #2, a **stream transform**: a
function from the request/response stream to a request/response stream.

```
namespace Web {
    // A handler is: (Request) => Response. Middleware wraps that shape.
    // One type. Logging, auth, CORS, compression, uploads are all this.
    interface Middleware { (Request, Handler next) => Response run; }
}
```

- **The pipeline is ordinary composition.** `pipeline([logging, cors, auth, compress])` is a fold
  over stream transforms — no framework-specific "middleware registration" DSL. Order is explicit
  and visible (the ASP.NET "order bug that breaks auth" is a *visible* ordering in a plain list, not
  a hidden convention).
- **Streaming responses fall out for free.** Returning an `HttpResponse` sends a buffered body;
  returning an `OutStream<string>` streams it (SSE, large files, chunked). *Same return position,
  same type family* — no separate `StreamingResponse` class (FastAPI) to learn.
- **WebSockets = `IOStream`.** A ws handler receives an `IOStream<string>` (the §13 diamond, both
  ends over one buffer). Read with `>>`/`subscribe`, write with `<<`. No third real-time API.
- **Errors → status via collapse #3.** The outermost transform is a `try/catch` that selects by
  interface: `catch (IValidationException)` → 422, `catch (INotFound)` → 404, `catch (IException)` →
  500. Catch-by-capability (`info.md` §12.6, multiple inheritance earning its keep) *is* the
  status-mapping table — no `IntoResponse`-vs-`HandleErrorLayer` duplication (Axum's anti-pattern).

```
Response handle(Request req) {
    try { return router.dispatch(req); }
    catch (IValidationException e) { return Response(422, e.toJson()); }
    catch (INotFound e)            { return Response(404, e.message); }
    catch (IException e)           { return Response(500, "internal error"); }  // logged, not leaked
}
```

---

## 7. Models & Database Access (collapse #5 + the DBMS direction)

The language's memory/roadmap already points at a **DBMS / value-type direction**: value `struct`s
give dense, unboxed, columnar-capable storage that "maps directly onto `mmap` and columnar forms"
(`info.md` §9). Loom's data layer is the web-facing surface of that direction. **A model is a value
`struct`** (collapse #5) — the same type used for request DTOs and API responses.

### 7.1 Models as structs (one type for row / DTO / response)
```
uses Web::Data;
@Table("users")
struct User {
    @Column @Id           int    id;
    @Column("full_name")  string name;
    @Column               string email;
    @Column @Default(true) bool  active;
    @HasMany(Post)        Array<Post> posts;     // relation, eager-loadable
}
```
- `@Table`/`@Column` are typed attributes; the `Web::Data` rule generates a **compile-time schema**
  (Doc 1 §10.2) — column names, types, id, defaults — as a constant, no runtime reflection.
- Because `User` is a `struct`, it is **dense and copyable** (a result set is a flat
  `Array<User>`), has **field-wise equality**, and **serializes by walking its fields** (a
  `@Serialize` rule generates `toJson`/`fromJson`). The *same* `User` value is what a handler returns
  and what the JSON encoder walks — no separate DTO/entity/response-model triplication (the thing
  FastAPI+SQLAlchemy+Pydantic makes you write three times).

### 7.2 Type-safe queries (kill the N+1 and the runtime-typo classes)
The research's two universal ORM complaints are **N+1** ("the #1 recurring ORM complaint") and
**runtime-only query errors** (Django's string lookups). Loom's query layer is the language's
existing array/relational surface (`where`/`select`/`join`/`groupJoin`, `info.md` §11) targeting a
table instead of an in-memory array — *the same LINQ-shaped methods*, so there is no separate query
DSL to learn (collapse: query = the array method surface).

```
// compile-time-checked: field references are real struct members, not strings
Array<User> admins = Users.where((u) => u.active && u.role == "admin")
                          .orderBy((u) => u.name)
                          .take(50);

// eager loading is explicit and type-checked — N+1 is hard to hit by default
Array<User> withPosts = Users.where((u) => u.active).include((u) => u.posts);
```
- **Field references are struct members**, so a typo (`u.naem`) or a wrong type is a **compile
  error** — the entire Django "string lookup fails at runtime" class is gone.
- **Eager loading (`include`) is explicit and analyzable.** There is no lazy-loading-by-default (the
  EF Core / Eloquent N+1 trap). Accessing a relation that wasn't `include`d is a **compile-time
  warning** ("relation `posts` accessed but not included; add `.include(...)` or load explicitly") —
  N+1 becomes a diagnostic, not a production surprise.
- **Transparent path to raw SQL is non-negotiable** (Drizzle's winning feature): `Users.sql("SELECT
  ... ")` returns typed `Array<User>` when shapes match, and every query object can `.toSql()` for
  inspection. You are never trapped above the SQL.
- **Eager vs lazy execution** is settled the way `info.md` §11/§19 leans for arrays: **eager**, in
  memory and bounded — with the query builder deferring the round-trip until the terminal operation
  (`take`/`all`/`first`) so a chain is one SQL statement, not N.

### 7.3 Migrations ("pit of success", conflict-aware — research §5)
- Migrations are **generated from the struct diff** (the schema is a compile-time constant, so the
  compiler can diff `schema(v_old)` vs `schema(v_new)`), with **data-loss warnings** (Prisma's
  loved behavior) before a destructive change.
- **Branch-conflict detection** (Django's loudest pain): migrations carry a content hash + parent
  chain; two divergent heads are a **build error with a merge prompt**, not a silently inconsistent
  DB.
- Applied via the dev CLI (`loom migrate`), never implicitly at boot (the "predictable, nothing
  hidden" rule).

### 7.4 Active-record vs data-mapper
Research: active-record (Eloquent) = velocity but "models with multiple responsibilities" and lost
analyzability; data-mapper (Doctrine/EF) = explicit, correct at scale, verbose. **Loom picks
data-mapper-leaning:** the `struct` is *pure data* (no `save()` method hanging off the row — a struct
is a value, §9, it has no identity to save). Persistence goes through a repository
(`Users.add(u)`, `Users.update(u)`), which keeps the model analyzable and side-effect-free and fits
the value-semantics rule exactly. (This also avoids Laravel's "model does filtering, sorting, and
more" critique — the model does *nothing* but hold fields.)

---

## 8. Validation, DI, Config

### 8.1 Validation (one rule, at the boundary)
Validation is typed attributes on struct fields, read by the extraction rule (§5) so **invalid input
never reaches the handler** — and the rules that OpenAPI reads are the *same* rules (one source of
truth, FastAPI's winning property):
```
struct NewUser {
    @Required @MaxLen(100)          string name;
    @Required @Email                string email;
    @Min(13)                        int    age;
}
```
A failed check throws `IValidationException` → 422 (collapse #3). No separate validation call in the
handler; no DTO-validated-here-but-not-there drift.

### 8.2 DI (already exists — §12.5)
Loom adds **no DI container.** It uses the language's compile-time `bind`/`inject`:
```
bind IUserStore => SqlUserStore();      // at namespace/app scope; propagates
class UserController : Controller {
    new UserController(@Inject IUserStore users) { this.users = users; }
}
```
- **Testability** (research's #1 "professional" property): swap the binding in a test scope —
  `bind IUserStore => FakeUserStore();` — nearest-wins lexical shadowing (§12.5). No mocking
  framework, no container reconfiguration.
- Because binds are compile-time and lexical, "which implementation does the app get" is decidable
  and greppable — the exact anti-footgun `info.md` §12.5 designed for.

### 8.3 Config (one composition root — ASP.NET's loved property)
A single `app.ext` composition root (the "one obvious place to wire everything" the research
praises), reading a typed `Config` struct from env/file. No config scattered across the tree; no
implicit env magic.

---

## 9. Static files & File Uploads

Both are collapse #2 (stream transforms) — no special subsystems:
- **Static files:** a `staticFiles("/public")` middleware transform that resolves a path and streams
  the file via the existing `File.reader() → FileInStream` (`info.md` §6.6.6). Range requests and
  streaming fall out because the response is an `OutStream`.
- **File uploads:** an upload is an `InStream<Block>` (a stream of body chunks) — the request body
  *is* a stream, so a large upload is consumed incrementally, never buffered whole (the thing
  Express/multer makes awkward). `@Upload Array<UploadedFile> files` on a handler is an extraction
  rule over a multipart body stream. (Binary bodies wait on the `Block` type — noted in
  prerequisites §16.)

---

## 10. Front end — SPA vs MPA, template engine vs Inertia

**This is the explicit "recommend with reasoning" ask.** The 2024–2026 research is unambiguous:
htmx/hypermedia is resurgent ("most admired," React satisfaction at its lowest despite high usage;
"tired of maintaining 200MB node_modules"), Inertia's "modern monolith" is loved ("SPAs without
building an API"), and "do I need React?" fatigue is real. The consensus rule of thumb: *"A Notion
clone needs React. An admin dashboard needs htmx. A SaaS might need both."*

### Recommendation: **MPA-first, hypermedia-native, with an optional Inertia-style adapter — and NO bundled SPA framework.**

Three tiers, in the order Loom invests:

1. **A built-in server-side template engine (primary).** Razor/Blade/Jinja-class, but — crucially —
   **the template language is the base language** wherever possible (loops, conditionals,
   expressions are ordinary Language code in the template, powered by the rule layer's quasiquote
   machinery, Doc 1 §4.2), so there is no second expression syntax to learn and templates are
   type-checked against the model. This is the "not a second language" principle (Doc 1 §3.4)
   applied to views. Server-rendered HTML is the default; it is fast, SEO-friendly, and needs no
   build step.
2. **First-class hypermedia partials (htmx-style, built in).** A handler can return a *fragment*
   (`@Route` + `@Fragment`) that Loom serves for a partial swap. This is the htmx sweet spot —
   dynamic UI, server-driven, ~14KB client shim, no bundler, no `node_modules`. Loom ships a tiny
   client helper and a `render(fragment, model)` convention; the server stays the source of truth.
3. **An optional Inertia-style adapter (for teams who genuinely want React/Vue).** For the "Notion
   clone" tier, Loom offers an Inertia-protocol adapter: the controller returns a `page(name, props)`
   value; a React/Vue front end (the team's own, unbundled by Loom) renders it. **"Server-side
   routing, controllers, and validation combined with the reactivity of Vue/React. No API to build,
   no auth tokens to manage, validation logic stays in one place."** This gives the SPA option
   *without* Loom bundling, versioning, or opining on a JS framework.

**Why not the alternatives:**
- **Not file-based SPA routing / bundled React (Next.js model):** the research's clearest cautionary
  tale — invisible server/client boundaries, caching-by-default surprises, RSC CVEs, "Next.js
  fatigue." It also violates Loom's "predictable, nothing hidden" rule. Loom will not bundle or
  opine on a client framework.
- **Not template-engine-only:** would strand the genuine "we need rich client interactivity" tier and
  push those teams to build a separate API (the thing Inertia exists to avoid).

So: **"Do I need React?" is answerable with "no, but you can."** MPA + hypermedia handles the large
majority; the Inertia adapter is the escape hatch for the minority — and both reuse the *same*
controllers, validation, and DI (no bifurcation).

---

## 11. Auth, OpenAPI, Observability

- **Auth:** `@Auth("role")` / `@Authenticated` attributes → guard stream-transforms (collapse #2)
  that run before the handler; identity is an injected service (`@Inject Principal`). Sessions and
  tokens are two `bind`-able strategies behind one `IAuth` interface — *one* auth model, strategy
  chosen by binding (not FastAPI's "assemble OAuth2+JWT+sessions yourself" from scratch, but also
  not Django's monolith — batteries, modular).
- **OpenAPI:** generated from the *same* route/validation/serialize attributes (FastAPI's single
  most-loved feature). Because attributes are typed and rules run at compile time, the spec is a
  **build artifact**, always in sync, zero runtime cost. `loom openapi` emits it.
- **Observability:** a logging/tracing/metrics transform at the top of the pipeline (collapse #2);
  structured logs via the stream model; request IDs threaded as a stream context value, not a
  hidden thread-local.

---

## 12. What Loom explicitly does NOT include (and why)

Minimalism-vs-batteries resolves to **"batteries included but replaceable/modular"** (research §6:
Django Ninja, NestJS, Inertia are all hybrids the market invented to escape the false choice). So
Loom includes the batteries every app needs, behind swappable interfaces — and deliberately
**excludes**:

| Excluded | Why (grounded) |
|---|---|
| A bundled JS/SPA framework | Next.js fatigue; Loom will not opine on/version a client framework (§10). Inertia adapter is the seam. |
| A second endpoint model (minimal vs controllers) | ASP.NET's split is a "which do I use" tax (research §2). One model (§5). |
| A runtime DI container | `bind`/`inject` is compile-time (§8.2); a container is runtime reflection Loom rejects (P1). |
| Lazy-loading ORM relations | the N+1 trap (research §5); Loom makes loading explicit (§7.2). |
| Active-record `save()` on models | breaks value semantics & analyzability (§7.4). |
| A bespoke async/reactive library (Rx-style) | collapse #1 — the stream *is* the reactive model; a second one re-creates "streams vs observables" fatigue (§3). |
| ORM "magic" (`__call`, facades, global helpers) | the Laravel analyzability critique (research §4); Loom's ergonomics are typed rules, not runtime magic. |
| File-based routing with hidden execution semantics | the Next.js anti-lesson (research §7). |
| A `finally`/GC-based resource story | `info.md` §15 scope-based cleanup covers it; keeps resource release deterministic. |

The rule for exclusion: **exclude nothing every app needs; make everything included overridable;
never ship two models where one applies.**

---

## 13. Directory structure & setup (easy default, advanced not locked out)

Convention-over-configuration is praised when it removes decisions and hated only when it hides
runtime behavior (research §7). Loom ships a scaffolding CLI (`loom new`) and a visible convention:

```
myapp/
  project.ext              # manifest (Doc 1 P-1): sources, entry, deps
  app.ext                  # composition root: binds, middleware pipeline, config (ONE place)
  config/                  # typed Config structs (env-bound)
  controllers/             # @Route-attributed Controllers
  models/                  # @Table value-structs (row = DTO = response)
  data/                    # repositories, migrations/
  views/                   # server-side templates (+ fragments/)
  middleware/              # stream transforms
  public/                  # static files
  tests/                   # DI-swapped tests
```

- **Easy setup:** `loom new myapp && loom dev` — a running server with one example controller, one
  model, one migration. `loom migrate`, `loom openapi`, `loom routes` (lists the route table —
  which is just `--expand` of the controllers, so the "magic" is always inspectable).
- **Not locked out:** every convention is a *default binding or a default middleware list in
  `app.ext`* — advanced users edit the composition root to swap the router, the template engine, the
  serializer, the auth strategy. The directory layout is a `sources` glob in the manifest, not a
  hardcoded compiler assumption (contrast Next.js's load-bearing filenames). Nothing about the
  layout changes *where code runs* or *what is cached* — the Next.js anti-lesson, avoided by
  construction.

---

## 14. Worked example — a real application walkthrough

A small "links" app: users post links, list them, view one, with server-rendered HTML + an htmx
partial for live search, plus a JSON API. Shows the whole surface tying together through the *one*
model per concern.

**`project.ext`**
```
name    = "links"
entry   = "app.ext"
sources = ["**/*.ext"]
```

**`models/link.ext`**
```
uses Web::Data;
@Table("links")
struct Link {
    @Column @Id                 int    id;
    @Column @Required @MaxLen(200) string title;
    @Column @Required @Url         string url;
    @Column @Default(0)         int    votes;
}
```

**`data/links.ext`**
```
uses Web::Data;
namespace Data {
    interface ILinks { Array<Link> top(int n); Link get(int id); Link add(Link l); }
    class SqlLinks : ILinks {
        Array<Link> top(int n) => Links.orderBy((l) => -l.votes).take(n);   // one SQL, no N+1
        Link get(int id)       => Links.where((l) => l.id == id).first();   // compile-checked fields
        Link add(Link l)       => Links.insert(l);
    }
}
```

**`app.ext` (composition root — the ONE wiring place)**
```
uses Web; uses Data;
bind ILinks => SqlLinks();                       // DI: swap in tests with FakeLinks
Web::app()
    .use(logging).use(cors).use(staticFiles("/public"))   // middleware = stream transforms, ordered & visible
    .mount(LinkController)
    .listen(8080);
```

**`controllers/link_controller.ext`**
```
uses Web;
namespace App {
    class LinkController : Controller {
        new LinkController(@Inject ILinks links) { this.links = links; }

        @Route("GET", "/")                          // server-rendered page
        View index() => view("links/index", { top: links.top(25) });

        @Route("GET", "/search") @Fragment          // htmx partial — same controller, returns a fragment
        View search(Query<string> q)
            => view("links/_list", { top: links.top(25).where((l) => l.title.contains(q)) });

        @Route("GET", "/links/:id")
        View show(int id) => view("links/show", { link: links.get(id) });   // id parsed & validated

        @Route("POST", "/links")                    // body -> validated struct -> handler
        View create(NewLink body) { links.add(Link(body.title, body.url)); return redirect("/"); }

        @Route("GET", "/api/links")                 // same model, JSON — no separate DTO
        Array<Link> api() => links.top(25);         // @Serialize rule renders Link[] to JSON
    }
}
```

**`views/links/index.html` (template — base-language expressions, type-checked against the model)**
```html
<h1>Top links</h1>
<input hx-get="/search" hx-target="#list" name="q" placeholder="search…">   <!-- htmx partial swap -->
<ul id="list">
  {{ for l in model.top }}
    <li><a href="/links/{{ l.id }}">{{ l.title }}</a> — {{ l.votes }} votes</li>
  {{ end }}
</ul>
```

**`tests/link_test.ext`**
```
uses Web;
bind ILinks => FakeLinks([ Link("a","http://a"), Link("b","http://b") ]);  // swap DI, nearest-wins
void test_index_lists_top() {
    var res = LinkController().index();
    assert(res.model.top.length() == 2);           // no HTTP, no mocks framework — just DI
}
```

**What the walkthrough demonstrates about consistency:**
- The **same `Link` struct** is the DB row, the API JSON, and the template model (collapse #5).
- The **same controller** serves a full page, an htmx fragment, and JSON — differing only by return
  type/attribute, not by a separate API layer (the Inertia/htmx "no separate API" win).
- **Middleware, static files, and the request** are all the one stream-transform/stream model
  (collapse #2/#1).
- **DI is compile-time and swapped in tests** with one `bind` line (collapse #4).
- Every ergonomic attribute (`@Route`, `@Column`, `@Validate`, `@Serialize`, `@Fragment`) is
  `--expand`-visible generated code (Doc 1) — **nothing hidden, zero runtime reflection.**

---

## 15. The consistency scorecard (many rules → one)

The brief asks explicitly to find places "where many special rules can collapse to a single rule."
Tally:

| Concern | Special rules elsewhere | Loom's single rule |
|---|---|---|
| Async | callbacks, promises, async/await, observables, streams, sync (6, + function coloring) | **the stream** (`Promise` = one-shot; `await` anywhere) — **1, no coloring** |
| Composition | middleware, filters, interceptors, pipes, guards, Rx operators (6) | **stream transform** `(stream) => stream` — **1** |
| Errors | exceptions, Result, framework error types, `IntoResponse` vs `HandleErrorLayer` (4+) | **throw + catch-by-type; `Result` for expected** — **1 path** |
| Wiring | DI container, service locator, route registry, decorator metadata, reflection scan (5) | **`bind`/`inject` + rules** — **1** |
| Data shape | entity, DTO, request model, response model, validation schema (5) | **the value `struct`** — **1** |
| Query | ORM DSL, query builder, raw SQL, active-record finders (4) | **the array method surface on a table** — **1** |
| Endpoints | minimal APIs vs controllers vs file-routes (3) | **attributed `Controller` methods** — **1** |
| Real-time | SSE API, WebSocket API, streaming-response class, long-poll (4) | **`In/Out/IOStream`** — **1** |
| Front end | template engine + SPA framework + separate API (3, bifurcated) | **one controller → page / fragment / JSON** — **1 surface** |

Each row is a place the language's "one rule over many special cases" (`info.md` §1) turns a
multi-model mess into a single model — which is precisely the "professional, consistent, simple"
the brief and the research both demand.

---

## 16. Prerequisites, phasing, risks

### 16.1 Prerequisites (beyond current state)
- **The project/file system (Doc 1 P-1..P-4)** — hard blocker: a web app is inherently multi-file
  and namespaced. This is shared with Doc 1's "Phase 0" and is the single biggest gate.
- **The rule layer (Doc 1 Phases 1–2)** — attributes + additive rules power every ergonomic in §4.
  Loom can *start* with hand-written registration and adopt attributes as the rules land (graceful
  path: `@Route` is sugar for `router.add`, which works today).
- **`Block` / binary bodies** (`info.md` §6.6.57) — for file uploads, binary responses, keep-alive/
  chunked. Text HTTP works now; binary waits on `Block`.
- **A DB driver over the syscall/stream floor** — the data layer needs a socket-based DB protocol
  (Postgres wire) implemented in-language over the existing TCP streams, *or* the `sysOpen`/file
  floor for an embedded store. The columnar/value-struct storage direction (`info.md` §9) is the
  long game; a wire-protocol client is the near-term path.
- **Native-backend event loop** — the pure ELF backend already emits the event loop/sockets/HTTP
  (`info.md` §17); the framework runs there. (emit-C++ is interpreter-bound for the system layer —
  the framework targets the ELF/interpreter engines.)

### 16.2 Phasing
- **Phase A — Core over what exists today:** routing + controllers + request/response as streams +
  DI (all buildable on current HTTP/stream/`bind` support), with hand-written registration.
- **Phase B — Ergonomics via rules:** `@Route`/`@Inject`/`@Validate`/`@Serialize` once Doc 1 Phase 2
  lands. OpenAPI generation.
- **Phase C — Data layer:** models-as-structs, type-safe query over a DB driver, migrations.
- **Phase D — Views:** template engine, htmx fragments, then the Inertia adapter.
- **Phase E — Auth, observability, uploads (`Block`-gated), polish, scaffolding CLI.**

### 16.3 Risks
| Risk | Mitigation |
|---|---|
| Depends on unbuilt project system + rule layer | Phase A runs on today's features with hand-written wiring; ergonomics layer on incrementally |
| DB access is a large subsystem | start with one wire-protocol client (Postgres) in-language over existing TCP streams; embedded store later |
| Binary/upload needs `Block` | text-first; gate binary features behind `Block`, which is already on the roadmap |
| Template engine as "second language" temptation | reuse the quasiquote/base-language expressions (Doc 1) — templates are type-checked Language, not a new grammar |
| Over-scoping (becoming Django) | "batteries included but modular" — every battery behind an interface + `bind`, excludable (§12) |
| Native-backend coverage of the system layer | framework targets the ELF/interpreter engines that already run sockets/loop; emit-C++ excluded for the system layer, as documented |

---

## 17. Headline recommendations (for the report)

1. **The framework's differentiator is consistency, and the language already owns it.** Adopt the
   existing stream model as the *single* data-flow abstraction — `Promise` = one-shot stream,
   `await` with **no function coloring**, middleware = stream transform, WebSocket = `IOStream`.
   This is the market's loudest unmet want (the "what color is your function" / "streams vs
   observables vs promises vs callbacks" fatigue), and it costs Loom nothing because `info.md` §13
   already decided it.
2. **Ergonomics through the rule layer, not runtime magic.** `@Route`/`@Column`/`@Inject`/`@Validate`
   are typed attributes read by namespace-scoped rules (Doc 1) that emit ordinary code —
   ASP.NET/FastAPI DX with **zero runtime reflection**, compile-time-checked, `--expand`-visible.
   This dodges the Laravel analyzability critique and the Next.js hidden-execution critique at once.
3. **Five collapses (§2/§15):** one data-flow model (stream), one composition model (stream
   transform), one error model (throw + `Result`), one wiring model (`bind`/`inject` + rules), one
   data model (value `struct` = row = DTO = response). Each eliminates 3–6 special-case models other
   frameworks carry.
4. **Data layer = the array/relational surface over value-structs**, with **explicit eager loading
   (N+1 as a diagnostic)**, **compile-time-checked field references (no runtime-typo class)**, a
   **transparent path to raw SQL**, and **diff-generated, conflict-aware migrations** — the web
   surface of the language's DBMS/value-type direction.
5. **Front end: MPA-first, hypermedia-native (htmx-style built in), with an optional Inertia-style
   adapter and NO bundled SPA framework.** "Do I need React? No, but you can." A type-checked,
   base-language template engine is primary; the Inertia adapter is the escape hatch — both reuse the
   same controllers/validation/DI, no separate API, nothing hidden (the Next.js anti-lesson avoided).
6. **Batteries included but modular:** DI, routing, data+migrations, validation, auth, OpenAPI,
   static/uploads, views — all in the box, all behind interfaces + `bind`, all overridable from one
   composition root (`app.ext`). Exclude a bundled SPA framework, a second endpoint model, a runtime
   DI container, lazy-load ORM relations, and any second async/reactive model — one model where one
   applies.
7. **The gating prerequisite is shared with Doc 1: the project/file system must exist first**, then
   the rule layer; the data layer needs an in-language DB client over the existing TCP streams, and
   binary uploads wait on `Block`. Phase A (routing/controllers/DI/streams) is buildable on **today's**
   HTTP + stream + `bind` support with hand-written registration, and adopts the ergonomic attributes
   as the rule layer lands.
