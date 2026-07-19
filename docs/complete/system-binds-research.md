# System-interface binds (capability injection over `bind`/`inject`) — technical research dossier

**Status:** pre-design research. **Not** a tech design. This is the complete technical
substrate a tech design for `designs/requests/accepted/system-binds.md` must build on: exactly what the
`bind`/`inject` DI core does today (verified live, on every active engine — not read off a
doc), which of `designs/requests/accepted/system-binds.md`'s own prerequisites and open questions have since
been resolved by other landed work, which are still open, and what a real downstream
consumer (Atlantis Track 04) has already learned by probing this exact surface. Every
implementation claim below is anchored to code (`file:line`) or to a command actually run
against `build/leviathan` on this tree — `info.md`/`docs/reference.md`/design docs are cited
only for *intent*, never as evidence that something is built.

Audience: whoever writes the tech design (a Fable-class author per
`[[feedback_techdesign-conventions]]`), and the implementers after them. This is intended to
be the **only** document needed to write the design.

---

## 0. One-paragraph orientation

`designs/requests/accepted/system-binds.md` proposes layering **capability injection** (`IEnv`/`IConsole`/
`IClock`/`IFileSystem`/`INet` interfaces, "root binds" to real system implementations,
nearest-wins shadowing for tests/sandboxes) on top of the language's existing `bind`/`inject`
dependency-injection primitive (info.md §12.5). Its own status header says the prerequisite
DI core is "parse-only... implicit injection never resolves" and cites `bug.md #9` as
"assigned and in progress" — **that header is stale.** `bug.md #9` was implemented in full on
2026-07-05 (commit `3722ded`) and hardened twice since (`1fe86cc` — constructor injection,
value-type-bind rejection); this dossier verified live, today, that factory binds, implicit
and explicit injection, and constructor injection all work identically on **all four active
engines including LLVM** (§3.6) — a claim no prior document had actually tested end-to-end.
What remains genuinely unbuilt is narrower and more specific than the stale header suggests:
system-binds.md's own **two activation channels** (§4) — "a bind travels with a selective
`use` import" and "an installable `Bindings` binder object" — plus the **per-block scope
unification** another proposal (`designs/techdesign-block-scoped-use.md`) was supposed to
deliver and never did (§5). A real downstream consumer, `designs/atlantis/techdesign-04-di-config.md`,
already spent a probe session against this exact gap in 2026-07-06/07 and designed around it
(§8) — its findings are the single best empirical map of what a v1 tech design needs to
either fix or design *for* rather than *around*.

---

## 1. Terminology, pinned

- **Factory bind** — `bind Type => expr;` / `bind Type { ...; return expr; }`. The only bind
  form that works today. `Type` must be a reference type (class/interface) — see §3.4.
- **Object-install bind** — `bind expr;` (no `Type`, per Ast.hpp:387-390). Parsed as a
  distinct AST shape but **not consumed anywhere** — see §3.5. This is the shape channel 2
  (`bind binds;` installing a `Bindings` value) would use.
- **Root bind** — system-binds.md's term for a factory bind that wires the *real* system
  implementation (`bind IEnv => systemEnv;`), as opposed to a test's shadowing bind.
- **Channel 1** — system-binds.md §5's proposed rule that `use NS::IThing;` (selectively
  importing an interface by name) *also* activates whatever root bind is keyed on that
  interface, wherever it was declared. **Not implemented** (§4.1).
- **Channel 2** — system-binds.md §5's proposed `Bindings` builder-object + `bind binds;`
  install form for bulk/explicit capability wiring. **Not implemented** — no `Bindings` type
  exists in the prelude at all (§4.2).
- **The block-scope substrate** — the unified per-block object (`imports.md` §9,
  `designs/techdesign-block-scoped-use.md`) that was supposed to carry both the name-import
  table and the bind table so `use`/`uses` and `bind` share one lexical-scope mechanism.
  **Designed but never implemented** — see §5.
- **Capability interface** — an interface like `IEnv` whose only purpose is to be an
  injectable contract over a system resource. None exist in the prelude today (§7).

---

## 2. The request, restated, and its lineage

