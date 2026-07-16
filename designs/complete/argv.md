# argv — command-line arguments for Leviathan (Technical Design)

**Status:** proposal (nothing implemented). **Date:** 2026-07-07.
**Supersedes** the 2026-07-04 draft of this file, which predated the portable
pivot: it framed the pure-ELF `X64Gen` path as "the real work," spelled the
toolchain `lang` / `.ext`, and centred on env vars. Since then LLVM became the
primary backend, `X64Gen` was **frozen** (no new work — see below), the runtime
became the C floor under `runtime/`, and the toolchain is `leviathan` (`.lev`)
driving `trident.toml` projects. This revision re-grounds every layer in the
*current* tree and re-centres it on **argv** (env vars kept as a short companion
in §12, so the prior design work is not lost). The one decision carried forward
intact is the 2026-07-04 owner ruling that argv is read through a **call**, not a
binding (§3) — it survives the architecture change unchanged, and here's why.

Everything in §1 was verified by reading this worktree (`agent3`, Jul 7) at the
cited `file:line`; nothing is assumed.

---

## 1. Current state (verified)

Leviathan already threads `argc/argv` to the very doorstep of the runtime and
then **throws them away**. Three facts, all cited:

1. **The runtime entry receives argv and discards it.**
   `runtime/lv_abi.h:503` declares `void lv_rt_init(int argc, char** argv);`.
   Its definition opens by dropping both:

   ```c
   // runtime/lv_runtime.c:1353
   void lv_rt_init(int argc, char** argv) {
       (void)argc; (void)argv;          // <-- the whole feature is thrown away here
       g_heap_base = ... lv_plat_map(LV_HEAP_BYTES);
       ...
   }
   ```

2. **The "argv capture" comment is aspirational.** The runtime-owned process
   entry already passes the real vector down:

   ```c
   // runtime/lv_entry.c:24
   lv_rt_init(argc, argv);            /* regions, registries, argv capture */
   ```

   The `argv capture` clause describes intent, not behaviour — `lv_rt_init`
   ignores what it's handed (fact 1). `runtime/lv_runtime.c:101` echoes the same
   promissory note: *"lv_rt_init stays the explicit entry: argv capture needs
   it."* Capture is designed-for, not built.

3. **No `args` native, no language surface.** The prelude's `std` namespace
   (`src/Resolver.cpp`, the `kPrelude` string) declares the floor natives
   `int sysWrite(int, string)` (`:653`), `string sysReadLine(int)` (`:654`),
   `int sysStat(string, int)` (`:715`), the socket/timer family, and wraps them
   in higher-level helpers (`fileExists` `:1028`, `console` `:1226`). There is
   **no `sysArgs`, no `args()`, no `env` namespace.** `grep -n args
   src/RuntimeNatives.cpp` finds only the dispatch-arg plumbing, never a program
   argument vector.

Consequence, visible today in the flagship example (§11): `examples/curl/`
("lurl") cannot see its command line, so it reads **line 1 of stdin** as its
argv (`examples/curl/main.ext:10`, `std::sysReadLine(0)`). That spends stdin
(so `-d @-`, "body from stdin," is inexpressible) and makes the bare binary
appear to freeze — it is blocking on a stdin read no one will satisfy. Its own
header comment calls argv *"the single highest-value missing feature for CLI
programs in this language."*

The gap is therefore **one native and its plumbing**, not a new subsystem: the
data is already at `lv_entry.c:24`.

## 2. Is argv a stream? (no — the general rule already answers it)

Leviathan models *data that arrives over time* — ticks, packets, connections —
as streams, and a value that arrives *once, later* as a `Promise`. **argv is
neither.** It is fixed at `exec` time, before the first statement runs; it does
not "arrive." The language already has a home for exec-time process facts that
cross the boundary as **plain values through floor natives**: `sysStat` and its
`fileExists/fileSize/fileModified` wrappers, the `OpenMode` prelude globals.
argv joins that family. A "stream of argv" would be a replay of static data
wearing an event-source costume — exactly what the streams design refuses.

So the mechanism is settled by the existing rule, with **no new machinery**:
**one floor native returning a plain value, plus a thin in-language wrapper.**

## 3. How is it read? A call, not a binding (owner ruling, carried forward)

