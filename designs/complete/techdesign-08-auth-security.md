# Atlantis Track 08 — Auth & Security

**Status:** draft for owner review. **Date:** 2026-07-06.
**Depends on:** Track 01 (Context/pipeline/error mapping — C2/C4), Track 02 (route table +
`@Auth`/`@NoAuth` markers — C5), Track 04 (config + composition root — C6),
`designs/request-tls-crypto.md` (R-4 `sysRandomBytes` is THIS track's hard dependency;
R-1 TLS gates the `Secure` cookie attribute and HSTS). Substrate: Track 09
(`digest::hmacSha256`, `encoding::base64Url*`, `DateTime` — AG-0).
**Owns:** `Atlantis::Auth` (exclusive). Attribute *semantics* for `@Auth`/`@NoAuth`
(names + shapes frozen in C5).

---

## 0. Mission, scope, non-goals

**Mission.** Ship auth as a designed subsystem, not a sketch (R1: Loom §11 had auth as a
paragraph). Concretely: a Principal model on `Context.user`, a composable strategy seam
with two shipped strategies (signed-cookie sessions, HS256 bearer tokens), request-time
guard enforcement for the R6 `authDefault` policy, honest in-language password hashing
(PBKDF2-HMAC-SHA256) with a written-down constant-time discipline, and the perimeter set:
CSRF, CORS, security headers, rate limiting, login lockout, audit events.

**Scope:** everything in §§1–9 below, in namespace `Atlantis::Auth`, pure `.lev`, zero
runtime reflection, C2 middleware shape only.

**Non-goals (v1):**
- **OAuth2 / OIDC providers — explicitly post-v1.** The `IAuthStrategy` seam (§2) is
  exactly where a `GoogleOidcStrategy` plugs in later; nothing in v1 may assume
  session/bearer are the only strategies.
- RS256/ES256 token verification (needs asymmetric crypto — waits on the crypto ticket
  family; stated loudly in §4).
- argon2id (the future native ask — rides LA-12; PBKDF2 is the honest pure-language v1).
- WebAuthn, SAML, API-key management UI, multi-node session replication (the
  `ISessionStore` seam is redis-shaped for later; v1 store is in-memory).
