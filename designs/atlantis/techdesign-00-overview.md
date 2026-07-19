# Atlantis — Tech Design 00: Overview, Contracts, Build Order

**Status:** design in progress (this doc is the anchor; track docs 01–09 hang off it).
**Date:** 2026-07-06. **Priority:** flagship framework — the premiere web framework for
Leviathan, distributed via Trident.
**Builds on:** `designs/proposal-web-framework.md` (the "Loom" proposal — superseded by this
design set, see R1), `designs/proposal-metaprogramming.md` + techdesigns phases 1–4,
`designs/complete/techdesign-09-web-foundations.md` (Track 09 — the substrate), Tracks 03/04/05/07/08,
`designs/complete/techdesign-package-manager.md` (Trident), `/sketch.md` (owner's direction notes).
**Owner directives:**
- Atlantis is an enterprise-grade MVC framework in the ASP.NET family, showcasing the
  language's strengths (multiple inheritance, metaprogramming, streams, compile-time DI).
- MySQL support out of the gate, behind an extensible driver seam.
- MCP servers / microservices / AI-adjacent interfacing in scope; a LangChain-style agent
  framework is **explicitly out of scope**.
- **The language is co-developed with the framework.** Owner (2026-07-06): *"We will have
  true threads … we are building the language, if you want it, I'll get it"* and *"make a
  ticket, I'll get it done… this is priority, so I'll work with you."* Tracks must NOT
  silently work around a missing language feature: file a formal request ticket
  (`designs/request-<slug>.md`, §2), **design assuming the ask lands**, and state the
  interim fallback.
- **Loom (`designs/proposal-web-framework.md`) is a competitor framework** — a first draft
  from before the metaprogramming layer matured. Atlantis must be **better**, not a rename:
  its research is fair input, its decisions are re-derived on merits, and where Loom is
  weak (pre-rules ergonomics, structs-only data model, no MCP/agent story, no MySQL) is
  exactly where Atlantis differentiates.

---

## 0. Read this first

### 0.1 The mission

Atlantis collapses the five-or-six models every mainstream framework accreted into the one
model the language was designed around, and makes the enterprise batteries (routing, DI,
data, validation, auth, OpenAPI, MCP) fall out of typed attributes + compile-time rules —
**zero runtime reflection, cost-identical to hand-written code, all magic greppable via
`--expand`.** Atlantis commits to five collapses (independently validated against the
market research the competitor Loom draft compiled — R1):

1. One data-flow model: the stream (`Promise` = one-shot stream, `await` anywhere, no coloring).
2. One composition model: the middleware transform (`(Context, Handler) => HttpResponse`).
3. One error model: exceptions primary (catch-by-interface → HTTP status), unions for expected outcomes.
4. One wiring model: `bind`/`inject` + attribute-driven rules.
5. One data shape per role: reference-class **entities** (identity + change tracking),
   value-`struct` **DTOs** (copy semantics, dense, serializable). (Refines Loom's collapse #5 — see R2.)

### 0.2 Rulings (owner Q&A, 2026-07-06)

Questions were put to the owner with recommendations; the owner answered on 2026-07-06.
Recorded here so they are revisitable — changing one is an escalation event, not a
drive-by edit.

| # | Ruling (owner's answer) |
|---|---|
| **R1** | **Loom is a competitor framework** ("be better — it was a first draft", designed before metaprogramming matured). Atlantis re-derives every decision on merits; Loom's market research is input; no syntax or decision is inherited by default. |
| **R2** | **Entities are reference classes** inheriting `DbTracking` + stateful MI mixins (`Timestamps`, `SoftDelete`) — the framework's flagship multiple-inheritance showcase. **DTOs are value structs.** (Owner: "not married to any syntax if you know better" — this is the better shape: entities want identity + dirty tracking; structs can't inherit.) |
| **R3** | **CLI per `designs/proposal-package-manager.md` §8.2** (owner's pointer): project **scaffolding belongs to Trident** (`trident init`; template support requested via `designs/request-trident-init-templates.md`). **Framework operations are app-hosted**: the Atlantis entrypoint dispatches `./myapp serve\|migrate\|routes\|openapi\|mcp` — consistent with Trident's run-no-dependency-code invariant. |
| **R4** | **Design assuming the language gets what it needs** (owner: "write up a requirement and design assuming we make it — we will make it, just write the ticket"). The MySQL driver designs `caching_sha2_password` + TLS as first-class (ticket `request-tls-crypto.md`); `mysql_native_password` over plain TCP is the explicitly-interim path until that ticket lands. |
| **R5** | **MCP-native, not agent-framework.** `@Tool`-attributed methods become MCP tools with compile-time-generated schemas; OpenAPI is generated from the same attribute source of truth. No LLM client, no chains, no agent runtime. ("That sounds great.") |
| **R6** | **Auth default is a setting**, not a fixed stance: `authDefault = auth \| noauth \| explicit`. `auth` = every route guarded unless `@NoAuth`; `noauth` = open unless `@Auth`; `explicit` = every route MUST carry `@Auth` or `@NoAuth` (boot-time error otherwise — fail-fast at startup, since absence isn't matchable at compile time). Framework ships `auth` as the shipped-default of the setting. |
| **R7** | **Per-verb route attributes** — `@Get("/path")`, `@Post`, `@Put`, `@Delete`, `@Patch` — plus `@RoutePrefix` on controllers. (Owner: "I trust you for a world-class solution, not married to the sketch's shape." Attribute args are positional+typed; bare-word args aren't expressible.) |
| **R8** | **Package family, batteries-included core:** `atlantis` alone covers the large majority (HTTP, routing, DI, config, JSON, validation, auth, MCP/OpenAPI, views); drivers are the separate packages (`atlantis-mysql` first — the template every future driver copies). |
| **R9** | **Language co-development via request tickets.** Missing language/toolchain features become `designs/request-<slug>.md` tickets (owner implements — "this is priority, so I'll work with you"); never silently worked around, never patched into the compiler by framework tracks. |
| **R10** | **The example app is the canonical surface** (`designs/atlantis/example/`, owner-authored 2026-07-07; adopted after comparison review): **explicit-first wiring with visible splice points.** One fluent route table (`Config/Routes.lev`: `app.AddRoute(Route('/login', AuthController::Login).requestType(RequestType::GET \| RequestType::POST).requireAuth())`) and one bindings function (`Config/Bindings.lev`), each carrying an `@InjectRoutes();` / `@InjectBindings();` splice statement where attribute-generated wiring lands — attributes remain available, explicit is the spine. This inverts the original Track 02/04 primacy (attribute-first with explicit escape hatch) and REPLACES constructor-side-effect route registration, which is retired. Also adopted: `Atlantis::Builder` → `Build()` → `app.useX()` staged bootstrap; per-request controller activation; `IActionResponse` action results (`Atlantis::View('home')`); `Model` entity base (mixins layer on top); PascalCase scaffold with `Resources/Views` (`.lhtml`) and `Public/`. **Spelling dispositions:** imports stay `uses`/`use` (the example's `using` collides with RAII `using` — flagged, not adopted); **`readonly` is a distinct language feature, NOT `const`** — owner ruling 2026-07-07: `const` is compile-time (value known and folded at compile time), `readonly` is a **runtime write-once slot** (assigned once during construction from runtime values, immutable thereafter) — ticketed LA-28 (`request-readonly.md`); the example's `private readonly IUserService userService;` is the intended spelling, kept verbatim, and every Atlantis DI-held field is `readonly`. Grouped `@attr(A, B)` and `self:` named args are ticketed (LA-26/LA-27), individual decorators are the interim. |

### 0.3 The tracks

Nine tracks with disjoint ownership. Design docs live in `designs/atlantis/`; framework
source will live in `packages/atlantis/` and `packages/atlantis-mysql/` (§4). "Owns" is
exclusive — another track editing an owned file/namespace is a coordination failure.

| Track | Doc | Role | Owns (namespaces / dirs) | Hard deps |
|---|---|---|---|---|
| 01 Kernel | `techdesign-01-kernel.md` | HTTP kernel, `Context`, middleware pipeline, error→status mapping, static files, structured logging, health endpoints | `Atlantis::Http`, `Atlantis::Log` | Track 09 (HTTP hardening) |
| 02 Routing | `techdesign-02-routing-controllers.md` | Router, `Controller` base, route attributes + rules, parameter binding, validation attributes | `Atlantis::Routing`, routing/validation attributes in `Atlantis` | 01 (C2), metaprog P1–3 |
| 03 Serialization | `techdesign-03-serialization.md` | JSON model binding, `@Serializable` derive-rules, content negotiation, DTO conventions | `Atlantis::Json` | Track 09 (json) |
| 04 DI & Config | `techdesign-04-di-config.md` | `@Injectable`, composition root, typed config, `.env`, options pattern, app-hosted command dispatch (R3) | `Atlantis::Config`, `Atlantis::App` | — |
| 05 MySQL driver | `techdesign-05-mysql-driver.md` | MySQL wire protocol over `TcpStream`, auth, text+binary protocol, pooling; implements C3 | package `atlantis-mysql` (`MySql` namespace) | Track 09 (digests), C3 |
| 06 ORM | `techdesign-06-orm.md` | `DbTracking` + mixins, change tracking, query builder, repositories/UoW, migrations | `Atlantis::Orm`, data attributes | 05 (C3), 03 (C7) |
| 07 Interop: MCP, OpenAPI, service clients | `techdesign-07-mcp-openapi.md` | `@Tool` → MCP server (JSON-RPC 2.0, streamable HTTP), OpenAPI generation, typed microservice clients (Refit-style interface + attributes → generated client) | `Atlantis::Mcp`, `Atlantis::OpenApi`, `Atlantis::Client` | 01, 02, 03 |
| 08 Auth & security | `techdesign-08-auth-security.md` | Principal, session + signed-token strategies, `@Auth`/`@NoAuth` guards, CORS, CSRF, rate limiting | `Atlantis::Auth` | 01, 02, 04 |
| 09 Views | `techdesign-09-views.md` | Type-checked templates, htmx-style fragments, Inertia-style adapter (deferrable) | `Atlantis::Views` | 02 |

**If only one agent is available:** land in gate order (§5): 01 → 04 → 03 → 02 → 05 → 06 → 08 → 07 → 09.

**Parallel-agent divergence points:**
- **Wave 1 (fully parallel, start now):** 01, 03, 04, 05 — pairwise disjoint; 05 codes
  against frozen C3 without touching core.
- **Wave 2:** 02 (after 01's C2 surface exists), 06 (against C3 + C7; driver can still be
  in flight — 06 tests against a `FakeDriver`).
- **Wave 3 (parallel):** 07, 08.
- **Wave 4:** 09, sample app, polish.

### 0.4 STOP protocol (model escalation)

A Sonnet-class implementation agent must **STOP — log in the track's Implementation log,
commit WIP, escalate to a Fable-class model / the owner — never improvise** — when any of
these would be violated:

- **(a) Zero runtime reflection.** If a design seems to need runtime attribute/type
  introspection, the design is wrong or a Language Ask is missing.
- **(b) Framework tracks never touch the compiler or toolchain.** No edits to `src/**`,
  `tools/**`, `runtime/**`, `CMakeLists.txt`. Language gaps go through the LA register (§2)
  to the owner. Atlantis is pure `.lev` packages + these design docs.
- **(c) One model per concern.** No second middleware shape, second error channel, second
  async primitive, or second endpoint model may be introduced — anywhere.
- **(d) Toolchain separation (owner hard req):** no dependency/manifest logic outside
  Trident; the framework CLI stays app-hosted (R3).
- **(e) Frozen contracts C1–C8 (§3) and rulings R1–R9 (§0.2)** change only by escalation.
- **(f) Files are `.lev`.** Never create `.ext` files, even where older docs show them.
- **(g) Engine targets:** oracle/IR for dev, **LLVM for production**. Never design a feature
  that requires extending the frozen ELF backend or the emit-C++ system layer.
- **(h) Bugs found in the language/compiler** are reported to `/bug.md` with a minimal repro
  and a proposed ruling — framework agents never fix compiler bugs themselves.

### 0.5 Frozen / do-not-touch

`src/**`, `tools/**`, `runtime/**`, `X64Gen.cpp`/`X64.hpp` (frozen backend), the Track 01–09
robustness designs (other owners), `examples/curl/**` (lcurl is a reference corpus),
Trident's build-plan contract. The prelude (`src/Resolver.cpp` regions) belongs to the
stdlib tracks — if Atlantis needs a prelude change, that is a Language Ask, not an edit.

### 0.6 Naming

- Framework: **Atlantis**. Packages: `atlantis`, `atlantis-mysql` (future: `atlantis-postgres`, …).
- Namespaces: `Atlantis` root facade (facade types only — rules and attributes live in
  their consuming subsystem, C1); subsystems as `Atlantis::X` (C1). App files import the
  subsystems they use (`uses Atlantis::Json;`, `uses Atlantis::Orm;`), not a blanket root
  import. Attributes are PascalCase types (`@Injectable`, not `@injectable` — normalizes
  the sketch).
- Design docs: `designs/atlantis/techdesign-NN-slug.md`. Completed docs move to
  `designs/atlantis/complete/` when landed.
- Future VCS identity for git-dep consumption: `github.com/thaden0/atlantis` (tagged
  `vMAJOR.MINOR.PATCH` per Trident MVS).

---

## 1. Context: what exists (verified 2026-07-06)

Ground truth from `docs/reference.md`, the metaprogramming docs, and the Trident tech design.
Track authors: trust this section over stale `[planned]` markers elsewhere.

**Runtime/engines.** Five engines off one IR. Single-threaded cooperative **event loop**
with `Promise<T>`/`await` (no coloring), timers, **sockets** (`TcpStream`/`TcpListener`),
and text-only HTTP — implemented on oracle, IR, ELF, **LLVM (primary, Gate G1 parity
2026-07-06)**. emit-C++ has no system layer (not a server target). No threads (`fork` is
reserved, unbuilt — LA-1). No TLS (LA-2). No binary bodies until `Block` (Track 03, LA-3).

**Track 09 (designed, targeted Aug 3–21)** delivers the substrate Atlantis builds on:
`namespace json` (JsonValue class, parse → `JsonValue?`), `DateTime`/`Duration`,
`encoding` (base64/percent/hex), `digest` (md5/sha1/sha256/hmacSha256), `HeaderMap`
(ordered, case-insensitive multimap), incremental request/response parsers, chunked both
directions, server keep-alive, a real `HttpClient` (redirects, timeouts), and the
handler-throw → 500 pattern. **Atlantis gates on Track 09 (AG-0).**

**Metaprogramming (Phases 1–3 implemented).** Typed attributes (`attribute Route { string
method; string path; }`, positional args, int/float/bool/string fields); rules
(`match @Route(r) on method m in class C : IController` → `inject
\`router.record($r.method, $r.path)\` at bottom of C.constructor`); anchors: top/bottom of
ctor (synthesizes one if absent), `member of C`, `namespace N`, marker, body top/bottom;
`where` predicates over `meta.*` (fields/methods/bases as **strings**, attr **names** only);
`$for` field iteration **inside array literals only**; expression macros `name!(…)`;
comptime (hermetic, no I/O). Rules fire only in files that `uses` the rule's namespace.
**Constraints that shape every track:** (i) a rule reads its *matched* attribute's args
(`$r.path`), but field iteration cannot read *other* attributes' argument values (P4 item I,
LA-4) — so per-field renames (`@Column("full_name")`) are deferred; (ii) body-rewriting
(`rewrites`/`$body`) is Phase 4, unbuilt (LA-5) — cross-cutting behavior is middleware, not
wrapping; (iii) attribute args are positional (LA-7); (iv) reference classes are
non-reifiable at comptime — permanently.

**Language surface highlights** (see §8 cheat sheet): multiple inheritance with
`distinct`/`::` selection; interfaces may require fields; catch-by-interface exception
dispatch on all engines; `using`/`IDisposable` RAII; compile-time `bind`/`inject` DI
(block-scoped, lexical, nearest-wins, duplicate = error); unions + `T?`/`None` + flow
narrowing (`??`, `?.`); `match`; generics with inference (+ gated HKT); pure-value
`Array`/`Map` (COW) with a LINQ-shaped surface; string interpolation `"${expr}"`;
**no `static` keyword** (free functions in namespaces; constructor labels like
`User::FromJson(v)` stand in for static factories); no regex (LA-13); no JSON until
Track 09; `File` I/O + `OpenMode` flags.

**Trident.** TOML manifest `trident.toml` (`name`/`entry`/`sources`/`version`/`[[dep]]`
with `path`/`version`/`as`/`dev`), local-path **and git-tag deps working today**, MVS
resolution, `trident.lock`, content-addressed store, phantom-dep prevention. `trident
build/run/add/lock/fetch/why` shipped. No registry/`publish` until P2.3 ("on Trident"
initially = depend by VCS path). **Trident ships no binaries and runs no dependency code**
— hence R3.

---

## 2. Language Asks register (LA-*)

The co-development channel (R9). Each ask: what, consumer, fallback until it lands.
Priority P0 = blocks a gate; P1 = blocks a headline feature; P2 = ergonomics/perf.
**High-priority asks have formal tickets at `designs/request-<slug>.md`** (owner: "make a
ticket, I'll get it done"). This table is the index; the ticket is the requirement+design
document. Tracks design assuming the ticket lands (R4) and note the interim fallback.

Tickets filed 2026-07-06: `request-threads.md` (LA-1), `request-tls-crypto.md` (LA-2),
`request-metaprog-attr-values.md` (LA-4/5/6/7 + 2026-07-07 addendum: LA-21), `request-regex.md`
(LA-13), `request-trident-init-templates.md` (R3 scaffolding). Filed 2026-07-07 from track
findings: `request-promise-rejection.md` (LA-19), `request-metaprog-splices.md` (LA-15/16/17/22),
`request-generic-static-members.md` (LA-18), `request-comptime-template-import.md` (LA-20).
LA-3/8/10/11/14 are owned by existing Tracks 03/08/09 designs — referenced, not re-ticketed.
Filed 2026-07-07 (R10 adoption): `request-method-references.md` (LA-25),
`request-readonly.md` (LA-28); LA-22/26/27 ride the splices and attr-values tickets (see
those files' addenda).
**Priority raises (2026-07-07):** LA-4 and LA-6 are wanted by **AG-2/AG-3 (Oct 5)**, not
AG-4 — Track 02's Era-B typed handler signatures and Track 03's Mechanism-A serializers
both gate on them.
**Enum is now on Atlantis's critical path (R10):** `RequestType::GET | RequestType::POST`
flags and entity enums (`RelationType`) need Track 03's `enum` — which is STOP-blocked on
the class-statics ruling (`/this_bug.mg`, commit affa8a4). Flagged for the owner's ruling;
not re-ticketed (Track 03 owns it). Interim: comptime const ints + `(|)`.

| ID | Ask | Priority | Consumers | Fallback until landed |
|---|---|---|---|---|
| **LA-1** | **True threads/workers** — **LANDED IN FULL** (`designs/complete/techdesign-threads-{2,3}.md`, 2026-07-11): `Worker<T> spawn<T>(() => T)` + `Channel<T>` with copy-always isolation, event-loop-per-worker, and `sysTcpListen(port, reusePort)` (SO_REUSEPORT) so N workers share one listener; `std::cpuCount()` sizes the pool, so `HttpServer(port, workers: cpuCount())` is a config flip. **Live on all three active engines byte-identically — true OS threads on LLVM** (per-worker TLS heap/arena/loop, real pthreads, eventfd join, reap-time munmap; Channel is a lock-free SPSC ring + two eventfds). Windows targets reject at compile time (POSIX-only v1). Leak/reap/fd verified by `fuzz/thread_leak.py` (+0B churn); ~520K channel msgs/sec | P1 (perf) | 01 (multi-core serving), 05 (pool concurrency) | Single-loop process; scale-out = N processes behind a reverse proxy. Design pipeline/pool to be worker-safe *in shape* (no ambient mutable globals) so adoption is a flip, not a rewrite |
| **LA-2** | **TLS** — either native seam (`sysTlsWrap(fd, config) -> fd`-shaped) or in-language over Block + crypto natives (AES-GCM, ECDHE, cert verify) | P1 | 01 (HTTPS), 05 (MySQL TLS, `caching_sha2_password`), 07 (remote MCP auth posture) | Plaintext HTTP behind a TLS-terminating proxy; MySQL on private network with `mysql_native_password` (R4) |
| **LA-3** | **`Block`** byte buffer + binary stream bodies (Track 03 owns the design) | P1 | 01 (uploads/binary responses), 05 (binary protocol efficiency) | Byte-in-string idiom (byte-clean strings + `byteAt`/`byteToString` — proven by lcurl and the digest designs) |
| **LA-4** | **Attribute-value reflection in `meta.*`** (metaprog P4 item I): read attribute args during `$for` field iteration | P1 | 06 (`@Column("full_name")`), 03 (`@Json("alias")`), 02 (validation table codegen) | Names derive from field names; per-attribute-match constructor-registration pattern (each rule match *can* read its own attribute's args) |
| **LA-5** | **Layer D rewrites** (`rewrites`/`replace`/`$body`, metaprog P4): `@Transactional`, `@Timed`, `@Cached` wrappers | P2 | 06, 01 | Explicit composition: middleware for cross-cutting HTTP concerns; `uow.transact((tx) => { … })` for transactions |
| **LA-6** | **Statement-position `$for`** (P4 item J) | P2 | 03, 06 codegen ergonomics | Array-literal `$for` + interpret the built array |
| **LA-7** | **Named attribute args** (P4 item M) | P2 | all attribute surfaces | Positional args; defaulted trailing fields |
| **LA-8** | **`enum`** (Track 03) | P2 | 01/02 (HttpMethod, StatusCode), 05 (protocol constants) | comptime-const strings/ints in a namespace |
| **LA-9** | **HttpClient keep-alive + connection pooling** (deferred from Track 09) | P1 (microservices) | 07, typed service clients | Connection-per-request (honest, correct) |
| **LA-10** | **`sysSpawn` + signal handling** (Track 08 scope): graceful shutdown (SIGTERM), worker-process supervision | P1 (ops) | 01 (graceful drain), deployment story | Document proxy-level draining; loop exits when work drains |
| **LA-11** | **DNS resolution** (Track 08 scope) | P1 (microservices) | HttpClient to hostnames | IP/localhost targets; `/etc/hosts`-style config entry |
| **LA-12** | **Native fast-path for digests** (perf only — in-language digests are correct but slow on interpreters) | P2 | 08 (session/token signing), 05 (auth handshake) | In-language digests (fine at request-per-connection rates; LLVM-compiled speed is respectable) |
| **LA-13** | **Regex engine** (stdlib) | P2 | 02 (`@Pattern` validation, route constraints) | Drop `@Pattern` v1; string-method validators (`contains`/`startsWith`/length/charset scans) |
| **LA-14** | **Multipart/form-data + URL-encoded form parsing helpers** (could land as a Track 09 follow-up or in Atlantis itself) | P1 | 02 (form binding), 09 (HTML forms) | Atlantis implements URL-encoded forms in-language v1; multipart waits on LA-3 |
| **LA-15** | **Member-selector splice**: `this.$f` with `$f` a `$for`-bound field, in member-access position (Rules.cpp reads it as an attribute value today — Track 03's source read) | P1 (AG-2/AG-4) | 03 (toJson Mechanism A), 06 (generated appliers) | Per-field-match rules + runtime key index (Track 03 Mechanism B); deletable post-landing with byte-equivalence acceptance |
| **LA-16** | **`$if` conditional splice + identifier/type-position splicing of comptime strings** (`$p.type`, `$p.type::FromJson` in type/ctor position; Track 04's γ is the same ask) | P1 (AG-2) | 02 (Era-B typed handler binding), 04 (generated binds) | Era-A uniform `(Context) => HttpResponse` signatures; hand-written ioc.lev binds |
| **LA-17** | **Template identifier synthesis** (declaration name built from a binding, e.g. `$ident(C.name, "Cols")` → `UserCols`) | P1 (AG-4) | 06 (compile-checked column descriptors) | Boot-validated `col("name")` strings |
| **LA-18** | **Static-shaped member resolution on generic type parameters** (`A::FromJson(v)` inside a duck-typed generic body — demand-driven monomorphization above IR) — **LANDED 2026-07-12** (`designs/complete/techdesign-generic-static-members.md`) | P1 (AG-5) | 07 (generic tool adapters), 03 (P-4) | none — generic callable-level `T::member` is live; class-level/HKT/override-set limits are explicit v1 errors |
| **LA-19** | **Promise rejection semantics** (info.md §19 #7 is still open: what does `await` do with a failed promise?) — a DB driver needs an error path to awaiting callers | **P0 if Track 05's P6 probe fails** (exceptions may already propagate through the await pump — verify first) | 05, 01, 07 | None acceptable long-term; probe before M1 |
| **LA-20** | **Comptime template import** (comptime file inclusion, content-hashed into the build) — unlocks compile-checked templates, per-template generated `render(Model)`, template typos as compile errors | P2 (post-AG-7) | 09 | v1 runtime engine behind the same `view()` surface |
| **LA-21** | **`meta.*` completions**: inherited/mixin fields visible (or a `C.allFields`), `meta::Class.attrs` (class-level attribute names) — rider on the LA-4 ticket | P1 (AG-4) | 06 (mixin columns invisible today), 03 | Framework mixins hand-implement contributors; entities serialize via DTOs |
| **LA-22** | **Named splice invocation** (R10 — supersedes the original cross-decl-marker ask, Track 04's β): `@InjectRoutes();` / `@InjectBindings();` as a STATEMENT in a function body = a user-placed, named anchor where matching rules inject statements, in the surrounding lexical scope (which is what makes spliced `bind`s scope correctly by construction) | **P1 (AG-2)** | 02 (attribute routes → the table), 04 (@Injectable binds) | Explicit-only wiring (the R10 table/bindings files work with zero metaprog); marker-anchor spelling if it lands first |
| **LA-25** | **Method references**: `AuthController::Login` as a typed function value (unbound instance method — receiver is the first parameter) | P1 (AG-2) | 02 (fluent `Route(path, handlerRef)`), 07 (client aspiration) | Route overload taking an explicit factory+lambda; or attribute routing via the splice |
| **LA-26** | **Statement-position grouped attributes**: `@attr(PrimaryKey, AutoIncrement);` preceding a field, with bare-identifier attribute references | P2 | 06 (example's entity spelling) | Individual decorators: `@PrimaryKey @AutoIncrement int id;` (works today) |
| **LA-27** | **Relation-attribute expressiveness**: named attribute args (LA-7) + MEMBER REFERENCES as attribute argument values + value pipes — `@hasManyBelongsToMany(App::Relationship, self: App::Relationship::user1 \| App::Relationship::user2)` | P1 (AG-4) | 06 (M2M self-join relations — owner ruled the `self:` semantics in the example's User.lev comment) | M2M deferred; HasMany/BelongsTo with string entity names |
| **LA-28** | **`readonly` — a runtime write-once field modifier** (owner ruling 2026-07-07: distinct from `const`; `const` = compile-time constant, `readonly` = runtime slot assigned once during construction, immutable after). The example's `private readonly IUserService userService;` | **LANDED** (`designs/techdesign-readonly.md`) | ALL tracks (every DI-held service field; entity/DTO immutable fields) | none — `readonly` ships; every example DI field already carries the real spelling |
| **LA-23** | **Small system natives batch**: TcpStream peer address (access logs/rate limits), `sysStat` realpath/symlink field (docroot escape defense), event-loop stdin watch (stdio MCP), TcpStream pause/resume (streamed result sets) | P2 | 01, 05, 07, 08 | Documented ops-discipline rules; features deferred |
| **LA-24** | **Comptime expression trees** (lambda-to-SQL) — future-flag only, nothing designed on it | P3 | 06 (query builder v2) | Typed column descriptors (LA-17) |

Adding an LA: append here with the track that needs it; ping the owner. Removing/landing
one: owner updates disposition; consuming tracks flip their fallback notes in their logs.

---

## 3. Cross-track contracts (frozen — changes are escalation events)

**C1 — Namespaces & packages.** Package `atlantis` exports root namespace `Atlantis`
(facade types only) with subsystems `Atlantis::Http`, `::Routing`, `::Json`,
`::Config`, `::App`, `::Orm`, `::Mcp`, `::OpenApi`, `::Client`, `::Auth`, `::Views`, `::Log`.
**Rules and attributes live in the subsystem namespace that consumes them** (amended
2026-07-18, retroactive — supersedes the original all-in-root rule). Rules fire only where
imported, so a file activates exactly the rule sets it imports and nothing is imported that
is not used: `uses Atlantis::Json;` on a DTO file, `uses Atlantis::Orm;` on an entity file;
an entity that is both persisted and JSON-exposed imports both. Metaprogramming consumed
only within a subsystem (e.g. `Atlantis::Orm`-internal rules) is defined in that subsystem.
Placement tiebreak for multi-consumer metaprogramming: the most specific namespace covering
all consumers; root `Atlantis` only for genuinely framework-wide surface, and it never holds
subsystem rules. Retroactive mapping for landed code: `@Serializable`/`@JsonIgnore`, the
serializer rule set, `ISerializable` → `Atlantis::Json`; `@Injectable` + its validation
rule → `Atlantis::App`. Every rule-bearing subsystem ships the loud silent-no-fire boot
warning (07's risk-#5 pattern: expected-but-missing generation names the class and the
required `uses`). Package `atlantis-mysql` exports `MySql` and depends on `atlantis` (for
C3 interfaces). App code namespaces itself `App::…` by convention.

**C2 — Handler & middleware shape (the one composition model).**
```
namespace Atlantis::Http {
    class Context {                       // per-request; reference type
        HttpRequest request;              // Track 09 type (HeaderMap headers)
        Map<string, string> routeParams;  // filled by the router
        Principal? user;                  // None until auth middleware runs (C4/08)
        Map<string, string> items;        // per-request bag (request id, trace id, …)
    }
}
// THE two function shapes. Everything composes from these:
//   Handler    = (Context) => HttpResponse
//   Middleware = (Context, (Context) => HttpResponse) => HttpResponse
```
The pipeline is an explicit, ordered `Array` of middleware folded over the terminal handler
in the composition root. Streaming/SSE: a handler may return a response whose body writes an
`OutStream<string>` (Track 09 chunked encoding); WebSockets ride `IOStream` post-v1.

**R10 additions (2026-07-07).** The `Atlantis::Builder` → `Build()` → `App` facade wraps
the pipeline array with named stages (`app.useAuthentication()`, `app.useAuthorization()`,
`app.use(mw)` — order of `useX` calls IS pipeline order, still explicit and visible) and
carries `app.AddRoute(...)`. **Action results:** controller methods may return an
`IActionResponse` (View / Json / Redirect / StatusCode — `Atlantis::View('home')` is the
canonical constructor) or a typed DTO; the terminal dispatcher renders either to
`HttpResponse`. The Middleware/Handler function shapes above are UNCHANGED — Builder and
IActionResponse are conveniences layered on them, not a second model.

**C3 — Data access seam (implemented by every driver package).**
```
namespace Atlantis::Data {
    // DbValue: what a cell holds. v1 (text protocol): string | int | float | bool | None
    interface IDbConnection {
        Promise<ResultSet> query(string sql, Array<DbValue> params);
        Promise<ExecResult> execute(string sql, Array<DbValue> params);  // ExecResult: affectedRows, lastInsertId
        Promise<ITransaction> begin();
        void close();                      // IDisposable — usable with `using`
    }
    interface IDbDriver { Promise<IDbConnection> connect(DbConfig cfg); }
    class ResultSet { Array<string> columns; Array<Row> rows; }          // Row wraps Array<DbValue> with byName access
    interface IDbPool { Promise<IDbConnection> acquire(); void release(IDbConnection c); }
}
```
Parameters are **always** bound via `params` (placeholder `?`) — string-concatenated SQL from
user input is a design violation. Driver selection: `bind IDbDriver => MySql::Driver();` in
the composition root. Track 05 implements this for MySQL; Track 06 consumes only this seam
(tested against a `FakeDriver`).

**C4 — Exception → status mapping (catch-by-capability).** Defined in `Atlantis::Http`;
the kernel's outermost middleware owns the mapping:
```
interface IHttpException : IException { int status; }
class HttpException        : Exception, IHttpException   // ctor (int status, string message)
class NotFoundException    : HttpException  // 404   (label ctors for common cases)
class ValidationException  : HttpException, IValidationException  // 422; carries field errors
class UnauthorizedException: HttpException  // 401
class ForbiddenException   : HttpException  // 403
```
Any user class can become status-mapped by implementing `IHttpException` — multiple
inheritance earning its keep (catch by contract, not by base class). Uncaught non-HTTP
exceptions → 500, logged, body never leaks internals.

**C5 — Attribute vocabulary (names + field shapes; owning track defines semantics).**
Routing (02): `Get/Post/Put/Delete/Patch { string path; }`, `RoutePrefix { string path; }`,
`Fragment {}` (09 consumes) — attribute routing is the OPTIONAL path under R10; it lands at
the `@InjectRoutes();` splice in `Config/Routes.lev`. Auth (08): `NoAuth {}`,
`Auth { string role; }` (fluent `.requireAuth()`/`.noAuth()` are the explicit-table
spellings of the same GuardSpec) — enforcement default is the `authDefault` setting (R6:
`auth`/`noauth`/`explicit`), checked at boot when the route table is built.
Wiring splices (R10): `InjectRoutes {}` / `InjectBindings {}` — statement-position splice
invocations (`request-metaprog-splices.md` item E), not declaration decorators.
DI (04): `Injectable { string iface = ""; }`.
Validation (02): `Required {}`, `MaxLen { int n; }`, `MinLen { int n; }`, `Min { int v; }`,
`Max { int v; }`, `Email {}`, `Url {}`. Serialization (03): `Serializable {}`, `JsonIgnore {}`.
Data (06, example vocabulary per R10): `Table { string name; }`, `PrimaryKey {}`,
`AutoIncrement {}`, `NotNull {}`, `Column { string name = ""; }` (rename arg inert until
LA-4), `HasMany { string entity; }`, `BelongsTo { string entity; }`,
`HasManyBelongsToMany { … }` (through-entity M2M with `self:` pipe — gated on LA-27; shape
in Track 06). `Id {}` is retired in favor of `PrimaryKey`.
MCP/OpenAPI (07): `Tool { string name; string description; }`, `Summary { string text; }`.

**C6 — Configuration.** `Atlantis::Config::load()` at startup: precedence **process env >
`.env` file > defaults**. `.env` is `KEY=VALUE` lines (no interpolation), parsed at runtime
(comptime is hermetic — cannot read files). Typed config: app declares a struct, binds it in
the composition root from the loaded map. `DbConfig { string host; int port; string user;
string password; string database; }` is the C3-adjacent shared shape.

**C7 — JSON interchange.** Track 09's `json::JsonValue` is THE interchange type. Track 03's
`@Serializable` rule generates: member `JsonValue toJson()` and a **labeled constructor**
`new FromJson(JsonValue v)` (the language's static-factory idiom). Every framework type that
crosses the JSON boundary conforms to this pair. Field naming: JSON keys = field names
verbatim (renames wait on LA-4).

**C8 — Versioning & engine policy.** Packages tag `vMAJOR.MINOR.PATCH` (Trident MVS; major
in identity). Dev engines: oracle/IR. Production: **LLVM**. ELF and emit-C++ are out of
scope for framework testing. All source `.lev`. Differential corpus discipline: every track
ships runnable example programs under its package's `tests/` that a future CI can run on
oracle + IR + LLVM.

---

## 4. Package & application layout

**Framework development layout (this repo):**
```
packages/
  atlantis/            trident.toml  (name = "atlantis")
    src/*.lev          # sources = ["src/**/*.lev"], namespaces per C1
    tests/*.lev        # runnable example/corpus programs
  atlantis-mysql/      trident.toml  (name = "atlantis-mysql", [[dep]] path="../atlantis")
    src/*.lev
    tests/*.lev
examples/
  atlantis-demo/       # the sample app (sketch layout below); local-path deps on both packages
```

**Blessed application scaffold (R10: mirrors `designs/atlantis/example/` exactly; docs +
demo app use this — supersedes the earlier lowercase `/sketch.md`-derived layout):**
```
myapp/
  .env                        # KEY=VALUE; overridden by process env (C6)
  trident.toml                # [[dep]] atlantis, atlantis-mysql; entry = "main"
  main.lev                    # class Main: Builder → Build() → app.useX(); calls
                              #   App::Routes::addRoutes(app) + App::AddBindings()
  Controllers/                # AuthController.lev … (uses Atlantis; + Atlantis::Routing)
  Models/
    Entities/                 # classes : Model (+ mixins) — Models/ root also legal
    Dtos/Requests/            # value structs, @Serializable + validation attrs
    Dtos/Responses/
  Config/
    Routes.lev                # THE route table: fluent AddRoute calls + @InjectRoutes();
    Bindings.lev              # App::AddBindings(): @InjectBindings(); + explicit binds
    Database.lev              # DbConfig construction from Config
  Middleware/                 # app middleware (C2 shape)
  Resources/Views/            # *.lhtml templates (Track 09)
  Public/                     # static files
  Tests/                      # DI-swapped tests (bind FakeX => …)
```
Namespace notes: nesting is `::` (`namespace App::Controllers { … }`); disk layout is
non-semantic to the compiler (§12), so the casing is convention, not mechanism. Imports
are `uses`/`use` — the example's `using X;` import spelling is NOT adopted (`using` is the
RAII statement; flagged in R10).

Scaffolding: `trident init --template atlantis` materializes this layout once
`request-trident-init-templates.md` lands (R3); until then the demo app under
`examples/atlantis-demo/` is the copyable template.

---

## 5. Roadmap and gates (authoritative)

| Gate | Scope ("done" =) | Deps | Target |
|---|---|---|---|
| **AG-0** | Track 09 landed (JSON, digests, HeaderMap, keep-alive, HttpClient) — external | Track 09 owner | 2026-08-21 |
| **AG-1** | Kernel: pipeline + Context + error mapping + static files + logging serve real traffic on IR & LLVM; demo hello-app runs | 01 | 2026-09-15 |
| **AG-2** | Routing end-to-end, R10 surface: fluent `Config/Routes.lev` table dispatches with bound+validated params; `@InjectRoutes();` splice lands attribute routes into it (LA-22); `--expand` shows all generated wiring; authDefault enforced at boot | 02 (+01) | 2026-10-05 |
| **AG-3** | MySQL driver: connect, auth, query/execute/transactions with `?` params, pooling, against a real MySQL 8 (`mysql_native_password`; `caching_sha2_password`+TLS activates when `request-tls-crypto.md` lands — designed in from day one, R4) | 05 | 2026-10-05 |
| **AG-4** | ORM: entities CRUD via `DbTracking` change tracking; query builder → one SQL per terminal op; migrations generate + apply via `./app migrate` | 06 (+03,05) | 2026-11-10 |
| **AG-5** | MCP server (initialize/tools list+call over streamable HTTP) + OpenAPI 3.1 emit from the same attributes; `./app mcp`, `./app openapi` | 07 | 2026-11-25 |
| **AG-6** | Auth: sessions + signed tokens, `@Auth` roles, CORS/CSRF middleware; demo app passes an auth corpus | 08 | 2026-12-10 |
| **AG-7** | Views: templates + htmx fragments; demo app serves HTML+fragments+JSON from one controller | 09 | 2027-01-15 |
| **AG-8** | 1.0-preview: packages consumable as git deps, docs, sample app polished; publish path per Trident P2.3 status | all | 2027-02 |

Waves (§0.3) mean AG-1/AG-3 and parts of AG-2's design proceed concurrently; gates state
*landing* order. Self-hosting (compiler G1–G5) proceeds in parallel and does not gate Atlantis.

---

## 6. Suspected hurdles (read before each milestone)

| # | Hurdle | Strategy |
|---|---|---|
| H-1 | **Track 09 slip** blocks AG-1 | Kernel designs against Track 09's *documented surface* (HeaderMap, parse-feed state machines); if slipped, Wave-1 tracks stub the current demo-grade HTTP behind the same names and swap later — additive, no redesign |
| H-2 | **Rule-injection patterns unproven at framework scale** (per-field ctor-registration, `namespace N` tables, name-splicing into strings) | Every track's doc carries P-probes: tiny `.lev` programs run against `build/leviathan --expand` BEFORE feature work; probe failures with a metaprog-shaped cause → file to `/bug.md` or raise an LA, never hack |
| H-3 | **Event-loop starvation** (long handler blocks all requests) | Doc the cooperative contract loudly; pool/db calls always `await`; LA-1 threads is the structural fix; consider a slow-handler warning in dev mode |
| H-4 | **MySQL protocol edge cases** (packet splitting at 16MB, EOF vs OK deprecation, charset) | Fixture-driven: recorded handshake/result hex fixtures in `atlantis-mysql/tests`; test against real MySQL 8 in Docker at AG-3; text protocol first, binary/prepared second |
| H-5 | **DI is lexical, not runtime-scoped** — "request-scoped service" has no container to live in | Request scope = the `Context` object (C2); services needing per-request state take `Context` as a parameter. Document this as the model, not a limitation |
| H-6 | **Change tracking without runtime reflection** (R2 entities) | `DbTracking` uses generated per-entity snapshot/diff members (rules over `$for` fields); dirty check = field-wise compare against snapshot — all compile-time-generated code |
| H-7 | **Whole-program compile times** as the framework + app grow | Packages keep `sources` globs tight; measure `trident build` wall-time in each track's log; escalate to owner if >20% regressions appear (same discipline as Track 09 §6#8) |
| H-8 | **JsonValue aliasing** (class values in Maps share nodes) | Follow Track 09 §6#3: read-mostly JsonValue, builders via `ofX` ctors; serializers always construct fresh trees |

---

## 7. Reference-doc duty & bookkeeping

- `designs/proposal-web-framework.md`: add a header note "superseded by `designs/atlantis/`
  (the framework is named Atlantis)" — done when Track 01 lands.
- Each track doc ends with an **Implementation log (append-only)** — same discipline as the
  robustness tracks: dated entries, deviations, "not a STOP because…" justifications.
- Bugs: `/bug.md` with repro + proposed ruling (owner fixes compiler-side).
- This overview's §2 LA register is the single place language asks live; duplicating an ask
  inside a track doc without registering it here is a coordination failure.

---

## 8. Leviathan cheat sheet for design authors (verified syntax)

Track authors: write every code sketch in *this* dialect. When in doubt, P-probe it.

```
// Attributes: typed fields ARE the positional args (int/float/bool/string only)
attribute Get { string path; }
@Get("/users/:id")  User show(int id) => users.get(id);

// Rules: fire only in files that `uses` the rule's namespace
rule registerRoutes {
    match @Get(r) on method m in class C : IController
    inject `router.record("GET", $r.path, ($_params) => this.$m($_args))`
        at bottom of C.constructor
}
// $for — ARRAY-LITERAL POSITION ONLY; meta gives names/types as strings, attr NAMES only:
inject `Array<string> columns() => [ $for f in C.fields.where((x) => x.hasAttr("Column")) : $f.name ];`
    at member of C

// DI — compile-time, lexical, nearest-wins; duplicate bind in scope = error
bind ILogger => ConsoleLogger();
new UserController(IUserService svc) { this.svc = svc; }   // filled when unambiguous
greet(inject ILogger);                                      // explicit on collision

// Classes vs structs; MI + distinct; interfaces may require fields; NO `static` keyword
class User : DbTracking, Timestamps { … }        // reference, identity, inheritable
struct LoginRequest { string email; string password; }   // value, deep-copy, final
User u = User();                                  // construction: no `new` at call site
User v = User::FromJson(jv);                      // labeled ctor = static-factory idiom
this.Counter::value = 5;                          // distinct-slot selection

// Optionals & narrowing — no null, no truthiness; conditions must be bool
string? name = m.atOrNone("k");
if (name != None) { use(name); }                  // narrowed to string
string s = name ?? "default";  obj?.method();

// match — type/value/range arms, first-match-wins
string kind(IShape s) => match (s) { Circle => "c"; Square => "s"; else => "?"; };

// Errors: throw/catch by interface; no finally — `using` for cleanup
try { … } catch (IValidationException e) { … } catch (IException e) { … }
using File f = File("x.txt", std::read);          // close() on every exit edge

// Async/streams — no coloring; single-threaded loop
Promise<ResultSet> rs = conn.query("SELECT …", []);
ResultSet r = await rs;
listener.connections((TcpStream c) => { c.onData((string chunk) => { … }); });

// Collections — pure values, COW; Map vocabulary at/with/without (get/set are keywords)
Array<int> evens = xs.where((x) => x % 2 == 0);
m = m.with("k", v);  m["k"] = v;                  // bracket-sugar (use this on LLVM — bug #18)
int? n = "42".toInt();                            // strict parse → optional

// Strings: interpolation ${expr}; byte-clean; no regex, no printf
console.writeln("Hello ${user.name}, ${2 + 2}");

// Namespaces & imports; manifest is trident.toml (TOML), files are .lev
namespace App::Controllers { … }
uses Atlantis;                                    // facade types only — no rules in root (C1)
uses Atlantis::Json;                              // opts this file into Json's rules (@Serializable)
use Atlantis::Http::Context as Ctx;              // single-name import + alias
```

Forbidden in designs: `any`, `null`, truthiness, `finally`, `static`, runtime reflection,
`derive` keyword, threads-before-LA-1, `.ext` files, named attribute args, statement-position
`$for`, reading other attributes' args in field iteration (LA-4), regex (LA-13).

---

## 9. Implementation log (append-only)

- 2026-07-06 — 00-overview authored; rulings R1–R9 recorded; LA register seeded (LA-1..14);
  contracts C1–C8 frozen; track docs 01–09 commissioned.
- 2026-07-06 (later) — Owner answered the Q&A round: R1 reframed (Loom = competitor, be
  better), R3 (scaffolding → Trident per PM proposal §8.2; ops commands app-hosted), R4
  (design-assuming-we-make-it; tickets are the mechanism), R6 (auth default is a setting
  with `auth`/`noauth`/`explicit`), R7/R8 confirmed (core batteries-included). Request
  tickets filed as `designs/request-*.md`; Track 07 scope extended to typed service clients
  (`Atlantis::Client`).
- 2026-07-07 — All nine track docs landed. LA register grew LA-15..24 from track findings;
  four new tickets + one ticket addendum filed; five compiler bugs from Track 04's probe
  pass filed as bug.md #22–26. **Coordinator contract rulings (all additive):**
  - **C3 amendment approved** (Track 05): `Atlantis::Data` gains driver-agnostic exception
    capability interfaces — `IDbException`, `IDuplicateKeyException`, `IDeadlockException`,
    `IConnectionLostException`, `ITransientDbException`; driver exception families implement
    them; Track 06 catches only these. Interim home in `MySql` until the core file exists.
  - **C5 amendments approved**: `Auth { string role = ""; }` (bare `@Auth` = any
    authenticated); new `@RateLimit` attribute (shape per Track 08 §, boot-wired);
    `Injectable { string iface = ""; }` optional explicit-interface arg (Track 04 M4).
  - **GuardSpec adopted** (Track 08): the route record's auth marker is the single string
    `noauth | auth | role:<r> | policy:<p>`; Track 02's `authMode`/`authRole` fields
    normalize to it at `finalize()`; Tracks 02/07/08 align on this spelling.
  - **RouteRec canonical = Track 02 §** (02 owns routing), extended additively with the
    `bodySchema` + `summary` fields Track 07 needs; Track 07 §5.1 defers to it.
  - **C7 note**: `@Serializable` pairs with a **manually declared** `: IJsonSerializable`
    (`{ JsonValue toJson(); }`) — rules cannot add bases (anchor inventory), and the manual
    declaration is load-bearing (it gates per-field rules). Marker name is
    `IJsonSerializable` (Track 02's "IToJson" renamed to match).
  - **C6 pinned to bug #23**: config structs are passed explicitly; struct-typed `bind` is
    forbidden in Atlantis code until the bug is ruled/fixed.
  - **C2 notes**: `Principal` is seeded by Track 01 in `Atlantis::Auth` with ownership
    transferring to Track 08 (blessed). Pre-approval on record: if Track 01's P3 probe
    (overlapping requests while a handler is parked on `await`) fails, the Handler type
    escalates to `HttpResponse | Promise<HttpResponse>` — bring the probe results to the
    owner either way.
  - **Coordination notes for the stdlib/Track-09 owner**: server-side streaming-response
    hook + `maxBodyBytes` parser knob (Track 01); incremental-SHA256/HMAC midstate (halves
    PBKDF2 cost, Track 08); JsonValue placement (prelude root vs `namespace json` —
    Track 03 templates want the stable spelling).
- 2026-07-07 (R10) — Owner authored `designs/atlantis/example/` and, after a comparison
  review, adopted it as the canonical app surface ("I like the example"). Ruling **R10**
  added; the wiring model inverts to **explicit-first with `@InjectX();` splices**
  (constructor-side-effect route registration is retired — it was the design's most
  magical piece and the splice model is more aligned with the language's explicit-over-
  implicit ethos). Contract updates: C2 gains Builder/App facade + `IActionResponse`
  (additive); C5 vocabulary updated (PrimaryKey/AutoIncrement/NotNull, splice attributes,
  `Id` retired, HasManyBelongsToMany gated on LA-27); §4 scaffold replaced with the
  example's PascalCase layout (`Resources/Views/*.lhtml`, `Public/`). LA-22 redefined as
  named splice invocation (better solution to the same problem — and to Track 04's
  @Injectable scoping); LA-25 (method references, new ticket), LA-26 (grouped statement
  attributes), LA-27 (relation-attribute expressiveness — the example's `self:` pipe
  comment is the owner ruling on M2M self-join semantics, lifted into Track 06). Enum
  (Track 03, STOP-blocked) flagged as now on Atlantis's critical path. Tracks 01/02/04/06/09
  updated with R10 sections; Tracks 03/05/07/08 unaffected (internals unchanged).
- 2026-07-07 (owner correction) — **`readonly` is NOT `const`.** Owner ruling: *"A const
  is compile time, readonly is run time. It will not be const."* R10's earlier disposition
  ("readonly is spelled const") was wrong and is corrected here, in the R10 row, and in
  Track 04 §3R: `readonly` is a distinct **runtime write-once** field modifier (assigned
  once during construction from runtime values — the injected-service case — immutable
  after), ticketed **LA-28** (`request-readonly.md`, P1). The example's `private readonly`
  spelling is kept verbatim everywhere; `const` (compile-time) is NEVER substituted for it
  (wrong semantics — these hold runtime-injected values). The ticket flags the
  `const`/`readonly` field-semantics division for the owner to formalize in the reference.
- 2026-07-18 (owner amendment) — **C1 rule placement inverted, retroactively.** The
  original "all attributes and all rules live in root `Atlantis`" clause is purged. New
  rule: rules and attributes live in the subsystem namespace that consumes them; nothing
  is imported that is not used; subsystem-internal metaprogramming (e.g. Orm's) is defined
  in that subsystem. Owner ruled retroactive with no grandfathering: landed
  `@Serializable`/`@JsonIgnore`/serializer rules/`ISerializable` relocate to
  `Atlantis::Json`, `@Injectable` + its rule to `Atlantis::App` (migration of
  `packages/atlantis/src/{json,di}` is a follow-up implementation task; app files then
  import per subsystem). Considered and rejected: a shared `Atlantis::Dto::{model,Json}`
  parent — ORM (`@Column`-selected fields on reference-class entities) and JSON
  (all-minus-`@JsonIgnore` fields on value-struct DTOs, class-level `@Serializable`)
  share no attribute vocabulary and only partially overlap in target types, so a shared
  parent would re-couple what this amendment decouples. Multi-consumer tiebreak recorded
  in C1; §0.6, §4 scaffold, and §8 sample updated to match; techdesign-07's `uses
  Atlantis;`-anchored MCP/OpenApi rules must move to `::Mcp`/`::OpenApi` (edit pending).