The user-facing shape is Rust's: the program's arguments as an
`Array<string>`. Rust spells it as a *call* — `std::env::args()` — and **that is
the ruling here too: `env::args()` is a namespace function** (no `this`), not a
`const` binding. The 2026-07-04 owner ruling stands; the C-floor rearchitecture
does not disturb its reasoning, and one of the four reasons is decisive:

1. **comptime hermeticity falls out for free (decisive).** Compile-time
   evaluation runs on the tree-walk oracle and is **hermetic — the `std::sys*`
   floor is denied** (`docs/reference.md:908`). A `const` global `args` would run
   its `sysArgs()` initializer *during prelude-global init inside the comptime
   evaluator* — invoking a denied native with no call site to blame, forcing a
   special case whose only options are erroring every compilation or fabricating
   empty args (the banned failure mode). A **function defers the native to the
   call site**: comptime code that actually touches `env::args()` gets the loud
   hermeticity error exactly where it asked; everything else never notices.
   Because `sysArgs` is a `sys*` member, it is denied **automatically** by the
   existing floor rule — zero new special cases.
2. **No startup materialization, no init-order constraint.** The native runs
   lazily at first use; the captured vector (§5) only has to exist before *user*
   code runs, which `lv_entry.c`'s entry order already guarantees — not before
   prelude-global initialization.
