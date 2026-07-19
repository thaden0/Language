# Request: FFI / Native Interop — a user-declarable foreign-function surface

**From:** cross-cutting (Trident ecosystem, Atlantis native drivers, Sonar terminal
backends, any future "we need a C library" need). **Date:** 2026-07-19.
**Priority:** P2 — nothing is *stopped* today (every native need so far — OpenSSL for
TLS, `pthread`/`eventfd` for threads, libm for float — has been met by a bespoke
compiler-internal `sys*` intrinsic behind the native seam). This ticket asks whether that
one-off-per-need model should be generalized into a single **user-facing FFI capability**,
so that a *library* can bind a foreign function without a compiler change, and so the
language can consume the enormous body of existing C-ABI code (C, C++, Rust, Zig, Go,
Swift, Fortran — anything that exposes a C ABI) directly.

## 1. Background — what exists, and the gap

Native interop today is real but **closed**. info.md §832: *"Method bodies are either
native intrinsics (declared with empty bodies in the prelude) or written in the language."*
The empty-body-plus-prelude form (`length`, `subStr`, `sysRead`, `sysTlsConnect`, …) is a
foreign-function declaration whose C++ implementation lives inside the compiler and is
selected per backend. That mechanism is the whole reason TLS (LA-2), threads (Track 10),
sockets (Track 08), and float math work — **"C++ stands behind these declarations
temporarily, never beside them"** (info.md §13), with OpenSSL and LLVM already sanctioned,
feature-detected dependencies.

The gap: **only the compiler authors can add one.** A user program or a Trident package
cannot say "call `compress()` from `libz`," "wrap `libsqlite3`," "drive `libcurl`," or
"reuse the Rust crate we already ship as a `cdylib`." Every such need currently becomes a
bespoke `sys*` intrinsic baked into the compiler — which does not scale, couples every
third-party native binding to a compiler release, and means the language can only talk to
the handful of native libraries the compiler team has personally wired.