- General cookie API for app code (we ship a minimal internal codec, §3.1; promoting it
  to `Atlantis::Http` is Track 01's call).

---

## 1. Principal model & handler ergonomics

C2 fixes `Principal? user` on `Context`. The shape from the overview is right — id +
display name + coarse roles + free-form claims covers session, JWT, and future OIDC
without a schema fight. We keep it and add read helpers only:

```
namespace Atlantis::Auth {
    class Principal {                       // reference class: one instance per request,
        string id;                          // threaded by reference through the pipeline
        string name;
        Array<string> roles;
        Map<string, string> claims;         // flat string claims v1 (JWT nesting flattened, §4)
        new (string id, string name, Array<string> roles, Map<string, string> claims) { … }
        bool hasRole(string r) => this.roles.contains(r);
        string? claim(string k) => this.claims.atOrNone(k);
    }
}
```

Why not richer (typed claims, generic identity)? Claims are transport-level facts; typed
domain identity is the app's job (look up your `User` entity by `principal.id`). Roles are
`Array<string>` not a Set: role lists are tiny (<10), `contains` is fine, and Array is the
COW-value collection we have.

**Anonymous = `None`; narrowing does the work.** On guarded routes the guard middleware
(§5) guarantees `ctx.user != None` before the handler runs, but the *type* stays
`Principal?` — the one-line idiom is a throwing getter:

```
namespace Atlantis::Auth {
    // On guarded routes this never throws (guard ran first); on @NoAuth routes it is
    // the explicit "must be logged in anyway" check. One function, both readings.
    Principal require(Context ctx) {
        Principal? u = ctx.user;
        if (u == None) { throw UnauthorizedException(); }   // C4 → 401 (Track 01 maps)
        return u;                                            // narrowed
    }
}

// Handler ergonomics:
@Get("/me")
HttpResponse me(Context ctx) {
    Principal u = Auth::require(ctx);
    if (u == None) { }                       // (unreachable — u is Principal, not Principal?)
    return this.json(MeResponse(u.id, u.name));
}

// Optional-flavored handler on an open route:
@Get("/")  @NoAuth
HttpResponse home(Context ctx) {
    Principal? u = ctx.user;
    string greeting = "Hello, guest";
    if (u != None) { greeting = "Hello, ${u.name}"; }        // narrowed in the branch
    return this.html(homePage(greeting));
}
```

## 2. Strategy seam & the auth middleware

```
namespace Atlantis::Auth {
    interface IAuthStrategy {
        // None = "this strategy does not claim the request" (no credentials of my kind).
        // Bad credentials of my kind (bad signature, expired token) also return None in
        // v1 — the guard turns absence into 401; strategies emit audit events (§9.3)
        // so the operator can tell the cases apart.
        Principal? authenticate(Context ctx);
    }
}
```

The auth middleware is first-match over an ordered `Array<IAuthStrategy>` built in the
composition root (DI is lexical/compile-time — the *array* is explicit; each strategy's
own dependencies arrive via `bind`/ctor injection):

```
// composition root (main.lev / config/ioc.lev)
bind ISessionStore => InMemorySessionStore();
SessionStrategy session = SessionStrategy(inject ISessionStore, authCfg);
BearerStrategy  bearer  = BearerStrategy(authCfg);
Middleware authn = Auth::middleware([bearer, session]);      // order = precedence

// Atlantis::Auth
Middleware middleware(Array<IAuthStrategy> strategies) =>
    (Context ctx, (Context) => HttpResponse next) => {
        int i = 0;
        while (i < strategies.length() && ctx.user == None) {
            ctx.user = strategies[i].authenticate(ctx);
            i = i + 1;
        }
        return next(ctx);                    // guard (§5) decides whether None is fatal
    };
```

**Multiple inheritance where it is real:** strategies are ordinary classes, so one class
can carry several capabilities — `class SessionStrategy : IAuthStrategy, ISessionIssuer`
(authenticates requests *and* issues/revokes sessions for the login controller, §3.4), and
the in-memory store is `class InMemorySessionStore : SweepTimer, ISessionStore` — a
stateful `SweepTimer` base (timer re-arm plumbing, shared with the rate limiter §9.2) plus
the store interface. Catch-by-interface, bind-by-interface: the composition root binds
`ISessionStore`; the login controller injects `ISessionIssuer`; both may be one object.

## 3. Shipped strategy A — SESSION (signed cookie + server-side store)

### 3.1 Cookie format & codec

Cookie name `atlantis_sid` (config). Value (no padding, `.` separators — all three parts
base64Url-safe by construction):

```
v1.<sid>.<expEpochMs>.<sig>
  sid = base64Url(randomBytes(32))                       // 256-bit, from R-4 sysRandomBytes
  sig = base64Url(digest::hmacSha256(key, "v1." + sid + "." + expEpochMs))
```

The HMAC covers version + id + expiry, so the expiry is tamper-evident *without* a store
hit; the store record remains authoritative (revocation wins over cookie expiry). Key =
`AUTH_KEY` from config (C6): base64 of ≥32 random bytes; boot error if absent/short/
undecodable. Key rotation (verify-against-old-list, sign-with-current) is a v1.1 line
item — noted, not built.

Set-Cookie attributes: `HttpOnly; SameSite=Lax; Path=/` always; `Secure` when the request
arrived over TLS (LA-2) **or** `cookies.forceSecure=true` (behind a TLS-terminating proxy
— must be set together with `http.trustProxy`, §9.2). `Max-Age` mirrors the signed expiry.
Minimal internal codec (parse `Cookie:` name=value pairs split on `"; "`, first `=`;
render Set-Cookie with attributes) — P-probe P4; values never need percent-decoding
because ours are base64Url.

### 3.2 `ISessionStore` seam + in-memory v1

```
namespace Atlantis::Auth {
    class SessionRecord {
        Principal? principal;        // None = anonymous pre-session (CSRF-only, §7)
        string csrfToken;            // base64Url(randomBytes(32))
        DateTime createdAt;          // absolute-lifetime anchor
        DateTime expiresAt;          // rolling expiry (authoritative over cookie)
    }
    interface ISessionStore {        // Promise-shaped so a redis-style store drops in
        Promise<SessionRecord?> get(string sid);
        Promise<void> put(string sid, SessionRecord rec);
        Promise<void> remove(string sid);
        Promise<int>  removeExpired(DateTime now);   // returns count (sweep metric)
    }
}
```

`InMemorySessionStore`: a class holding `Map<string, SessionRecord>` (COW value — mutate
by reassignment; single-threaded loop makes this race-free), plus a sweep timer
(`std::after`, re-armed each fire, every 60s) calling `removeExpired(DateTime::now())`.
Restart loses sessions — documented v1 posture; the seam is the fix, not a rewrite.

### 3.3 Authenticate path (per request)

1. Parse cookie; missing → `None`. Malformed / wrong part count → `None` + audit.
2. Recompute HMAC over `"v1." + sid + "." + exp`; compare with presented sig using
   `constantTimeEquals` (§6.2 — comparison site CS-1). Mismatch → `None` + audit.
3. Signed expiry in the past (server clock, `DateTime::now()`) → `None` (skip store hit).
4. `await store.get(sid)`; missing or `expiresAt` past or `principal == None` → `None`
   (anonymous pre-sessions authenticate nobody).
5. **Rolling expiry:** if remaining lifetime < half of `session.ttl`, extend
   `expiresAt = now + ttl` (capped by `createdAt + session.absoluteMax`), `put` back, and
   flag `ctx.items["auth.reissue"] = "1"` — on the way out the session middleware appends
   a fresh Set-Cookie (middleware wraps `next`, so it holds the response; HeaderMap `add`
   keeps duplicate Set-Cookie lines — Track 09 §4.1).
6. Return the record's `Principal`.

### 3.4 Login / logout helpers (`ISessionIssuer`)

```
interface ISessionIssuer {
    Promise<string> issue(Context ctx, Principal p);   // returns Set-Cookie header value
    Promise<string> revoke(Context ctx);               // returns expiring Set-Cookie value
}
// Login controller usage:
@Post("/login")  @NoAuth
Promise<HttpResponse> login(Context ctx, LoginRequest req) { … verify (§6) …
    string setCookie = await this.issuer.issue(ctx, principal);
    return this.redirect("/dashboard").withHeader("Set-Cookie", setCookie);
}
```

`issue` **always generates a fresh sid** (session fixation defense): if the request
carried a valid pre-session (anonymous or other user), its record is deleted, its CSRF
token is NOT carried over — new sid, new token. `revoke` deletes the record and returns
`atlantis_sid=; Max-Age=0; …` to clear the cookie. Audit events fire on both (§9.3).

## 4. Shipped strategy B — BEARER (JWT-shaped HS256)

`Authorization: Bearer <token>` (scheme match case-insensitive). Token =
`base64Url(headerJson) + "." + base64Url(payloadJson) + "." + base64Url(sig)` — **unpadded**
base64Url per RFC 7515; if Track 09's `base64Url` emits padding we strip on encode /
accept-both on decode inside `Atlantis::Auth` (P-probe P2 decides; deviation logged).

Verify order (fail = `None` + audit, never throw — the guard owns 401):
1. Split on `.`; exactly 3 parts.
2. Decode header JSON (`json::parse`); require `alg == "HS256"` **exactly** — anything
   else, *especially* `"none"`, is rejected before any signature work (alg-confusion is
   the classic JWT break; the check is on OUR expectation, not the token's claim). `typ`
   ignored. `kid` ignored v1 (single key = `AUTH_KEY`, shared with sessions by default;
   `token.key` config overrides for separation).
3. `sig' = digest::hmacSha256(key, part1 + "." + part2)`; `constantTimeEquals(sig', sig)`
   — comparison site CS-2 (compare raw bytes, not base64 text).
4. Decode payload JSON; validate claims against `TokenConfig { string iss; string aud;
   int leewaySec /*=60*/; bool requireExp /*=true*/; }`:
   `exp` (reject if `now > exp + leeway`; missing exp rejected when `requireExp`),
   `nbf` (reject if `now + leeway < nbf`), `iss` / `aud` exact string match when
   configured non-empty (aud may be a JSON array — membership test).
5. Principal: `id = sub` (required), `name` = `name` claim or `""`, `roles` = `roles`
   claim (JSON array of strings; absent = empty), `claims` = every top-level claim whose
   value is a string (numbers/bools rendered to strings; nested objects dropped v1 —
   documented flattening).

**No RS256/ES256 until the crypto ticket family delivers asymmetric primitives** — HS256
means Atlantis v1 *validates tokens it (or a shared-secret peer) minted*, which covers
first-party SPAs, service-to-service inside one trust domain, and the MCP/service-client
story (Track 07). Third-party IdP tokens (RS256) are exactly the OIDC post-v1 item.
Token *minting* helper: `Auth::mintToken(Principal p, Duration ttl, TokenConfig cfg)` —
same primitives in reverse; provided because tests and service clients need it.

## 5. Guard enforcement (the middleware YOU own)

**Division of labor (contract with Track 02):** Track 02 builds the route table, reads
`@Auth{string role;}` / `@NoAuth{}` (C5), applies `authDefault` (R6) at boot — including
the `explicit`-mode boot error — and stamps each route with a **GuardSpec** string. At
match time the router puts it in `ctx.items["route.guard"]`. Track 08 owns the GuardSpec
vocabulary, its boot-time validation helper, and request-time enforcement.

**GuardSpec vocabulary (frozen small):**

| Spec | Source | Meaning |
|---|---|---|
| `noauth` | `@NoAuth`, or absent under `authDefault=noauth` | skip all checks |
| `auth` | `@Auth("")`, or absent under `authDefault=auth` | any authenticated principal |
| `role:<name>` | `@Auth("admin")` | authenticated + `hasRole("admin")` |
| `policy:<name>` | `@Auth("policy:canEditPost")` | authenticated + policy fn true |

`@Auth`'s single string is deliberately overloaded via the reserved `policy:` prefix:
**one attribute, one route-table column, one enforcement path, zero C5 changes** — and
richer logic (AND/OR of roles, ownership checks, tenancy) goes into *named policies*,
which are real typed code in the composition root, not a string DSL grown inside an
attribute. Boot validation makes the overload safe: `Auth::validateGuards(specs,
policyNames)` (called by Track 02's table builder) errors on a `policy:` spec with no
registered policy and on role names containing `:`. Ergonomics note: bare-`@Auth` (no
arg) would need a default field value in C5 (`string role = ""`), which is a frozen-
contract amendment — raised as an open question, NOT assumed; until ruled, `@Auth("")`
is the spelling for "any authenticated".

```
// Policies: registered in the composition root, referenced by name.
// app.policy("canEditPost", (Context ctx, Principal p) =>
//     p.hasRole("editor") || ctx.routeParams.atOrNone("authorId") == p.id);
class PolicyRegistry {
    Map<string, (Context, Principal) => bool> policies;   // P-probe P6: fn-typed Map values
    void add(string name, (Context, Principal) => bool fn) { this.policies[name] = fn; }
}

Middleware guard(PolicyRegistry reg) =>
    (Context ctx, (Context) => HttpResponse next) => {
        string spec = ctx.items.atOrNone("route.guard") ?? "auth";   // fail CLOSED
        if (spec == "noauth") { return next(ctx); }
        Principal? u = ctx.user;
        if (u == None) { throw UnauthorizedException(); }            // C4 → 401
        if (spec == "auth") { return next(ctx); }
        if (spec.startsWith("role:")) {
            if (!u.hasRole(spec.substring(5))) { throw ForbiddenException(); }  // C4 → 403
            return next(ctx);
        }
        if (spec.startsWith("policy:")) {
            (Context, Principal) => bool p = reg.policies.at(spec.substring(7)); // boot-validated
            if (!p(ctx, u)) { throw ForbiddenException(); }
            return next(ctx);
        }
        throw HttpException(500, "unknown guard spec");   // unreachable post-boot-validation
    };
```

Note the asymmetry, per the classic semantics: **unauthenticated → 401** (you may retry
with credentials), **authenticated-but-insufficient → 403** (retrying won't help).
Track 01's outermost middleware owns the exception→status mapping (C4); we only throw.
One `@Auth` per method; duplicates are a Track 02 boot error (richer combos = a policy).

## 6. Password hashing (PBKDF2-HMAC-SHA256) & the constant-time discipline

### 6.1 The KDF

RFC 2898/8018 PBKDF2 with HMAC-SHA256, dkLen=32 (single block — the whole `F` loop is one
chain), salt = 16 bytes from `randomBytes` (§6.4). Stored string format:

```
pbkdf2$<iterations>$<base64UrlSalt>$<base64UrlHash>
```

Self-describing params ⇒ **upgrade-on-verify**: `verifyPassword(password, stored)` returns
`struct VerifyResult { bool ok; string? upgradedHash; }` — on success with stored
iterations < configured iterations, `upgradedHash` carries a fresh hash at current params
(the app's user store persists it; Auth never touches app storage). Prefix `pbkdf2$` also
future-proofs the argon2id migration (LA-12): new hashes `argon2id$…`, old ones verify
and upgrade on next login.

**Iterations — measurement procedure + floor.** OWASP's current PBKDF2-HMAC-SHA256 target
is 600,000; in-language HMAC won't hit that at acceptable latency on day one, and we say
so instead of pretending. Procedure: app-hosted command `./myapp auth-calibrate` (R3
dispatch) times 10,000 iterations on the *production engine* (LLVM), extrapolates
iterations for a 250ms budget, rounds down to 10k, prints the config line
(`AUTH_PBKDF2_ITERS=…`). **Hard floor: 100,000 — boot error below it** (not a warning).
Shipped default 210,000, revisited at M4 with real LLVM numbers (P-probe P5 seeds this at
design time). When LA-12 lands a native fast-path, recalibrate toward 600k+. Dev-mode
note: on the IR interpreter this is O(seconds) — tests use a `FakePasswordHasher` bound
via DI; corpus programs that exercise the real KDF use 1,000 iterations with a loud
NOT-A-PRODUCTION-PARAMETER comment.

**Implementation notes (normative for the implementer):**
- One-shot `hmacSha256(key, salt || INT32BE(1))` then chain `U_{j+1} = hmacSha256(P, U_j)`,
  XOR-accumulating into the output bytes (`byteAt` + StringBuilder; mask to 8 bits).
- The naive one-shot HMAC costs ~4 SHA-256 compressions/iteration; a pad-precomputed
  (midstate) HMAC costs ~2. That 2× is worth having: **coordination ask to Track 09** for
  an incremental `Sha256` (update/finalize) or `HmacSha256Key` precomputed form — filed
  as a note in their log, NOT a new LA (it's a stdlib-surface tweak). Fallback: naive
  one-shot, calibration simply yields fewer iterations (honest, still floored).
- **Event-loop blocking:** 250ms of CPU starves the loop (overview H-3). The KDF runs in
  chunks of 5,000 iterations with an `await std::after(Duration(0))` yield between chunks
  — same total work, loop keeps breathing; login latency budget already assumes 250ms.
  `hashPassword`/`verifyPassword` are therefore `Promise`-returning. LA-1 threads is the
  structural fix (worker offload) — the API shape is already async so adoption is a flip.

### 6.2 Constant-time comparison — NORMATIVE CODE (copy verbatim)

No early-exit comparison on secret material ANYWHERE in this track. `==` on strings
short-circuits at the first differing byte — that is a timing oracle. The following is
the one blessed function; it branches only on *lengths* (public for all our uses:
MACs and hashes are fixed-size) and accumulates differences bitwise:

```
namespace Atlantis::Auth {
    // Constant-time equality for secret byte-strings (MACs, hashes, tokens).
    // Data-independent control flow over the bytes; length difference => false,
    // but LENGTHS ARE ASSUMED PUBLIC (fixed-size digests). Never "optimize" this.
    bool constantTimeEquals(string a, string b) {
        int la = a.length();
        int lb = b.length();
        int diff = la ^ lb;                 // nonzero if lengths differ
        int i = 0;
        while (i < la) {                    // always walk ALL of a
            int ba = a.byteAt(i);
            int bb = 0;
            if (lb > 0) { bb = b.byteAt(i % lb); }   // index math, not data, decides
            diff = diff | (ba ^ bb);
            i = i + 1;
        }
        return diff == 0;
    }
}
```

Caveat, stated honestly: on an interpreter we cannot promise microarchitectural constant
time; what this removes is the *algorithmic* early-exit channel, which is the one that is
practically exploitable over a network. Belt-and-suspenders option where the compared
value is attacker-supplied (CS-1/CS-2): double-HMAC comparison
(`constantTimeEquals(hmac(k2,x), hmac(k2,y))`) — noted, not required v1.

### 6.3 Comparison-site registry (audited at every milestone)

| Site | What is compared | Function |
|---|---|---|
| CS-1 | Session cookie sig vs recomputed HMAC (§3.3) | `constantTimeEquals` |
| CS-2 | JWT signature vs recomputed HMAC, raw bytes (§4) | `constantTimeEquals` |
| CS-3 | PBKDF2 output vs stored hash bytes (§6.1) | `constantTimeEquals` |
| CS-4 | CSRF submitted token vs session token (§7) | `constantTimeEquals` |
| CS-5 | Future strategies (API keys, webhook sigs) | MUST use it — review gate |

Unknown-user timing: `verifyPassword` against a *fixed dummy stored hash* when the
account doesn't exist, then return the same generic failure — login timing must not
distinguish "no such user" from "wrong password". Non-secret compares (role names,
GuardSpecs, `alg`, cookie names) use ordinary `==` — listed so nobody "fixes" them.

### 6.4 Random bytes — R-4 dependency + LOUD interim

`Auth::randomBytes(n)` is the single choke point (sids §3.1, CSRF tokens §7, salts §6.1):
- **Target (design assumes it lands, R4):** `sysRandomBytes(n)` from
  `request-tls-crypto.md` R-4 — CSPRNG, the only production path.
- **INTERIM — NOT FOR PRODUCTION:** until R-4 lands, a mix of `DateTime::now().epochMs`,
  a process-lifetime counter, and request-derived bytes fed through
  `digest::sha256` — **predictable to a motivated attacker; guessable session ids are
  full account takeover.** Gated: boot **fails** unless config sets
  `auth.allowInsecureRandomDEV=true`, and every boot with it set logs a WARNING banner.
  The corpus runs with the flag; no example/doc shows the flag without the warning text.

## 7. CSRF

Layered stance:
1. **SameSite=Lax (primary):** §3.1 sets it on the session cookie — modern browsers don't
   attach it to cross-site POSTs. Necessary but not sufficient (old browsers, subdomain
   edge cases) ⇒ layer 2.
2. **Synchronizer token for form posts:** every session record (including anonymous
   pre-sessions — created on first GET of a page containing a form, which solves the
   login-form chicken-and-egg; §3.4 rotates the token at login) carries `csrfToken`.
   `Auth::csrfToken(ctx)` exposes it; Track 09-views' form helper emits
   `<input type="hidden" name="_csrf" value="…">` (hook: they call our function, we never
   render HTML). The CSRF middleware validates **non-GET/HEAD/OPTIONS** requests whose
   `Content-Type` is `application/x-www-form-urlencoded` (multipart waits on LA-14/LA-3):
   token from `_csrf` field must `constantTimeEquals` the session's (CS-4); missing/wrong
   → `ForbiddenException` + audit. Routes can opt out via config list (webhooks).
3. **JSON APIs — custom-header stance (justified):** requests with `Content-Type:
   application/json` are exempt from the token *when* the request carries
   `X-Requested-With: XMLHttpRequest` (config-adjustable header name). Rationale (current
   OWASP guidance): a cross-site attacker cannot set custom headers without a CORS
   preflight, and our CORS layer (§8) never reflects arbitrary origins with credentials —
   so header presence proves a same-origin-or-approved caller. Bearer-authenticated
   requests are exempt entirely: the browser never attaches the Authorization header
   cross-site, so there is no ambient credential to ride.

## 8. CORS

Config-driven middleware, placed **before the auth strategies and guard** (§10) — a
preflight `OPTIONS` carries no credentials and must short-circuit rather than 401:

```
struct CorsConfig {
    Array<string> origins;        // exact-match allowlist; ["*"] allowed ONLY w/o credentials
    Array<string> methods;        // default GET,POST,PUT,PATCH,DELETE
    Array<string> allowHeaders;   // default Content-Type,Authorization,X-Requested-With
    Array<string> exposeHeaders;  // default []
    bool credentials;             // default false
    int maxAgeSec;                // default 600
}
```

- **Boot validation: `origins == ["*"] && credentials` is a hard error** — never
  wildcard-with-credentials (the spec forbids it; browsers enforce it; we fail at boot,
  not in production behavior).
- Preflight (method `OPTIONS` + `Access-Control-Request-Method` present): if Origin is
  allowlisted → `204` with `Access-Control-Allow-Origin: <echoed origin>` (never `*` when
  `credentials`, plus `Access-Control-Allow-Credentials: true`), allow-methods/-headers,
  `Access-Control-Max-Age`; if not allowlisted → `204` with **no** CORS headers (the
  browser enforces; we don't leak the allowlist). Either way: short-circuit, no `next`.
- Simple/actual requests: matched Origin → append ACAO (+credentials), always
  `Vary: Origin`. No Origin header → pass through untouched (same-origin/non-browser).

## 9. Hardening set

### 9.1 Security headers middleware

Applied on the way out (wraps `next`, sets only if handler didn't): `X-Content-Type-
Options: nosniff`; `X-Frame-Options: DENY` (config `SAMEORIGIN`/off); `Referrer-Policy:
strict-origin-when-cross-origin`; **HSTS only when the request came over TLS** (LA-2 —
`Strict-Transport-Security: max-age=31536000; includeSubDomains`, config; emitting HSTS
on plaintext is meaningless and mildly harmful); minimal default CSP:
`default-src 'self'; frame-ancestors 'none'` — deliberately permissive-enough for
server-rendered pages + htmx (which is `'self'`-hosted script), overridable string in
config, documented that inline `<script>` needs the app to loosen it consciously.

### 9.2 Rate limiting (token bucket) + login lockout

```
struct Bucket { float tokens; int lastMs; }
class TokenBucketLimiter : SweepTimer, IRateLimiter {   // MI again: timer base + seam
    Map<string, Bucket> buckets;     // sweep evicts entries idle > 10 min
    // allow(key, ratePerMin, burst): refill = elapsed*rate/60000 capped at burst;
    // tokens<1 => denied + retryAfterSec = ceil((1-tokens)*60000/rate/1000)
}
```

Middleware: key selector `(Context) => string` — default client IP; **IP comes from the
socket unless `http.trustProxy=true`, in which case rightmost non-trusted
`X-Forwarded-For` hop (Track 01 owns remote-addr extraction; we consume `ctx.items
["remoteAddr"]`)** — spoofable-header bypass is a config-documented trap. A second
limiter instance after auth may key by `principal.id`. Denied → throw
`HttpException(429, "rate limited")` + `Retry-After` header (Track 01's mapper carries
headers on HttpException — coordination note; fallback: return the 429 response directly
from the middleware, which C2 permits). Per-route overrides v1 = composition-root config
(`limits.route("/login", 10, 5)`); a `@RateLimit` attribute is a C5 addition — proposed
as an open question, not assumed.

**Login throttling/lockout policy:** failure counters keyed `login:acct:<idLower>` and
`login:ip:<ip>` (fixed 15-min windows, in-memory, swept): ≥5 failures on an account →
account locked 15 min (verify still runs against dummy hash; response stays the generic
failure — no lockout oracle in the body, only in audit + optional email later); ≥20/ip →
IP cooldown via the bucket limiter. Counters reset on success. All knobs in config.

### 9.3 Audit events (Track 01 owns the logger; we own the vocabulary)

Emitted through the structured logger with `requestId` from `ctx.items` (Track 01's
request-id middleware runs first — §10): `auth.login.success`, `auth.login.failure`
(reason: `bad_password|unknown_user|locked` — internal only), `auth.lockout`,
`auth.logout`, `auth.session.reissue`, `auth.token.invalid` (reason), `auth.csrf.reject`,
`auth.ratelimit.reject`. Fields: event, requestId, principalId?, ip, reason?, route.
**Redaction rule (hard): passwords, tokens, cookie values, Authorization headers never
appear in any log line at any level.** Grep-gate at every milestone: no log call in
`Atlantis::Auth` references those values.

### 9.4 Canonical pipeline order (for Track 01/04's composition docs)

```
requestId (01) → logging (01) → errorMapper (01, outermost catcher)
  → securityHeaders (08) → CORS (08, preflight short-circuits HERE — before any auth)
  → rateLimit-ip (08) → routerMatch (02, stamps route.guard)
  → authStrategies (08, fills ctx.user) → guard (08, 401/403)
  → csrf (08, needs session) → rateLimit-principal (08, optional) → handler
```

Ordering invariants (violating any is a design bug, documented for reviewers): CORS
before auth (preflights are credential-less); rate-limit-by-IP before auth (cheap
rejection shields PBKDF2 and the store); guard after strategies; CSRF after auth (needs
the session record); everything inside errorMapper so 401/403/429 render uniformly (C4).

## 10. Posture summary — "vs Loom" (R1)

| Concern | Loom §11 (competitor sketch) | Atlantis Track 08 (designed) |
|---|---|---|
| Default stance | implicit, undefined | R6 `authDefault` setting; shipped default `auth`; `explicit` mode boot-fails unmarked routes; guard fails CLOSED on missing spec |
| Sessions | "cookie sessions" one-liner | HMAC-signed cookie format, HttpOnly/Lax/Secure-when-TLS, store seam + sweep, rolling+absolute expiry, fixation defense (sid rotate on login) |
| Tokens | none | HS256 JWT verify+mint, alg pinned, exp/nbf/iss/aud + leeway; RS256 explicitly deferred |
| Passwords | unspecified "hash" | PBKDF2-HMAC-SHA256, self-describing hash string, calibration cmd + 100k floor, upgrade-on-verify, dummy-hash timing equalization |
| Secret compares | n/a | single blessed `constantTimeEquals` + audited site registry (CS-1..5) |
| CSRF | absent | SameSite=Lax + synchronizer token + JSON custom-header stance |
| CORS | absent | allowlist config, correct preflight ordering, wildcard+credentials = boot error |
| Headers / rate limit / audit | absent | §9 set: nosniff/XFO/Referrer/HSTS/CSP; token bucket + lockout; named audit events + redaction rule |

## 11. P-probes (run before feature work; failures → /bug.md or LA, never hack)

- **P1 — HMAC vector:** `digest::hmacSha256` against RFC 4231 cases 1, 2, and 6
  (key > block size) via `encoding::hexEncode`; on oracle + IR + LLVM.
- **P2 — base64Url round-trip + padding:** encode/decode 0/1/2/3-byte tails; determine
  whether Track 09 emits padding; verify our strip/accept shim yields RFC 7515-unpadded
  JWT segments byte-exact against a known-good HS256 token (jwt.io fixture).
- **P3 — constant-time compare sanity:** time 100k compares of 32-byte equal vs
  first-byte-differs vs last-byte-differs on IR and LLVM; assert ratios ~1.0 (±noise) —
  i.e., no early-exit scaling; record numbers in the log.
- **P4 — cookie parse/render:** round-trip `Cookie:` with multiple pairs, odd spacing,
  valueless names; render Set-Cookie with full attribute set; feed back through parse.
- **P5 — PBKDF2 timing spike:** 1,000 iterations on IR and LLVM (naive HMAC), extrapolate
  to 100k/210k/600k; this seeds the §6.1 default and the chunk size (5,000) sanity.
- **P6 — function-typed Map values:** store/fetch/call `(Context, Principal) => bool` in
  a `Map<string, …>` (PolicyRegistry depends on it) on all three engines.

## 12. Foreseeable problems

| # | Problem | Strategy |
|---|---|---|
| F-1 | **Clock skew on token expiry** (peer-minted JWTs) | `leewaySec` (default 60) applied to exp AND nbf; sessions use server clock only (no skew); document that leeway>300 is a smell |
| F-2 | **Session fixation** | sid regenerated on every `issue`; pre-session record deleted; CSRF token rotated with it (§3.4) — corpus asserts old sid dead after login |
| F-3 | **Timing side-channels** | §6.2 blessed function + CS registry + dummy-hash on unknown user; P3 keeps us honest per engine; double-HMAC escalation path noted |
| F-4 | **PBKDF2 blocks the event loop** | chunked iterations + zero-delay await breathing (§6.1); IP rate limit BEFORE auth shields CPU; calibrate on LLVM; LA-1 workers later — API already async |
| F-5 | **sysRandomBytes not landed at M2** | §6.4 interim is boot-gated + bannered NOT-FOR-PRODUCTION; if still missing at AG-6 → STOP S-2 |
| F-6 | base64Url padding mismatch vs JWT spec | P2 decides; shim inside Auth; log deviation |
| F-7 | In-memory store: restart logout + single-node only | Documented v1 posture; `ISessionStore`/Promise shape is the redis seam; sweep keeps memory bounded |
| F-8 | COW `Map` churn on hot session/bucket maps | Fine at v1 rates (one reassign per login/sweep, not per request — reads dominate); measure at M4; if hot, ask stdlib for in-place map, don't hack |
| F-9 | X-Forwarded-For spoofing defeats IP limiter | socket addr unless `trustProxy`; rightmost-untrusted-hop rule; documented loudly (§9.2) |
| F-10 | JWT alg confusion / `none` | alg pinned to expectation before signature work (§4); corpus includes a `"alg":"none"` rejection case |
| F-11 | Retry-After needs headers on exceptions | Coordination with Track 01 on HttpException carrying headers; fallback: limiter returns the 429 response directly (C2-legal) |
| F-12 | Bare `@Auth` inexpressible (C5 has non-defaulted `role`) | `@Auth("")` documented; C5 amendment (`string role = ""`) raised as escalation question, not assumed |

## 13. Milestones & acceptance (aligned to AG-6 — 2026-12-10)

| M | Scope ("done" =) | Target |
|---|---|---|
| M1 | P-probes P1–P6 green + logged; `constantTimeEquals`, cookie codec, base64Url shim, `randomBytes` seam (+gated interim); PBKDF2 passing published PBKDF2-HMAC-SHA256 vectors (RFC 7914 §11 set) on oracle/IR/LLVM | 2026-10-20 |
| M2 | Session strategy end-to-end: issue/authenticate/revoke, rolling+absolute expiry, fixation rotation, sweep; `ISessionIssuer` used by a demo login controller | 2026-11-05 |
| M3 | Bearer strategy (verify+mint, all claim checks, alg-none rejection); guard middleware + PolicyRegistry + `validateGuards`; 401/403 mapping verified through Track 01 | 2026-11-20 |
| M4 | CSRF, CORS (preflight ordering test), security headers, token-bucket limiter + lockout, audit events + redaction grep-gate; PBKDF2 calibration numbers on LLVM recorded, default iterations ratified | 2026-12-03 |
| M5 = AG-6 | **Auth corpus** in `packages/atlantis/tests/`, green on IR + LLVM: register(hash) → login (bad pw ×N → generic failure + lockout event) → login ok → cookie attrs asserted → guarded route 200 → role route 403 → policy route 200/403 → CSRF-missing form post 403 → bearer request to same guarded route → logout → old cookie 401 → old sid replay 401. Demo app passes it | **2026-12-10** |

## 14. STOP conditions (per overview §0.4, plus track-specific)

- **S-1:** Any change needed to C2 (`Context` fields), C4 (exception set), or C5
  (`Auth`/`NoAuth` shapes — incl. F-12's default-arg wish) → STOP, escalate; never edit
  the overview or another track's doc drive-by.
- **S-2:** `sysRandomBytes` (R-4) not landed by 2026-11-20 → STOP and put the interim-
  random question to the owner explicitly; the corpus must not silently ship on the
  insecure path.
- **S-3:** Any secret comparison that cannot go through `constantTimeEquals` (e.g., a
  store that only exposes boolean lookup) → STOP; do not approximate.
- **S-4:** Asymmetric-crypto need materializes (RS256 demand from Track 07 interop) →
  STOP; that is a crypto-ticket extension, not an in-track project.
- **S-5:** P3 shows a >2× early-exit-shaped timing ratio on LLVM (optimizer folding the
  accumulator into a branch) → STOP; report to /bug.md; do NOT hand-obfuscate codegen.
- **S-6:** PBKDF2 on LLVM can't reach the 100k floor within a 500ms budget → STOP;
  escalate LA-12 priority with measurements rather than lowering the floor.
- Standing: no edits outside `Atlantis::Auth` + this doc; `.lev` only; no runtime
  reflection; compiler bugs → /bug.md with repro.

## 15. Implementation log (append-only)

- 2026-07-06 — Design authored. GuardSpec contract offered to Track 02; pipeline-order
  invariants (§9.4) offered to Tracks 01/04; incremental-SHA256/HMAC-midstate ask noted
  for Track 09's log; open questions raised to owner: C5 `Auth.role` default (F-12),
  `@RateLimit` attribute addition, HttpException-with-headers (F-11).

- 2026-07-19 — Core subsystem implemented in `packages/atlantis/src/auth/` (principal,
  crypto, cookie, pbkdf2, strategy, session, bearer, guard, csrf, cors, headers, ratelimit,
  audit), plus additive `App.useAuthentication()/useAuthorization()` in `kernel/facade.lev`.
  Corpus: `packages/atlantis/tests/corpus/auth/` (oracle+IR green and byte-identical;
  LLVM lane currently red — see below). Landed as-designed:
  - Principal (id/name/roles/claims), `Auth::require`, session strategy (signed-cookie
    envelope, in-memory store + sweep, rolling+absolute expiry, fixation defense on
    `issue()`, CSRF token carried by every live record including anonymous pre-sessions),
    bearer strategy (HS256 verify+mint, alg pinned before signature work, exp/nbf/iss/aud
    + leeway, claim flattening), `Auth::middleware` first-match fold, guard enforcement
    (401 vs 403 asymmetry, `role:`/`policy:` specs, `PolicyRegistry`, `validateGuards` boot
    check), PBKDF2-HMAC-SHA256 (chunked, `Http::nextTick()` yield, upgrade-on-verify,
    dummy-hash timing equalization, 100k floor / 210k default constants), constant-time
    compare (verbatim per §6.2), CSRF (SameSite + synchronizer token + JSON header stance),
    CORS (preflight ordering, wildcard+credentials boot error), security headers, token-
    bucket rate limiting + login lockout, audit event vocabulary with redaction by
    construction.
  - **Deviations from the letter of the design** (owner should re-read before treating
    this as final):
    1. **R-4 `sysRandomBytes` → `sysRandom`.** The crypto ticket landed under a different
       name (`designs/complete/techdesign-tls-crypto.md` §8 ruling) before this track
       started; `Auth::randomBytes` is a direct pass-through, no gated interim/
       `allowInsecureRandomDEV` path exists because none was needed (STOP S-2 moot).
    2. **base64Url padding shim (P2) never built.** `encoding::base64UrlEncode/Decode`
       already emit/accept unpadded per `tests/corpus/encoding`; F-6 does not fire.
    3. **`@Auth`/`@NoAuth` attributes were never declared.** Track 02 only landed the
       explicit-fluent surface (`.requireAuth()/.noAuth()/.auth(role)` → `RouteRec.
       authMode`/`authRole`) — attribute-rule auto-wiring is blocked upstream on
       LA-4/LA-16/LA-22 (LA-4 items A/B landed 2026-07-19, same day, but LA-16/LA-22 —
       the splice mechanism attribute routing would need — remain an open, un-accepted
       request, `designs/requests/request-metaprog-splices.md`). §5's GuardSpec is
       therefore *derived* at request time from `Router.authModeOf`/`authRoleOf` (both
       already public on the landed `Router`) rather than stored as a new field —
       `.auth("policy:name")` threads the `policy:` prefix through the existing
       `authRole` string with zero C5/Router-schema changes. `Auth::validateGuards`
       replaces the design's boot-time attribute check with the equivalent walk over
       `router.routes()`.
    4. **No `attribute Auth`/`NoAuth` means no bare-`@Auth` question (F-12) to resolve** —
       moot until Track 02's attribute layer lands.
    5. **Session cookie's `Secure` determination** has no landed TLS-on-Context signal to
       read (C2's frozen fields don't carry one) — `cookieShouldBeSecure` reads the
       documented interim convention `ctx.items["tls"] == "1"` (no track stamps it yet)
       OR `forceSecure`, whichever is true.
    6. **HttpException-with-headers (F-11) never materialized** — rate-limit 429 and (by
       extension) any future header-bearing throw uses the design's own documented
       fallback: build and return the response directly from the middleware.
  - **Not built / open:**
    - Real RFC-vector PBKDF2 corpus (design's M1 "passing published PBKDF2-HMAC-SHA256
      vectors") — the corpus proves the algorithm/format/upgrade-path self-consistently
      at a tiny iteration count (engine cost reasons, see LLVM note below), not against
      RFC 7914 §11 ground truth.
    - P3's timing-ratio measurement (100k-compare equal/first-differs/last-differs on IR
      vs LLVM) and P5's iteration/latency extrapolation were not run — no calibration
      numbers exist yet to ratify the 210k default (§6.1's own M4 item).
    - `@RateLimit` attribute (F-11's sibling ask) not raised further; per-route overrides
      remain a plain composition-root config concern (`TokenBucketLimiter` + explicit
      `rateLimitMiddleware(...)` calls), matching the design's stated v1 fallback.
    - The literal M5 acceptance script (one continuous register→lockout→login→...→old-sid-
      replay-401 narrative) was not written as one sequence; the corpus instead covers each
      piece (issue/authenticate/revoke/reissue, role/policy guards, CSRF reject, bearer
      round-trip, login lockout) as independent sections.
  - **LLVM lane, known pre-existing platform issue (NOT a Track 08 defect):** the native
    binary currently segfaults partway through the corpus on `Router`/`Context`
    construction sequences. Bisected to a genuine, independently-reproducible minimal
    repro (`known_bugs_1.md` #97: a function taking a class-typed parameter that also
    returns from inside a `for` loop over `Array<Struct>` corrupts a later, unrelated
    heap-touching call on LLVM) and fixed at the one site this track hit
    (`cookie.lev`'s `cookieValue`). Re-verifying afterward showed the corpus still
    crashes on LLVM, later and at a different point, for reasons that do NOT match that
    narrow shape — and directly re-running the untouched, pre-existing
    `packages/atlantis/tests/corpus/routing` corpus (Track 02, no Track 08 code
    involved) confirmed IT ALSO segfaults on LLVM within its first two lines, matching
    `known_bugs_1.md` #95's own description almost exactly. Conclusion: this is a
    systemic, already-tracked, out-of-scope LLVM defect broad enough that essentially
    any non-trivial Atlantis program touching `Router`/`Context` construction is
    currently affected — not something introduced by or fixable within this track.
    Oracle and IR are green and byte-identical (`auth.expected`); that pair is the
    correctness reference until the compiler-side fix lands.