3. **No contact with the namespace-global machinery**, currently the buggiest
   corner of the language (`bug.md` #1: user-namespace globals never initialize;
   #7: assignment to globals silently ignored). Namespace *functions* are the
   green path.
4. **Read-only falls out of existing rules.** A nullary function is the
   get-only view of a value; the returned `Array<string>` is a pure value —
   fresh from the native each call, nothing aliased to tamper with — and
   copy-on-write keeps the hand-out cheap.

**Considered and rejected — `System.args()` / a bare `Args` type** (the shapes
the brief floated as examples):

- `System.args()` imports a foreign convention: capitalized namespace,
  `.`-access. Leviathan namespaces are **lowercase and `::`-accessed** (`std`,
  and the proposed `env`); `grep` finds `std::`/`env::` established across the
  design corpus and **zero** `System.` precedent. A capital-`S` `System.`
  member would be the only one of its kind — friction for no gain.
- A singleton `Args` **type** (`Args.all()`, `Args[0]`) needs a const global or
  a materialized struct to hang methods on — dragging in the `bug.md` #1/#7
  global-init path (reason 3) and forcing eager materialization of the whole
  vector on every touch. `Array<string>` is already a pure value with `skip`,
  `take`, `length`, `map` (`src/Resolver.cpp:349`; `docs/reference.md:631`); a
  wrapper type adds ceremony, not capability.

So: **`env::args() -> Array<string>`**, `args()[0]` the program name (§9),
`args().skip(1)` the argument list.

## 4. Proposed surface

Added to the `kPrelude` `std` namespace (`src/Resolver.cpp`), mirroring the
existing `sys*` declarations and their thin wrappers:

```lev
namespace std {
    // ...existing floor: sysWrite, sysReadLine, sysStat, sockets...
    Array<string> sysArgs();            // native (RuntimeNatives.cpp / lv_runtime.c):
                                        // the process argument vector, argv[0] first

    namespace env {
        Array<string> args() => std::sysArgs();      // a call, not a binding (§3)
        string name()          => std::sysArgs()[0]; // convenience: the program name
        // §12 companion: string? variable(string) => std::sysEnv(...)
    }
}
```

`std` is implicitly imported, so `env::args()` resolves through the one-level
qualification path (a nested-namespace *function call*, the green shape). Usage:

```lev
for (string a in env::args().skip(1)) handle(a);   // args()[0] is the program name

Array<string> a = env::args();
if (a.length() < 2) {
    console.writeln("usage: " + a[0] + " <file>");
    return;
}
string path = a[1];
```

Decisions folded in:

- **`args()` is a function** (§3): a fresh pure `Array<string>` per call, native
  deferred to the call site.
- **`args()[0]` is the program name** (C/Rust/Go convention; §9 says what that is
  per engine). The list of *real* arguments is `args().skip(1)`.
- **`args()` is never empty** — length ≥ 1 always, even with no arguments
  (`[programName]`). Callers never guard against a zero-length vector.
- **Read-only, no setter.** There is no `setArgs`; the vector is an exec-time
  fact, immutable for the process lifetime.

## 5. Where the value comes from, per engine

The native `sysArgs` is the single seam; each execution path fills it from its
own reality. **This is the layer the brief asks be spelled out across all
backends**, so each is stated with its exact mechanism and its argv[0].

| engine | primacy | argv source | argv[0] |
|---|---|---|---|
| **LLVM** (`--build-native` / `--emit-llvm`) | **primary** | real process argv, already at `lv_entry.c:24` → stashed in `lv_rt_init` | real `argv[0]` |
| tree-walk oracle (`--run`) | interp | tail after a new `--` on the `leviathan` command line, stashed by `src/main.cpp` | source basename, or manifest `out`/`name` for `--project` |
| IR interpreter (`--ir`) | interp | same `--` stash (shared `nativeFreeCall`) | same as `--run` |
| emit-C++ (`--emit-cpp` / `--build`) | port | generated `main(argc, argv)` capture | real `argv[0]` |
| pure ELF (`--emit-elf`, `X64Gen`) | **frozen** | **not extended** — see below | n/a |

### 5.1 LLVM (primary) — the short path

argv is already in hand; the fix is to **stop discarding it**. Three edits, all
in files that already exist:

1. **`runtime/lv_runtime.c`** — `lv_rt_init` stashes instead of `(void)`-ing:
   `g_argc = argc; g_argv = argv;` into two file-static globals (`:1354`).
2. **`runtime/lv_runtime.c`** — a new native
   `void lvrt_sysargs(LvValue* out)` that materializes an `Array<string>` from
   the stash, using the runtime's own constructors exactly as `lvrt_sysstat`
   (`:1251`) and `lvrt_sysread` (`:1264`) build their outputs:

   ```c
   void lvrt_sysargs(LvValue* out) {
       lvrt_arr_new(out, g_argc);                        // boxed Array<string>
       for (int i = 0; i < g_argc; i++) {
           LvValue s; lvrt_str_new(&s, g_argv[i], (int64_t)strlen(g_argv[i]));
           LvValue idx = { LV_INT, i };
           LvValue next; lvrt_idxset(&next, out, &idx, &s);  // COW slot store
           *out = next;
       }
   }
   ```

   (Final form follows the array-build idiom the runtime already uses; the point
   is that no new primitive is needed — `lvrt_arr_new` / `lvrt_str_new` /
   `lvrt_idxset` all exist.)
3. **`src/LlvmGen.cpp`** — declare and call it beside the other floor natives.
   The declaration slots into the block at `:262–274` that already binds
   `lvrt_sysread`, `lvrt_sysstat`, `lvrt_sysopen`, …:

   ```cpp
   rtSysArgs = fn("lvrt_sysargs", voidTy, {ptrTy});   // out-param only
   ```

   and `CallNativeFn "sysArgs"` lowers to `call rtSysArgs(outSlot)`.

**Critical caveat, grounded in the tree:** argv only reaches `lv_rt_init` when
the program goes through the **runtime-owned entry** (`lv_entry.c`'s
`main(argc, argv)`, the A-M5 flip that `src/LlvmGen.cpp:551–562` emits `lv_main`
for). The transitional **lazy fallback** — `lv_ensure_init()` calling
`lv_rt_init(0, NULL)` when generated code still emits its own `main`
(`runtime/lv_runtime.c:98–104`) — supplies **no** argv. So the design's
guarantee is: *once the program links through `lv_entry.o` (already the emitted
shape), `env::args()` is the real vector; on the lazy path it is
`[programName]`, length 1, never a crash.* This is a real precondition, not a
hope, and it is already satisfied by the current LlvmGen entry emission.

### 5.2 Interpreters (`--run`, `--ir`) — a `--` separator

The interpreters share `nativeFreeCall` (`src/RuntimeNatives.cpp:175`), so both
get `sysArgs` from **one** implementation, exactly as they share `sysStat`. The
only new plumbing is feeding the stash from the driver:

1. **`src/main.cpp`** — the flag loop (`:190–237`) currently treats any
   unrecognized token as the source path (`else path = argv[i];`, `:237`) and has
   **no `--`**. Add: on seeing `--`, stop flag parsing and stash the tail — plus a
   synthesized `argv[0]` (source basename, or the manifest `out`/`name` under
   `--project`) — into a small runtime-args registry.
2. **`src/RuntimeNatives.cpp`** — `nativeFreeCall` gains `if (name == "sysArgs")`,
   returning `varr(...)` built from that registry (the same `Value` array shape
   `sysReadLine`/`sysStat` already produce).

CLI contract:

```
leviathan --run  examples/kilo/main.lev -- notes.txt          # program sees ["main", "notes.txt"]
leviathan --ir   --project examples/curl -- -v http://127.0.0.1:8099/
```

`--` ends `leviathan`'s own flag parsing; everything after it is the program's
argv verbatim (the shell already did the quoting). No `--` → `env::args()` is
just `[argv0]`.

### 5.3 emit-C++ (`--emit-cpp` / `--build`) — capture in generated `main`

`src/CGen.cpp:920` emits `int main() {` today — **no** parameters. Two edits:

1. Emit `int main(int argc, char** argv) {` and stash `argc/argv` into the
   embedded runtime's registry as the first statement.
2. The generated runtime's `sysArgs` builds a `std::vector<V>` of `vstr` from
   that registry — the CGen analogue of the `nativeFreeCall` version, matching
   the "Native free functions (RuntimeNatives.cpp / CGen)" split the prelude
   already documents (`src/Resolver.cpp:1267`).

argv[0] here is the **real** `argv[0]` (a compiled binary), matching LLVM.

### 5.4 Pure ELF (`X64Gen`) — explicitly out of scope (frozen)

**This reverses the 2026-07-04 draft**, which made ELF "the real work" (capture
`rsp` at `_start`, walk the SysV stack). `X64Gen` is **frozen** — no new feature
work, per the portable pivot. So argv is **not** added to the ELF backend.
`sysArgs` on `--emit-elf` follows the precedent already set for the `std::math`
transcendental group, which "the zero-dep ELF backend … defers … with a
diagnostic" (`src/RuntimeNatives.cpp:178`): a compile-time *"native `sysArgs`
not available on the ELF backend"* diagnostic, or a frozen stub returning
`[argv0]`. ELF is the pre-pivot reference; portable CLI programs target LLVM.
This is a deliberate scope cut, called out so it is not mistaken for an omission.

## 6. Prior art → the Leviathan-native shape

| language | shape | argv[0] | notes |
|---|---|---|---|
| C | `main(int argc, char** argv)` | program name | raw, positional; every program re-parses |
| Go | `os.Args []string` | program name | package-level *variable* (slice) |
| Rust | `std::env::args() -> Args` (iterator) | program name | a **call**; `args_os()` for raw bytes |
| Zig | `std.process.args()` / `argsAlloc(alloc)` | program name | allocator-explicit, iterator |

Leviathan takes **Rust's call shape** (§3's decisive comptime reason) but returns
a **materialized `Array<string>`**, not an iterator — the language's collections
are eager pure values with `skip`/`take`/`map` (`Resolver.cpp:349`), so an
iterator type would be a foreign abstraction where a plain array reads
idiomatically. Like Go, argv[0] is the program name; unlike Go it is a call, not
a mutable package global (§3 reason 3). Byte-exactness (Rust's `args_os`, Zig's
raw bytes) needs no separate API here because Leviathan **strings are already
byte strings** (§9).

