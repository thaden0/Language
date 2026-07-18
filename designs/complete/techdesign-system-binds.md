# Tech design — System-interface binds: capability injection over `bind`/`inject`

**Status:** LANDED IN FULL 2026-07-14 (M1–M3, all four active engines: oracle/IR/emit-C++/
LLVM). **Date:** 2026-07-13.
**Source:** `designs/requests/accepted/system-binds.md` (the accepted request; §5's two-channel
namespace-wall ruling is owner-ruled, 2026-07-05) + `docs/system-binds-research.md` (the
pre-design dossier, 2026-07-13 — every implementation claim below that is not re-derived here
is anchored there, and through it to `file:line` or a live run on this tree).
**Priority:** P1 — Atlantis Track 04 (`designs/atlantis/techdesign-04-di-config.md`) names this
design's Channel 1 as the dependency it is currently designing *around* (probes A7/A9), and the
test story (§3 of the request) is the language's answer to "how do I mock the system boundary"
— a question every downstream package (Atlantis, Sonar, lcurl) has already hit.
**Track:** single track, one implementer — front-end (Resolver/Checker/Lower) + prelude. No
new backend work: `bind`/`inject` lowers to ordinary IR consumed identically by all four
active engines (dossier §3.6, verified live), and this design adds no new opcode.
**Owns (regions):** `src/Resolver.cpp` — prelude additions (capability interfaces + shims +
root binds, near the existing `Console`/`console` block ~:2592-2601) and the one-field `use`
annotation (§5.3, `Use` handling ~:5146,5168,5307); `src/Ast.hpp` — that one annotation field
(~:323 region); `src/Checker.{hpp,cpp}` — the namespace-bind index + activation scan
(`pushBindScope` ~:1955, its three call sites ~:2635/3233/3422, `lookupBind` Checker.hpp:245);
`src/Lower.cpp` — reachability for activated prelude bind factories (Bind handling :129,853);
`tests/corpus/di_capabilities.lev` (new), `tests/test_checker.cpp` (additions);
`docs/reference.md` §4.7 (+ new §6 entry for the capability namespace), `info.md` §12.5.
**Does NOT touch:** `src/X64Gen.cpp` (frozen — ELF is not a target; no ELF lane anywhere in
this design), the `use`/`uses` *name*-import machinery's semantics (imports.md is landed and
unchanged — this design only reads a resolved fact off the `Use` statement), the `bind expr;`
object-install parse shape (stays parsed-and-unconsumed; §7), and the block-scope-substrate
refactor (`designs/complete/techdesign-block-scoped-use.md` stays an independent proposal; §8).

---

## 1. The one rule

The language already has two lexical provision systems that differ only in their key: `use`
provides **names**, `bind` provides **values by type** (request §1). This design's single new
semantic rule welds them at exactly one point:

> **A `use` of a type name also installs that type's namespace bind.**
> If the namespace named in `use NS::T;` declares, at the top level of its body, a factory
> bind keyed on `T`, then the `use` installs that bind into the importing scope — the same
> scope, same nearest-wins, same lifetime as a `bind T => ...;` written where the `use` is.

