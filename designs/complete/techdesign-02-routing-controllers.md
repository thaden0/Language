# Atlantis Track 02 — Routing & Controllers

**Status:** draft for owner review.
**Date:** 2026-07-07 (commissioned 2026-07-06 with the 00-overview set).
**Depends on:** Track 01 (C2 `Context`/Handler/Middleware surface), metaprogramming
Phases 1–3 (attributes, rules, ctor/member/namespace anchors, `where`, `$for`,
`$_params`/`$_args`), Track 09 types via Track 01 (`HttpRequest`, `HeaderMap`, `json`).
**Owns:** `Atlantis::Routing` (Router, Controller, RouteRec, binding/validation runtime)
+ the routing and validation **attributes and rules in namespace `Atlantis`** per C1/C5:
`Get/Post/Put/Delete/Patch { string path; }`, `RoutePrefix { string path; }`,
`Required {}`, `MaxLen { int n; }`, `MinLen { int n; }`, `Min { int v; }`, `Max { int v; }`,
`Email {}`, `Url {}`. (`Fragment {}` is declared here per C5; Track 09-views consumes it.
`Auth`/`NoAuth` attribute *semantics* are Track 08's; the route-table metadata rules and
the boot check are ours, §6.)

---

## 0. Mission, scope, non-goals

**Mission.** `@Get("/users/:id") User show(int id)` on a controller becomes, at compile
time, an ordinary `router.record(...)` call with a typed handler lambda — the pattern the
corpus already proves (`tests/corpus/meta/rule_routes.ext`, `rule_forward_args.ext`,
byte-identical to the hand-written twin). Zero runtime reflection; every route visible in
`--expand` and greppable in the composition root. The route table is simultaneously the
dispatch structure, the auth-enforcement ledger (R6), and Track 07's OpenAPI source of truth.

**In scope:** route table + segment-trie matching (`:params`, `*wildcard`, precedence,
404/405), the per-verb attributes and their rules (R7), `@RoutePrefix` composition,
`Controller` base + the explicit mounting story, parameter binding (path/query/body/header),
the C5 validation attributes → `ValidationException` → 422, the `authDefault` boot check,
and the manual `router.record(...)` escape hatch.

**Non-goals:** auth *middleware* (Track 08); JSON codegen internals (`FromJson`/`toJson`
are consumed via C7, owned by Track 03); OpenAPI emission (Track 07 reads our RouteRec);
views/fragments rendering (Track 09-views); `@Pattern` validation (regex — LA-13,
`request-regex.md`); multipart/form binding (LA-14; URL-encoded forms arrive with it);
**bare `@Get` free functions** — Loom §5 allowed attribute routes on free functions "as the
same rule with namespace scope"; that is a second endpoint model in embryo (STOP (c)) and a
worse OpenAPI story (no controller grouping). Atlantis v1: controllers only.

**vs Loom (summary; details inline):** Loom's `@Route("GET", "/users")` makes the verb a
string — a `"GTE"` typo dispatches nothing and fails silently at runtime. Per-verb
attributes (R7) make it an unknown-attribute **compile error**. Loom *asserted*
"extraction is generated code, so mismatches are compile errors" without a mechanism;
this doc specifies the exact splice mechanics, what is compile-time vs 400/boot-time
today, and the metaprog asks that close the gap (§8). Loom had no 405 semantics, no
precedence rule, no mounting story, no auth-default boot check, and no OpenAPI-ready
route record.

---

## 0R. R10 revision (2026-07-07) — explicit-first wiring. READ BEFORE §2/§3.2/§7.

The owner authored `designs/atlantis/example/` and adopted it as the canonical surface
(overview ruling R10). For this track that **inverts the wiring primacy**: the explicit
fluent route table is the spine; attributes are the optional layer that splices INTO it.
Constructor-side-effect registration (§3.2's "constructing a controller IS mounting") is
**retired** — it was this design's most magical piece, and the splice model is more
aligned with the language's explicit-over-implicit ethos. §1 (trie/dispatch/RouteRec),
§4 (binding), §5 (validation), and §6 (authDefault) are **unchanged** — this revision
changes how RouteRecs are *created*, not what they are or how they dispatch.

### 0R.1 The canonical surface (from the example, normalized to language spelling)

```
uses Atlantis;
namespace App::Routes {
    public void addRoutes(Atlantis::App app) {
        @InjectRoutes();                                   // attribute routes land HERE (LA-22)
        app.AddRoute(Atlantis::Routing::Route('/',       App::Controllers::HomeController::Index)
                        .requestType(RequestType::GET));
        app.AddRoute(Atlantis::Routing::Route('/login',  App::Controllers::AuthController::Login)
                        .requestType(RequestType::GET | RequestType::POST).noAuth());
        app.AddRoute(Atlantis::Routing::Route('/logout', App::Controllers::AuthController::Logout)
                        .requestType(RequestType::GET).requireAuth());
    }
}
```

### 0R.2 The fluent `Route` builder

```
namespace Atlantis::Routing {
    class Route {
        // LA-25 form (canonical): unbound method reference + generated factory
        new Route(string path, /* method reference */ handler);
        // Interim form (works today): explicit factory + invoker lambda
        new Route(string path, () => Controller factory, (Controller, Context) => HttpResponse invoke);

        Route requestType(RequestType t);     // GET|POST flags; default GET
        Route requireAuth();  Route noAuth(); // GuardSpec "auth" / "noauth"
        Route auth(string role);              // GuardSpec "role:<r>" / "policy:<p>"
        Route name(string n);                 // route name (OpenAPI operationId, url helpers)
        Route fragment();                     // Track 09 @Fragment equivalent
    }
}
```

`App.AddRoute(route)` lowers the builder into the **same RouteRec** §1 defines (path,
verbs, GuardSpec, ParamDescs, handler thunk) — Track 07's OpenAPI/MCP consumption is
unaffected. `RequestType` is flag-shaped and needs Track 03's `enum` + `(|)` (now on the
critical path — overview §2); interim: `comptime int` constants under
`Atlantis::Routing::RequestType` with the same spelling.

### 0R.3 Controller activation: per-request, from a factory