`designs/requests/accepted/system-binds.md` (12.6 KB, `git log` shows it last substantively
edited `ee2e183`, the 2026-07-06 designs/ reorg commit, prior to this dossier's move) is
itself the artifact being researched here — there is no separate
`designs/requests/request-system-binds.md`; the proposal document *is* the request, and this
research lands alongside its `git mv` from top-level `designs/` into
`designs/requests/accepted/` (matching the precedent of `request-decimal-type.md`,
`request-columnar-dense-array-struct.md`, etc., all moved to `designs/requests/accepted/`
alongside a research dossier or the design itself).

**The idea, verbatim from §0 of the source doc:** piggyback on the existing `bind`/`inject`
system to give user code capability-shaped access to the system boundary (`env::args()`,
`console`, `File`, `TcpStream`) through injectable interfaces instead of ambient globals, so
that (a) tests/sandboxes can shadow the real system with a fake via ordinary nearest-wins
`bind` shadowing, and (b) a function's parameter list honestly says what system capabilities
it touches.

**Why now:** two other proposals this design explicitly depends on have since either landed
or gained a probe-based downstream consumer:
- `imports.md` (the `use`/`uses` scoping model channel 1 is spelled with) — **landed**,
  `designs/complete/imports.md`.
- `argv.md` (the `IEnv` system impl's underlying surface) — **landed**,
  `designs/complete/argv.md` — see §6.3 for the one naming correction this creates.
- `const.md` (the shared-instance spelling system-binds.md §6.2 wanted) — **landed**,
  `designs/complete/const.md`.
- Atlantis Track 04 (`designs/atlantis/techdesign-04-di-config.md`) has already built an
  entire framework-level DI/config layer *on top of* plain `bind`/`inject` (not on
  system-binds.md's capability interfaces, which don't exist) and explicitly names
  `system-binds.md §5` as a dependency for its own future work (§8 below).

---

## 3. Current state of the DI core — grounded, verified live

Everything in this section was re-verified by running `build/leviathan` against small
programs on this tree today (2026-07-13), not inferred from commit messages.

### 3.1 Factory binds: syntax, scope grain, lifetime

```
bind ILogger { return ConsoleLogger(); }   // full body (one statement, §12.7)
bind ILogger => ConsoleLogger();           // arrow-sugar — a FRESH instance per injection
bind ILogger => sharedInstance;            // the body just returns a value — a SHARED instance
```

Confirmed via `tests/corpus/di.ext` run on `--run`/`--ir`/`--build` (emit-C++)/`--build-native`
(LLVM), byte-identical output on all four:

```
$ ./build/leviathan --run tests/corpus/di.ext   # and --ir, --build, --build-native — identical
hello
hello
hi
hello
```

That corpus program exercises **nearest-wins block shadowing**: a top-level
`bind IGreeter => Hello();`, an inner function's `bind IGreeter => Hi();` that shadows it for
calls made from inside that function, and confirms the outer bind is restored once the inner
scope ends. All four active engines agree.

**Scope grain — three call sites, one mechanism (`Checker::pushBindScope`,
`src/Checker.cpp:1955`, `src/Checker.hpp:107` for the storage,
`src/Checker.hpp:245` for the nearest-wins lookup):**

| Grain | Call site | Effective scope |
|---|---|---|
| Block | `Checker.cpp:2635` | The `{ }` block and everything lexically inside it |
| Namespace body | `Checker.cpp:3233` | The namespace body and everything inside it |
| Program top level | `Checker.cpp:3422` (`Checker::run`) | **The whole program** — every file, because `Checker::run` checks one gathered `Program` across all files (info.md §12: "file boundaries dissolve at gather time; namespace boundaries persist") |

This directly confirms two facts system-binds.md and Atlantis Track 04 both assume but that
are worth pinning precisely: an un-namespaced top-level `bind` in *any* file is visible to
*every* file in the build (Atlantis's probe A10), and a duplicate top-level bind for the same
type across two different files is a **hard compile error** (`Checker.cpp:1976`, "duplicate
binding for '...' in this scope" — Atlantis's probe A11). The one-file-owns-each-bind
convention Atlantis's `config/ioc.lev` relies on (§8.4 below) is therefore enforced by the
compiler, not just documentation discipline.

### 3.2 Implicit vs. explicit injection

`pickInjecting` (introduced by `3722ded`) tries an exact-arity overload first, then fills
trailing unfilled parameters that have a bind in scope by synthesizing `inject Type` argument
nodes; ambiguity (two candidates, or an exact-arity overload shadowing the injecting one)
pushes the caller toward the explicit `inject Type` selector — the same "explicit only on
collision" rule as `::` qualification and constructor labels (info.md §2, §12.5). Both forms
verified live in `tests/corpus/di.ext` (implicit `speak()` and explicit
`speak(inject IGreeter)`, lines 13-14).

### 3.3 Constructor injection — FIXED since Atlantis's probe, verified live

Atlantis Track 04's probe A4 (2026-07-06/07, `designs/atlantis/techdesign-04-di-config.md:62`)
found implicit constructor-parameter injection **RED**: `Service()` against
`new Service(ILogger lg)` failed with "no overload of 'constructor' matches the arguments,"
and the whole design was built around that gap (explicit `inject` at every constructor call
site, chained factory binds). **This is now fixed** (`bug.md #24`, commit `1fe86cc`,
2026-07-07 — one day after Atlantis's probe session). Re-verified today:

```lev
interface ILogger { void log(string m); }
class ConsoleLogger : ILogger { void log(string m) => console.writeln(m); }
bind ILogger => ConsoleLogger();
class Service {
    ILogger lg;
    new Service(ILogger lg) { this.lg = lg; }
    void run() => lg.log("service ran");
}
Service s = Service();   // <-- implicit constructor injection
s.run();
```
`--run` → `service ran`. `--build-native` (LLVM) → `service ran` (plus the heap-tier trailer,
harmless). **Both engines resolve the constructor parameter implicitly.** A tech design for
system-binds.md can assume constructor injection works uniformly with function/method
injection — it does not need to route around it the way Atlantis's already-written doc does
(that doc's §2.2/§9-row-3 workaround language is now obsolete and worth flagging back to that
track, though fixing that doc is out of scope here).

### 3.4 Struct/value-type binds — now a loud compile error, not a silent bug

Atlantis's probe A17 found `bind Cfg => Cfg(9);` (a struct type) **silently injects a
default-constructed value** — a §16 loudness violation, filed as a bug and forbidden in
Atlantis code as a STOP tripwire. **This is now fixed** (`bug.md #23`, same commit `1fe86cc`).
Re-verified today:

```lev
struct Cfg { int port; }
bind Cfg => Cfg(9);
```
→ `error: cannot 'bind' the value type 'Cfg' (a struct/primitive): dependency injection
cannot carry a value through — pass it as an explicit argument instead` (`Checker.cpp:1966-1969`).
Reference-type (class/interface) binds are unaffected; this rejection happens at the single
`pushBindScope` chokepoint, so it applies uniformly at block/namespace/top-level grain. A
capability-interface design is unaffected in practice (`IEnv`/`IConsole`/etc. are all
reference-type interfaces by construction), but the tech design should say explicitly that
value-typed capability *payloads* (a `struct Cfg` a capability hands back) are not themselves
injectable — they get carried inside a class-typed capability's method return, never bound
directly.

### 3.5 What's NOT implemented: the object-install bind form / `Bindings`

The AST already has a distinct shape for this (`Ast.hpp:387-390`):
```
// Bind:  bind Type => body  |  bind Type { ... }  |  bind object;
//   type set => factory binding (memberBody is the factory body)
//   init set => object-install form (`bind expr;`)
```
and the Checker's own comment flags it as deliberately out of scope of the current work:
`Checker.cpp:1954`, *"Object-install binds (`bind expr;`) are staged separately."*
**"Staged separately" has not landed anything** — `pushBindScope` only registers bind
statements with `s->type` set (the factory form, `Checker.cpp:1958`:
`if (!s || s->kind != StmtKind::Bind || !s->type) continue;   // factory form only`). A
`bind expr;` statement with no explicit type parses but is never registered into any bind
scope, so it can never satisfy an injection. There is also **no `Bindings` class anywhere in
the prelude** (confirmed: `grep -n "interface I\|class Bindings" ` across the prelude source
in `src/Resolver.cpp` finds no such declaration) — Atlantis's probe A7 confirmed this
independently (`Bindings b = Bindings();` → `"unknown type 'Bindings'"`). This matches
`info.md §12.5`'s own "[planned] Binder objects" note and `docs/reference.md §4.7`'s
"`Bindings` values and `bind someBindings;` are not implemented" — both current and accurate.

**This means system-binds.md's Channel 2 (§5, "exported binder packages") requires
originating an entirely new prelude type**, not just wiring up an existing parser shape to
an existing runtime object. The parser/AST support is a scaffold, not a partial
implementation.

### 3.6 Engine coverage — verified, not assumed

The `bug.md #9` landing commit's own message claims coverage on "`--run`, `--ir`, emit-C++,
and pure ELF" — conspicuously **not** LLVM, because LLVM was not yet the primary backend on
2026-07-05 (the portable-backend pivot landed the same day). Since `bind`/`inject` lowers
through `Lower.cpp` (`StmtKind::Bind` at `Lower.cpp:129,853`; `ExprKind::Inject` at
`Lower.cpp:1786`) into the **same shared `IrModule`/`Op::` opcode stream** every backend
consumes (`IrInterp.cpp`, `CGen.cpp`, `X64Gen.cpp`, and `LlvmGen.cpp` all read `Op::` — grep
counts: 52/56/64/80 respectively), a factory bind lowers to an ordinary zero-arg IR function
call with nothing backend-specific about it. This dossier verified that inference directly:

| Engine | Command | `di.ext` output | Constructor injection (§3.3) |
|---|---|---|---|
| Oracle (tree-walk) | `--run` | `hello / hello / hi / hello` | ✅ `service ran` |
| IR interpreter | `--ir` | `hello / hello / hi / hello` | (not re-run; shares the same `Lower.cpp` path as oracle/LLVM below) |
| emit-C++ | `--build` | `hello / hello / hi / hello` | (not separately re-run for §3.3; struct-bind error (§3.4) confirmed at Checker stage, which is backend-independent) |
| LLVM (`--build-native`) | `--build-native` | `hello / hello / hi / hello` | ✅ `service ran` |
| Pure ELF (frozen) | `--emit-elf` | not re-run (X64Gen is frozen, never a project target — `[[feedback_x64gen-frozen]]`) | not applicable |

**Conclusion: `bind`/`inject`, including constructor injection, is full-coverage on all four
active engines today.** No prior document states this as a tested fact — the bug-#9 commit
predates LLVM primacy, and Atlantis's probe table never ran its probes through
`--build-native`. A capability-injection design built on top of this substrate inherits that
coverage for free; it does not need its own per-engine verification of the *injection
mechanism itself* (only of whatever new prelude interfaces/classes it adds).

---

## 4. The two channels system-binds.md proposes — true current status

### 4.1 Channel 1 — "a bind travels with the selective import of its key"

**Status: not implemented. Not merely unwired — the underlying concept (a bind "belonging
to" a declaration, such that importing the declaration re-activates the bind) does not exist
anywhere in the current model.**

Today, a `bind` statement is purely positional: it is a statement that executes (at
check-time, conceptually) in whatever lexical scope it textually sits in, and it registers
into that scope's `bindScopes_` entry. There is no notion of a bind being "the" bind
*associated with* an interface declaration — nothing links `interface IEnv { ... }` to
`bind IEnv => systemEnv;` beyond both existing (by convention) in the same source region. The
Resolver's `Use` handling (`Resolver.cpp:5146, 5168, 5307`) only ever populates a **name**
table (`useOne`, importing a name into a scope) — it has zero interaction with
`bindScopes_`/`pushBindScope`, which is Checker-only state, populated from bind *statements*,
not from anything the Resolver's import machinery walks. Atlantis Track 04 probed this
directly and independently confirmed the same conclusion (probe A9, `use Lib::IThing;`
activating the bind keyed on it → **RED** — "ruled (system-binds §5) but not implemented";
probe A8, a namespace-wrapped bind not leaking via `uses` → **GREEN (by design)**, which is
the *negative* space of the same gap).

**What a tech design actually has to invent here**, not just schedule: a mechanism by which
`use std::IEnv;` (importing the interface **name**) causes the **bind statement that lives
beside it** to also install into the importer's scope. Candidate shapes worth the design
author's attention (not prescribing — this is exactly the open design work):
- Special-case: a factory bind whose target type is an interface, written in the **same
  scope as the interface's own declaration**, is tagged as that interface's "declaration
  bind"; a `use` of the interface name additionally installs the declaration bind into the
  importer's scope (alongside the name). This requires the Resolver to find that
  co-located bind at `use`-processing time (today `useOne` only sees the imported
  namespace's **name** table, not any bind table — the Resolver doesn't currently walk
  `bindScopes_` at all, which is entirely Checker-side state built during checking, *after*
  resolution).
- Alternative: `bind` gains a way to explicitly mark itself as "activates with type T's
  import" (a keyword/attribute), decoupling "co-located" from "the interface's own bind" —
  more explicit, less magic, but new syntax.
- Either way, this needs the Resolver and Checker's bind machinery to share data they
  currently don't (Resolver does names; Checker does binds) — which is exactly what §5's
  substrate proposal was supposed to unify, and didn't.

### 4.2 Channel 2 — "exported binder packages, explicitly installed"

**Status: not implemented — blocked on originating the `Bindings` type itself (§3.5), which
this design (or a design it explicitly delegates to) has to author.** The `bind object;`
parse shape exists (§3.5) but the Checker never consumes it. system-binds.md §5.1 assumes
"binder literals" (`Bindings b = { A => ...; B => ...; };`) are "sugar over §12.5's builder
(`.add(...)`)" — but neither the builder (`Bindings.add(...)`) nor the literal syntax exists;
this is new-type-plus-new-syntax design work, not "sugar over" anything currently present.

**A newer, competing mechanism has since displaced Channel 2 as the preferred answer to the
*aggregation* problem Channel 2 was partly aimed at** (routing many scattered
`@Injectable`-annotated classes' bindings into one place) — see §9. The tech design should
treat Channel 2 (the general-purpose `Bindings` builder object, independent of
metaprogramming) and the splice-based aggregation mechanism (§9) as two separate asks that
happen to share a prerequisite (some notion of "many binds, one install"), and decide
explicitly whether system-binds.md still needs the general Channel 2 builder for *manual*
multi-bind installation (e.g., a test wanting to swap five capabilities at once) even if the
metaprogramming-driven aggregation problem gets solved a different way.

---

## 5. The block-scope substrate — proposed, never implemented, worth flagging plainly

`designs/techdesign-block-scoped-use.md` (proposal, dated 2026-07-06, **still sitting in
`designs/` — not moved to `designs/complete/`**) designed exactly the shared substrate
system-binds.md §7 staging item 2 names as a prerequisite: one `Scope` object carrying both
an imported-name table and a type-keyed bind table (`Scope::binds`,
`designs/techdesign-block-scoped-use.md:186`), with `Stmt::importScope` renamed to
`Stmt::blockScope` and `Checker::bindScopes_`/`pushBindScope`/`popBindScope` deleted in favor
of a shared `LexicalStack`.

**That design was never implemented.** Verified directly against the current tree:
- `src/Ast.hpp:323` still declares `Scope* importScope = nullptr;` — not renamed to
  `blockScope`.
- `src/Checker.hpp:107` still declares the separate `bindScopes_` stack; `pushBindScope` /
  `popBindScope` / `lookupBind` (`Checker.hpp:243-245`, bodies at `Checker.cpp:1955-1993`)
  are all still present and unchanged in shape from the pre-deferral-doc description.
- No `LexicalStack`, `BlockScopeGuard`, or `Scope::binds` field exists anywhere in
  `src/Symbols.hpp` or elsewhere.

The proposal's own §7 laid out four dated milestones (M1 2026-07-07 through M4 2026-07-10,
gated on owner sign-off for the one interpretive ruling it made in §3.3). **All four dates
have passed with no implementation commits** (`git log --oneline --all | grep -i
"block-scoped\|shared substrate\|per-block"` returns only the design's own precursor commits,
`bac5e9a` and `d51366a`, both predating the deferral doc itself). This is worth surfacing to
the tech design author plainly, without editorializing further: **the exact prerequisite
system-binds.md's own §7.2 named ("that per-block lexical scope should be built once") has a
complete, reviewed design sitting unbuilt, and the tech design has to decide whether to (a)
require that substrate land first as a hard blocking dependency, (b) design Channel 1's
activation to work against today's two-mechanism reality (Resolver-side name imports,
Checker-side binds, no shared lookup) and accept some duplication, or (c) fold the minimal
piece of the substrate it actually needs into its own scope, rather than waiting on a
separate design that has already missed its own timeline once.**

One piece of good news buried in that proposal: its own §4.3 already pre-negotiates the
grain question — *"this design owns the tables; system-binds.md owns bind activation
semantics"* — so whichever way the tech design resolves (a)/(b)/(c) above, it is not
stepping on that other proposal's territory; the file/program-wide grain question for
top-level binds (§3.1's table) is explicitly system-binds.md's call to make, not something
blocked on the substrate.

---

## 6. Adjacent landed infrastructure the design can now assume

### 6.1 `use`/`uses` (imports.md) — landed, file-level only

`designs/complete/imports.md`. `uses NS;` dumps a namespace's names; `use Path::name (as
alias)?;` selectively imports one name. Both are **lexically scoped to where they're
written**, with **top-of-file = file-wide** as the *consequence* of "an import is a
declaration in its enclosing lexical scope" (info.md §12, "The lexical model"), not a special
file-wide primitive. **Block-level `use`/`uses` is real and functions today** (info.md's
"12.5/imports.md" cross-reference; `techdesign-block-scoped-use.md §1.1` documents the two
same-day, feature-private mechanisms — `Stmt::importScope` + the Lowerer's private
`blockImportScopes_` — that deliver it) — what's missing is only the *unification* with
`bind`'s scope tracking (§5), not block-level imports themselves.

**Consequence for system-binds.md:** `use std::IEnv;` written inside a function body, not
just at file top level, already imports the name correctly today. Channel 1's remaining gap
(§4.1) is purely "does importing the name also install the bind" — not "does block-level
import work at all."

### 6.2 `const.md` — landed; the "shared instance" spelling is resolved

`designs/complete/const.md`. system-binds.md §6 item 2 flagged as open: *"the sketch uses a
prelude global + `bind IEnv => systemEnv;`, which needs no new syntax and becomes `const`
under const.md."* Verified: the prelude's own `console` singleton is already spelled exactly
this way — `const Console console = Console();` (`src/Resolver.cpp:2601`) — and a namespace-
scoped `const <Class> <name> = <Ctor>();` followed by a `bind <Interface> => <name>;` was
directly re-tested and works (compiles and injects correctly) on this tree. **This open
question is closed**; the tech design can spell root binds exactly as system-binds.md's own
§2 sketch shows, with `const` in front of the shared-instance declaration.

### 6.3 `argv.md`/Track 08 F1 — landed; one naming correction to the sketch

`designs/complete/argv.md`. The `env` namespace is a **top-level** namespace (not nested
under `std`, deliberately — nested `std::env::...` hits an unrelated qualified-path
resolution gap noted in the prelude source, `src/Resolver.cpp:2634-2640`), giving:

```
namespace env {
    Array<string> args() => std::sysArgs();
    string  name();
    string? get(string key) => std::sysEnv(key);   // None = unset, distinct from set-but-empty
    void    exit(int code);
    void    setExitCode(int code);
}
```
(`src/Resolver.cpp:2666-2685`.) **system-binds.md §2's sketch calls this `env::variable(name)`
— the real function is `env::get(key)`.** This is a small but real correction the tech design
should make when it writes `SystemEnv`'s `IEnv` implementation:
```
class SystemEnv : IEnv {
    Array<string> args() => env::args();
    string? variable(string n) => env::get(n);   // was: env::variable(n) in the sketch — doesn't exist
}
```
Also worth noting for the design: Atlantis's probe A19 (2026-07-06/07) found `env::args()`
**RED** ("unknown name 'env'" — Track 08 F1 not landed at the time); Atlantis's own
implementation log (§12, last entry) records this as fixed by 2026-07-07. Both `args()` and
`get()` are confirmed present in the prelude source today.

### 6.4 Streams as the system boundary (info.md §13) — the capability-grain map

info.md §13: *"Streams are THE system boundary: nothing crosses the process boundary except
through a stream."* The concrete, already-implemented stream endpoints a capability-interface
design would wrap: `File` (read/write, `IDisposable` via `using`), `TcpStream`/`TcpListener`
(sockets, `designs/complete/techdesign-08-system-natives.md`, `designs/complete/techdesign-tls-crypto.md`
for the TLS-wrapped variant), `HttpServer`/`HttpClient` (Track 09 web foundations, layer-2
over TCP). This is directly relevant to system-binds.md §6 open question 1 ("capability
grain — one `IEnv` vs `IArgs`/`IEnvVars`; one `IFileSystem` vs read/write splits"): the
existing stream-direction machinery (`InStream<T>`/`OutStream<T>`/`IOStream<T>`, info.md §13,
"capability is which operators the type exposes") already gives a *structural* precedent for
splitting a capability by direction cheaply — an `IFileSystem` could plausibly decompose into
read/write the same way streams do, for free, rather than needing a bespoke split.

### 6.5 Interfaces-as-contracts / catch-by-capability (info.md §12.6) — precedent intact

system-binds.md §1 point 3 cites §12.6's catch-by-capability (catching by interface, not
class) as the precedent for "interfaces define injectability" being the same shape as
"interfaces define catchability." That section is unchanged and current
(`info.md:1067-1095`); the precedent still holds as stated.

---

## 7. Prelude capability-interface surface: currently empty

None of the candidate capability interfaces system-binds.md §2 sketches
(`IEnv`/`IConsole`/`IClock`/`IFileSystem`/`INet`) exist in the prelude today. Verified: the
only `interface I...` declarations in the prelude source
(`grep -n "interface I" src/Resolver.cpp`) are the iterator protocol (`IIterator<T>`,
`IIterable<T>`) and the exception hierarchy (`IException`, `IRuntimeException`,
`ILogicException`, `ICancelledException`, `IFileException`, `IDisposable`) — none of them
system-boundary capability contracts. **This is a clean slate; there is no partial capability
interface to reconcile with.**

One concrete wrinkle for whichever interface wraps `console`: the existing `Console` class
(`src/Resolver.cpp:2592-2600`) declares **generic** methods —
`void write<T>(T v); void writeln<T>(T v); void writeln();` — and `console` is a `const`
prelude instance implementing no interface today. info.md §8 states interface method
requirements "compare names and parameter types exactly," which is straightforward for
non-generic signatures but underspecified for a **generic** method requirement (`write<T>(T)`)
— the tech design needs to either (a) settle that interfaces may require generic methods and
that satisfaction compares the generic signature shape, or (b) narrow `IConsole`'s
requirement to a fixed non-generic overload set (e.g. `void writeln(string s);`) that
`Console` already happens to satisfy via its non-generic overloads, sidestepping the
open question entirely. Given `IConsole` is explicitly on system-binds.md's own candidate
list (§2), this is not a hypothetical — it is the first concrete interface the design will
need to write.

---

## 8. Real-world consumer evidence: Atlantis Track 04

`designs/atlantis/techdesign-04-di-config.md` ("Status: draft for owner review," dated
2026-07-06) is a **framework-level DI/config design already built on plain `bind`/`inject`**
— not on system-binds.md's capability interfaces (which, per §7, don't exist) — and it
explicitly names `designs/requests/accepted/system-binds.md §5` as a dependency (its own header, line 6-7). It
ran a 20-probe evidence session (§1 of that doc) against the exact DI surface this dossier
also re-verified, and its findings are the best available empirical map of what real
consumer code needs from this system. Restated here with **current-tree status** (several
have changed since 2026-07-06/07):

| Probe | What it tested | 2026-07-06/07 verdict | **Current status (verified today)** |
|---|---|---|---|
| A1/A2 | Singleton vs. fresh-per-injection factory binds | GREEN | unchanged — GREEN |
| A3 | Implicit + explicit function-parameter injection | GREEN | unchanged — GREEN |
| A4 | Implicit **constructor**-parameter injection | **RED** | **FIXED (§3.3)** — GREEN on oracle + LLVM |
| A5/A6 | Explicit `inject` at ctor call site; chained factory binds | GREEN | unchanged — GREEN |
| A7 | `Bindings` object exists | RED | **still RED** (§3.5, §4.2) |
| A8 | Namespace-wrapped bind leaking via `uses` | RED (by design) | unchanged — correctly RED (§4.1) |
| A9 | Channel 1 (`use` activates a bind) | RED | **still RED** (§4.1) |
| A10/A11 | Cross-file top-level bind; duplicate-bind hard error | GREEN | unchanged — GREEN (§3.1) |
| A16 | `where`-clause rule validation (unrelated to bind, metaprog) | GREEN, two caveats | not re-verified (out of this dossier's scope) |
| A17 | Struct-typed bind silently wrong | **RED (silent)** | **FIXED (§3.4)** — now a loud compile error |
| A18 | Bind with a concrete class key | GREEN | unchanged — GREEN |
| A19 | `env::args()` | RED (Track 08 F1 not landed) | **FIXED** (§6.3) — landed, confirmed present |
| A20 | `File` read/write for `.env` loading | GREEN | not re-verified (unrelated to bind) |

**Two of Atlantis's four filed compiler gaps (A4, A17) are now fixed; the DI-relevant one
that remains open (A7/Bindings) is exactly system-binds.md's own Channel 2 (§4.2); A9
(Channel 1) is exactly system-binds.md's own Channel 1 (§4.1).** The tech design is, in a
real sense, the thing Atlantis's Track 04 has been designing *around* rather than *for* —
closing A7/A9 is squarely this design's job, and Atlantis's own document says as much
(`techdesign-04-di-config.md:191-192`: *"The `Bindings` runtime object does not exist (A7) —
documented in reference §4.7, unimplemented (bug #9 remainder; system-binds §7 gate 1/2)"*).

Beyond the probe table, Atlantis Track 04 has already worked out and shipped (in design form)
three things a capability-injection design should read before inventing its own answers:

**8.1 A working lifetime model without a runtime container (§2 of that doc).** Three
lifetimes, all expressed with today's `bind`/`inject`, no new mechanism: **singleton** (bind
a shared const instance — §6.2 here), **transient** (arrow-bind a constructor call, chained
via nested `inject`), **request-scoped** (not a bind at all — a service takes the
per-request `Context` object as an ordinary constructor parameter, because "binds are
compile-time and lexical; a request scope is a runtime notion, so it cannot be a bind"). A
capability design that wants a "current request's IEnv-shaped view" or similar should reuse
this third pattern rather than inventing a new one.

**8.2 A hand-tested composition-root file convention (§7 of that doc).** All app-wide binds
go in one file (`config/ioc.lev`), top-level, un-namespaced, so the compiler's own
duplicate-bind check (§3.1 above) enforces single ownership; a bind written inside `main()`'s
body only shadows calls made from inside `main()` — it does **not** reach injection sites in
framework code, because injection resolves lexically at the call site, not dynamically. This
"app-wide binds must be un-namespaced top-level, never inside a function body" rule is not
written down anywhere in system-binds.md itself, and the tech design should state it
explicitly (or explain why capability root-binds are exempt) since a `bind IEnv => ...;`
placed inside `main()`, following the sketch's own §2 example structure, would have exactly
this scoping trap.

**8.3 A concrete testing story (§7.4 of that doc) that is exactly system-binds.md §2's `main`
test example**, hand-verified rather than sketched: a test's `bind IUserService =>
FakeUserService();` inside its own `main()`/entry function shadows the app's top-level bind
for every call made from inside that test body — "no mocking framework, no container
reconfiguration... the `bind` *is* the mock installation." This is independent confirmation
that system-binds.md §2's `FakeEnv` test sketch will work exactly as written, once `IEnv`
itself exists.

**8.4 A superseding mechanism for the aggregation problem Channel 2 partly targeted** — see §9.

---

## 9. LA-22 / metaprogramming splices — the mechanism that has displaced Channel 2's original aggregation use case

system-binds.md §5's Channel 2 (`Bindings` builder + `bind binds;`) was aimed partly at
letting many scattered declarations (e.g., every `@Injectable`-annotated service class)
contribute a binding without a hand-maintained `ioc.lev`. Atlantis Track 04 independently
explored exactly that problem (§3 of that doc, "the hard problem") and, after rejecting two
candidate mechanisms on probe evidence (token-pasting for generated names doesn't exist,
A15; a generated declaration named after the matched class self-captures and **segfaults the
compiler**, A14 — filed as a bug), landed on a **splice-based** mechanism instead
(`§3R`/"R10 addendum," 2026-07-07):

```
namespace App {
    void AddBindings() {
        @InjectBindings();                    // rule-generated `bind` statements land HERE
        bind IUserService => UserService();   // hand-written binds beside them
    }
}
```

A rule targeting `at splice InjectBindings` injects ordinary `bind` statements directly into
the splice site's own lexical scope — no `Bindings` object, no marker-file indirection,
duplicate-bind collisions surface exactly like hand-written code (because they *are*
hand-written-shaped code, just generated). This is tracked as item E of
`designs/requests/request-metaprog-splices.md:38,57-58` (LA-22) — **still an open, un-accepted
request** (the file is at `designs/requests/`, not `designs/requests/accepted/`), so this
mechanism is itself unimplemented.

**Implication for the tech design:** Channel 2's original motivating use case (many
generated bindings, one install point) already has a *different*, already-designed successor
that a real consumer prefers, and that successor doesn't need a `Bindings` type at all. The
tech design should explicitly decide whether Channel 2 (the general-purpose, hand-authored
`Bindings` builder — e.g., a test wanting to swap five capabilities in one call, with no
metaprogramming involved) is still worth building given the aggregation problem it partly
motivated now has a preferred non-`Bindings` answer, or whether to scope Channel 2 down to
"the general builder object, for manual multi-bind installation" and explicitly disclaim the
generated-bindings use case as LA-22's territory.

---

## 10. Corrections to system-binds.md's own sketch (§2), verified against the real prelude

1. **`env::variable(name)` → `env::get(key)`** (§6.3 above) — the sketch's `IEnv`
   implementation calls a function that doesn't exist; the real one is `env::get`.
2. **`IEnv.variable` naming is otherwise fine** — nothing requires the *interface's* method
   name to match the underlying `env::` function name; only `SystemEnv`'s implementation
   body needs the correction.
3. **The shared-instance spelling (`const SystemEnv systemEnv = SystemEnv(); bind IEnv =>
   systemEnv;`) works exactly as sketched** once `const` (landed, §6.2) is added — no
   change needed there beyond adding the `const` keyword the sketch's own §6 item 2 already
   anticipated.
4. **`console` staying "a lowercase instance, not a lowercase class"** (§5.1 of the source
   doc) is confirmed as the current, unchanged shape (`src/Resolver.cpp:2601`) — no drift to
   correct there, but see §7 above for the generic-method-requirement wrinkle `IConsole`
   will hit that the sketch doesn't anticipate.

---

## 11. The hard design questions synthesized (what the tech design must actually resolve)

Restating system-binds.md's own §6 open questions with their current-tree resolution status,
plus the new ones this research surfaced:

1. **Capability grain** (system-binds.md §6.1) — *still open.* §6.4 above gives a concrete
   structural precedent (stream direction-splitting) the design can lean on for
   `IFileSystem`/`INet`-style splits.
2. **Shared-instance spelling** (§6.2) — **resolved**, `const` landed (§6.2 above).
3. **Constructor injection** (§6.3) — **resolved**, already works (§3.3 above); the sketch's
   open question is moot, not because it was answered by this design but because the
   compiler now does it uniformly.
4. **comptime** (§6.4) — *still open, but low-risk*: binds are compile-time data already
   denied nothing new; a `comptime` injection of `IEnv` resolves fine, and any call into a
   system capability's *native* fails loudly at the native (the hermetic-comptime `sys*`
   deny is unrelated to and unaffected by this design) — not independently re-verified in
   this dossier, but no code path suggests it would behave differently from any other
   comptime-evaluated interface method call.
5. **Channel 1's actual mechanism** (§4.1 above) — *new, and the single largest unresolved
   question*: how does "importing a name" reach into the Checker's bind tables, which the
   Resolver's import machinery has zero connection to today? This needs real design, not
   scheduling.
6. **Channel 2's necessity given LA-22** (§9 above) — *new*: decide whether the general
   `Bindings` builder is still worth building, or whether to scope it down.
7. **The block-scope substrate dependency** (§5 above) — *new*: decide whether to block on,
   route around, or partially absorb the unified-substrate proposal that already exists but
   was never built.
8. **`IConsole`'s generic-method requirement** (§7 above) — *new*: interfaces requiring
   generic methods is untested territory; needs an explicit ruling before `IConsole` can be
   written.
9. **The `main()`-body bind-scoping trap** (§8.2 above) — *new, but small*: state explicitly
   that app-wide root binds belong at top level, un-namespaced, never inside a function body
   — Atlantis already learned this the hard way; the tech design shouldn't make every future
   reader re-derive it.

---

## 12. File & line index (quick reference)

| What | Where |
|---|---|
| `bindScopes_` storage | `src/Checker.hpp:107` |
| `pushBindScope`/`popBindScope`/`lookupBind` declarations | `src/Checker.hpp:243-245` |
| `pushBindScope`/`popBindScope`/`lookupBind` bodies | `src/Checker.cpp:1955-1993` |
| Value-type-bind rejection (bug #23) | `src/Checker.cpp:1966-1969` |
| Block-grain bind push | `src/Checker.cpp:2635` |
| Namespace-grain bind push | `src/Checker.cpp:3233` |
| Top-level/program-wide bind push | `src/Checker.cpp:3422` (`Checker::run`) |
| `Stmt` AST shape for `Bind`/object-install | `src/Ast.hpp:387-390` |
| `Stmt::importScope` (not yet renamed/unified) | `src/Ast.hpp:323` |
| `StmtKind::Bind` resolve (type + factory body) | `src/Resolver.cpp:5358` |
| `Use`/`uses` resolution (no bind interaction) | `src/Resolver.cpp:5146, 5168, 5307` |
| `env` namespace (top-level, `args`/`name`/`get`/`exit`/`setExitCode`) | `src/Resolver.cpp:2666-2685` |
| `Console` class + `console` const instance | `src/Resolver.cpp:2592-2601` |
| Prelude `interface I...` declarations (exhaustive list today) | `src/Resolver.cpp:350,354,2392,2416,2463,2472,2476,2486` |
| `StmtKind::Bind` / `ExprKind::Inject` lowering (shared across all backends) | `src/Lower.cpp:129,853,1786` |
| `ExprKind::Inject` oracle evaluation | `src/Eval.cpp:1142` |
| `di.ext` acceptance corpus | `tests/corpus/di.ext` |
| Bug #9 DI-core landing commit | `3722ded` |
| Bug #24/#23 (ctor injection, value-bind rejection) landing commit | `1fe86cc` |
| system-binds.md source (the request itself) | `designs/requests/accepted/system-binds.md` (this dossier's companion) |
| Block-scope substrate proposal (unimplemented) | `designs/techdesign-block-scoped-use.md` |
| Atlantis Track 04 (the primary downstream consumer) | `designs/atlantis/techdesign-04-di-config.md` |
| LA-22 splice mechanism (unimplemented, un-accepted request) | `designs/requests/request-metaprog-splices.md` |
| Normative bind/inject spec | `docs/reference.md` §4.7 (lines 713-724) |

---

## 13. Suggested v1/deferred split (non-normative — for the design author's convenience only)

This dossier does not make design decisions; the tech design owns that. But given how much
of §11's question list is genuinely new design work rather than "wire up the existing
scaffold," a design author may find it useful that the material naturally separates into a
tier that needs **no** new compiler mechanism and a tier that needs real new work:

- **Buildable today, zero new compiler mechanism:** the prelude capability interfaces
  themselves (`IEnv`/`IConsole`/`IClock`/`IFileSystem`/`INet`, modulo §7's `IConsole`
  generic-method question), their `System*` implementations, and root binds via plain
  `const` + factory `bind` — exactly system-binds.md §2's sketch, corrected per §10, with
  consumers writing `use std::IEnv; void main(IEnv env) { ... }` and getting the capability
  via an **explicit local `bind`** (not yet Channel 1) wherever it's needed outside the
  scope the root bind is declared in. This alone delivers §3 of the source doc's "what it
  buys" (mockable seams, capability-honest signatures) for any code willing to write the
  bind explicitly — which is exactly the shape Atlantis Track 04 already validated works
  end-to-end (§8 above).
- **Needs new compiler/language design work:** Channel 1 (§4.1, §11.5), Channel 2 as a
  general builder (§4.2, §11.6) if kept, and the block-scope substrate question (§5, §11.7).

---

*Prepared for the tech design of `designs/requests/accepted/system-binds.md`. Every claim above was verified
against `build/leviathan` on this tree (2026-07-13) or cited to a specific `file:line`; where
a prior document's claim could not be independently re-verified in the scope of this dossier,
that is stated explicitly rather than silently inherited.*
