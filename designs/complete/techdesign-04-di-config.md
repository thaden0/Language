# Atlantis Track 04 — DI, Config & App Bootstrap

**Status:** IMPLEMENTED IN FULL (M1–M4), 2026-07-13 — M5 gated per §3R (see §12 log).
**Date:** 2026-07-06 (authored); 2026-07-13 (implemented).
**Depends on:** `techdesign-00-overview.md` (rulings R3/R4/R9, contracts C1/C2/C5/C6, hurdle
H-5, LA register), `docs/reference.md` §4.7 (bind/inject), `designs/system-binds.md` §5
(namespace-wall ruling, Bindings channels), `designs/complete/techdesign-08-system-natives.md` F1
(argv/env — **not landed**, see §4.3), `designs/complete/techdesign-metaprog-phase3.md` (anchors, `meta.*`,
`where`), `designs/proposal-metaprogramming.md` §13-Q2 (rules may *emit* binds; they do not
subsume them).
**Owns (exclusive):** `Atlantis::Config`, `Atlantis::App`, the `@Injectable` attribute + its
rules (declared in namespace `Atlantis` per C1), scaffold conventions for `main.lev`,
`config/ioc.lev`, `config/database.lev`.
**Wave:** 1 (fully parallel; no hard deps on other Atlantis tracks).

---

## 0. Mission, scope, non-goals