Ethos check: **one rule across scopes** — argv reuses the existing floor-native
+ namespace-function pattern, no new concept. **Dependency-free** — the LLVM path
adds one `libc`-free runtime function (`strlen` is the only touch, already used
throughout `lv_runtime.c`). **Whole-program AOT** — `env::args()` is an ordinary
call the checker and every backend already know how to lower. **Simplicity** —
net new *concepts*: zero.

## 7. Implementation inventory (grounded in real files)

1. **`runtime/lv_runtime.c:1354`** — replace `(void)argc; (void)argv;` with a
   stash into two file-static globals; add `lvrt_sysargs(LvValue* out)` beside
   `lvrt_sysstat`/`lvrt_sysread`.
2. **`src/LlvmGen.cpp:262`** — bind `rtSysArgs = fn("lvrt_sysargs", voidTy,
   {ptrTy})`; route `CallNativeFn "sysArgs"` to it.
3. **`src/RuntimeNatives.cpp:175`** — `nativeFreeCall` gains a `"sysArgs"` arm
   reading the driver's registry (shared by `--run` and `--ir`).
4. **`src/main.cpp:190`** — recognize `--`; stash the tail + synthesized argv[0]
   into the registry before the engines run.
5. **`src/CGen.cpp:920`** — emit `main(int argc, char** argv)`, capture into the
   embedded runtime; add its `sysArgs` core.
