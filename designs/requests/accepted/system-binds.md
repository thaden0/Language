# System-interface binds — Design Exploration

**Status:** proposal — the namespace-wall ruling landed 2026-07-05 (§5: binds cross
walls explicitly only, two channels); the binder-literal / export surface notes in §5.1
are sketch-level.
**Prerequisite (probed 2026-07-05):** the `bind`/`inject` system this design builds on
is currently **parse-only** — implicit injection never resolves, a block-level `bind`
is mis-checked as a return, and no `Bindings` class exists (bug.md #9, **assigned and
in progress** as of 2026-07-05; implementation guidance for it lives in that entry).
§1's claims about bind scoping are design facts (info.md §12.5), not yet implementation
facts. See §7 for the staging order.
**Related:** `imports.md` (the scoping model it shares), `argv.md` (whose surface it
wraps), `const.md`.

The idea, as raised: *piggyback on the `bind` system — system-interface binds.* Instead
of (or alongside) reaching for ambient globals (`console`, `env::args()`, `File`,
`TcpStream`), the prelude declares **capability interfaces** for the system boundary,
installs **root binds** to the real implementations, and user code receives capabilities
through ordinary injectable parameters. Tests and sandboxes shadow the root binds with
nearest-wins.

---

## 1. Why the existing machinery already fits

Three facts, all already in the design docs — nothing new has to be invented:

1. **Binds are block-scoped, lexically resolved, nearest-wins** (info.md §12.5). With
   `imports.md` §2 landing the same model for imports, the language ends up with one
   scoping story and **two lexical provision systems that differ only in their key**:

   | system | key | provides |
   |---|---|---|
   | `use` / `uses` | **name** | names — classes, functions, values, namespaces |
   | `bind` / inject | **type** | values — filled into parameters by contract |

   That is the piggyback, precisely: not one mechanism absorbing the other, but both
   riding the same scoping rule. (Neither subsumes the other — a type-keyed bind cannot
   pull a class name into scope for construction; a name-keyed import cannot fill a
   parameter by contract.)
2. **Injection is ambient, not threaded** (§12.5: "a binding is compile-time ambient
   context — never passed at runtime; the value it produces flows through ordinary
   parameters"). A function that wants the environment declares the parameter; its
   *callers* declare nothing. So capability-passing here does **not** have the
   signature-coloring contagion the language rejects elsewhere — the anti-contagion
   analysis is already done and already favorable.
3. **Interfaces-as-contracts is the established move**: §12.6 uses interfaces to define
   *catchability* (catch by capability, not by class). This design uses interfaces to
   define *injectability*. Same shape, second application.

## 2. Sketch

```
namespace std {
    // The capability contract (what a consumer may do)...
    interface IEnv {
        Array<string> args();
        string? variable(string name);
    }
    // ...the system implementation (thin shim over the argv.md surface)...
    class SystemEnv : IEnv {
        Array<string> args() => env::args();
        string? variable(string n) => env::variable(n);
    }
    // ...and the ROOT BIND: the real system, shared instance.
    SystemEnv systemEnv = SystemEnv();        // const, once const.md lands
    bind IEnv => systemEnv;
}

// Application code — importing the interface BY NAME activates its root bind here
// (§5 channel 1); the implicit `uses std` brings the name only, never the bind.
use std::IEnv;
void main(IEnv env) {                         // parameter filled by the nearest bind
    for (string a in env.args().skip(1)) handle(a);
}

// A test — shadow the system with a fake; nearest-wins does the rest.
class FakeEnv : IEnv {
    Array<string> canned;
    new FakeEnv(Array<string> a) { canned = a; }
    Array<string> args() => canned;
    string? variable(string n) => None;
}
{
    bind IEnv => FakeEnv(["lcurl", "-v", "http://127.0.0.1:8199/fixed"]);
    main();          // sees the fake — a local bind needs no import-activation,
}                    // and nearest-wins shadows the system bind either way
```

The candidate capability set mirrors the system boundary the docs already carve out
(§13: "streams are THE system boundary"): `IEnv` (args + env vars), `IConsole`,
`IClock` (timers), `IFileSystem`, `INet` (connect/listen). Each is a thin contract over
the corresponding convenience surface.

## 3. What it buys

- **Mockable system seams, from scoping alone.** lcurl's transfer engine could take an
  `INet` and be tested against a scripted fake — no fixture server, no real sockets, no
  monkeypatching machinery; a `bind` in the test's block *is* the mock installation.
  (The `argv.md` §8 env tests become fakes for free.)
- **Capability honesty.** `void main(IEnv env)` *says* it touches the environment; a
  function without the parameter provably doesn't (it has no other path — the globals
  remain, but a codebase can lint them to the shims).
- **A future gating story with zero new features.** §16's "bazooka in a marked room"
  pattern, expressed by scoping: a sandboxing scope binds `IFileSystem` to a denier or
  a chrooted impl, and everything beneath it — however deep — sees the restricted
  capability. Deny-by-default sandboxes become a library pattern.

## 4. Layering — every prior design stays intact

```
std::sys* floor natives            (the only privileged seam — unchanged)
  └─ convenience surface           (env::args(), console, File — argv.md; unchanged)
       └─ capability interfaces + system impls    (this design: thin shims)
            └─ root binds                          (bind IEnv => systemEnv)
```

The globals stay — they are the right floor for scripts and small programs. The binds
are the seam for applications and tests. The impls call the same convenience surface,
so there is exactly one behavior underneath both.

## 5. How binds cross namespace walls — owner ruling (2026-07-05)

Binds **do** cross namespace walls — explicitly and controlled, through two channels.
Bulk import is never one of them:

- **`uses NS` imports names only; no bind activates.** The implicit `uses std`
  included: having `IEnv` *visible* never silently makes it *provided*. (This kills the
  scenario the machinery would otherwise invite — every file in the program ambiently
  bound to the real system just by existing.)
- **Channel 1 — a bind travels with the selective import of its key.**
  `use std::IEnv;` imports the interface name **and** activates the bind keyed on it in
  the importing scope. The consent is precise: a bind is type-keyed, so naming the type
  is naming the bind — one statement, both facts, no ambient surprise. Per imports.md
  §2 it scopes exactly where it is written: top of file → file-wide; inside a block →
  that block.
- **Channel 2 — exported binder packages, explicitly installed.** A `Bindings` value is
  already ordinary data (§12.5); with imports.md it exports and imports like any value,
  and activation stays a separate, visible act — the existing `bind <value>;` install:

  ```
  // provider file
  namespace App {
      public Bindings binding = {
          MyNamespace::IMyInterface => MyClass();   // fresh per injection
          ILogger => sharedLogger;                   // shared instance
      };
  }

  // consumer file
  use App::binding as binds;
  bind binds;                        // activation is this word, nowhere else
  ```

  Nothing activates without either the interface-by-name import (channel 1) or the
  literal word `bind` (channel 2).

### 5.1 Surface notes (sketch-level)

- **The bind arrow stays `=>`.** The prompting sketch wrote
  `IMyInterface <= MyClass();`; recommend against `<=` — it already means
  less-than-or-equal, and the bind arrow is already `=>`
  (`bind ILogger => ConsoleLogger();`, §12.5). One arrow, one meaning; a binder literal
  is the same declaration form, braced and plural.
- **Binder literals** (`Bindings b = { A => ...; B => ...; };`) are sugar over §12.5's
  builder (`.add(...)`) — same install-time collision checking; duplicates inside one
  literal are the §12.5 hard error.
- **"Export" needs no new statement.** The prompting sketch wrote `export console;` /
  `export binding;`. The lean: the export surface *is* the access system — `public`
  members of a namespace are importable, `private` ones are not; a "bind export" is
  just a `public Bindings` value (or a publicly-bound interface). Explicit export lists
  / re-export facades remain imports.md §8.1's open question; nothing here depends on
  them.
- **`console` stays a lowercase *instance*, not a lowercase class.** The prompting
  sketch wrote `class console {}`. Keeping the current shape — capitalized `Console`
  class, lowercase `console` prelude instance
  ([Resolver.cpp:679](src/Resolver.cpp#L679)) — preserves the wanted ergonomics
  (`console.writeln("Hello")`, lowercase, never `new`ed by users) **and** keeps the
  console bindable and fakeable (`bind IConsole => console`; a test binds a silent
  fake). A static-only class is not a value, satisfies no interface slot, and cannot be
  injected — it would exempt the console from this very design. The casing rule that
  falls out: **caps = shapes (types); lowercase = things (values) and rooms
  (namespaces: `std`, `meta`, `env`)** — with the lowercase primitives
  (`int`/`string`/`bool`/`float`) as the deliberate keyword-feel exception.

## 6. Remaining open questions

1. **Capability grain.** One `IEnv` vs. `IArgs`/`IEnvVars`; one `IFileSystem` vs.
   read/write splits. Coarse is convenient; fine is least-privilege (and §3's gating
   story gets sharper with finer grain). Suggest: start coarse, split only when a real
   sandbox needs it.
2. **Shared-instance spelling.** §12.5 already contemplates `bind ILogger => shared`;
   the sketch uses a prelude global + `bind IEnv => systemEnv;`, which needs no new
   syntax and becomes `const` under const.md. Fine for v1.
3. **Constructor injection.** Does injection fill constructor parameters the same as
   function parameters (e.g. lcurl's `Transfer(...)` taking an `INet`)? §12.5 implies
   yes (constructors are members selected like any callable); should be stated and
   tested.
4. **comptime.** Binds are compile-time data, so nothing new to deny — the system
   impls' *natives* are already denied by the hermetic floor. A `comptime` injection of
   `IEnv` would resolve but any call into it fails loudly at the native, which is the
   correct (and already-specified) behavior.

## 7. Staging — when can this be implemented?

The design itself is settled: §5 is ruled, and §6's remainders are small enough to
resolve during implementation (adopt "start coarse" for grain; constructor injection
should simply be stated and tested alongside the DI core). The gates are
implementation prerequisites, in order:

1. **DI core — bug.md #9 (in progress).** Must land binds per lexical scope with
   implicit fill, `inject Type`, nearest-wins shadowing, and the duplicate-bind error —
   see the guidance and acceptance probes written into that bug entry (in particular:
   no shared-table dumping, the bug #8 mistake).
2. **imports.md `use`** — both channels are spelled with it (channel 1: the interface's
   selective import carries its bind; channel 2: importing a `Bindings` value).
   imports.md v1 is **file-level only** (its §9: the compiler has no per-block symbol
   scope); block-level `use` is deferred to a shared substrate that bug #9's block-scoped
   `bind` needs identically. **Coordination point:** that per-block lexical scope should
   be built **once**, carrying both an imported-name table and a type-keyed bind table —
   see imports.md §9 and bug.md #9's shared-substrate note. Channel 1/2 activation at
   *file* grain can proceed on v1 imports; scoping a bind or import to an inner block
   waits on the shared substrate.
3. **argv.md** — needed only for `IEnv` specifically (its system impl wraps
   `env::args()`/`variable()`); `IConsole`/`IClock` shims could precede it.

When (1) lands: run the bug #9 acceptance probes as the gate, then build the
capability layer (prelude interfaces + system impls + root binds — small), then wire
the two channels with imports.md. `const.md` and `argv.md` are implementation-ready
now and nearly collision-free with (1), so they are the natural parallel tracks while
this waits.