A Route holds a controller **factory** plus an unbound method reference (LA-25;
`AuthController::Login` ≡ `(AuthController, ILoginRequest) => LoginResponse`). Dispatch:
construct the controller via the factory (its ctor params fill by DI — explicit `inject`
until bug.md #24 lands), bind params (§4), apply the reference. Fresh controller state
per request (the ASP.NET model), no long-lived mounted instances, nothing keeping
closures alive in the router. Until LA-25 lands, the interim Route overload takes the
factory + invoker lambda explicitly; until LA-22 lands, the `@InjectRoutes();` line is
simply omitted (explicit-only — fully functional).

### 0R.4 What happens to the attribute path (§2 revised, not deleted)

Verb attributes keep their exact C5 shapes and the same match clauses — only the **anchor
and payload change**: rules now emit `app.AddRoute(Route($r.path, C::m-reference or
factory+lambda).requestType(...).<guard>())` statements **`at splice InjectRoutes`**
(LA-22) instead of `record(...)` calls at ctor-bottom. Consequences: the generated wiring
lands in the app's own table function, greppable next to the hand-written routes;
`--expand` on `Config/Routes.lev` shows the *entire* route table in one place; a file's
attributes only wire if the app placed the splice — the extension point is opt-in and
visible. `@RoutePrefix` composes at expansion (prefix + method path), dropping §2's
ctor-top `__prefix` mechanism.

### 0R.5 Boot flow (replaces §3.2's `buildRouter`)

`main.lev`'s `Main` (Track 04 §R10): `Builder()` → `Build()` → `app.useX()` stages →
`App::Routes::addRoutes(app)` → `App::AddBindings()` → `app.finalize(cfg.authDefault)`
(R6 boot checks unchanged — `explicit` mode = every Route must carry `.requireAuth()`,
`.noAuth()`, or an auth attribute) → serve. `Controller` base (§3.1) survives as the
helper/response surface minus the `Router` field and base-ctor registration duty; its
DI-by-ctor story is unchanged.

---

## 1. The Router — table, trie, dispatch

### 1.1 The registration record (frozen for Track 07)

`RouteRec` is pure data (a value struct) so Track 07 can iterate it without touching
dispatch internals. The handler rides beside it, not inside it.

```
namespace Atlantis::Routing {
    struct ParamDesc {
        string name;          // method parameter name
        string type;          // canonical type string ("int", "string", "App::NewUser", "Query<Filter>", "Atlantis::Http::Context")
        string source;        // "" at record time; finalize() classifies: "path" | "query" | "body" | "header" | "context"
    }
    struct RouteRec {
        string verb;              // "GET" | "POST" | "PUT" | "DELETE" | "PATCH"
        string path;              // full path incl. @RoutePrefix, e.g. "/api/v1/users/:id"
        string controller;        // "App::Controllers::UserController" (spliced $C.name)
        string action;            // "show" (spliced $m.name)
        Array<ParamDesc> params;  // handler param descriptors (spliced via array-literal $for — proven shape)
        string returnType;        // canonical return type string (OpenAPI response schema seed)
        int authMode;             // 0 = unset, 1 = auth, 2 = noauth (int until enum, LA-8)
        string authRole;          // "" unless @Auth("role")
        string key() => controller + "." + action;
    }
    // THE handler shape stored in the table — C2's Handler, always:
    //   (Atlantis::Http::Context) => HttpResponse
}
```

Everything Track 07 needs is here: path, verb, param descriptors (name/type/source),
return type, auth marker, controller/action grouping. `@Summary`/`@Tool` are Track 07's
own attributes matched by Track 07's own rules — they key into this table by `key()`.

### 1.2 Router surface

```
namespace Atlantis::Routing {
    class Router {
        Router record(RouteRec rec, (Atlantis::Http::Context) => Atlantis::Http::HttpResponse h);
        void markAuth(string controller, string action, int mode, string role);   // §6; order-independent
        void finalize(string authDefault);       // boot checks (§1.5, §6); freezes the table, builds the trie
        MatchResult resolve(string verb, string path);   // 200-with-key | 404 | 405-with-allow
        (Atlantis::Http::Context) => Atlantis::Http::HttpResponse handlerOf(string key);
        RouteRec recOf(string key);
        Array<RouteRec> routes();                // Track 07 / `./app routes` iteration
        int authModeOf(string key);  string authRoleOf(string key);   // Track 08 handshake
    }
    struct MatchResult {
        int status;                    // 200 | 404 | 405
        string key;                    // controller.action when 200
        Map<string, string> params;    // decoded :param captures when 200
        string allow;                  // "GET, POST" when 405
    }
}
```

### 1.3 Segment trie, precedence, 404/405

Path split on `/`; each node holds `Map<string, Node> statics`, `Node? param`,
`Node? wildcard`, and at terminals `Map<string, string> byVerb` (verb → route key).
Matching is depth-first with **backtracking**, trying children in precedence order
**static > `:param` > `*wildcard`** at every level — so `/users/new` beats `/users/:id`,
and `/files/*rest` only fires when nothing more specific survives. `*rest` is legal only
as the final segment (boot error otherwise) and captures the remaining path joined by `/`.

- Node found, verb present → 200; `MatchResult.params` filled with percent-decoded
  captures (Track 09 `encoding`), param names recovered by re-splitting the winning
  route's `path` (names live per-route, so `/a/:id` and `/a/:name` can share a trie node).
- Node found (any verb terminal at that path), verb absent → **405** with `allow` = the
  sorted verb list (the kernel emits the `Allow` header). `HEAD` resolves via the `GET`
  entry (kernel strips the body). vs Loom: Loom had no 404/405 distinction at all.
- No node → **404**.
- Trailing-slash policy: `/users/` normalizes to `/users` at both record and resolve time
  (documented, boring, no redirect magic in v1).

### 1.4 Two-phase dispatch (the Track 08 handshake)

Auth middleware must see the matched route *before* the handler runs; C2's `Context.items`
is `Map<string, string>`, so we pass the route **key**, not an object:

1. **`Routing::resolveMiddleware(router)`** (C2 Middleware shape) runs early in the
   pipeline: calls `resolve()`, short-circuits 404/405, else fills `ctx.routeParams`
   (from `MatchResult.params`) and `ctx.items["atlantis.route"] = key`, then calls `next`.
2. Track 08's auth middleware (between resolve and terminal) reads the key and asks
   `router.authModeOf(key)` / `authRoleOf(key)`, combining with `authDefault` (§6).
3. **`Routing::dispatch(router)`** is the terminal handler: `router.handlerOf(
   ctx.items["atlantis.route"])` — an O(1) map hit, one trie walk per request total.

Both pieces are ordinary C2 shapes; the composition root places them explicitly. No
second composition model.

### 1.5 `finalize()` boot checks (fail-fast, R6-adjacent)

Run once in `App::run` before listening. Boot **errors** (throw, process exits):
duplicate `verb+path`; `*wildcard` not final; `authDefault == "explicit"` and any
`authMode == 0` (§6); a route whose `path` has `:seg` with no same-named param descriptor
of type `int`/`string` **or vice versa** (Era-B signatures, §4 — in Era A params are
`(Context)` and the check is skipped for that route); `markAuth` key matching no route
(stray `@Auth` on a method with no verb attribute). Also classifies `ParamDesc.source`
(§4.3) — boot-time string inspection of descriptors, never per-request.

---

## 2. Attributes and rules (the compile-time wiring) — **[R10: anchor/payload revised by §0R.4; match clauses and C5 shapes stand]**

### 2.1 Declarations (namespace `Atlantis`, per C1/C5)

```
namespace Atlantis {
    attribute Get    { string path; }
    attribute Post   { string path; }
    attribute Put    { string path; }
    attribute Delete { string path; }
    attribute Patch  { string path; }
    attribute RoutePrefix { string path; }
    attribute Fragment {}                       // declared per C5; Track 09-views consumes
    // validation set — §5
    attribute Required {}   attribute MaxLen { int n; }   attribute MinLen { int n; }
    attribute Min { int v; }  attribute Max { int v; }  attribute Email {}  attribute Url {}
}
```

### 2.2 The verb rules (five near-twins; a rule matches one attribute)

Era-A template (works on today's proven mechanisms; Era-B body in §4.4):

```
namespace Atlantis {
    rule routesGet {
        match @Get(r) on method m in class C : Atlantis::Routing::IController
        inject `this.router.record(
                    Atlantis::Routing::RouteRec("GET", this.__prefix + $r.path,
                        $C.name, $m.name,
                        [ $for p in m.params : Atlantis::Routing::ParamDesc($p.name, $p.type, "") ],
                        $m.returnType, 0, ""),
                    ($_params) => this.$m($_args))`
            at bottom of C.constructor
    }
    // routesPost / routesPut / routesDelete / routesPatch: same shape, verb string differs.
}
```

Mechanism inventory, mapped to proof status:
- `$r.path` (own attribute's arg) — **proven** (`rule_routes.ext`).
- `($_params) => this.$m($_args)` handler lambda at ctor bottom — **proven**
  (`rule_forward_args.ext`, incl. the `Lower.cpp` this-capture fix; oracle==IR==ELF).
- `$C.name` / `$m.name` / `$m.returnType` as *string values* in template expression
  position — the Phase-3 §2 unified-Binding design says decl bindings materialize their
  `meta.*` object and value-holes fold field chains; **P-probe P2** confirms.
- `[ $for p in m.params : … ]` array-literal splice — `$for` over `C.fields` is proven
  (`rule_orm`); iteration over `m.params` + nesting inside a call argument is **P4**.
- Constraint `: Atlantis::Routing::IController` where the user class writes
  `: Controller` (transitive base) — corpus only proved a *direct* interface base;
  **P1** probes transitivity. Insurance: `Controller` implements `IController` and docs
  tell users to extend `Controller`; if the matcher is direct-only, the rule constraint
  becomes `: Atlantis::Routing::Controller` (class-base constraint — also in P1) and
  deep controller hierarchies wait on a matcher fix (bug.md, not a hack).

In Era A the method signature must be `(Context) => HttpResponse`; anything else makes
the injected `record(...)` call a **type error at the use site** (loud, correct — a
`where`-predicate would *silently skip* the route instead, which is the worst failure
mode; we deliberately do not gate the rule on signature).

### 2.3 `@RoutePrefix` — class-level, composed via ctor-top

A rule matches one attribute, and field iteration cannot read other attributes' args
(LA-4) — so the verb rule cannot see the prefix. It doesn't need to: a **second rule**
sets a base-class field at **ctor top**, and ctor-top insertions run before ctor-bottom
appends (Phase-3 anchor discipline — probed as **P3**):

```
rule routesPrefix {
    match @RoutePrefix(p) on class C : Atlantis::Routing::IController
    inject `this.__prefix = $p.path;` at top of C.constructor
}
```

`__prefix` lives on `Controller` (default `""`), so unprefixed controllers need nothing.
Composition happens at construction time in ordinary code — greppable in `--expand`.
vs Loom: same attribute name, but Loom never said *how* prefix reaches the method routes.

### 2.4 Auth-marker rules (route-table side of R6; middleware is Track 08's)

```
rule marksAuth {
    match @Auth(a) on method m in class C : Atlantis::Routing::IController
    inject `this.router.markAuth($C.name, $m.name, 1, $a.role);` at bottom of C.constructor
}
rule marksNoAuth {
    match @NoAuth on method m in class C : Atlantis::Routing::IController
    inject `this.router.markAuth($C.name, $m.name, 2, "");` at bottom of C.constructor
}
```

Keyed by `controller.action`, **order-independent** w.r.t. the verb rule's `record(...)`
call on the same method — `markAuth` stages into a pending map; `finalize()` joins it
onto `RouteRec.authMode/authRole` and errors on stray keys. This deliberately avoids
depending on cross-rule firing order (a foreseeable-problem row regardless).

---

## 3. `Controller` base and the mounting story

### 3.1 The base class

```
namespace Atlantis::Routing {
    interface IController { }        // the rules' match constraint (cheap, stable)
    class Controller : IController {
        Router router;
        string __prefix = "";
        new Controller(Router r) { router = r; }

        // response helpers (all RETURN; throwing is the C4 channel, also fine)
        HttpResponse ok(json::JsonValue v);          // 200, application/json
        HttpResponse okText(string body);            // 200, text/plain
        HttpResponse created(string location, json::JsonValue v);   // 201 + Location
        HttpResponse noContent();                    // 204
        HttpResponse redirect(string to);            // 303 + Location
        HttpResponse notFound(string message);       // 404 (or `throw NotFoundException` — same C4 result)

        // Era-A binding helpers (§4.2) — all read C2 Context, all fail loud:
        int pathInt(Context ctx, string name);       // 400 HttpException on missing/unparsable
        string pathStr(Context ctx, string name);
        string? queryStr(Context ctx, string name);  int? queryInt(Context ctx, string name);
        string? header(Context ctx, string name);
        json::JsonValue jsonBody(Context ctx);       // 415 wrong content-type, 400 parse failure
    }
}
```

A derived controller runs the base ctor explicitly (reference §4.5) and takes services
by constructor DI — **no per-request service magic**; request scope is the `Context` (H-5):

```
uses Atlantis;
namespace App::Controllers {
    @RoutePrefix("/api/v1")
    class UserController : Atlantis::Routing::Controller {
        IUserService users;
        new UserController(Atlantis::Routing::Router r, IUserService svc) {
            Controller::Controller(r);
            users = svc;
        }   // ← verb rules inject record(...) calls here, after user code (ctor-bottom)

        @Get("/users/:id")
        HttpResponse show(Atlantis::Http::Context ctx) {          // Era A shape
            int id = this.pathInt(ctx, "id");
            return this.ok(users.get(id).toJson());
        }
    }
}
```

### 3.2 Mounting — explicit composition root, zero scanning — **[R10: RETIRED — per-request activation per §0R.3/§0R.5; kept for the record]**

Constructing a controller **is** mounting: the injected ctor-bottom `record(...)` calls
register the routes, and the handler lambdas capture `this`, so the router's table keeps
every controller alive — no registry, no classpath scanning, no reflection.
`config/routes.lev` is the one greppable place:

```
namespace App::Boot {
    Atlantis::Routing::Router buildRouter() {
        bind App::Boot::services();           // Bindings object from config/ioc.lev (Track 04 pattern)
        Atlantis::Routing::Router r = Atlantis::Routing::Router();
        App::Controllers::UserController(r);  // ctor self-registers; closure keeps it alive
        App::Controllers::HealthController(r);
        // escape hatch (§7) lives here too
        return r;
    }
}
```

DI is **lexical** — the `bind` install must be in scope at the construction site, hence
the `bind services();` line at the top of `buildRouter`, not in a far-away `main`.
`main.lev` calls `buildRouter()`, then `router.finalize(cfg.authDefault)`, then folds the
pipeline (`[resolveMiddleware(r), …auth…, …] → dispatch(r)`) and hands it to `App::run`.
Forgetting to construct a controller = its routes 404 = visible in `./app routes` output
(which prints `router.routes()`); this is the honest, explicit trade against scanning.

---

## 4. Parameter binding

Two eras, per R4: **Era B** is the design (assuming the §8 metaprog asks land — the
tickets exist or are proposed there); **Era A** is the shipping interim, fully buildable
on today's proven mechanisms. Both keep the table's handler type uniform (C2's Handler).

### 4.1 The binding menu (both eras; frozen semantics)

| Source | Signature form | Mechanism | Failure |
|---|---|---|---|
| Path | `int id`, `string slug` matching `:id`/`:slug` | `ctx.routeParams` + strict parse (`toInt()`) | 400 |
| Query | `Query<Filter> q` (Era B) / `queryStr` helpers (Era A) | query map → lenient-scalar `JsonValue` → `Filter::FromJson` (C7) | 400 |
| Body | one DTO struct param (Era B) / `jsonBody` + explicit `Dto::FromJson` (Era A) | parse body → validate (§5) → `Dto::FromJson(v)` | 415 / 400 / **422** |
| Header | Era A helper `header(ctx, "X-Request-Id")`; Era B `@FromHeader` **deferred** (needs LA-4-class attr-arg reads in binding position) | `ctx.request.headers.first(n)` | optional → handler decides |
| Services | constructor DI only | `bind`/`inject` (Track 04) | compile error |
| Context | `Atlantis::Http::Context` param | passed through | — |

### 4.2 Era A (today): uniform signatures, helper binding

Handler methods are `(Context) => HttpResponse`; the proven
`($_params) => this.$m($_args)` lambda *is* the stored Handler with no adaptation layer.
Binding is 1–3 helper lines in the body (§3.1 example). Honest costs: binding mistakes
(`pathInt(ctx, "idd")`) surface at **boot** (finalize's `:seg`-coverage check can't see
helper calls) or as a 400 at first hit — this is exactly the gap Era B closes, and why
Era B is the AG-2 headline.

### 4.3 Compile-time vs boot vs runtime (the honest table Loom never drew)

| Mismatch | Era A | Era B |
|---|---|---|
| Handler wrong shape / unknown type | compile error (record call typecheck) | compile error |
| DTO lacks `FromJson` (C7) | compile error at the explicit call | compile error at the spliced call |
| Return type not `HttpResponse`/`toJson()`-bearing/`void` | compile error | compile error (no `respond` overload) |
| `:id` in path, no matching param (or reverse) | not detectable | **boot error** (finalize, §1.5) |
| Route param name typo in helper call | 400 at first hit | impossible (generated from names) |
| Client sends `"abc"` for `int id` | 400 | 400 |
| Body fails validation | 422 (§5) | 422 |
| Wrong/missing content type | 415 | 415 |

### 4.4 Era B (target): typed signatures, generated binding prologue

`@Get("/users/:id") User show(int id) => users.get(id);` — the verb rule's template
grows a statement prologue that declares one local per method param, then reuses the
**proven** `$_args` splice (the locals carry exactly the param names):

```
inject `this.router.record(<RouteRec as §2.2>,
        (Atlantis::Http::Context __ctx) => {
            $for p in m.params :
                $if (p.type == "int")    : int $p.name = Atlantis::Routing::Bind::pathInt(__ctx, $p.name);
                $if (p.type == "string") : string $p.name = Atlantis::Routing::Bind::pathStr(__ctx, $p.name);
                $if (p.type == "Atlantis::Http::Context") : Atlantis::Http::Context $p.name = __ctx;
                $if (p.type.startsWith("Query<")) : $p.type $p.name = Atlantis::Routing::Bind::query...;
                $else : $p.type $p.name = $p.type::FromJson(
                            Atlantis::Routing::Bind::validatedBody(__ctx, $p.type.name));
            $if (m.returnType == "void") : { this.$m($_args); return Atlantis::Routing::noContentResp(); }
            $else : return Atlantis::Routing::respond(this.$m($_args));
        })`
    at bottom of C.constructor
```

`respond` is an ordinary overload set (overloading by argument types is core language):
`respond(HttpResponse) => passthrough`, `respond(IToJson v) => 200 json` — DTOs and
entities qualify via C7's `toJson()` behind an `IToJson` interface (coordination item
with Track 03: `@Serializable` should also stamp `: IToJson`; struct-implements-interface
is core language). **Required metaprog features** (all compile-time, all P4-family —
§8): statement-position `$for` (LA-6), `$if` conditional splice, and identifier/type-
position splicing of comptime strings (`$p.name` as a declared local name, `$p.type` as
a type and as `$p.type::FromJson`'s callee — the DTO type is *concrete at expansion
time*, so no generic-factory machinery is needed). Everything else in the template is
already proven or probed (P2/P4). Era B is cost-identical to the hand-written adapter —
the twin-file discipline (rule_routes_twin style) is the acceptance gate.

---

## 5. Validation (C5 attributes → `ValidationException` → 422)

Validation is **JSON-shape checking against a compile-time descriptor table**, run on the
parsed body `JsonValue` *before* `FromJson` — so "missing required field" is a clean 422
with field errors, never a `FromJson` explosion. Zero per-request registration cost;
descriptors are built once.

**Check semantics** (v1, string-method validators per LA-13 fallback): `Required` = key
present and non-null; `MinLen`/`MaxLen` = string length bounds; `Min`/`Max` = int bounds;
`Email` = contains `@` with a `.` after it (documented heuristic until regex);
`Url` = `startsWith("http://") || startsWith("https://")`. `@Pattern` deferred
(`request-regex.md`). Failure builds `Array<FieldError>` (`struct FieldError { string
field; string rule; string message; }` — proposed to live beside `ValidationException`
in `Atlantis::Http`, Track 01 coordination) and throws `ValidationException` (C4) → 422.

### 5.1 Target design (assuming LA-4 attribute-value reflection — R4)

One class-level rule, firing once per DTO, generates the descriptor function into a
namespace table (the `rule_ns_inject.ext` proven pattern — `$D` as the generated
function's name is exactly corpus `$C`):

```
rule genChecks {
    match struct D                              // attr-less match — probe P10; fallback: match @Serializable
        where D.fields.any((f) => f.attrs.contains("Required") || f.attrs.contains("MaxLen") || …)
    inject `Array<Atlantis::Routing::Check> $D() => [
                $for f in D.fields.where((x) => x.attrs.length() > 0) :
                    Atlantis::Routing::Check($f.name,
                        [ $for a in f.attributes : Atlantis::Routing::CRule(a.name, a.argIntOr(0, 0)) ])
            ];` at namespace Atlantis::Checks
}
```

(`f.attributes`/`argIntOr` is LA-4's requested surface — exact spelling is the metaprog
owner's.) Era B's `Bind::validatedBody` calls `Atlantis::Checks::<D>()` via the same
type-name splice as `FromJson` and validates before constructing. **Priority note:** the
LA-4 ticket (`request-metaprog-attr-values.md`) is currently framed as AG-4/ORM-driven
(2026-11-10); Track 02 wants it for AG-2 (2026-10-05) — an escalation for the owner,
recorded in §8.

### 5.2 Interim design (today): per-attribute-match registration, boot-time

Each match *can* read its own args — seven small rules (one per C5 validation attribute)
inject a **registration statement** into a namespace anchor. Statements have no names, so
per-field firing cannot collide (the flaw that kills member-of-D and named-item designs):

```
rule vMaxLen {
    match @MaxLen(v) on field f in struct D
    inject `Atlantis::Routing::Validation::register($D.name, $f.name,
                Atlantis::Routing::CRule("MaxLen", $v.n));` at namespace Atlantis::Checks
}
// vRequired / vMinLen / vMin / vMax / vEmail / vUrl: same shape ($v-less kinds pass 0)
```

Every splice is a value-hole fold (`$D.name`, `$f.name` — meta objects; `$v.n` — own
attr): no identifier splices needed. **Load-bearing probes:** P6 — namespace-anchored
*executable statements* parse (`parseItemsFragment` → `parseTopLevelItem`, which does
accept statements at file scope) and run at program start; injected `namespace … { }`
blocks are appended to `program.items`, so they execute **after** hand-written top-level
code — fine, because lookups happen per-request (event-loop callbacks run after all
top-level code) and `finalize()` does not read the validation table. P8 — `in struct D`
encloser (corpus proved `in class C` only). Interim invocation: Era A handlers call
`Atlantis::Routing::Validation::checkByName("App::NewUser", body)` explicitly before
`FromJson`; flips to the generated per-D function the day LA-4 lands.

**Rejected alternatives** (recorded so nobody re-derives them): ctor-bottom check
injection into DTO ctors — per-construction cost, breaks benign invalid construction in
tests, and reaching Track 03's *rule-generated* `FromJson` ctor depends on cross-rule
expansion order (fragile — P7 documents the question but nothing depends on it);
member-of-D generated members — member names can only be a single hole (`$f`), colliding
when one field has two constraint attributes.

---

## 6. `authDefault` enforcement (R6)

Setting: `authDefault = auth | noauth | explicit` (Track 04 config; framework default
`auth`). Ownership split per overview: Track 08 owns the guard middleware; **Track 02
owns the metadata and the boot check**. Mechanics: §2.4 marker rules → `markAuth` staging
→ `finalize(authDefault)` joins onto `RouteRec.authMode` and enforces:
- `explicit`: any route with `authMode == 0` → boot error listing every offender
  (fail-fast at startup — absence isn't matchable at compile time, per R6's own ruling).
- `auth`/`noauth`: `authMode == 0` routes inherit the default; effective mode is
  queryable per key (`authModeOf`), which is what Track 08's middleware consumes (§1.4).
- Stray marker (`@Auth` on a method with no verb attribute) → boot error.
vs Loom: Loom §11 mentioned auth attributes; it had no default-stance setting and no
boot-time completeness check at all.

---

## 7. Escape hatch — manual `router.record(...)` — **[R10: INVERTED — the explicit table is now primary (§0R.1); raw `record()` remains available beneath the Route builder]**

Same record, same handler shape, no attributes — for the odd dynamic case (a path only
known from config, a quick internal endpoint), written in `config/routes.lev`:

```
r.record(Atlantis::Routing::RouteRec("GET", "/healthz", "Manual", "healthz",
             [], "Atlantis::Http::HttpResponse", 2, ""),        // authMode 2 = noauth, explicit
         (Atlantis::Http::Context ctx) => Atlantis::Routing::respond(healthBody()));
```

It flows through the same finalize checks, auth ledger, `./app routes`, and Track 07
OpenAPI emission (controller "Manual" groups them). One model, zero magic.

---

## 8. Language asks discovered by this track (for the LA register — owner to file/dispose)

| Proposed ask | What | Consumers | Interim fallback |
|---|---|---|---|
| **LA-6 priority raise** (already registered; `request-metaprog-attr-values.md` item B) | statement-position `$for` | Era B binding prologue (§4.4) | Era A helper binding |
| **NEW: `$if` conditional splice** (proposed ticket `request-metaprog-route-binding.md`, to be filed with LA-6's raise) | comptime-conditional element/statement selection inside templates, predicate over bindings (`$if (p.type == "int") : … $else : …`) — the template twin of `comptime if` | Era B per-param type dispatch (§4.4) | Era A |
| **NEW: identifier/type-position splicing of comptime strings** (same proposed ticket) | a folded string value usable as a declared local name (`$p.name`), a type (`$p.type`), and a qualified callee (`$p.type::FromJson`) — types are concrete at expansion, so this replaces any generic-factory machinery | Era B (§4.4), validation invocation (§5.1) | Era A |
| **LA-4 priority raise** (`request-metaprog-attr-values.md` item A) | attribute-value reflection wanted by **AG-2 (2026-10-05)**, not just AG-4 | §5.1 target validation | §5.2 interim rules |
| *(noted, not asked)* ctor-requirements in interfaces (`T::FromJson` on type params) | would enable generic binding helpers | 07 typed clients mostly | type-position splice covers Track 02's need |

Per R9 these go through the owner — this doc designs assuming they land (R4) and ships
Era A / §5.2 until then. No compiler work happens in this track (STOP (b)).

---

## 9. P-probes (run against `build/leviathan --expand` BEFORE feature work; failures with a metaprog-shaped cause → /bug.md or LA, never a hack)

| # | Probes | Pass looks like |
|---|---|---|
| **P1** | Verb attribute + rule with constraint `: IController` where the class inherits it **transitively** through `Controller`; also a *class* (not interface) constraint; two different verb attrs on two methods of one class, five rules loaded | both records injected; transitive base matches (else: fallback per §2.2, file bug) |
| **P2** | `$C.name`, `$m.name`, `$m.returnType`, and (field subject) `$f.name` as string arguments in a ctor-bottom template | `--expand` shows folded string literals; runs oracle==IR==LLVM |
| **P3** | `@RoutePrefix` rule at ctor **top** + verb rule at ctor **bottom** on one class | expansion order: `__prefix` assignment precedes every `record(...)`; injected code may assign a **base-class** field |
| **P4** | `[ $for p in m.params : ParamDesc($p.name, $p.type, "") ]` nested as a call argument inside the ctor-bottom `record(...)` template, alongside `$_params`/`$_args` in the same template | descriptor array materializes per method; typed-param forwarding still byte-identical to a twin |
| **P5** | Two rules (`routesGet` + `marksAuth`) both firing ctor-bottom on the same method | deterministic order across runs (we don't *depend* on which — §2.4 — but nondeterminism is a bug to report) |
| **P6** | Rule injecting an **executable statement** at `namespace Atlantis::Checks` (the §5.2 registration) | parses via items fragment; statement runs at program start; observed ordering vs hand-written top-level code logged |
| **P7** | Ctor-bottom anchor on a struct with (a) a source-written labeled ctor, (b) only a default ctor, (c) a ctor injected by an earlier rule | documents exactly which ctors receive injections (informational — §5.2 depends on none of them) |
| **P8** | `match @MaxLen(v) on field f in struct D` — struct as encloser kind | fires per field; `$v.n` reads |
| **P9** | Era A twin test: full `UserController` with rules vs hand-written `record(...)` calls | byte-identical output on oracle + IR + LLVM (the rule_routes_twin discipline at Atlantis shape) |
| **P10** | Attribute-less `match struct D where …` (RuleMatch's attr pattern is optional in the AST) | parses and fires; else §5.1 falls back to `@Serializable`-gated matching |

---

## 10. Foreseeable problems

| # | Problem | Mitigation |
|---|---|---|
| 1 | Decl-binding value-holes (`$m.name`) unimplemented in template position despite the unified-Binding design | P2 first; if it fails → /bug.md with repro (it's designed behavior per Phase 3 §2/§3); RouteRec is blocked on it — schedule buffer before M2 |
| 2 | Transitive-base constraint matching absent (corpus proved direct bases only) | P1; fallback constraint on `Controller` + "extend Controller directly" v1 rule; bug filed for transitivity |
| 3 | Cross-rule firing order at one anchor is engine-internal | Never depend on it: auth markers keyed + joined at finalize (§2.4); P5 only checks determinism |
| 4 | Injected namespace statements run **after** hand-written top level (appended to `program.items`) | §5.2 analysis: request-time lookups only; finalize never reads the validation table; documented loudly |
| 5 | `Context.items` is `Map<string,string>` — can't carry the matched route object | Route-**key** handshake + O(1) `handlerOf` (§1.4); no C2 change requested |
| 6 | `where`-gated rules skip **silently** — a gated route just vanishes | Rules never gate on signature; wrong signatures fail the injected call's typecheck (loud) — §2.2 |
| 7 | Trie backtracking corner cases (`/a/:x/c` vs `/a/b/*rest`) | Precedence corpus with a table-driven test file; backtracking DFS specified §1.3 |
| 8 | Controllers constructed but router discarded / never finalized | `App::run` refuses to serve an unfinalized router (boot error) |
| 9 | Era B asks slip past 2026-09-10 | Era A ships AG-2 with helpers + interim validation; escalation logged (not a silent workaround) — see §12 |
| 10 | Query lenient-scalar coercion surprises (`?n=007`) | Documented: query scalars parse as int only when the whole segment is digits; strings otherwise; `Query<T>` may slip to post-AG-2 without blocking the gate |
| 11 | Percent-decoding / UTF-8 in path params | Decode via Track 09 `encoding` at resolve time only (raw kept in `request`); byte-clean strings carry the rest |
| 12 | Rules fire only in files that `uses Atlantis` — a controller missing the import silently registers nothing | `./app routes` diff discipline + the engine's dangling-attribute warning; doc the failure mode in the scaffold README |

---

## 11. Milestones & acceptance (aligned to AG-2, 2026-10-05)

| M | Target | Done = |
|---|---|---|
| **M1** | 2026-08-10 | P1–P10 run and logged (Implementation log); attribute+rule skeleton compiles against a stub Context (Track 01 may still be landing); probe fallout filed |
| **M2** | 2026-09-01 | Router core: trie + precedence + 404/405 + finalize checks + escape hatch + `routes()`; table-driven match corpus green on oracle/IR/LLVM |
| **M3** | 2026-09-15 | Era A end-to-end on the kernel (AG-1): controllers mount via `config/routes.lev`, prefix composition, auth markers + `authDefault` boot check (all three modes), two-phase dispatch; twin test (P9) byte-identical |
| **M4** | **2026-10-05 (AG-2)** | Bound + validated params: **Era B if §8 asks land by 2026-09-10**, else Era A helpers + §5.2 interim validation; `--expand` review of a demo controller checked into the demo app; 422 field-error corpus; secure-by-default demo (authDefault=auth + stub guard from 08) |

Acceptance evidence per overview C8: runnable programs under `packages/atlantis/tests/`,
each with expected output, run on oracle + IR + LLVM.

---

## 12. STOP conditions (per overview §0.4 — STOP, log, commit WIP, escalate; never improvise)

- Any probe P1–P10 fails in a way that invites "just parse the type string at runtime" or
  any runtime type/attribute introspection — STOP (a): the fix is a bug report or an LA.
- Temptation to patch the rule engine / parser to make a template work — STOP (b).
- Any second endpoint or composition model (free-function routes, a second middleware
  shape for auth, a per-request DI container) — STOP (c)/(d).
- C5 attribute names/fields, the RouteRec field set (Track 07 depends on it), or the C2
  handshake of §1.4 needing a change — STOP (e): escalate, don't drive-by edit.
- 2026-09-10 arrives without the §8 asks and someone proposes shipping AG-2's "typed
  binding" via stringly runtime dispatch — STOP: AG-2 ships Era A honestly instead, with
  the gap logged against the tickets.
- A language/compiler bug (e.g. P2/P5 failures) — /bug.md with minimal repro + proposed
  ruling; never fixed here — STOP (h).

---

## 13. Implementation log (append-only)

- 2026-07-07 — Track 02 design authored (this doc). No implementation yet. Probes P1–P10
  defined but not run. New asks recorded in §8 pending owner filing/disposition
  (`request-metaprog-route-binding.md` proposed; LA-4/LA-6 priority raises proposed).
- 2026-07-07 (coordinator) — Asks FILED: §8's proposal became
  `designs/request-metaprog-splices.md` (LA-16 here = `$if` + identifier/type-position
  splicing); LA-4/LA-6 priority raises recorded in overview §2. Contract rulings binding
  on this track (overview log 2026-07-07): (1) RouteRec is canonical and gains two
  additive fields Track 07 consumes — `bodySchema` (JsonValue-shaped schema ref for the
  body DTO, filled by the schema rule when present) and `summary` (from `@Summary`);
  (2) `authMode`/`authRole` normalize at `finalize()` to Track 08's single GuardSpec
  string `noauth | auth | role:<r> | policy:<p>` — the route table stores the GuardSpec;
  (3) the `IToJson` marker this doc references is named `IJsonSerializable` (Track 03
  §16) and is MANUALLY declared on DTOs (rules cannot add bases) — binding-side checks
  key off the declared interface, exactly as designed.
- 2026-07-07 (R10) — Owner adopted `designs/atlantis/example/` as the canonical surface.
  §0R added: explicit-first fluent route table + `@InjectRoutes();` splice; ctor-bottom
  registration and boot-time mounting RETIRED (§2 anchor revised, §3.2 marked, §7
  inverted); per-request controller activation from factories. New/changed asks this
  track rides: LA-22 (named splice — now P1/AG-2), LA-25 (method references — new ticket
  `request-method-references.md`), enum criticality flagged (RequestType flags). RouteRec,
  trie/dispatch, binding eras, validation, and authDefault enforcement all UNCHANGED.
  P-probe set needs one addition before implementation: rules injecting statements at a
  splice site inside a plain function body (replaces the namespace-anchor probes where
  they covered registration).

- **2026-07-13 — Era A LANDED IN FULL** on all four checked engines (oracle, IR interpreter,
  emit-C++ via `trident build`, and LLVM native; full byte-identical corpus run on
  oracle/IR/LLVM — `packages/atlantis/tests/RESULTS.md`'s Track 02 section, corpus at
  `packages/atlantis/tests/corpus/routing/`). Implements: §1 (`RouteRec`/`ParamDesc`/
  `MatchResult`/`Router` — segment trie with static > `:param` > `*wildcard` precedence and
  backtracking, 404/405 with sorted `Allow`, trailing-slash normalize, HEAD-via-GET), §0R.2/
  §0R.3 (the fluent `Route<C>` builder — the "interim form," factory + invoke, made the
  **only** form since LA-25 method references were independently confirmed to flow correctly
  through it — see below), §0R.5/§1.4 (`App.AddRoute`/`useRouting`/`finalize` added to
  `src/kernel/facade.lev`'s `App`, the two-phase `resolveMiddleware`/`dispatchHandler`
  handshake), §3.1 (`Controller`/`IController`, response helpers, Era-A `pathInt`/`pathStr`/
  `queryStr`/`queryInt`/`header`/`jsonBody` binding helpers — this track's own query-string
  parser, since reference.md §6.6.57 still lists URL-string parsing as roadmap), §5 (`Check`/
  `CRule`/`Validation::validate` → `ValidationException` → 422, **explicit** invocation rather
  than attribute-driven), §6 (`authDefault` — `auth`/`noauth`/`explicit`, all three verified),
  §7 (the raw `router.record(...)` escape hatch, unchanged), route-table introspection
  (`routes()`, the Track 07 handoff shape).

  **NOT implemented — Era B / the attribute-rule wiring (§2, §4.4, §5.1, §5.2, the
  `@InjectRoutes()` splice)**, and deliberately so: it depends on LA-4 (attribute-value
  reflection) and LA-16/LA-22 ($if + identifier/type splicing + the named splice), none
  landed in this repo as of this pass — both already filed
  (`designs/requests/request-metaprog-attr-values.md`,
  `designs/requests/request-metaprog-splices.md`) before this implementation pass began, per
  this doc's own §8/§13 history. Per §0R.3/§0R.5, the explicit `Route`/`Controller`/
  `App.AddRoute` surface is "fully functional" on its own — the design itself anticipated
  exactly this fallback (§11 M4's "Era B if §8 asks land by 2026-09-10, else Era A"; §12's
  STOP condition for the same date). Shipping Era A now, ahead of that date, is the honest
  choice per §12's own STOP wording rather than leaving the track undone while waiting.
  Consequences: the C5/routing attributes (`Get`/`Post`/`Put`/`Delete`/`Patch`/`RoutePrefix`/
  `Fragment`/`Required`/`MaxLen`/`MinLen`/`Min`/`Max`/`Email`/`Url`) ARE declared (Layer A,
  inert) per this track's ownership commitment in the header, ready for the rule layer;
  P-probes P1–P10 (§9) were **not run** (they test the rule-based wiring path this pass
  doesn't attempt — re-run them when Era B is picked up). The §13
  2026-07-07-coordinator-logged ruling that `RouteRec` gains additive `bodySchema`/`summary`
  fields for Track 07 is **deferred, not implemented** — both fields were specified to be
  filled by the schema-generation rule and `@Summary` respectively, i.e. by Era B machinery
  this pass doesn't build; adding them empty now would be dead weight. Track 07 (or whoever
  picks up Era B) should add them alongside the rule layer.

  **Canonical-vs-interim Route form, resolved in favor of one form.** §0R.2 sketched two
  `Route` constructors (a single-method-reference "canonical" LA-25 form, and an explicit
  "interim" factory-plus-invoke form). Empirically, an **unbound method reference**
  (`AuthController::Login`) flows correctly through a generic class field + returned-closure
  shape (`Route<C>`'s `factory`/`invoke` fields, captured and called inside `toHandler()`'s
  returned lambda) — proven by direct probe before committing. A **bare free function**
  reference in the same position does not (bug.md #41's sibling finding, not separately
  filed since it never reaches user-facing Atlantis code — this track only ever spells
  `invoke` as a lambda or a method reference). Since the "canonical" single-argument form
  still needs an implicit per-controller-type factory (DI-by-ctor, bug.md-tracked as blocked
  — §0R.3's own "until bug.md #24 lands" note), and Era A already accepts a method reference
  in the `invoke` slot of the interim two/three-argument form, **the interim form is the only
  `Route` constructor shipped** — it already covers both spellings design's own canonical
  example wanted (`Route(path, factory, AuthController::Login)` reads the same as `Route(path,
  factory, (c, ctx) => c.Login(ctx))`), so the "canonical" single-arg overload adds no
  reachable capability today and was not built.

  **Language findings.** Eight compiler bugs filed (bug.md #40–#47) while implementing this
  track, none blocking after the documented workarounds: #40 nullable field/local narrowing
  blocks a same-slot lazy-init write; #41 a generic constructor's type argument doesn't infer
  through a nested call argument or across a fluent method chain (workaround: bind to an
  explicitly-typed, unchained local first — used throughout `Route<C>` construction);
  #42 a namespace-scope free function (or local variable) with a lambda-typed return/
  annotation fails to parse, methods don't (workaround: `Router.resolveMiddleware`/
  `dispatchHandler` are methods, not free functions, and test-corpus handler locals use
  `var`); #43 `uses NS;` doesn't reach code inside this file's own reopening of a namespace
  (narrower #37 instance — worked around with single-`::` qualification throughout
  `router.lev`/`controller.lev`); #44 a two-`::`-hop qualified name fails IR lowering
  (`Http::Respond::text` avoided; `HttpResponse` constructed directly instead); #45 a
  call-expression-as-callee chained call fails IR lowering (`Router.dispatchHandler` binds
  the intermediate handler to a local first); #46 a qualified generic type argument doesn't
  type-match its bare spelling across a qualification boundary (`var`, not an explicit
  qualified-generic annotation, at the two call sites this hit); **#47 [P1] — the one worth
  flagging loudest — a `Map<K, Struct>` class field silently corrupts memory on the LLVM
  backend once it holds 3+ entries** (oracle/IR unaffected; found via the full-corpus test
  crashing `--build-native` only). Router's `recsByKey : Map<string, RouteRec>` was rewritten
  to `keyIndex : Map<string, int>` indexing into the existing `routeList : Array<RouteRec>`
  to work around it — any future track storing a struct as a `Map` value in a class field
  should read #47 first.

  **Acceptance vs §11 milestones.** M1 (probes) — superseded by shipping Era A directly, see
  above. M2 (router core) — done, table-driven precedence/404/405/finalize checks/escape
  hatch/`routes()` all in the corpus. M3 (Era A end-to-end) — done: controllers mount via
  explicit `App.AddRoute` calls (this pass's equivalent of `config/routes.lev`), auth markers
  + all three `authDefault` modes, two-phase dispatch, verified in the corpus (no separate
  hand-written "twin" file was built — the corpus test itself plays that role, asserting
  exact response bodies/statuses/headers). M4 — Era A helpers + explicit `Validation`
  (§5.2's interim design, minus its own rule-based auto-registration half — see above);
  no `--expand` demo-controller review was produced (no attribute-driven code to expand).