6. **`src/Resolver.cpp` (`kPrelude`)** — declare `Array<string> sysArgs();` in
   `std`; add the `env` namespace with `args()` / `name()`.
7. **Docs** — a `docs/reference.md` entry for `env::args()`; drop the "no argv"
   caveats from `examples/curl/main.ext:1–5` and `curl-design.md`'s gap table
   when it lands.
8. **`src/X64Gen.cpp`** — **no change** (frozen; §5.4). Emit the deferral
   diagnostic only.

## 8. Phased plan (what lands in what order)

Each phase is independently shippable and leaves the tree green.

- **P0 — capture (inert).** `lv_rt_init` stashes `argc/argv`; no surface yet.
  Pure addition, nothing reads it. Verifiable by a runtime selftest that
  `lv_rt_init(2, {"x","y"})` records length 2. *De-risks the seam with zero
  language-visible change.*
- **P1 — LLVM primary path.** `lvrt_sysargs` + `LlvmGen` wiring +
  `sysArgs`/`env::args()` in the prelude. After P1, a `--build-native` binary
  reads its real command line. **This is the milestone** — the primary backend
  is done.
- **P2 — interpreters.** `--` separator in `main.cpp` + `nativeFreeCall`
  `sysArgs`. `--run`/`--ir` reach parity so the differential harness (§10) can
  run.
- **P3 — emit-C++.** `CGen` `main(argc, argv)` capture. All non-frozen backends
  now agree.
- **P4 — consumers.** Migrate lurl off the stdin workaround (§11.1); land the
  kilo port skeleton (§11.2).
- **(not a phase) X64/ELF** — frozen; ships the deferral diagnostic only.

Dependency: P1 needs the runtime-owned entry (A-M5, already emitted). P2/P3 are
independent of P1 and of each other. P4 needs P1 (LLVM) at minimum.

## 9. Edge cases

- **No arguments.** `env::args()` is `[argv0]` — **length 1, never empty**.
  Callers use `args().length() < 2` to detect "no real args," never a
  zero-length check.
- **argv[0] = program name.** Its *value* differs by engine (§5): the real
  `argv[0]` for compiled binaries (LLVM, emit-C++); the source basename or
  manifest `out`/`name` for the interpreters (there is no real process name for
  a tree-walk). Programs that need a stable name should not lean on argv[0]'s
  exact spelling across engines — documented, not silently divergent.
- **Unicode / bytes.** argv is raw OS bytes. Leviathan strings are **byte
  strings** (Track 04 — `byteAt`, `fromByte`, no UTF-8 validation on
  construction), so `sysArgs` copies each `char*` **verbatim** via `lvrt_str_new`
  — no decode, no normalization, no failure mode on non-UTF-8 (a filename in a
  latin-1 locale round-trips). This matches Rust's `args_os` guarantee without a
  second API, precisely because the string type is already the byte type.
- **Empty-string argument.** `-- "" a` yields `["prog", "", "a"]`; the empty arg
  is preserved and distinct from "absent" — the shell's tokenization is honored
  exactly.
- **Embedded NUL.** POSIX argv is NUL-terminated C strings, so an argument
  cannot contain a NUL; `strlen` is the correct length and there is no lossy
  case to handle. (Called out so the `strlen` in §5.1 is understood as total, not
  a truncation.)
- **Huge argv.** No cap; the array is sized to `argc`. The 64-arg
  `LV_MAX_DISPATCH_ARGS` limit (`lv_runtime.c:479`) is unrelated — it bounds
  *call* arity, not the argv array.

## 10. Testing — differential across engines

The five-engine agreement rule drives the test shape:

- **Corpus program** (`tests/corpus/args_echo.lev`): print `args().length()`
  then each `args()[i]` on its own line. Drive it with `-- alpha "b c" ""` and
  fix the expected output.