This request is for the **general substrate**: the same empty-body-native-intrinsic rule,
lifted from *closed compiler-authored* to *open, user-declarable, library-linkable* —
generalizing one existing rule rather than adding a special case (info.md §1, "one rule
over many special cases").

## 2. The full feature array

Numbered so a design can accept/sequence them individually. F-1..F-6 are the core call
path; F-7..F-9 are build/link/embed; F-10..F-16 are completeness and safety.

- **F-1. Foreign-function declaration.** An empty-body free function or method carrying an
  `@extern` attribute that names the ABI, the library, and (optionally) the foreign
  symbol — e.g. `@extern("C", lib: "z", symbol: "compress2")`. The Leviathan signature is
  the source of truth for arity and marshaling; the attribute supplies linkage. This is
  the exact shape of the existing prelude intrinsic, with the C++ body replaced by an
  external symbol. Sibling to the wasm frontend's `@extern("dom")`
  (`proposal-wasm-frontend.md` §337) — same attribute family, different ABI channel
  (`"C"` = native symbol, `"dom"`/`"js"` = wasm import).

- **F-2. Type ↔ ABI marshaling table.** A defined, honest mapping for every scalar and
  reference type crossing the boundary:
  - `int` ↔ `int64_t`, `float` ↔ `double`, `bool` ↔ `_Bool`, `char` ↔ the Unicode scalar.
  - **Narrow/unsigned widths** (`int8/16/32`, `uint*`, `size_t`, `intptr_t`) are a
    correctness requirement at an ABI boundary, not a style choice — this is the concrete
    forcing case for `request-narrow-integer-types.md` (that ticket's item 2 names FFI
    explicitly). FFI cannot be complete while `int` is the only integer width.
  - `string` ↔ `const char*` (NUL-terminated) with a stated copy/borrow rule and who owns
    the returned buffer.
  - **`Block` ↔ pointer/`void*`/`char*`** — the zero-copy handoff. `Block` is already the
    language's *gated* mutable byte buffer (info.md §892, the §16 "bazooka in a marked
    room"), which makes it the natural, already-gated marshaling substrate: out-buffers,
    `read(fd, buf, n)`-shaped calls, and struct scratch all ride `Block`. Its aliasing
    `slice` gives sub-buffer pointers for free.
  - An **opaque handle type** (`Ptr`, or `Ptr<Tag>`) for C pointers Leviathan never
    dereferences (a `FILE*`, a `sqlite3*`) — ids/handles cross the boundary, not
    Leviathan-counted values, exactly the precedent the thread model set (info.md §14, A-1:
    "ids, never handles, cross the native boundary"). Null ↔ `Ptr?`/`None`.

- **F-3. C-layout structs by value.** A `@repr(C)` marker on a `struct` guaranteeing
  field order, C alignment/padding, and **opt-out of columnar SoA** (columnar is the
  default since 2026-07-12; a struct handed to C must be AoS/row-major). Passing/returning
  small structs by value, and pointers to structs (F-4), both need this. This is where the
  ABI classifier is hardest (see Known Warnings).

- **F-4. Pointers, out-params, and buffers.** Passing `&x` for primitive out-params,
  pointer-to-`@repr(C)`-struct, and array/buffer arguments (over `Block`). A **pin
  contract**: any `Block`/struct whose pointer is handed to C must not be moved or freed
  for the duration of the call — this must interact correctly with the move/COW machinery
  (info.md §15) and columnar gather-on-escape.

- **F-5. Callbacks (Leviathan → C function pointer).** Passing a lambda where C expects a
  function pointer, via a generated trampoline. Must state capture handling (a capturing
  lambda needs the C `void* userdata` convention, or a bounded no-capture restriction),
  and the trampoline's lifetime (tied to what — the call? a registered handle?). Needed
  for the large class of C APIs that take a callback (`qsort`, event loops, `libcurl`
  write-callbacks).

- **F-6. Ownership & lifetime across the boundary.** The rules for: who frees a
  C-returned pointer (caller-frees with a named deleter, borrowed-not-owned, or
  Leviathan-adopts-with-a-registered-`free`); borrow-vs-copy for `string`/`Block`
  arguments; and — critically — **non-interference with the non-atomic refcount fast path**
  (info.md §15). A pointer sitting in C must not participate in Leviathan's refcount at
  all; the boundary is a copy/pin/handle boundary, never a shared-count boundary.

- **F-7. Linking & loading.** Two mechanisms:
  - **Static/AOT link** on the LLVM backend (and emit-C++): `@extern(lib: "z")` contributes
    a link input (`-lz` / a resolved path) to the final link. This is the production path.
  - **Dynamic load** for (a) the interpreters, which cannot statically link (F-13), and
    (b) a *runtime plugin* surface — a `Library.load(path) -> Library` / `sym(name)` API
    (`dlopen`/`dlsym` + Windows `LoadLibrary`) for host programs that discover extensions
    at runtime.

- **F-8. Trident native-dependency manifest.** Because Trident and Leviathan are separate
  apps (HARD, PM/compiler-separation memory), the *what-to-link* lives in `trident.toml`,
  not the compiler: a `[native]` section declaring link libraries, `pkg-config` names,
  include/link search dirs, optional **vendored source** to build, and **feature detection
  with graceful absence** — the exact "build without OpenSSL still compiles, clean
  not-built diagnostic" pattern (info.md §13) generalized so any native dep can be
  optional. Trident resolves this into the plan; Leviathan consumes the resolved link set.

- **F-9. Exporting Leviathan → C (`@export("C")`).** The reverse direction: mark a
  Leviathan function/method as a C-callable symbol, and emit a **static/shared library
  artifact** (`.a`/`.so`/`.dylib`) with a generated C header — so Leviathan code can be
  *embedded in* or *called from* another language. "Implement code from other languages"
  cuts both ways; a full FFI story includes being the callee.

- **F-10. Errors across the boundary.** `errno` capture and mapping; a stated policy for
  turning a C error return (`-1`/`NULL`/a status enum) into either a Leviathan exception
  (§12.6 loudness) or a `T?` — declared per binding, since C has no single convention.
  Null-pointer returns surface as `Ptr?`.

- **F-11. Variadic C functions.** A bounded story for `printf`-family variadics
  (fixed-shape call sites, or an explicit `va`-list builder) — full generality is rare;
  state what is and isn't supported rather than leaving it undefined.

- **F-12. Bindgen / header import.** Automated generation of F-1 declarations and F-3
  `@repr(C)` structs from a C header, driven by the §16.5 rules engine. This is the
  automation layer over the runtime substrate this ticket requests, and it is already
  partly scoped: `request-bindgen-metaprog-scope.md` identifies the metaprog gaps for the
  *wasm* `@extern` bindgen, and the same interface-reflection / cross-class-injection
  primitives would serve native bindgen. FFI substrate first; bindgen rides it.

- **F-13. Interpreter (oracle/IR) execution.** The interpreters emit no native code, so
  they must reach a foreign symbol another way. **Owner's call to surface, two shapes:**
  - **(a) Dynamic invoke everywhere** — the interpreters use a libffi-style
    `dlopen`+`ffi_call` path so FFI programs stay in the differential corpus and run
    byte-identically on oracle/IR/LLVM (adds libffi as a sanctioned, feature-detected dep,
    same pattern as OpenSSL).
  - **(b) LLVM-only, clean interpreter deferral** — FFI calls compile only under the AOT
    backends; on oracle/IR they report a clean "native FFI not available on this engine"
    diagnostic, exactly like the existing `sys*` comptime/native-floor denies
    (reference.md §830). Cheaper, but splits the corpus.
  Recommend (a) if the differential corpus is to stay whole for FFI programs.

- **F-14. Language reach.** v1 is **C ABI only** — the universal waist. C++ is reached via
  `extern "C"` shims (name-mangling and exceptions are out of v1 scope); Rust/Zig/Go/
  Swift/Fortran all expose C ABIs and need no special case. State this boundary explicitly
  so "call C++ directly" is a recorded non-goal, not a surprise.

- **F-15. Safety gating.** FFI voids the language's safety guarantees by construction, so
  it belongs behind an explicit marker — the §16 gate pattern, "gate the dangerous,
  guarantee the safe" (info.md §1). Two tiers to decide: a **call-site/declaration marker**
  (an `unsafe`/`extern` gate that makes the danger visible in the source), and a
  **package-capability opt-in** in `trident.toml` (a package that uses FFI declares it, so
  a consumer can see it before depending on it — parallel to the DI capability model).

- **F-16. Platform / ABI portability.** Correct calling conventions per target (SysV
  AMD64, Windows x64, AArch64 AAPCS) — LLVM supplies the conventions; the struct-by-value
  ABI classification (F-3) is the part the design must not hand-roll. Cross-compilation
  keys off `target::triple`/`target::os`/`target::arch` (already landed as comptime
  constants). The wasm target's `@extern` is the JS-import channel, not native FFI — the
  two share the attribute surface and diverge at the ABI.

## 3. How this advances the language's ideology (info.md §1)

- **One rule over many special cases.** This is not a new construct — it is the existing
  empty-body-native-intrinsic rule (info.md §832) with its "which C stands behind it"
  answer opened from *the compiler's fixed set* to *any linked symbol*. Same declaration
  shape, wider provider.
- **Gate the dangerous, guarantee the safe** (F-15). The unsafe majority of C interop is
  fenced by an explicit marker so the safe language keeps its guarantees intact — the same
  move that made `Block` and interception safe to have.
- **A low-level language that feels high-level.** Direct, zero-copy C-buffer interop over
  the already-gated `Block`, with honest ownership rules, lets Leviathan reuse the entire
  native ecosystem without dropping to a second language — the "bazooka in a marked room"
  extended from memory to linkage.
- **Data / analytical performance.** Zero-copy `Block`/pointer handoff and `@repr(C)`
  layout control keep the boundary allocation-free for the hot buffer-passing path.
- **Honesty over hidden magic.** No implicit conversions across the boundary (nothing
  silently becomes a `char*`); every marshal is a stated rule, and the danger is marked,
  not hidden.

## 4. Requested specific feature (preferred shape — owner's syntax call)

The `@extern(abi, lib, symbol?)` attribute on an empty-body function/method (F-1), with
`@repr(C)` on structs (F-3) and `@export("C")` for the reverse direction (F-9), is the
shape that reuses the most existing machinery and lines up with the already-proposed wasm
`@extern`. The public-facing syntax needs owner approval (info.md ideology on public
syntax), specifically: the gate keyword for F-15, whether `Ptr` is one opaque type or
`Ptr<Tag>`, and the `[native]` manifest keys for F-8. This ticket asks for a direction and
sequencing, not a finished spec.

## 5. Known warnings

- **`@extern` is about to mean two things.** The wasm frontend
  (`proposal-wasm-frontend.md`) and its bindgen ticket
  (`request-bindgen-metaprog-scope.md`) already use `@extern` for wasm/DOM imports. Native
  FFI must share that attribute family cleanly (an `abi` discriminant), not collide with
  it — decide the unified shape once, across both.
- **Struct-by-value ABI classification is the deep end.** SysV AMD64's
  register-vs-memory classification for aggregates is notoriously subtle; lean entirely on
  LLVM's ABI lowering (F-16), never a hand-rolled classifier.
- **Refcount / move / columnar interaction (F-4, F-6).** Handing a pointer into a
  `Block`/struct to C while the mover or a columnar gather-on-escape relocates it is a
  use-after-free class of bug. The pin contract must be airtight, and it must not perturb
  the non-atomic refcount fast path (info.md §15).
- **Struct-as-value corruption precedent.** The `Map<K,Struct>`-field LLVM corruption
  family (bug memory: never store a struct as a `Map` value in a class field) shows struct
  value-handling on LLVM has sharp edges; `@repr(C)` struct marshaling should be
  cross-checked against that class of bug before trusting by-value struct returns.
- **Interpreter differential (F-13)** is a real fork in the corpus story — decide (a) vs
  (b) before implementing, not per bug.
- **Capability leakage.** Once a package can link arbitrary native code, the trident
  capability opt-in (F-15) is the only thing standing between a dependency and the host —
  it is a security surface, not a formality.

## 6. Acceptance criteria

1. A recorded design (`designs/techdesign-ffi-*.md`) that resolves every owner's-call point
   above (F-13 interpreter path, F-15 gate/capability shape, F-2/F-3 marshaling table,
   `@extern` unification with wasm) with **zero deferrals and a fixed ending state**
   (no-deferrals-in-designs memory).
2. Core call path (F-1..F-6) working end-to-end against a real system library (e.g.
   `libz` `compress`/`uncompress` round-trip, or `libsqlite3` open/exec/close), with the
   zero-copy `Block` buffer path exercised.
3. `@repr(C)` struct passed by value and by pointer, verified byte-compatible with a C
   reference program.
4. A Leviathan-registered lambda invoked as a C callback (F-5).
5. `@export("C")` producing a `.so`/`.a` + header that a plain C `main` links against and
   calls (F-9).
6. Trident `[native]` manifest (F-8) resolving a `pkg-config` dep and a
   feature-detected-optional dep (present → links, absent → clean not-built diagnostic).
7. Corpus coverage on every active engine (oracle / IR / emit-C++ / LLVM), consistent with
   the F-13 decision (if (a): byte-identical across all; if (b): LLVM/emit-C++ run, clean
   interpreter deferral diagnostic).

## 7. Relationship to other tickets

- **Depends on / forces:** `request-narrow-integer-types.md` (F-2 needs real narrow/
  unsigned widths — FFI is that ticket's strongest forcing case).
- **Enables / rides:** `request-bindgen-metaprog-scope.md` (F-12 native bindgen reuses the
  same interface-reflection/injection primitives), `proposal-wasm-frontend.md` (shared
  `@extern` family, F-1/F-16).
- **Builds on landed work:** `Block` (Track 03), the sanctioned-dependency seam + OpenSSL
  precedent (LA-2 / info.md §13), the thread boundary's ids-cross-not-handles rule
  (Track 10 / info.md §14), columnar `@repr` opt-out (2026-07-12), and `target::*`
  comptime constants.

## 8. Interim fallback

The compiler-internal `sys*` native seam **remains the only path**, and it works: any
specific native capability the platform needs continues to be added as a bespoke intrinsic
behind the seam (as TLS, threads, and sockets already were), vendored/feature-detected per
the OpenSSL pattern. Nothing is blocked today. The cost this ticket removes is the
*bespokeness* — every third-party native binding currently requires a compiler change
instead of a library. No lost work: this is a net-new capability request, not a
work-stoppage report.

## 9. Not in scope

Direct C++-ABI calling (name mangling / cross-boundary exceptions) — v1 is C ABI, C++ via
`extern "C"` shims (F-14). Automatic garbage-collection integration of foreign allocations
beyond the explicit ownership rules of F-6. A managed/sandboxed FFI that *removes* the
danger rather than gating it (F-15 marks the danger; it does not pretend to eliminate it).
The full §16.5 metaprog roadmap — only the bindgen primitives F-12 actually needs.