**Mission.** Make the language's compile-time `bind`/`inject` the framework's *only* wiring
model (overview collapse #4), specify the configuration layer (C6), and give apps the
app-hosted command dispatcher (R3) — with every claim backed by a probe run against the
current compiler, not by hope.

**Scope.**
1. `@Injectable` (C5): what it can honestly do today, the target aggregation mechanism, and
   the exact rule text for both (§3).
2. The lifetime model without a runtime container (H-5): singleton / transient /
   request-scoped-on-`Context` — documented as THE model (§2).
3. `Atlantis::Config::load()`: strict `.env` parser, process-env override, typed config
   structs, `DbConfig`, secrets hygiene (§4).
4. Options pattern: prefix sections → structs, validate-at-boot with **all** errors listed (§5).
5. `Atlantis::App::run(argv, app)`: `serve` (default) | `migrate` | `routes` | `openapi` |
   `mcp` + app-defined commands (§6).
6. Composition-root conventions for the blessed scaffold + the testing story (§7).

**Non-goals.** No runtime container, no service locator, no reflection-driven registration
(STOP (a)/(c)). No scaffolding CLI (Trident owns it, R3). No auth/session config semantics
(Track 08) and no route table (Track 02) — this track only dispatches to their seams. No
`.env` interpolation, no YAML/TOML app config in v1 (the manifest is Trident's TOML; app
runtime config is `.env` + process env, C6).

**vs Loom (proposal §8.2–8.3).** Loom correctly picked "no DI container" but stopped at the
slogan: it used a decorative `@Inject` attribute the language doesn't need, never confronted
the namespace-wall/aggregation problem, had no lifetime model, and its config section was two
sentences. Atlantis specifies the lifetime semantics from probes (§1), designs the
aggregation problem honestly (§3), and ships a full config grammar with fail-fast validation
(§4–5) plus the R3 ops dispatcher Loom lacked entirely.

---

## 1. Evidence base — probe session 2026-07-06/07 (build/leviathan, current master)

Every design decision below cites this table. Probe sources live in the scratchpad; each is
≤ 20 lines and reproducible verbatim. **GREEN** = works as documented; **RED** = does not.

| # | Probe | Verdict |
|---|---|---|
| A1 | `bind IFace => sharedValue;` then two injections mutate one instance | **GREEN** — singleton semantics (printed 1, 2) |
| A2 | `bind IFace => Ctor();` arrow-with-constructor, two injections | **GREEN** — fresh per injection (printed 1, 1) |
| A3 | Implicit fill of a **function** parameter; explicit `inject IFace` argument | **GREEN** (matches `tests/corpus/di.ext`) |
| A4 | Implicit fill of a **constructor** parameter (`Service()` with `new Service(ILogger lg)`) | **RED** — "no overload of 'constructor' matches the arguments". Overview §8 cheat sheet and reference §4.7 imply it fills → doc/impl divergence, report to `/bug.md` |
| A5 | Explicit `inject IFace` **at a constructor call site** (`Service(inject ILogger)`) | **GREEN** |
| A6 | Chained factory binds: `bind ISvc => Svc(inject IDb);` | **GREEN** — the composition-root workhorse |
| A7 | `Bindings` object (`Bindings b = Bindings();`) | **RED** — "unknown type 'Bindings'". Documented in reference §4.7 (`bind someBindings;`) but not implemented (bug #9 remainder) |
| A8 | Bind inside `namespace Lib { … }` reaching an importing file via `uses Lib;` | **RED (by design)** — matches system-binds §5 ruling: `uses` imports names only |
| A9 | Channel 1: `use Lib::IThing;` activating the bind keyed on it | **RED** — ruled (system-binds §5) but not implemented |
| A10 | **Cross-file top-level bind** under a whole-program `trident run` (bind in `ioc.lev`, injection in `main.lev`) | **GREEN** — un-namespaced file-top binds land in the shared outermost scope, program-wide |
| A11 | Duplicate top-level bind for one type across two files | **GREEN (error)** — "duplicate binding for 'IGreeter' in this scope" — the compiler enforces the one-ioc-file convention for us |
| A12 | Rule `at namespace Reg` generating an item; `$C` as a **constructor call** in the template | **GREEN** — generated `Reg::make()` constructs the matched class |
| A13 | Two `@Injectable` classes each generating same-named `Reg::make()` | **RED (silent)** — no collision error; first match wins silently. Aggregation cannot ride same-named items |
| A14 | `$C` in **declared-name** position (`Fw::IThing $C() => $C();` at namespace anchor) | **RED (crash)** — `--expand` shows the intended tree, but `--run` **segfaults**: the spliced call `$C()` inside a function *named* `$C` self-captures (resolves to the function, infinite recursion). Compiler bug → `/bug.md` with repro |
| A15 | Identifier–hole fusion (`make$C`) for unique generated names | **RED** — parse error; no token pasting exists |
| A16 | `where C.bases.where((b) => b.startsWith("I")).length() == 0` negative-match validation rule | **GREEN** — `where` filters correctly (only the violating class got the injected member). Caveats: (i) a bare `this.someMissingField;` expression statement in the injected body did **not** produce a check error — use an unknown-*type* member instead (§3.4); (ii) cosmetic wrong warning "attribute matched no imported rule" when `where` excludes a decl |
| A17 | `bind` with a **struct** key (`bind AppConfig => cfg;`) | **RED (silent wrong value)** — parameter filled with a *default-constructed* struct (`":0"`), not the bound value → `/bug.md`; do not bind struct types until fixed |
| A18 | `bind` with a **concrete class** key (`bind Holder => h;`) | **GREEN** — bound shared instance flows through |
| A19 | `env::args()` / env access | **RED** — "unknown name 'env'"; Track 08 F1 not landed (its implementation log is empty) |
| A20 | `File` read/write + `using` inside a function block; whole-file read via `size()` + `read(n)`; `splitLines()` | **GREEN** — the `.env` loader needs nothing that doesn't exist today |

**Two compiler bugs and two doc/impl divergences found** (A4, A7, A14, A17). Per the
bug-reporting workflow they belong in `/bug.md` with repro + proposed ruling; this track's
brief is write-only-this-file, so the repros are preserved here (§8) for filing by the
coordinator. None blocks v1 (§10): the v1 design uses only GREEN rows.

---

## 2. The DI model Atlantis ships (H-5: lifetimes without a container)

There is no container. There are three lifetimes, all expressed with machinery that exists
and is probe-proven today. **This is the model, not a workaround.**

### 2.1 Singleton — bind a shared instance (A1, A18)

```
// config/ioc.lev — file top level (see §7 for why top level matters)
MySql::Pool dbPool = MySql::Pool(dbConfig);        // constructed once, program lifetime
bind Atlantis::Data::IDbPool => dbPool;            // every injection sees THIS instance
```

The body of a bind decides the lifetime: a bind whose body returns an existing value is a
singleton. Connection pools, config objects, the logger sink — singletons.

### 2.2 Transient — arrow-bind a constructor call (A2, A6)

```
bind IUserService => SqlUserService(inject Atlantis::Data::IDbPool);
```

`=> Ctor(…)` runs per injection: every injection site gets a fresh `SqlUserService`, whose
own dependency is filled by the nearest `IDbPool` bind — **chained factory binds** (A6) are
how the object graph composes without a container. Note the explicit `inject` inside the
factory body: implicit constructor fill is currently RED (A4); when that lands, the explicit
selector simply becomes optional — no design change.

### 2.3 Request-scoped — state lives on `Context` (C2), period

Binds are compile-time and lexical; a "request scope" is a runtime notion, so it cannot be a
bind. The request-scoped state carrier is the per-request `Context` object the kernel
constructs (C2: `Map<string, string> items` is the per-request bag). A service that needs
per-request state **takes the Context as a parameter** — capability-honest, greppable, and
free of ambient magic:

```
// Per-request logger with request id — the canonical example.
namespace Atlantis::Log {
    class RequestLog {
        Atlantis::Log::ILogger sink;               // singleton, injected once
        string requestId;
        new RequestLog(Atlantis::Http::Context ctx, Atlantis::Log::ILogger sink) {
            this.sink = sink;
            this.requestId = ctx.items.atOrNone("requestId") ?? "-";
        }
        void info(string msg) { sink.info("[${requestId}] ${msg}"); }
    }
}

// In a handler or service method — construct from the ctx you already have:
HttpResponse show(Atlantis::Http::Context ctx) {
    RequestLog log = RequestLog(ctx, inject Atlantis::Log::ILogger);
    log.info("showing user");
    ...
}
```

Rules of thumb (docs + demo app teach exactly these):
- Singleton state → bound shared instance. Per-request state → constructed from `Context`.
- A service holding per-request state as a *field* while bound as a singleton is the
  documented anti-pattern (it is cross-request data leakage).
- Middleware (Track 01) populates `ctx.items` ("requestId", trace ids); this track only
  documents the consumption pattern.

### 2.4 Where binds resolve — the lexical fine print (A3, A10, di.ext)

Injection resolves against the binds lexically in scope **at the call site of the injecting
call**, nearest-wins. Consequences the scaffold conventions (§7) encode:
- Binds intended app-wide go at **file top level, un-namespaced** (A10: they land in the
  shared outermost scope of the whole-program build; A8: namespace-wrapped binds do not leak).
- A bind written inside `main()`'s body shadows for calls *made from main's body* only —
  it does not reach injection sites inside framework functions. App-global wiring therefore
  never goes inside a function body; test shadowing (§7.4) deliberately does.
- Duplicate binding for one type in the top-level scope is a hard error even across files
  (A11) — the compiler enforces "exactly one file owns each app-wide bind".

---

## 3. `@Injectable` (C5) — the hard problem, designed honestly

### 3.1 The problem

`bind` is block-scoped and lexical, and binds do not cross namespace walls via `uses`
(A8, ruled in system-binds §5). So a rule that expands *inside the service's own
file/namespace* cannot place a bind where the composition root sees it. `@Injectable` needs
a way to move information from N scattered class declarations into ONE place the app
installs — an **aggregation** problem the rule engine (per-match, additive, no cross-match
fold) does not natively solve.

### 3.2 Candidates evaluated

**(b) Per-class generated factories + generated aggregate — REJECTED on probe evidence.**
Three independent kill shots: no token pasting for unique generated names (`make$C` is a
parse error, A15); naming the factory `$C` itself self-captures and segfaults the compiler
(A14 — and even with the hygiene bug fixed, a function named exactly like the class it
constructs is a resolution trap); same-named aggregate items silently first-match-win
instead of colliding (A13), so there is no honest merge point. Nothing here is one fix away;
(b) is structurally wrong under current metaprogramming.

**(a) Rule appends entries into a well-known `Bindings` builder; `main.lev` installs
`bind Atlantis::Ioc::collect();` — the TARGET mechanism, gated on three asks.** This is the
right end state: one greppable install line, `--expand` shows every generated entry, zero
runtime reflection. It is not buildable today because:
1. The `Bindings` runtime object does not exist (A7) — documented in reference §4.7,
   unimplemented (bug #9 remainder; system-binds §7 gate 1/2).
2. The `marker` anchor is searched **within the matched subject's body only** (metaprog
   phase-3 §8.2) — a rule matching `@Injectable class C` in app code cannot target a marker
   inside `Atlantis::Ioc::collect()`. Needs a **cross-decl (global) marker anchor** or an
   aggregate anchor — new language ask.
3. The rule must splice the *interface type* into the entry. `meta.bases` is strings; a
   string cannot become a type token in a template today — needs an
   **attr-string-to-token splice** (or LA-4-adjacent computed-name splice) — new language ask.

**(c) `@Injectable` validates + documents; `config/ioc.lev` stays hand-written `bind`
lines — the zero-magic floor. CHOSEN PRIMARY for v1.** Every ingredient is GREEN today
(A1/A2/A6/A10/A11/A18). The hand-written ioc file is small (one line per service), the
compiler itself rejects duplicates and ambiguity, and `--expand`-grade greppability is
trivially exceeded (the binds are literally source text). Per R4 we design assuming the
asks land — but we do not *gate the framework* on them when the floor is this good.

**Decision: primary = (c) now; (a) is the specified successor that activates when its three
asks land.** The ioc.lev convention is designed so adoption of (a) is a deletion (replace a
block of bind lines with one `bind Atlantis::Ioc::collect();`), not a migration.

### 3.3 Which interface does it bind? (the meta.bases constraint)

`meta.bases` gives canonical base *strings* with no interface-vs-class discrimination at
rule time. Resolution, honestly:
- **v1 (mechanism c):** the question is moot for wiring (humans write the bind and name the
  interface), and handled for *validation* by convention: `@Injectable` requires **at least
  one base whose name (last `::` segment) starts with `I` followed by an uppercase letter** —
  the framework's stated interface-naming convention (C3/C4 already follow it). Documented
  limitation: a concrete class named `IcedTea` as a base would fool the check; the check is
  a lint, not the wiring.
- **Target (mechanism a):** sidestep inference entirely — **the attribute names the
  interface explicitly**: `@Injectable("IUserService")`. Explicit beats inferred here: it
  survives multi-interface services (pick which contract to bind), it reads at the
  declaration site, and it is the only shape a rule can act on without new meta surface.
  The C5 vocabulary line `Injectable {}` gains an optional field:
  `Injectable { string iface = ""; }` — `""` = validate-only (v1 behavior forever, so v1
  code never breaks).

### 3.4 v1 rule text (mechanism c — validation + documentation)

Lives in namespace `Atlantis` (C1: one `uses Atlantis;` opts in). Two pieces:

```
namespace Atlantis {
    attribute Injectable { string iface = ""; }   // iface used by mechanism (a) later

    // Validation: an @Injectable class with no I-prefixed base gets a member whose
    // RETURN TYPE is unknown — the resolver error names the problem legibly.
    // (A16: bare `this.missingField;` statements are NOT reliably diagnosed; an
    // unknown type in a signature is. Probe P-5 pins the exact error text.)
    rule injectableRequiresInterface {
        match @Injectable on class C
            where C.bases.where((b) => b.startsWith("I")).length() == 0
        inject `ERROR_Injectable_class_needs_an_interface_base_see_atlantis_di_docs
                    atlantisInjectableContractCheck();`
            at member of C
    }
}
```

What v1 `@Injectable` buys (and claims — no more): (i) the convention check above;
(ii) discoverability — `leviathan --rules` / `--expand` list every service class;
(iii) forward-compatibility — code carrying `@Injectable("IUserService")` today upgrades to
mechanism (a) with zero edits. The wiring truth stays in `config/ioc.lev` (§7.2).

### 3.5 Target rule text (mechanism a — activates when asks land)

Recorded now so the asks are designed against a concrete consumer (R4). Requires: Bindings
object (ask α), cross-decl marker anchor (ask β), attr-string-to-type-token splice (ask γ).

```
namespace Atlantis {
    rule injectableCollect {
        match @Injectable(i) on class C
            where i.iface != ""
        // $i.iface splices as a TYPE token (ask γ); anchor is the framework-side
        // marker in Atlantis::Ioc::collect() (ask β — cross-decl marker).
        inject `b.add($i.iface => $C());`
            at marker "atlantis.ioc.entries"
    }
}

namespace Atlantis::Ioc {
    Bindings collect() {
        Bindings b = Bindings();                   // ask α (reference §4.7 semantics:
        @anchor("atlantis.ioc.entries")            //  .add duplicates error, .replace overrides)
        return b;
    }
}

// main.lev — the whole upgrade:
bind Atlantis::Ioc::collect();                     // install-time duplicate checking (§12.5)
```

Lifetime note for (a): `b.add(I => C())` entries are transient (factory per injection);
singletons keep hand-written shared-instance binds in ioc.lev — mixing generated transients
with hand-written singletons is fine because install-time collision checking makes overlap
loud. `.replace` is the app's escape hatch to override a generated entry deliberately.

### 3.6 Q2 alignment

Per `proposal-metaprogramming.md` §13-Q2: rules do not subsume bind/inject; a rule MAY emit
binds. Mechanism (a) conforms exactly — the rule emits *builder entries*; the app's single
lexical `bind` statement remains the only activation act (system-binds §5 channel 2).

---

## 3R. R10 addendum (2026-07-07) — `@InjectBindings();` supersedes channel (a)'s spelling

Overview ruling R10: the owner's example writes the bindings file as an ordinary function
with a **user-placed splice statement** — and this dissolves §3's hard problem better than
any of the three candidate mechanisms:

```
namespace App {
    void AddBindings() {
        @InjectBindings();                    // @Injectable-generated binds land HERE
        bind IUserService => UserService();   // hand-written binds beside them
    }
}
```

Rules targeting `at splice InjectBindings` (LA-22, `request-metaprog-splices.md` item E)
inject their `bind` statements **into the splice site's own lexical scope** — no
marker-file indirection, no `collect()` Bindings object (so no dependency on bug.md #25),
duplicate-bind collisions surface at the splice site exactly like hand-written code.
Mechanism ladder is now: **(c) hand-written binds — v1 primary, unchanged and shipping
today** → **(E-splice) once LA-22 lands** — mechanism (a) (`bind Atlantis::Ioc::collect();`)
is retired as the target; its analysis in §3 stands as the record of why the splice wins.
`@Injectable`'s own semantics are unchanged (explicit `@Injectable("IUserService")`
interface selection; validation lint in the interim).

**Composition root shape (replaces §7's `main.lev` sketch — same content, example's
structure):**

```
namespace app {
    class Main() {
        private readonly Atlantis::Builder builder;  private readonly Atlantis::App app;
        new Main() {
            builder = Atlantis::Builder();
            app = builder.Build();                 // Config loaded here (C6)
            app.useAuthentication();  app.useAuthorization();
            App::Routes::addRoutes(app);           // Config/Routes.lev (Track 02 §0R)
            App::AddBindings();                    // Config/Bindings.lev (above)
        }
    }
}
```

(Note for implementers: `class Main()` parens are not current language — plain `class
Main`. **`private readonly` IS the intended spelling** — `readonly` is a distinct runtime
write-once field modifier (owner ruling 2026-07-07, NOT `const`: `const` is compile-time,
`readonly` is runtime write-once), landed LA-28 (`designs/techdesign-readonly.md`); every
service field a controller/handler holds by DI is `readonly`. `App::run(argv)` command
dispatch (§6) is unchanged and wraps `Main`.)

---

## 4. Configuration (C6) — `Atlantis::Config`

### 4.1 Surface

```
namespace Atlantis::Config {
    interface IConfigException : IException { }
    class ConfigException : Exception, IConfigException {
        Array<string> errors;                      // every problem, not the first
        new ConfigException(Array<string> errs) {
            this.errors = errs;
            this.message = "configuration errors:\n" + errs.joinToString("\n");
        }
    }

    class Config {
        Map<string, string> values;
        new Config(Map<string, string> values) { this.values = values; }

        string? atOrNone(string key) => values.atOrNone(key);
        string  at(string key, string fallback) => values.atOrNone(key) ?? fallback;
        string  require(string key) { ... throw ConfigException(["missing key '${key}'"]) ... }

        // Typed reads for the struct-binding pattern (§4.4): loud, error-accumulating.
        int  readInt(string key, int fallback, Array<string> errs) { ... }
        bool readBool(string key, bool fallback, Array<string> errs) { ... }
        // bool grammar is STRICT: "true" | "false" only. int via string.toInt()
        // (strict optional parse). Bad value appends "key 'X': expected int, got 'abc'".

        Config section(string prefix);             // §5
        string dump();                             // §4.5 — ALWAYS redacted
    }

    Config load() => load(".env");
    Config load(string path) { ... }               // §4.2 + §4.3
}
```

### 4.2 `.env` grammar — minimal and strict (all-errors-then-throw)

Loader: if `std::fileExists(path)` is false → empty map (a missing `.env` is legal; process
env and defaults still apply). Otherwise read the whole file (`f.read(f.size())` — **not**
`readln()`, whose `"" = EOF` cannot represent blank lines, A20) and `splitLines()` (handles
trailing `\r`). Per line, in order; violations accumulate into `errs` (line-numbered) and one
`ConfigException` throws at the end listing all of them:

1. Trim. Empty line or first char `#` → skip. (No inline `# comments` after values —
   a `#` in an unquoted value is part of the value; keep the grammar decidable.)
2. Must contain `=`: split at the **first** `=`. No `export ` prefix, no `:` syntax.
3. **Key:** `[A-Za-z_][A-Za-z0-9_]*` checked by charset scan (no regex, LA-13). Convention
   (docs + samples): UPPER_SNAKE.
4. **Value:** if it starts with `"` it must end with a matching `"`; escapes are exactly
   `\"`, `\\`, `\n`, `\t` (anything else after `\` = error). Otherwise the trimmed raw text.
   Single quotes have no special meaning (minimal grammar; one quoting form).
5. **No interpolation** (`${…}` is literal — C6), no line continuations, no multiline values.
6. **Duplicate key = error** (fail-fast ethos; mirrors duplicate-bind).

### 4.3 Precedence and the Track 08 dependency

C6 precedence: **process env > `.env` > defaults** (defaults = the fallback arguments at
read sites, §4.4). Process-env overlay requires Track 08 F1 (`std::env::get(string) ->
string?` per `argv.md`) — **not landed** (A19; Track 08's implementation log is empty).

- **Now (interim):** `load()` is `.env`-only. `dump()` prints a fixed notice line
  `"(process-env overlay inactive: std::env pending)"` so the truth is visible at boot.
- **When F1 lands:** overlay step — for each key **already present** from `.env`, replace
  the value with `env::get(key)` if that returns non-None; additionally, keys read via
  `require`/`readX` that are absent from `.env` consult `env::get` before falling back.
  (Enumerating the whole process environment is not required and not asked for.)
- No design change either way — the overlay is one internal function. Flip recorded in §12.

### 4.4 Typed config structs — the binding pattern (C6)

Apps declare value structs and build them **once, at boot, with error accumulation**:

```
struct AppConfig { string host; int port; bool debug; }

// C6-frozen shared shape (C3-adjacent; Track 05 consumes):
struct DbConfig { string host; int port; string user; string password; string database; }

// config/database.lev
Atlantis::Config::Config raw = Atlantis::Config::load();

DbConfig loadDbConfig(Atlantis::Config::Config c, Array<string> errs) =>
    DbConfig(
        c.at("DB_HOST", "127.0.0.1"),
        c.readInt("DB_PORT", 3306, errs),
        c.require("DB_USER"),                      // no default for credentials — absent = boot error
        c.require("DB_PASSWORD"),
        c.require("DB_DATABASE"));
```

**How typed configs reach consumers.** C6 says "binds it in the composition root" — probe
A17 found `bind` with a **struct key silently injects a default-constructed value** (a
compiler bug, §8). Until that is fixed, struct-typed binds are **forbidden in Atlantis
code** (a foreseeable-problems row and a STOP tripwire). The v1 pattern, fully within GREEN
territory: config structs are **passed explicitly at composition time** — they are built in
the root and handed to constructors/factory binds (`bind IDbDriver => MySql::Driver();`
receives `DbConfig` via `connect(cfg)` per C3; services take config in their constructors
wired in ioc.lev). The `Config` object itself is a class → bindable today (A18):
`bind Atlantis::Config::Config => raw;` for the rare consumer that wants raw access. When
the struct-bind bug is fixed, `bind AppConfig => appConfig;` becomes available and C6's
wording is literal — additive, no migration.

### 4.5 Secrets hygiene (values never logged)

- `Config` has **no** `toString()`. The only stringifier is `dump()`, which redacts the
  value of any key whose name contains `PASSWORD`, `SECRET`, `TOKEN`, `KEY`, or `PRIVATE`
  (case-insensitive via `toUpper()` scan) to `"****"` — and `dump()` is for the operator
  (`./app config` is deliberately NOT a builtin; printing config is an explicit app choice).
- Error messages (ConfigException, validators) name **keys only, never values** — the §4.2
  parser reports "line 7: bad value for 'DB_PASSWORD'" without echoing the value.
- `DbConfig.password` is never interpolated by framework code; Track 05's connection
  errors must echo host/port/user/database only (noted as a C3-adjacent requirement).
- Docs: `.env` goes in `.gitignore` in the blessed scaffold; a committed `.env.example`
  carries keys with empty values.

---

## 5. Options pattern — named sections, validated at boot

Sections are **key prefixes** (the `.env` universe is flat; no new syntax):

```
// .env
SMTP_HOST=mail.example.com
SMTP_PORT=2525
CACHE_TTL_SECONDS=300
```

`Config.section("SMTP_")` returns a new `Config` whose map holds the matching keys with the
prefix stripped (`whereEntries((k, v) => k.startsWith(prefix))` + rebuild loop) — so section
structs read bare names and stay reusable:

```
struct SmtpOptions { string host; int port; }

SmtpOptions loadSmtp(Atlantis::Config::Config c, Array<string> errs) {
    Atlantis::Config::Config s = c.section("SMTP_");
    return SmtpOptions(s.at("HOST", ""), s.readInt("PORT", 25, errs));
}

void validateSmtp(SmtpOptions o, Array<string> errs) {
    if (o.host.isBlank()) { errs = errs.add("SMTP_HOST: must not be blank"); }
    if (o.port < 1 || o.port > 65535) { errs = errs.add("SMTP_PORT: out of range"); }
}
```

**Boot contract (fail fast, list everything):** the composition root threads ONE
`Array<string> errs` through every `loadX`/`validateX` (arrays are COW values — loaders
return updated arrays or take/return; the demo app uses the return style consistently).
After all sections load: if `errs` is non-empty, print every line, and exit non-zero
(exit code 2; until `std::exit` lands — Track 08 — `App::run` returns 2 and `main` ends,
§6.4). One run reveals every config mistake, not the first.

---

## 6. `Atlantis::App` — the app-hosted command dispatcher (R3)

Trident scaffolds and builds; it never runs dependency code. The built app binary hosts its
own operations: `./myapp serve | migrate | routes | openapi | mcp | <app-defined>`.

### 6.1 Surface

```
namespace Atlantis::App {
    class Command {
        string name;
        string description;
        (Atlantis::Config::Config, Array<string>) => int handler;   // P-7 probes fn-typed fields
        new Command(string name, string description,
                    (Atlantis::Config::Config, Array<string>) => int handler) { ... }
    }

    class Application {
        string name = "app";
        Atlantis::Config::Config config;
        Array<Command> commands = [];              // builtins + app-defined, uniform
        void register(Command c) { commands = commands.add(c); }
    }

    int run(Application app, Array<string> argv) {
        // argv includes the program name at [0] when it comes from env::args().
        string cmd = "serve";                       // DEFAULT: no subcommand = serve
        Array<string> rest = [];
        if (argv.length() >= 2) { cmd = argv[1]; rest = argv.skip(2); }
        if (cmd == "help" || cmd == "--help") { printUsage(app); return 0; }
        Command? found = app.commands.find((c) => c.name == cmd);
        if (found == None) { printUsage(app); return 1; }   // unknown → usage, exit 1
        return found.handler(app.config, rest);
    }
}
```

If P-7 (function-typed field) probes RED, `Command` becomes
`interface ICommand { string name(); string description(); int run(Config cfg, Array<string> args); }`
with `Array<ICommand>` — same dispatcher, zero design change elsewhere (interfaces are
bedrock-GREEN). The record shape ships if the probe is green; the doc records whichever.

### 6.2 Builtins are registrations, not dependencies

Track 04 does not import Orm/Mcp/OpenApi (C1 ownership). The owning tracks export command
**factories**; `main.lev` registers them like any app command:

```
app.register(Atlantis::App::Command("serve",   "Run the HTTP server",  serveHandler));  // Track 01 seam
app.register(MySqlAppGlue::migrateCommand());   // Track 06 exports migrateCommand() -> Command
app.register(Atlantis::Routing::routesCommand());   // Track 02: print route table
app.register(Atlantis::OpenApi::openapiCommand());  // Track 07
app.register(Atlantis::Mcp::mcpCommand());          // Track 07
```

An unregistered builtin is simply absent from usage — a JSON-API app without the ORM has no
`migrate`, honestly. The extension seam for app-defined commands is the same `register`
call (worked example: `seed`, §7.3).

### 6.3 argv status and fallback

`App::run` needs `env::args()` — Track 08 F1, **not landed** (A19). Per R4 the design
assumes it lands; the fallback is explicit and tiny:

```
void main() {
    ...
    int code = Atlantis::App::run(app, []);        // TODAY: empty argv → always "serve"
    // WHEN Track 08 F1 lands (one-line flip, recorded in §12):
    // int code = Atlantis::App::run(app, env::args());
}
```

Serve-only until argv lands: `migrate`/`routes`/`openapi`/`mcp` are unreachable from the
shell but fully wired — the flip is one argument.

### 6.4 Exit codes

`0` success · `1` unknown command / command failure (handlers return their own non-zero) ·
`2` configuration validation failure (§5). Until `std::exit(int)` lands (Track 08),
`run`'s return value is the app's last word: the demo `main` stores it and, interim,
prints `"exit: ${code}"` on non-zero (documented, ugly, temporary). When `std::exit`
lands: `std::exit(code);` at the end of `main` — flip recorded in §12.

---

## 7. Composition-root conventions + worked example

**What goes where (the blessed scaffold, overview §4):**
- **`main.lev`** — the only entry: load+validate config, build `Application`, register
  commands, fold the pipeline (C2, Track 01's seam), call `App::run`. NO binds inside
  `main()`'s body (§2.4); `main.lev` may own top-level binds that are app-wide but not
  service wiring (in practice: none — keep them in config/).
- **`config/ioc.lev`** — ALL service binds, at file top level, **un-namespaced** (A10).
  Exactly this file owns them (A11 makes a second owner a compile error). One bind per
  line, one comment block per subsystem. This file is what mechanism (a) later deletes.
- **`config/database.lev`** — `DbConfig` construction from `Config` + the driver/pool
  binds (`IDbDriver`, `IDbPool`). DB wiring is the most-swapped wiring (tests, drivers) —
  isolating it pays.

### 7.1 `config/ioc.lev`

```
// config/ioc.lev — the composition root's wiring. Top-level binds: program-wide (A10).
uses Atlantis;

// --- infrastructure (singletons: shared instances) ---
Atlantis::Log::ConsoleLogger appLogger = Atlantis::Log::ConsoleLogger();
bind Atlantis::Log::ILogger => appLogger;

// --- application services (transients: factory binds, deps chained via inject) ---
bind App::Services::IUserService =>
    App::Services::SqlUserService(inject Atlantis::Data::IDbPool,
                                  inject Atlantis::Log::ILogger);
```

### 7.2 `config/database.lev`

```
// config/database.lev
uses Atlantis;

Atlantis::Config::Config appRaw = Atlantis::Config::load();   // shared load (singleton value)
bind Atlantis::Config::Config => appRaw;                       // raw config, bindable (A18)

DbConfig loadDbConfig(Atlantis::Config::Config c, Array<string> errs) =>
    DbConfig(c.at("DB_HOST", "127.0.0.1"), c.readInt("DB_PORT", 3306, errs),
             c.require("DB_USER"), c.require("DB_PASSWORD"), c.require("DB_DATABASE"));

bind Atlantis::Data::IDbDriver => MySql::Driver();             // C3: driver selection
MySql::Pool dbPool = MySql::Pool();                            // configured in main (explicit cfg pass)
bind Atlantis::Data::IDbPool => dbPool;
```

### 7.3 `main.lev`

```
// main.lev — composition root. trident.toml: entry = "main".
uses Atlantis;

void main() {
    // 1. Config: load happened at top level (database.lev); validate everything, fail fast.
    Array<string> errs = [];
    DbConfig db = loadDbConfig(inject Atlantis::Config::Config, errs);
    App::Cfg::SmtpOptions smtp = App::Cfg::loadSmtp(inject Atlantis::Config::Config, errs);
    errs = App::Cfg::validateSmtp(smtp, errs);
    if (errs.length() > 0) {
        for (string e in errs) { console.writeln("config: ${e}"); }
        console.writeln("exit: 2");                 // std::exit(2) once Track 08 lands
        return;
    }
    dbPool.configure(db);                           // explicit struct pass (§4.4, A17)

    // 2. Pipeline (C2): explicit ordered middleware folded in Track 01's seam.
    Array<Atlantis::Http::Middleware> pipeline =
        [ Atlantis::Http::requestId(), Atlantis::Http::logging(), App::Mw::session() ];

    // 3. App + commands (R3).
    Atlantis::App::Application app = Atlantis::App::Application();
    app.name = "myapp";
    app.config = inject Atlantis::Config::Config;
    app.register(Atlantis::App::Command("serve", "Run the HTTP server",
        (cfg, args) => Atlantis::Http::serve(cfg, pipeline)));      // Track 01 seam
    app.register(MySqlAppGlue::migrateCommand());
    app.register(Atlantis::App::Command("seed", "Load fixture data",
        (cfg, args) => App::Fixtures::seed(inject App::Services::IUserService)));

    int code = Atlantis::App::run(app, []);          // env::args() once Track 08 lands (§6.3)
    if (code != 0) { console.writeln("exit: ${code}"); }
}
```

### 7.4 Testing story — nearest-wins shadowing, complete example

Tests are their own entry programs compiled with the app sources (overview C8 corpus
discipline). The app's top-level binds are the *outer* scope; a test shadows **inside its
function body** — never at test-file top level (that would be the A11 duplicate error):

```
// tests/user_service_test.lev — own trident target / test entry.
uses Atlantis;

class FakeUserService : App::Services::IUserService {
    Array<string> served = [];
    Array<App::Models::UserDto> list() =>
        [ App::Models::UserDto(1, "alice"), App::Models::UserDto(2, "bob") ];
}

void main() {
    // Nearest-wins: this block's bind shadows config/ioc.lev's SqlUserService bind
    // for every injection resolved at call sites within this body (di.ext semantics).
    bind App::Services::IUserService => FakeUserService();

    App::Controllers::UserController c =
        App::Controllers::UserController(inject App::Services::IUserService);

    Array<App::Models::UserDto> got = c.index();
    if (got.length() != 2)          { console.writeln("FAIL: expected 2 users"); return; }
    if (got[0].name != "alice")     { console.writeln("FAIL: expected alice");   return; }
    console.writeln("PASS user_service_test");
}
```

No mocking framework, no container reconfiguration, no test-only assembly: the `bind` *is*
the mock installation. (vs Loom §8.2: Loom claimed this property; this track probed the
exact file/scope layering that makes it true — top-level outer scope + block shadow — and
documents the one trap, top-level duplicate, that Loom's sketch would have hit.)

---

## 8. P-probes

Executed 2026-07-06/07 (results in §1, sources reproducible from the table): A1–A20.
**Probe #1 (the @Injectable anchor mechanism) = A12–A15 — executed;** verdicts drove §3.2.
Bug repros preserved for `/bug.md` filing (coordinator): **A14** (segfault: rule template
`Fw::IThing $C() => $C();` at `namespace Reg`, one `@Injectable class UserService : IThing`,
`--expand` correct / `--run` SIGSEGV; proposed ruling: spliced `$C` in call position must
resolve to the class constructor, never the enclosing generated decl; declared-name splices
that collide with an in-scope type should be a rule-stage error), **A17** (struct-key bind
silently injects default-constructed struct; proposed ruling: struct keys either work by
value-copy or are a compile error — silence is the one wrong answer), **A4** (ctor implicit
fill vs reference §4.7/overview §8), **A7** (`Bindings` documented, missing).

Pending probes (run before the milestone that consumes them):

| # | Probe | Expect | Consumer |
|---|---|---|---|
| P-5 | §3.4 validation rule verbatim: unknown-return-type member on violating class; error text contains `ERROR_Injectable_class_needs_an_interface_base` | compile error naming the marker type; GREEN class unaffected (A16 shape) | M4 |
| P-6 | `Bindings` `.add` shape once bug-#9 remainder lands: `b.add(IFace => Impl());` vs `.add` with two args; duplicate `.add` = install-time error; `.replace` overrides | matches reference §4.7 + info.md §12.5 | M5 gate |
| P-7 | Function-typed field in `Command` + `Array<Command>` + call through field | GREEN, else ICommand fallback (§6.1) | M3 |
| P-8 | Nearest-wins layering exactly as §7.4: app sources with top-level ioc.lev bind + test body shadow, under `trident run` multi-file build | test sees fake; removing shadow sees real | M2 |
| P-9 | `section()` map rebuild (`whereEntries` + entries loop) on oracle+IR+LLVM (Map bracket-sugar bug #18 discipline) | identical output on all three | M1 |
| P-10 | Mechanism (a) end-to-end (rule → marker in foreign decl → `bind collect()`) | blocked until asks α/β/γ land; run as the M5 gate | M5 |

---

## 9. Foreseeable problems

| # | Problem | Mitigation |
|---|---|---|
| 1 | `Bindings` object documented but unimplemented (A7) | v1 never uses it; mechanism (a) and channel-2 gated behind it (M5); filed via coordinator |
| 2 | `$C`-named generated decl self-captures → compiler segfault (A14) | Mechanism (b) rejected outright; bug filed; no Atlantis rule ever names a decl `$C` |
| 3 | Implicit constructor injection missing despite docs (A4) | Composition root uses explicit `inject` at ctor call sites + chained factory binds (A5/A6, both GREEN); doc divergence reported; lands later = optional sugar, zero migration |
| 4 | Struct-typed bind silently injects default value (A17) | Struct binds FORBIDDEN in Atlantis code until fixed (STOP tripwire); config structs passed explicitly; C6 "binds it" wording becomes literal post-fix |
| 5 | `meta.bases` can't distinguish interface from class | v1: I-prefix convention lint only; target: explicit `@Injectable("IFace")` — inference never required |
| 6 | Binds in `main()` body don't reach framework-internal call sites (lexical resolution) | Convention: app-wide binds at file top level only (§2.4/§7); demo app models it; docs call it out with the failing counter-example |
| 7 | Test file re-binding at top level = duplicate-bind error (A11) | Testing story mandates block-scope shadowing (§7.4); the error message itself points the right way |
| 8 | `readln()` can't distinguish blank line from EOF | `.env` loader reads whole file + `splitLines()` (A20) |
| 9 | Process-env override blocked on Track 08 F1 (A19) | `.env`-only interim with a visible boot notice; overlay is one internal function; flip logged |
| 10 | Cosmetic wrong warning when `where` excludes all rules for an attributed decl (A16) | Harmless; note filed with the A-series bugs so diagnostics can special-case where-excluded matches |
| 11 | Two files calling `Config::load()` double-parses `.env` | Convention: `database.lev` owns the single top-level load + `bind Config => appRaw;` everyone else injects |

---

## 10. Milestones & acceptance

| M | Scope ("done" =) | Target |
|---|---|---|
| **M1** | `Atlantis::Config`: loader + strict `.env` grammar (§4.2) with all-errors ConfigException; typed-read helpers; `section()`; `dump()` redaction. Corpus programs (good env / every malformed-line class / duplicate key / section+options) pass identically on oracle+IR+LLVM (P-9) | 2026-07-20 |
| **M2** | Composition-root conventions landed as the demo-app skeleton: ioc.lev/database.lev/main.lev exactly as §7; lifetime examples (singleton/transient/Context-scoped) as runnable corpus; P-8 test-shadow probe green and committed as the testing template | 2026-07-27 |
| **M3** | `Atlantis::App`: Command/Application/run + usage/help + exit-code contract; P-7 resolved (record vs ICommand recorded here); serve-only argv fallback wired; command registration corpus (unknown cmd → usage+1; app-defined `seed` runs) | 2026-08-10 |
| **M4** | `@Injectable` v1: attribute + validation rule (§3.4) with P-5 green; `--rules`/`--expand` discoverability documented; demo services annotated | 2026-08-17 |
| **M5** | Mechanism (a) — **gated, unscheduled**: activates when asks α (Bindings), β (cross-decl marker), γ (attr-string type splice) land; gate = P-6 + P-10 green; delivery = `Atlantis::Ioc::collect()` + demo ioc.lev shrunk to singletons + one `bind` line | on asks |

Feeds AG-1 (2026-09-15): the demo hello-app's boot path (config → binds → pipeline →
`App::run` → Track 01 serve) is M1–M3 output. Wave-1 independence holds: nothing above
waits on Tracks 01/03/05 (the serve handler is a seam, not a dependency).

## 11. STOP conditions (per overview §0.4, plus track-specific)

STOP — log here, commit WIP, escalate — if any implementation step would:
- introduce a runtime container, service locator, string-keyed registry, or any second
  wiring model (violates (c)/collapse #4) — including "temporary" runtime registries to
  fake mechanism (a);
- need runtime type/attribute introspection for `@Injectable` (violates (a));
- touch `src/**`, `tools/**`, `runtime/**` — A4/A7/A14/A17 are owner-side fixes, never ours;
- bind a struct type anywhere while problem #4 stands, or work around A17 by mutating
  injected defaults;
- require regex in the `.env` parser (charset scans suffice; else it's an LA-13 escalation);
- change C6 precedence, the C5 `Injectable` field shape (the optional `iface` addition
  needs owner sign-off before M4 ships it), or R3's app-hosted command ruling;
- find `trident run` multi-file bind behavior (A10/A11) changed by a toolchain update —
  those two probes are the scaffold's load-bearing wall; re-run them at every milestone.

## 12. Implementation log (append-only)

- 2026-07-06/07 — Design authored. Probe session A1–A20 executed against current
  `build/leviathan` + `build/trident` (results §1; scratchpad sources). Decisions:
  mechanism (c) primary for v1, (a) specified as gated successor with explicit
  `@Injectable("IFace")` interface selection; lifetimes = shared-bind / factory-bind /
  Context-state; `.env` grammar frozen at §4.2. Found for filing: segfault A14,
  silent-struct-bind A17, ctor-fill divergence A4, missing `Bindings` A7, cosmetic
  warning A16-ii. New language asks for the LA register: cross-decl marker anchor (β),
  attr-string-to-type-token splice (γ); Bindings completion (α) is bug-#9 remainder /
  system-binds §7 gate, not a new ask. Track 08 F1 (argv/env, exit) confirmed not landed;
  fallbacks wired into §4.3/§6.3/§6.4.
- 2026-07-07 (R10) — §3R added: `@InjectBindings();` named splice (LA-22, splices-ticket
  item E) supersedes mechanism (a) as the generated-bindings target — spliced binds land
  in the splice site's lexical scope by construction, killing the scoping problem and the
  bug-#25 dependency. Ask (β) folded into LA-22's new definition; (γ) now rides
  LA-16 in the splices ticket. v1 remains hand-written `Config/Bindings.lev` (unchanged).
  Composition root restructured to the example's `Main` class + Builder/App staging.
- 2026-07-07 (coordinator, post-merge) — master landed argv (`env::args()`, all engines)
  and exit codes since this doc was written: §6's "serve-only until argv lands" fallback
  is OBSOLETE — `App::run(argv)` command dispatch is unblocked from day one. `.env`-only
  config note (§4.3) likewise: process-env override can ship v1 if Track 08's env-var
  read surface is included in what landed — implementer verifies at M1.
- 2026-07-13 (implementer) — **IMPLEMENTED IN FULL (M1–M4); M5 stays gated per §3R.**
  Landed the framework package: `Atlantis::Config` (`src/config/config.lev` — strict `.env`
  parser, typed error-accumulating reads, `section()`, redacted `dump()`),
  `Atlantis::App` (`src/app/app.lev` — `Command`/`Application`/`run` dispatcher, argv as a
  parameter), and `@Injectable` (`src/di/injectable.lev` — attribute + sentinel-guard
  validation rule). Acceptance corpora byte-identical on oracle/IR/LLVM
  (`tests/corpus/{config,app,di}`, results in `tests/RESULTS.md`); `@Injectable` negative
  case (P-5) errors at compile time as intended (`tests/probes/di_p1_injectable_negative.lev`).
  Milestone probes resolved: **P-5** green (named compile error), **P-7** GREEN (fn-typed
  field `Command` record ships — no `ICommand` fallback), **P-8** the nearest-wins shadow
  works via the `=> Ctor()` factory form this doc already mandates (§7.4
  `=> FakeUserService()`), **P-9** section/dump identical on all three engines.
  Coordinator note confirmed: argv/exit had landed, so §6's serve-only fallback is retired.
  **Design-vs-reality corrections** (full detail in `tests/RESULTS.md`): (1) error
  accumulation is a **reference collector** (`Config::Errors`), not an `Array<string>`
  parameter — an Array parameter is a value copy and can't accumulate (probe-proven);
  (2) external entry points are reached **bare via `uses`**, never by deep qualification —
  a doubly-nested free function doesn't resolve fully-qualified from outside `Atlantis`
  (IR can't even lower the deep name); (3) `@Injectable` validation uses the **sentinel-guard
  idiom** (Track 03's `where … && false` + throwing guard), superseding §3.4's
  inject-unknown-member sketch (A16); (4) `Map()` needs a declared-target binding to infer
  K/V in a body, and flow narrowing doesn't carry past an early `return`/`continue` (guards
  use `else` arms). **New language findings filed** (bug.md): **#55 [P2]** a bare
  named-function reference stored in a function-typed field fails to resolve when dot-called
  (lambda-wrap works — the design's own §7.3 `Command`-handler idiom); **#56 [P2]** a
  block-scope `bind IFace => localVar;` yields an injection value whose method dispatch fails
  (the mandated `=> Ctor()` test-shadow and `=> globalVar` singleton forms both work, so no
  design pattern is affected). The struct-key bind bug (A17/§8) remains out of scope —
  Track 04 binds only classes/interfaces, never struct types (foreseeable-problem #4 holds).