- **Engine parity:** run the same program green on `--run`, `--ir`,
  `--emit-cpp` (compiled), and `--build-native` (LLVM). Diff all four against the
  fixed expectation. **`--emit-elf` is explicitly SKIP/XFAIL** (frozen; §5.4) —
  logged, not silently dropped.
- **No-args case:** run with no `--` tail; assert length 1 and `args()[0]`
  non-empty.
- **Byte-exactness:** an argument with a non-UTF-8 byte (e.g. `$'\xff'`) must
  round-trip unchanged through the compiled paths (proves the verbatim copy).
- **comptime hermeticity:** a program doing `comptime env::args()` must **fail
  compilation** with the floor-denied error (`reference.md:908`), like every
  other `sys*` in comptime. This is the §3-reason-1 guard, tested directly.
- **P0 unit:** a `runtime/selftest.c` case that `lv_rt_init(argc, argv)` records
  the vector and `lvrt_sysargs` reproduces it (independent of any frontend).

## 11. Consumers

### 11.1 lurl — retire the stdin workaround

`examples/curl/main.ext:10` reads its command line from stdin. With argv it
becomes a normal CLI program:

```lev
// before (examples/curl/main.ext:9-13)
void main() {
    string line = std::sysReadLine(0);
    Array<string> tokens = tokenize(line);
    Options o = parseOptions(tokens);

// after
void main() {
    Options o = parseOptions(env::args().skip(1));   // shell already tokenized
```

Payoff, each item a current pain point removed:

- `./lurl -v http://127.0.0.1:8099/index.html` works **bare** — no pipe, no
  apparent freeze (the blocking `sysReadLine(0)` is gone).
- **stdin is freed**, so `-d @-` (request body from stdin) moves from
  impossible to implementable.
- `cli.ext`'s quote-aware `tokenize` (`examples/curl/cli.ext:40`) is retired for
  the normal path — the shell did the quoting — surviving only if a `-K -`
  config-file mode is wanted later.
- The "no argv" caveat block atop `main.ext` and the `curl-design.md` gap row
  are struck.

### 11.2 kilo — the port this unblocks

A Leviathan port of the kilo editor (antirez's ~1K-line terminal editor) needs
`kilo <filename>` as its first act — open the file named on the command line.
Without argv it cannot exist; with it, the entry is idiomatic:

```lev
// examples/kilo/main.lev  (future)
void main() {
    Array<string> a = env::args();
    if (a.length() != 2) {
        console.writeln("usage: " + a[0] + " <file>");
        return;                         // §12: a real exit code wants sysExit
    }
    string path = a[1];
    Editor ed = Editor.open(path);      // reads via std::sysOpen/sysRead
    ed.run();
}
```

This exercises exactly the surface §4 defines: `args()` length-checked, `args[0]`
in the usage line, `args[1]` the positional file — the canonical
`tool <required-arg>` CLI shape, which lurl (optional flags) and kilo (one
required positional) together cover.

## 12. Companion: env vars & exit codes (not this design, same seam)

Preserved from the prior draft so the design work is not lost, but **out of
scope for the argv deliverable**:

- **Environment variables.** Same floor pattern: a `string? sysEnv(string)`
  native (`::getenv`; unset → `None`, set-empty → `""`, set → value — the
  three-state distinction the `None` design exists to keep honest) wrapped as
  `env::variable(string) -> string?`. `variable`, not `var` (keyword clash);
  read-only, no `setEnv` (process-global mutable state, thread-hostile
  `setenv(3)`). It rides the *exact* `env` namespace and the *exact* §5 per-engine
  seam as argv, so it lands cheaply once argv's plumbing exists.
- **Exit codes.** `env::args()`'s natural companion for CLI tools (kilo's usage
  error above wants a non-zero exit). `lv_rt_exit_code()` currently returns a
  hard `0` (`runtime/lv_runtime.c:1366–1371`). A `sysExit(int)` native + a
  per-backend epilogue is one floor native — but it is process *control*, not
  process *facts*, so it gets its own short design and is not folded in here.

Both share argv's `main.cpp`/entry plumbing pass; sequencing them right after P3
is natural. Neither blocks the argv deliverable.