Everything else in this design is either prelude library code (the capability interfaces and
their system implementations — zero new mechanism, dossier §13's "buildable today" tier) or a
consequence of rules that already exist (nearest-wins shadowing gives the test story;
duplicate-bind detection gives single ownership; `const` gives the shared instance).

Why this is the right shape and not magic: a bind is type-keyed, so **naming the type is
naming the bind** — `use std::IEnv;` states both facts in one statement, and nothing activates
without it. `uses NS;` (including the implicit `uses std;`) imports names only and never
activates a bind — that is the owner-ruled wall (request §5), and it is what keeps "every file
in the program ambiently bound to the real system just by existing" impossible.

---

## 2. What ships, at a glance

| Piece | Kind | Status after this design |
|---|---|---|
| `IEnv`/`IConsole`/`IClock`/`IFileSystem`/`INet` capability interfaces | prelude code | **v1** (§4) |
| `SystemEnv`/`SystemConsole`/`SystemClock`/`SystemFileSystem`/`SystemNet` + const instances | prelude code | **v1** (§4) |
| Root binds (`bind IEnv => systemEnv;` in `namespace std`) | prelude code | **v1** (§4.3) |
| Channel 1 — `use NS::T;` activates T's namespace bind | compiler (Resolver+Checker) | **v1** (§5) |
| Channel 2 — `Bindings` builder object + `bind binds;` install | new type + new syntax | **deferred** (§7) |
| Block-scope substrate unification | refactor | **not a dependency; unchanged** (§8) |
| Fine-grain capability splits (`IFileRead`/`IFileWrite`, …) | prelude code | **deferred until a real sandbox asks** (§4.5) |
| Interfaces requiring **generic** methods | checker semantics | **deferred; v1 sidesteps** (§4.2) |

Dossier §11's nine open questions, disposition: (1) grain → §4.5; (2) shared-instance
spelling → resolved, `const` (dossier §6.2); (3) constructor injection → resolved, already
works (dossier §3.3); (4) comptime → §6.4; (5) Channel 1 mechanism → §5, the heart of this
design; (6) Channel 2 necessity → §7; (7) substrate dependency → §8; (8) `IConsole` generic
wrinkle → §4.2; (9) `main()`-body trap → §6.1.

---

## 3. Grounding (what is true on this tree today)

Everything this design builds on was live-verified in the dossier on 2026-07-13; the load-
bearing facts, restated:

- **The DI core is complete and full-coverage.** Factory binds, nearest-wins block shadowing,
  implicit + explicit injection, and **constructor injection** all work identically on oracle,
  IR, emit-C++, and LLVM (dossier §3.1–§3.3, §3.6; `tests/corpus/di.ext`). The request's
  "parse-only" status header is stale.
- **Value-type binds are a loud compile error** (`Checker.cpp:1966-1969`) — capability
  interfaces are reference types by construction, so this never bites the design, but §4.4
  states the consequence for capability *payloads*.
- **Bind scope-tracking is Checker-only state** (`bindScopes_`, `Checker.hpp:107`), pushed at
  three grains (block :2635, namespace body :3233, program top level :3422). The Resolver's
  `use` machinery has zero connection to it. Channel 1 must bridge that — §5.3 does it with
  one annotation field, not a shared substrate.
- **The Checker never checks the prelude** (`[[leviathan-prelude-not-checked]]`) — so a `bind`
  statement sitting in the prelude's `namespace std` body is **never seen by
  `pushBindScope` today**. This is the unstated reason root binds are not just prelude code:
  they need §5's index (built by a read-only walk, not by checking) to exist at all. §5.2
  makes this explicit; it is the one wrinkle neither the request nor the dossier's "buildable
  today" tier fully prices in.
- **No capability interface exists in the prelude** (dossier §7) — clean slate; the only
  prelude `interface I...` declarations are the iterator protocol and the exception hierarchy.
- **The real `env` surface is `env::args()` / `env::get(key)`** — top-level namespace, not
  `std::env`; the request sketch's `env::variable(n)` does not exist (dossier §6.3, §10).

---

## 4. The capability layer (prelude — no new mechanism)

### 4.1 The five v1 interfaces

All in `namespace std` (so the implicit `uses std;` makes the *names* visible everywhere,
qualification-free — and, per the wall, visibility never means provision; activation is only
ever the explicit `use std::IEnv;`). Contracts are deliberately thin wrappers over surfaces
that already exist and are already tested; a capability gates **acquisition and access**, it
does not re-abstract the objects it hands back.

```
namespace std {
    // What a consumer may learn from the process environment.
    interface IEnv {
        Array<string> args();
        string? variable(string name);      // None = unset (distinct from set-but-empty)
    }

    // What a consumer may say to / hear from the terminal.
    interface IConsole {
        void write(string s);
        void writeln(string s);
        void writeln();
    }

    // What a consumer may know about time.
    interface IClock {
        int now();                          // epoch milliseconds (std::sysNow)
    }

    // What a consumer may do to the filesystem: acquire File objects + cheap queries.
    interface IFileSystem {
        File open(string path, OpenMode mode);   // throws FileException, same as File()
        bool exists(string path);
    }

    // What a consumer may do to the network: acquire streams/listeners.
    interface INet {
        TcpStream? connect(string host, int port);   // None = connect failure
        TcpListener listen(int port);                 // caller checks .ok(), same as today
    }
}
```

Notes, each a deliberate ruling:

- **`IEnv.variable`, not `IEnv.get`.** Nothing requires the interface's method name to match
  the underlying `env::` function (dossier §10.2); `env.variable("HOME")` reads as a
  capability, `env.get("HOME")` reads as a Map. The *implementation* calls the real
  `env::get` (§4.2) — this is the dossier's sketch correction, applied.
- **`IEnv` carries no `exit`/`setExitCode`.** Terminating the process is process *control*,
  not environment *inspection* — a different capability with a different risk profile. If a
  sandbox ever needs to intercept exit, that is an `IProcess` for the §4.5 split round, not a
  rider on `IEnv` v1.
- **`IConsole` requires only non-generic members.** The real `Console` declares generic
  `write<T>`/`writeln<T>` (`Resolver.cpp:2592-2600`), and whether an interface may require a
  *generic* method is untested territory (dossier §7, §11.8). v1 **rules that capability
  interfaces use non-generic requirements only** and sidesteps the open question entirely —
  the shim (§4.2) satisfies `IConsole` with ordinary `string`-typed methods that delegate.
  Interfaces-requiring-generic-methods stays open, unowned by this design; if it is ever
  wanted, it should arrive as its own request.
- **`IClock` is `now()` only.** The request parenthesizes "(timers)", but timers on this
  substrate are callback natives (`sysTimerStart`) with engine-specific pumping seams (Sonar
  T01's `__sonarRegisterTimer`); wrapping them in v1 would freeze a callback ABI this design
  has no need to own. A time-*reading* fake (fixed or scripted `now()`) already unlocks the
  dominant test cases (timestamps, expiry logic, backoff). Timer capability = deferred.
- **`IFileSystem`/`INet` gate acquisition, not the streams themselves.** `open` returns the
  real `File`; `connect` returns the real `TcpStream`. A fake can deny (throw / return
  `None`), redirect (`connect("example.com", 80)` → a local fixture — exactly the lcurl
  test story, request §3), or point at a temp directory. What a fake *cannot* do in v1 is
  hand back a fully scripted in-memory stream, because `File`/`TcpStream` are concrete
  classes over fds — making them interface-shaped is the §4.5 split round's decision, with
  the existing `InStream`/`OutStream` direction machinery (dossier §6.4) as the structural
  precedent. Deny/redirect is the honest v1 capability, and it is the one both sketches in
  the request actually use.
- **`INet.connect` returns `TcpStream?`.** The floor (`sysTcpConnect`) reports failure as
  `-1`, and `TcpStream` over a dead fd silently no-ops its writes — a capability contract
  should not launder "connection refused" into a black-hole stream. `None` forces the caller
  through narrowing (`if (s != None)`), which is the language's own absence discipline
  (info.md §9) applied at the boundary. `listen` mirrors today's `TcpListener(port)` +
  `.ok()` shape unchanged — inventing a second failure convention for the same class in the
  same design would be a special case.

### 4.2 The system implementations (thin shims)

**System impls are always shim classes, never the existing globals directly.** `console`
cannot satisfy `IConsole` (generic methods, §4.1); making some capabilities shims and some
not would be two rules where one suffices. Each shim is a handful of one-line delegations to
the convenience surface, so there is exactly one behavior underneath both paths (request §4's
layering, preserved):

```
namespace std {
    class SystemEnv : IEnv {
        Array<string> args() => env::args();
        string? variable(string name) => env::get(name);     // env::get, NOT env::variable
    }
    class SystemConsole : IConsole {
        void write(string s) { console.write(s); }
        void writeln(string s) { console.writeln(s); }
        void writeln() { console.writeln(); }
    }
    class SystemClock : IClock {
        int now() => std::sysNow();
    }
    class SystemFileSystem : IFileSystem {
        File open(string path, OpenMode mode) => File(path, mode);
        bool exists(string path) => std::fileExists(path);
    }
    class SystemNet : INet {
        TcpStream? connect(string host, int port) {
            int fd = std::sysTcpConnect(host, port);
            if (fd < 0) return None;
            return TcpStream(fd);
        }
        TcpListener listen(int port) => TcpListener(port);
    }
}
```

Implementation cautions, from `[[leviathan-prelude-backend-gotchas]]` (these are the exact
traps prior prelude tracks hit):

- **Qualify same-namespace calls and constructors** inside the shim bodies where the gotcha
  list requires it (`std::sysNow()`, not bare `sysNow()`).
- **emit-C++ eager-global-instance:** a `const` prelude instance forces emission of all its
  class's methods. Every shim method above is a one-line delegation to an already-emitted
  surface, so this should be inert — but if any shim method fails to lower on emit-C++, the
  documented fallback is the data-holder-global + free-functions shape. Verify on all four
  engines at M1 before wiring anything else (§9).
- **No flow-narrowing tricks in shim bodies** (`[[leviathan-prelude-no-narrowing]]`) — the
  one place it could arise, `SystemNet.connect`, uses the plain `if (fd < 0) return None;`
  early-return shape, which is a union *construction*, not a narrowing *read*, on both arms.

### 4.3 The root binds and shared instances

```
namespace std {
    const SystemEnv        systemEnv        = SystemEnv();
    const SystemConsole    systemConsole    = SystemConsole();
    const SystemClock      systemClock      = SystemClock();
    const SystemFileSystem systemFileSystem = SystemFileSystem();
    const SystemNet        systemNet        = SystemNet();

    bind IEnv        => systemEnv;          // shared instance: the body returns a const global
    bind IConsole    => systemConsole;
    bind IClock      => systemClock;
    bind IFileSystem => systemFileSystem;
    bind INet        => systemNet;
}
```

- `const` + factory-bind-returning-a-global is the resolved shared-instance spelling
  (dossier §6.2; the prelude's own `console` is already `const Console console = Console();`,
  `Resolver.cpp:2601`). All five capabilities are stateless shims, so shared instances are
  correct; nothing here wants fresh-per-injection.
- **These binds are inert until Channel 1 exists.** The Checker never walks prelude bodies
  (§3), so nothing registers them into any bind scope; and even once indexed, they activate
  *only* through `use std::IEnv;` — never through the implicit `uses std;`. Both facts are
  the same wall (§1) seen from two sides, and M1 (which lands the code above without §5's
  mechanism) must include a negative test proving the inertness (§9).

### 4.4 Payloads are values; capabilities are references

Restating the compiler's own rule (bug #23, `Checker.cpp:1966-1969`) as design guidance: a
value type (struct/primitive) is never bindable. A capability that produces configuration
hands back a struct **through a method return** (`IEnv.variable` → `string?`); the struct
itself is never a bind key. Any future capability whose natural product is a `struct Cfg`
follows the same shape: bind the class-typed capability, return the value.

### 4.5 Grain: start coarse, split on demand — with the split pre-planned

v1 ships the five coarse capabilities above (request §6.1's own suggestion). The split
trigger is concrete: **the first real sandbox or test that cannot express its restriction at
this grain.** Two splits are pre-sketched so they arrive as additive interface extractions,
not redesigns:

- **`IFileSystem` → read/write** by the stream-direction precedent (dossier §6.4): an
  `IFileRead { File open(path, /*read modes*/); bool exists(path); }` /
  `IFileWrite { File open(path, /*write modes*/); }` pair, with `IFileSystem` becoming their
  join. Coarse consumers keep `IFileSystem`; a read-only sandbox binds `IFileRead` plus a
  denying `IFileWrite`.
- **Scripted streams** (a fake `TcpStream` with canned bytes) require interface-shaping the
  stream types themselves — a real design, owned by whoever needs it, not smuggled in here.

Because a capability is just an interface and a bind, splits are backward-compatible: new
interfaces + new root binds; existing consumers' parameters keep resolving.

---

## 5. Channel 1 — the activation mechanism (the new compiler work)

### 5.1 Semantics (normative)

1. **Namespace binds.** A factory bind (`bind T => expr;` / `bind T { ... }`) written at the
   **top level of a namespace body** is that namespace's bind for `T` — its *exported* bind,
   keyed by the type. Binds nested in blocks/functions inside the namespace are not exported
   (they remain what they are today: local scope shadowing). One namespace, one bind per
   type: a duplicate at namespace top level is already today's hard error, unchanged.
2. **Activation.** `use NS::T;` (or `use NS::T as A;`) where `T` resolves to a **type**
   (interface or class) installs `NS`'s bind for `T`, if one exists, into the scope the `use`
   is written in — exactly as if `bind T => <that same factory>;` were textually present
   there. Top-of-file `use` → file-wide activation; block-level `use` → that block
   (imports.md's one lexical rule, inherited without addition). An alias changes the *name*
   only; the bind is type-keyed, so activation is identical under `as`.
3. **No other path activates.** `uses NS;` never activates (implicit `uses std;` included).
   `use NS::fn;` of a non-type imports the name and activates nothing (no type key). A `use`
   of a type whose namespace has no top-level bind for it imports the name and activates
   nothing — silently; wanting a bind the provider didn't export is expressed by writing
   your own (`bind IEnv => std::systemEnv;`), which needs no permission today and continues
   to work unchanged.
4. **Textual beats activated, same scope, no error.** A hand-written `bind T => ...;` in the
   same scope as a `use`-activation of `T` wins, silently. Rationale: the duplicate-bind
   hard error (info.md §12.5) exists because two *textual* binds are two equal claims and
   the winner would be a silent-distant fact; a `use`-rider vs. a textual bind is not that —
   the textual bind is strictly the more deliberate act, and "specific beats bulk" is
   already the imports precedent (`use` shadows `uses`, info.md §12). This also keeps the
   obvious test idiom frictionless: a file may `use std::IEnv;` for the name *and* bind its
   fake in the same scope without contortions.
5. **Activated-vs-activated cannot collide.** Bind keys are the resolved types themselves,
   not names: two `use` statements can only install two binds for the *same* key by
   importing the same type twice, which dedupes to the one namespace bind. (Two same-named
   interfaces from different namespaces are different types — different keys, no collision;
   their *name* collision is imports.md's existing territory.)
6. **Nearest-wins is unchanged.** An activated bind participates in shadowing exactly like a
   textual bind at the same position: a block-level `bind IEnv => FakeEnv(...);` under a
   file-level `use std::IEnv;` shadows the system bind for that block — the request's §2
   test sketch, verbatim.
7. **Visibility.** v1 binds carry no access modifier; a namespace bind is activatable iff
   the `use` of its key type succeeds. (A non-importable type name already can't be written
   in the `use`.) If bind-level `private` is ever wanted, it composes here without breaking
   anything.

### 5.2 The namespace-bind index (Checker, read-only pre-pass)

New state: `std::unordered_map<NamespaceKey, std::unordered_map<TypeKey, Stmt*>>
namespaceBinds_` on the Checker (beside `bindScopes_`, `Checker.hpp:107` region).

Built once at the top of `Checker::run` (before the :3422 top-level `pushBindScope`) by a
**read-only walk** over every namespace body's top-level statements — both the user program's
and **the prelude's** — collecting `StmtKind::Bind` with `s->type` set (factory form only;
same filter `pushBindScope` uses, `Checker.cpp:1958`). Two properties matter:

- **Walking is not checking.** The pass records `(namespace, key type) → bind stmt` and
  nothing else; prelude bodies are not type-checked (preserving
  `[[leviathan-prelude-not-checked]]` exactly — the pass never descends into the factory
  body). The Resolver already resolves the prelude, so the bind statements and their key
  types exist as resolved AST by the time the Checker runs. The implementer needs an
  accessor from Checker to the prelude's namespace decls if the gathered `Program` the
  Checker sees does not already contain them — this is expected to be the only Resolver-side
  plumbing beyond §5.3.
- **User-namespace binds are indexed AND still checked normally** — the index is a lookup
  structure, not a second registration path. When the Checker walks a user namespace body it
  pushes that namespace's bind scope exactly as today (:3233); the index adds only the new
  cross-namespace reach through `use`.

### 5.3 The `use` annotation (the one Resolver→Checker bridge)

The dossier's central finding (§4.1): the Resolver does names, the Checker does binds, and
they share nothing. This design bridges them with **one field, written once, read once**:

- `src/Ast.hpp` (Use/`Stmt` region ~:323): a resolved-target annotation on the `use`
  statement — the imported declaration's kind and, when it is a type, the type key (whatever
  the codebase's canonical type identity is — the same key `bindScopes_` maps use). Call it
  `useResolvedTypeKey` (empty/none = not a type import).
- `src/Resolver.cpp` (`useOne` path, ~:5146/5168/5307): when a selective `use` resolves to a
  class or interface, stamp the annotation. No behavior change; the Resolver still touches
  no bind table.
- `src/Checker.cpp` `pushBindScope` (:1955): after the existing textual-bind registration
  loop over the scope's statements, a second loop over the same statements handles
  `StmtKind::Use` with a stamped type key: look up `namespaceBinds_[NS][key]`; if present
  **and** the scope has no textual bind for that key (§5.1 rule 4), register the found bind
  statement into this scope's map — the same map entry shape as a textual bind, so
  `lookupBind` (`Checker.hpp:245`) needs **no change at all**. Dedupe by bind-stmt identity
  (rule 5). All three grains (block/namespace/top-level) get the behavior from the one
  chokepoint, which is also where the value-type rejection already lives — an activated bind
  re-uses that check for free (it can't trigger: a value-typed bind was already rejected at
  its declaration, but the invariant costs nothing).

The `NS` in the lookup is the namespace path as written in the `use` (resolved, so aliases
and nesting are already normalized by the Resolver). The bind must live in the namespace the
consumer actually names — there is no transitive search, no re-export walking (imports.md
§8.1's re-export question stays open and undepended-on).

### 5.4 Lowering activated prelude binds

A bind factory lowers at its declaration site today (`Lower.cpp:129,853`). User-namespace
binds keep that path. **Prelude** binds' factories must lower only when some `use` activated
them — the same demand-driven reachability rule prelude *methods* already follow. Concretely:
the Checker records which indexed binds were ever activated (a small set filled in §5.3's
scan); the Lowerer lowers those factories (each is a trivial global read — `return
systemEnv;`) alongside the prelude functions it already emits on demand. An unactivated root
bind costs nothing in any binary. This is the only Lower.cpp work in the design, and it is
bounded: five one-expression factories.

### 5.5 What Channel 1 deliberately does not do

- **No bind "belongs to" an interface declaration.** The dossier's candidate-shape A
  (co-location tagging) is rejected: it makes adjacency semantic (moving a bind ten lines
  out of the interface's scope silently changes meaning — a silent-distant footgun). The
  unit of export is the **namespace**, which is already the language's provision boundary,
  and the key is the **type**, which is already the bind's identity. No new syntax
  (candidate-shape B's keyword) is needed either: `use NS::T` already says everything —
  which namespace, which key, which scope.
- **No runtime anything.** Activation is entirely a check-time scope-table fact; the value
  still flows through ordinary parameters (info.md §12.5's "compile-time ambient context",
  untouched).
- **No change to injection resolution order** (defaults beat injection; explicit `inject` on
  collision — reference.md §4.6/§4.7, unchanged).

---

## 6. Usage rules and the composition story (documentation-normative)

### 6.1 Root binds and app wiring: top level, never in a function body

Atlantis learned this by probe (dossier §8.2) and this design promotes it to documented rule:
**a bind inside a function body only reaches calls made from inside that body** — injection
resolves lexically at the *injection site*, not dynamically along the call chain. App-wide
wiring therefore lives at top level (Atlantis's `config/ioc.lev` convention), where the
duplicate-bind error enforces single ownership. The request's §2 sketch — a
`bind IEnv => FakeEnv(...)` in a block *around the call to `main()`* — works because
injection fills `main`'s parameter at the **call site**, which sits inside the block holding
the fake bind. The distinction to document: **the bind must enclose the call site whose
argument is being filled**, not the callee's body. reference.md §4.7 gains this paragraph
with a two-line example; every future reader stops re-deriving it.

### 6.2 The consumption idiom

```
use std::IEnv;                       // name + bind, this file
use std::IConsole;

void main(IEnv env, IConsole out) {  // filled by the activated root binds
    for (string a in env.args().skip(1)) out.writeln(a);
}
```

(Bug #70, §11 — fixed 2026-07-14 — once made this parameter's natural name, `env`, mis-lower on
IR/native, because it shadows the `env` namespace. `Lower.cpp`'s NS::fn fallback now requires the
callee's base name NOT resolve to a local/parameter before taking the namespace path, so the
canonical spelling above is safe again; `sysEnv` is no longer required anywhere.)

And the test idiom (request §2/§3, now with real semantics behind every line):

```
use std::IEnv;                       // brings the name; activates the system bind file-wide
class FakeEnv : IEnv {
    Array<string> canned;
    new FakeEnv(Array<string> a) { canned = a; }
    Array<string> args() => canned;
    string? variable(string n) => None;
}
{
    bind IEnv => FakeEnv(["prog", "-v"]);   // block-level: shadows the activated root bind
    main();                                  // main's IEnv parameter fills with the fake
}
main();                                      // outside the block: the system bind again
```

### 6.3 Capability honesty, stated at its real strength

`void main(IEnv env)` *says* it reads the environment. The claim's honest boundary
(unchanged from the request): the ambient globals (`console`, `env::args()`, `File(...)`)
remain — the guarantee is **lintable discipline, not compiler enforcement**. A codebase that
wants the guarantee bans the globals outside the `System*` shims by review/lint; the
language's contribution is that the disciplined path is now *cheaper* than the ambient one
(one parameter vs. plumbing), which is the only way discipline ever holds. A compiler-
enforced "no ambient system access" mode is a plausible far-future gate (§16 shape) and is
out of scope.

### 6.4 comptime

Nothing new to deny (request §6.4, dossier §11.4): binds are compile-time data; a `comptime`
context may resolve an injection of `IEnv`, and any call into the shim then hits a `sys*`
native, which the hermetic comptime floor already rejects loudly. The design adds one
sentence to reference.md §6.9's comptime notes saying exactly that, and M3 adds the one
negative test.

---

## 7. Channel 2 (`Bindings` builder objects) — deferred, with reasons

Channel 2 is **not in v1**. Three facts, none of which existed when the request was ruled:

1. **Its aggregation use case has a preferred successor.** The "many scattered `@Injectable`
   declarations, one install point" problem is owned by LA-22's splice mechanism
   (`designs/requests/request-metaprog-splices.md` item E) — rule-generated *ordinary* `bind`
   statements at a splice site, which a real consumer (Atlantis §3R) chose over `Bindings`
   after probing both shapes (dossier §9). Building a second aggregation mechanism here
   would be two features for one job.
2. **Its remaining use case is thin.** Manual multi-capability swap ("bind five fakes at
   once") is N lines of `bind` in one block today — already atomic (one scope), already
   collision-checked. A `Bindings` value adds *passing bindings around as data*, which is
   the modules-as-values question (info.md §12, open) arriving through a side door; it
   deserves its own design when someone actually needs to select wiring at runtime.
3. **It is not a wiring job.** No `Bindings` type exists anywhere (dossier §3.5) — this is
   new-type + new-literal-syntax + a compile-time/runtime boundary ruling, i.e. a full
   design, and reference.md §4.7's `[planned]` note already says so.

What v1 does: **keep** the `bind expr;` parse shape and the AST comment (`Ast.hpp:387-390`)
as the reserved slot; **update** reference.md §4.7's planned-note to name this design as
having explicitly deferred it and LA-22 as owning aggregation. If Channel 2 is revived, its
activation-visibility rule is already settled by §1 here: installation is the literal word
`bind`, nothing ambient.

---

## 8. The block-scope substrate: routed around, not blocked on

`designs/complete/techdesign-block-scoped-use.md`'s unified `Scope` (one object carrying name table +
bind table) remains unbuilt, four milestones past its own dates (dossier §5). This design
takes the dossier's option (b/c) hybrid: **route around it with the minimal bridge** — one
annotation field (§5.3) and one Checker-side index (§5.2) — rather than blocking a P1
feature on a refactor, or absorbing the whole refactor into this design's scope.

Compatibility both ways, stated so neither design steps on the other (that proposal's §4.3
already divides territory the same way): if the substrate later lands, §5.3's scan collapses
naturally (the unified scope would carry activated binds in the same table as textual ones,
and `useResolvedTypeKey` remains the input that says *what* to activate); nothing in this
design's semantics (§5.1) changes. This design owns bind-activation semantics; that design
owns the tables.

---

## 9. Milestones, tests, acceptance

Single implementer; per project convention the implementer STOPs and escalates (rather than
improvising) on: any need to check prelude bodies, any change to `lookupBind`'s signature or
nearest-wins order, any new syntax, or any test needing an ELF run (never gate on ELF —
X64Gen is frozen).

**M1 — capability layer, inert (target 2026-07-14).** §4 in full: five interfaces, five
shims, five const instances, five root binds, all in the prelude. New corpus
`tests/corpus/di_capabilities.lev` (`.lev`, never `.ext`) part 1: consumers wiring
capabilities with **explicit textual binds** (`bind IEnv => std::systemEnv;`), fake-shadowing
in a block, `INet.connect` `None`-arm on a refused port, `IFileSystem` open/exists round-trip
in a temp path. Byte-identical on `--run` / `--ir` / `--build` / `--build-native`.
**Negative acceptance:** a program with *no* textual bind and no `use`-activation whose
injection of `IEnv` fails to resolve — proving root binds are inert (§4.3) and `uses std`
activates nothing.

**M2 — Channel 1 (target 2026-07-15/16).** §5 in full: annotation field, index, activation
scan, demand lowering. Corpus part 2: file-level `use std::IEnv;` filling `main(IEnv)` with
no textual bind (Atlantis probe **A9 flips GREEN** — announce to that track); block-level
`use` activating only within its block; alias activation (`use std::IEnv as E;`);
textual-beats-activated in one scope; `uses std;`-only still failing (the wall holds);
a user-namespace bind activated cross-file by `use`; shadowing order (block fake over
file-level activation). `tests/test_checker.cpp` units for: index build over prelude +
user namespaces, dedupe, rule-4 precedence. All four engines, byte-identical.

**M3 — docs + closure (target 2026-07-16).** reference.md §4.7 rewritten: Channel 1
semantics (§5.1's seven rules, compressed), the §6.1 bind-placement paragraph, the updated
Channel-2 planned-note; a §6 prelude entry for the capability namespace surface; info.md
§12.5 + §0 log updated; the comptime sentence (§6.4). Flag to Atlantis Track 04 that its
A4/A17 workaround language is obsolete and A9 is closed (its doc, not ours, to edit). On
landing: this design moves to `designs/complete/`, per `[[design-workflow-file-moves]]`.

**Explicitly deferred (recorded here, no open bug):** Channel 2 (§7), grain splits + scripted
streams (§4.5), `IProcess`/exit interception (§4.1), timer capability (§4.1), interfaces
requiring generic methods (§4.1 — file a `designs/requests/` request if ever wanted).

---

## 10. Risks

| Risk | Exposure | Mitigation |
|---|---|---|
| Prelude walk: Checker can't currently see prelude namespace decls | §5.2 needs an accessor | Small Resolver-side getter; read-only; STOP if it turns into checking prelude bodies |
| emit-C++ eager-global forces a shim method that won't lower | M1, low (all one-liners) | Known fallback: data-holder global + free fns (`[[leviathan-prelude-backend-gotchas]]`) |
| A `use`-heavy file pays an index lookup per use per scope push | negligible (hash lookups) | none needed |
| Rule 4 (textual-beats-activated) surprises someone expecting the duplicate error | docs | reference.md example states it; the surprising alternative (error) breaks the primary test idiom |
| Type identity mismatch between `useResolvedTypeKey` and bind-scope keys | M2 correctness | Use the same canonical key type `bindScopes_` already maps; unit test the aliased path |

---

## 11. Implementation log (2026-07-14)

**Landed in full — M1, M2, M3, all four active engines byte-identical.** One implementer, one
day, no STOP triggered.

- **M1 (§4).** All five interfaces + shims + const instances + root binds added verbatim as
  designed, as a `namespace std { ... }` reopening in `src/Resolver.cpp` (kPreludeRest segment,
  right after `namespace env` closes and before `namespace term` — a few dozen lines from the
  `Console`/`console` block the design pointed at, not inside it, to avoid a forward-reference
  question that turned out to be moot but wasn't worth risking). `File`/`OpenMode`/`TcpStream`/
  `TcpListener`/`fileExists`/`sysTcpConnect`/`sysNow` (all declared in the FIRST `namespace std`
  block) and `env::args`/`env::get` (declared after, in `namespace env`) were both reachable
  either way — namespace symbol merging across textual reopenings + whole-program gather before
  resolution means declaration order never mattered here.
- **M2 (§5) — smaller than scoped.** §5.2's index and §5.3's annotation landed exactly as
  designed: `Symbol* useResolvedNs` + `std::string_view useResolvedTypeKey` on `Stmt`, stamped
  in `Resolver::useOne` (only for a qualified `use` resolving to a `SymbolKind::Class`), a
  read-only `namespaceBinds_` index built once by `Checker::buildNamespaceBindIndex` (walking
  `Namespace` statements over both `Program`s, keyed by the namespace `Symbol*` and the bind's
  own canonical type name), and a second pass in `pushBindScope` activating a stamped `use` —
  **one correction against the design's literal text**: the activated entry is registered in
  the scope's bind map under **`s->name`** (the `use`'s own, possibly-aliased LOCAL name), not
  `useResolvedTypeKey` (the type's canonical name in its declaring namespace) — the two differ
  exactly under `use ... as`, and keying by the wrong one silently broke the alias rule (§5.1
  rule 2) in testing; `namespaceBinds_` itself is still looked up by `useResolvedTypeKey` (the
  bind's own declared key), so both keys are needed, just for different sides of the lookup.
  **§5.4 turned out to be unnecessary**: bug.md #56 (landed the same day the design was
  authored) already changed factory-bind lowering from "a standalone `$bind.<Type>` function,
  demand-emitted" to "inline the bind's `=> expr` body at each inject site" — so none of the
  five (all arrow-sugar) root binds ever produce a standalone IR function to gate reachability
  on in the first place. **Zero `src/Lower.cpp` changes** were needed anywhere in this design;
  §5.4's "bounded... five one-expression factories" turned out to bound to zero.
- **Checker::run signature.** Needed one additive parameter, `const Program* prelude =
  nullptr` (defaults preserve every existing call site but `main.cpp`'s, which now passes
  `&R->preludeProgram()`) — the accessor risk in §10's table.
- **M3 (§9).** `reference.md` §4.7 rewritten with the seven-rule-compressed Channel 1
  semantics, the bind-placement paragraph, and the Channel-2 deferral note; a new §6.6.6a
  documents the capability surface; a comptime sentence landed in both §4.7 and §6.9 — see the
  finding below, the sentence differs from what §6.4 predicted. `info.md` §12.5 and §0 updated.
- **M1 test note:** the negative acceptance ("no textual bind, no `use` ⇒ injection fails")
  and the M2 activation/alias/scoping/dedupe/cross-scope cases all landed as `test_checker.cpp`
  units (18 new checks) rather than corpus files — sharper, faster, and Channel 1 is a
  check-time-only fact, so a checker unit test is a strictly more direct proof than a runtime
  differential for it. `tests/corpus/di_capabilities.lev` (new, `.lev`) covers the *runtime*
  acceptance criteria from §9 M1 instead: textual bind, fake-shadowing in a block, `INet`'s
  `None` arm, an `IFileSystem` round-trip, and — since M2 landed same-day — Channel 1
  activation filling every parameter with no textual bind anywhere. Byte-identical on
  `--run`/`--ir`/`--mem-verify`/`--ir-verify` (whole-corpus lanes) and a dedicated
  `corpus_di_capabilities_llvm` CMake entry (`--build-native`); `--build`/emit-C++ SKIPs it via
  `run_native.sh`'s existing unsupported-construct pattern (a pre-existing, unrelated
  reachability-over-marking gap on object-heavy programs — same bucket as `class_dispatch.lev`/
  `iterator.lev`, not a system-binds defect). No ELF lane (`.lev`, and `run_elf.sh` only reads
  `.ext` besides).

**Found bug #70 [P1] (`known_bugs_1.md`, footguns.md) — not a system-binds defect, but hit
while writing the corpus. Fixed 2026-07-14.** A local/parameter whose name shadows an in-scope
namespace (`env`, `math`, `term`, ...), dynamically dispatched (any interface-typed receiver
qualifies) and calling a method that namespace *also* declares as a free function with the same
arity, silently resolved to the NAMESPACE FUNCTION on `--ir`/native — never consulting the
receiver's actual runtime class — while the oracle dispatched correctly. Root cause:
`Lower.cpp`'s NS::fn-call fallback (~line 1424) checked only "does the callee's base TEXT name a
known namespace," with no check that the same name resolved to a local/parameter first; it
exists for unchecked prelude bodies (where `e->resolved` is null because the prelude is never
checked) but also fired whenever the CHECKER itself deliberately left `e->resolved` null for an
unrelated reason — here, `resolveDispatch`'s unconditional "interfaces always dispatch
dynamically" rule (§3.4a). This design's own canonical usage idiom (`void main(IEnv env)`,
§6.2) walked directly into it. **Fix:** the fallback now also requires
`!findLocal(callee->a->text)` — a local/parameter of the shadowing name always wins over the
namespace fallback, the same precedent already used for the `console` shadow guard a few lines
above it in `lowerCall`. The `sysEnv` naming workaround is retired; `tests/corpus/
di_capabilities.lev` and §6.2's usage example below are back to the natural `env` spelling, and
`tests/corpus/namespace_shadow_dispatch.lev` is the dedicated regression corpus (oracle/IR/LLVM).

**Comptime finding, correcting §6.4's prediction.** §6.4 predicted an injection inside
`comptime` would resolve normally and then hit the ordinary hermetic `sys*` deny when the
capability's shim calls its native floor. Verified behavior is stricter: an injection written
*directly inside* a `comptime`-folded root (`comptime T x = (inject ICap)...;`, an explicit
`inject`) fails at the injection site itself with "no binding in scope for injected type" —
**for any bind, textual or activated, system-binds-specific or not** — because comptime folding
(the `RuleEngine` pass) runs *before* `Checker::run`'s bind-scope pass exists at all; there is
structurally no bind-scope state yet at that point in the pipeline. This is a pre-existing,
general `bind`/`inject`-and-comptime architectural gap, not something this design introduced or
could fix within its scope (`Checker::checkComptimeRoot` is a deliberately standalone entry
point, per its own doc comment, "does not touch whole-program state"). The practical guarantee
§6.4 wanted — a build never quietly reaches live system I/O through an injected capability —
still holds, just enforced one step earlier than predicted. `reference.md` §4.7 and §6.9 state
the corrected mechanism; `tests/negative/system_binds_comptime_inject.lev` +
`system_binds_comptime_error` (CMake) pins the actual diagnostic as a regression guard.

**§4.3 revised post-landing: shared `const` instances broke `--build` for every program, not
just system-binds users.** §4.3's shared-instance spelling (`const SystemX x = SystemX();` +
`bind IX => x;`) triggered exactly the eager-global-instance gotcha §10's risk table flagged —
but worse than scoped: `IFileSystem`/`INet`'s shims construct `File`/`TcpStream`/`TcpListener`
(their `close()` methods pull in `IDisposable`-name over-marking, which reaches the unrelated
`TaskGroup.cancelAll()` → `sysTaskCancel`, unimplemented on emit-C++); `IEnv`'s shim calls
`sysEnv`, also unimplemented there. Because these are prelude globals, they are constructed
**unconditionally in every program's `@ginit`**, regardless of whether that program ever uses a
capability — so `--build`/`--emit-cpp` failed to compile even `console.writeln("hello");`.
**Fixed** by making all five root binds fresh-per-injection instead
(`bind IEnv => SystemEnv();`, ...) with no backing `const` global at all — since every shim is
stateless, this is unobservable to any consumer; only the reachability shape changes, and an
unactivated capability now truly costs nothing in any binary (§5.4's original claim, now actually
true for `--build` too, not only `--run`/`--ir`/`--build-native`). Verified: `console.writeln`
alone and an unrelated user class as a `const` global both compile via `--emit-cpp` again;
`tests/corpus/di_capabilities.lev` unchanged (`--run`/`--ir` still byte-identical).

---

*Design authored 2026-07-13 against the dossier's verified tree state. Implementation claims
about current behavior are the dossier's (live-verified same day); everything normative is
this document's.*
