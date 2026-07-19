# Research: FFI / Native Interop

**Purpose:** source material for the design phase of `designs/requests/request-ffi-native-interop.md`
(now moved to `designs/requests/accepted/`). This document does not decide anything — it
inventories every existing mechanism the FFI design would generalize, extend, or collide
with, with file:line citations and code snippets, so the design can be written without
the designer having repo access. Where research surfaced a fact that changes the shape of
an owner's-call the request lists, that is flagged explicitly as a **Finding**.

Companion reading: the request itself (`request-ffi-native-interop.md`) numbers the
feature array F-1..F-16 and the owner's-call points; this doc is organized to answer each
of those in turn. Section numbers below are this doc's own, cross-referenced to F-numbers.

---

## 1. The current native-intrinsic mechanism (what F-1 generalizes)

### 1.1 The empty-body rule is structural, not prelude-scoped — Finding

info.md §832 states the rule in prose: *"Method bodies are either native intrinsics
(declared with empty bodies in the prelude: `length`, `subStr`, …) or written in the
language."* The prose reads as if "native" is a prelude-only concept. **It is not — the
mechanism is a pure structural check in the lowerer, blind to source file:**

`src/Lower.cpp:1428-1429`:
```cpp
if (e->resolved && e->resolved->memberBody &&
    e->resolved->memberBody->kind == StmtKind::Empty) {
    std::vector<int> argRegs;
    for (const ExprPtr& arg : e->list) argRegs.push_back(lowerExpr(arg.get()));
    int base = F().nregs;
    emitArgCopies(argRegs, e->list);
    int dst = newReg();
    emit(Op::CallNativeFn, dst, 0, base, (int)argRegs.size());
    last().sname = std::string(callee->text);        // <-- the ONLY linkage info carried forward
    return dst;
}
```

Any declaration anywhere — prelude or user `.lev` file — whose body is literally empty
(`StmtKind::Empty`) lowers to `Op::CallNativeFn` with `sname` set to the declared name.
Nothing else is carried forward: no library, no calling convention, no symbol name. This
is confirmed by a design comment in the prelude source itself, describing a **user-style**
abstract method that must avoid this trap:

`src/Resolver.cpp:841-846`:
```cpp
class Seq<T> : IIterable<T> {
    // Abstract: every concrete Seq overrides this. A throwing body (rather than
    // a bodyless declaration, which the IR treats as a native) keeps the class
    // valid; it never runs, since a Seq value is always a concrete subclass and
    // the terminals below pull through `for..in`'s dynamic iterator() dispatch.
    IIterator<T> iterator() { throw RuntimeException("Seq is abstract: a subclass provides iterator()"); }
```

**Implication for F-1:** the front end needs no new grammar to make an empty-body
declaration "native-shaped" — that already works, in user code, today. What's missing is
purely what happens *after* — every backend currently treats `sname` as a lookup key into
its own fixed table (§1.2) and fails closed when the name isn't in that table (§1.3). The
`@extern` attribute's whole job is to give codegen an alternative to that table lookup:
read linkage off the attribute instead of matching the name.

### 1.2 Per-backend dispatch is a hardcoded string-match chain, repeated 4x

There is no single dispatch point — each of the four active engines maintains its own
if/else chain keyed on the native's name string, all reading the same prelude declarations
but implemented independently:

- **Oracle + IR interpreter (shared):** `src/RuntimeValue.hpp:339` declares
  `bool nativeCall(std::string_view cls, const std::string& method, ...)`, implemented in
  `src/RuntimeNatives.cpp:621` (2084-line file). Called from `src/Eval.cpp:922/953-956`
  (tree-walk oracle) and `src/IrInterp.cpp:126` (bytecode IR interpreter) — both engines
  share this one C++ file, which is why they agree on native semantics by construction.
  Free functions (not methods) are matched separately by `name ==` comparisons later in
  the same file, e.g.:

  `src/RuntimeNatives.cpp:1312-1338`:
  ```cpp
  if (name == "sysWrite") {
      long long fd = args.size() > 0 ? args[0].i : 1;
      if (args.size() > 1 && args[1].kind == VKind::Block) {   // Track 03 M4: (fd, b, off, len)
          ...
          const char* p = (const char*)bd.bytes->data() + bd.off + off;
          if ((fd == 1 || fd == 2) && sink) *sink += std::string(p, (size_t)len);
          else { ssize_t n = write((int)fd, p, (size_t)len); (void)n; }
          out = vint(len); return true;
      }
      ...
  }
  ```

- **emit-C++ (CGen):** `src/CGen.cpp:1039-1057`, a chain on `in.sname` inside the
  `Op::CallNativeFn` case, emitting calls to small inline C++ helper functions it defines
  itself elsewhere in the same file (e.g. `rt_syswrite`, `src/CGen.cpp:293`, which itself
  wraps the real libc `write()`):
  ```cpp
  case Op::CallNativeFn:
      if (in.sname == "sysWrite" && in.d == 2)
          out += "    " + R(in.a) + " = rt_syswrite(" + R(in.c) + ", " + R(in.c + 1) + ");\n";
      else if (in.sname == "sysReadLine")
          out += "    " + R(in.a) + " = rt_readline();\n";
      ...
  ```

- **LLVM backend:** `src/LlvmGen.cpp:2653ff`, a chain on `n` (the same `sname`) inside the
  `Op::CallNativeFn` case, each branch emitting `b.CreateCall(rtSysWrite, {...})` against a
  `FunctionCallee` declared once in the `Gen` constructor (§2.2). The chain has a **named
  failure default**:

  `src/LlvmGen.cpp:2909-2911`:
  ```cpp
  } else {
      fail("native floor function '" + n + "'");
  }
  ```
  This is the exact mechanism that currently rejects any unrecognized native name — the
  "closed compiler-authored set" the request describes, made concrete. **F-1's minimum
  LLVM-side change is: before falling to this `fail()`, check whether the declaration
  carries an `@extern` attribute and, if so, emit a genuine external call instead of
  failing.**

- **X64Gen (ELF):** has the identical shape (`src/X64Gen.cpp:3799` for `sysWrite`) but is
  **frozen** (`[[feedback_x64gen-frozen]]` — never touch, never mention in FFI bugs/tests).
  FFI is LLVM/emit-C++/interpreter-only by the same standing rule every post-freeze feature
  follows.

### 1.3 What actually happens today for an unrecognized native name

Confirmed at `src/LlvmGen.cpp:2909-2911` above: a clean, named compile-time failure
("native floor function 'foo'"), not a crash or silent miscompile. This is the same
clean-diagnostic house style as the ELF-DNS precedent (`techdesign-08-system-natives.md`
§6) the request cites. CGen and the interpreters have analogous (if less uniformly named)
"native not implemented" failure paths — none of the four active engines silently
miscompiles an unrecognized native name.

### 1.4 The `sys*` hermetic-comptime gate — a naming-convention gate, not attribute-based

`src/Eval.cpp:803`:
```cpp
if (comptime_ && hermetic_ && name.rfind("sys", 0) == 0) {
```
Any native whose name starts with `sys` is denied at comptime evaluation (folding), full
stop — a simple prefix check, not an attribute or declared capability list. **Finding for
F-15/comptime interaction:** an `@extern`-declared FFI function will almost never be named
`sys*` (it's a user/library-chosen name, e.g. `compress2`), so this prefix gate **will not
catch it automatically**. If FFI calls must be comptime-denied (they should be — arbitrary
side effects during a hermetic compile-time fold is exactly the hazard the hermeticity
model exists to prevent), the design needs its own explicit gate keyed on the `@extern`
attribute's presence, not on naming convention. This is a real gap to close, not an
inherited freebie.

---

## 2. The runtime ABI and calling convention (F-2, F-3, F-4)

### 2.1 Everything today crosses the native boundary as a tagged, boxed pointer — Finding

The `LvValue` pair is the **universal** calling convention for every existing native, on
every backend. `runtime/lv_abi.h:1-11`:
```c
/* Boundary rule (§2.1, hurdle H-1): every value crosses the generated<->
 * runtime boundary as an `LvValue*` pointer (out-param first, then inputs).
 * No function here passes or returns LvValue by value — clang lowers
 * 16-byte structs differently per target (SysV splits into two scalars,
 * Win64 passes by pointer); the pointer convention sidesteps that entirely.
 */
typedef struct LvValue { int64_t tag; int64_t payload; } LvValue;   /* 16 bytes */
```
Every `rt*` `FunctionCallee` declared in `LlvmGen.cpp`'s `Gen` constructor
(`src/LlvmGen.cpp:285-320`, ~140 functions) follows this shape:
```cpp
auto fn = [&](const char* name, Type* ret, std::vector<Type*> args) {
    return module->getOrInsertFunction(name, FunctionType::get(ret, args, false));
};
rtSysWrite   = fn("lvrt_syswrite", voidTy, {ptrTy, ptrTy, ptrTy});   // out, fd-LV*, data-LV*
```
Every argument and every return value is a pointer to a 16-byte `{tag, payload}` pair;
scalars are boxed into `payload`, heap values put a pointer in `payload`. CGen's inline
helpers (`rt_syswrite`, `src/CGen.cpp:293`) and the interpreters' `Value` struct
(`src/RuntimeValue.hpp:41-54`) follow the same shape in their own value model.

**This means: FFI is not "one more entry in the existing rt* table." A real C ABI call
(`compress2(dest, &destLen, source, sourceLen)`) needs actual C types at actual argument
positions — `unsigned char*`, `unsigned long*`, `int` — not `LvValue*` boxes.** No existing
codegen path emits this shape of call. The LLVM backend's underlying machinery
(`FunctionType::get`, `CreateCall` against arbitrary `Type*` signatures, `TargetMachine`
ABI lowering for the real target) is fully capable of it — LLVM is a general codegen
library, this is exactly its job — but building it means a **new, parallel calling-
convention path** in `LlvmGen.cpp`, unboxing/re-boxing `LvValue` at the `@extern` call
boundary, not a table row reusing the existing `rt*` pattern. Likewise CGen would need to
emit genuine `extern "C"` declarations with real C types and unwrap/rewrap its `V` value
type at the boundary. Budget design effort here accordingly — F-2's marshaling table isn't
just documentation, it's a new lowering path on two backends.

### 2.2 The value tag model (what F-2's marshaling table must map from)

`src/RuntimeValue.hpp:37-54` (interpreter side) and `runtime/lv_abi.h:38-53` (native side)
are the two representations FFI marshaling must read from. Native/LLVM side (the one that
matters for a compiled FFI call):
```c
enum {
    LV_VOID = 0, LV_INT = 1, LV_FLOAT = 2, LV_BOOL = 3, LV_STR = 4,
    LV_OBJ  = 5, LV_ARR  = 6, LV_MAP  = 7, LV_NONE = 8, LV_CLO  = 9,
    LV_CHAR = 10, LV_BLOCK = 11,
    LV_WEAK_PROXY = 12   /* runtime-internal only, never a language value */
};
```
`LV_INT`/`LV_BOOL`/`LV_CHAR` store the value directly in `payload`; `LV_FLOAT` stores the
double's bit pattern via memcpy (never a cast-through-long, per struct-equality design §8);
tags 4/5/6/7/9/11 store heap pointers in `payload`. This is the **complete current type
universe** — narrower/unsigned widths (`int8`/`uint`/etc., F-2's forcing case for
`request-narrow-integer-types.md`) do not exist as tags today; `int` is the only integer
width (info.md §9, confirmed no `int8`/`uint`/`byte` primitive exists anywhere in
`src/RuntimeValue.hpp`, `runtime/lv_abi.h`, or `docs/reference.md`).

### 2.3 `Block` — the existing zero-copy substrate (F-2, F-4)

Interpreter side, `src/RuntimeValue.hpp:25-33`:
```cpp
// Track 03 §3: `Block` — a fixed-length mutable byte buffer (contract C4). A
// slice is an ALIASING view: it shares the same `bytes` with a different
// off/len, so a write through any view is visible to the parent (zero-copy,
// documented). Reference semantics (like Array/Map): assignment shares the
// BlockData; there is NO copy-on-write (mutation is honest, §3.1).
struct BlockData {
    std::shared_ptr<std::vector<uint8_t>> bytes;   // the backing store (root-owned)
    size_t off = 0, len = 0;                        // this view's window into `bytes`
};
```
Native/LLVM side, `runtime/lv_abi.h:44-45,467ff`: a `LV_BLOCK` heap record laid out as
`{parentPtr, off, len, dataPtr}` — **`dataPtr` is the real byte pointer**. For an FFI call
needing `void* buf, size_t len`, the marshaling is: read the Block's heap record, take
`dataPtr + off` as the pointer argument and `len` as the length argument. This is exactly
the zero-copy handoff F-4 wants, and the accessor family already exists and is fully
landed on oracle/IR/emit-C++/LLVM (no ELF lane):
```c
void lvrt_block_new(LvValue* out, int64_t size);
void lvrt_block_slice(LvValue* out, const LvValue* b, int64_t off, int64_t len);
/* ... byteat/setbyte/int32at/setint32/int64at/setint64/fill/blit/equals/mismatch */
```
The full accessor list: `runtime/lv_abi.h:475-488`. Every accessor bounds-checks and
raises `RuntimeException` on OOB (never silently clamps) — a property FFI's own pin
contract (F-4) will need to reconcile with, since a raw C pointer handed across the
boundary has no equivalent bounds check on the C side.

### 2.4 The opaque-handle / "ids not handles" precedent (F-2's `Ptr`)

No `Ptr` type exists anywhere in the codebase today (`grep -rn '\bPtr\b'` across
`src/*.hpp`, `docs/reference.md`, `info.md` returns nothing) — F-2's opaque handle type is
**fully greenfield**, but there is a directly-applicable precedent for the *rule* it should
follow: threads. `designs/complete/techdesign-threads-3.md:80-96` (Amendment A-1):
```
A `Worker<T>` or `Promise<T>` value may not cross a thread boundary — capturing
one in a spawn body or sending one through a channel is a loud, catchable error
naming the type ("pass a Channel"). `Channel<T>` remains the one sanctioned
cross-worker conduit.
```
The concrete mechanism today is even simpler than a handle type: fds/join-ids are **plain
`int`s** crossing the boundary (`sysThreadStart` returns a join fd, `sysThreadTransfer`
deep-copies everything else — `techdesign-threads-3.md:129-133`). Nothing today wraps a
raw pointer in a language-visible type; `Ptr`/`Ptr<Tag>` would be a new construct, but the
*rule it enforces* (an id/handle crosses; a counted/managed value never does, and the
runtime never dereferences or refcounts it) is the same rule already proven at the thread
boundary.

### 2.5 Struct-by-value: the columnar SoA collision — Finding (important for F-3)

The request assumes a per-struct `@repr(C)` marker would "opt out of columnar SoA," as if
columnar were a per-declaration attribute today. **It is not — columnar is a single global
compile flag, and eligibility is a purely structural, automatic rule, not something a
struct opts into or out of per-declaration.**

`designs/complete/techdesign-columnar-arrays.md:24`:
```
Columnar becomes the default representation for eligible structs — no new
user-facing type, no annotation — but staged behind a compile-time flag
(`--columnar`, default off) until the corpus/churn gates are green, then
flipped to default-on with `--no-columnar` retained one cycle as the escape
hatch. Ineligible structs (any non-scalar field) keep today's row-major dense
layout permanently.
```
Eligibility (`techdesign-columnar-arrays.md:56-58`):
```
Refcounted columns — structs containing string/reference-class/array/map/
closure/Block/weak/union/nested-struct fields are ineligible and stay
row-major. The eligibility line is static and crisp (§3).
```
**Consequence:** an FFI-bound struct is typically *exactly* the shape that qualifies as
"eligible" today (plain scalar fields — ints, floats, bools) — meaning it would default to
columnar SoA layout once `--columnar` is the shipped default, which is column-major, not
the row-major/AoS layout a C struct-by-value ABI requires. **F-3's `@repr(C)` therefore
needs to be a genuinely new per-declaration override — the compiler has no existing
per-struct escape hatch to repurpose; today's only lever is the single global
`--columnar`/`--no-columnar` flag, which would force *every* eligible struct in the whole
program to row-major just to satisfy one FFI boundary struct.** This is a real, not
cosmetic, design gap: the design must specify how a `@repr(C)` struct's eligibility is
suppressed without perturbing the columnar decision for every other struct in the same
compilation.

### 2.6 Struct-by-value fragility precedent (Known Warning cross-check)

Project memory (not a currently-live `bug.md` entry — the file wasn't found in this
worktree, likely tracked on a sibling agent branch) records: storing a struct as a `Map`
value in a class field corrupted memory on LLVM specifically once the map held 3+ entries
(bug #47), while the interpreters were unaffected — the workaround was indexing via
`Map<K,int>` into a parallel `Array<Struct>` instead of storing the struct directly as a
map value. This is independent evidence that **struct-by-value handling on the LLVM
backend has had real, LLVM-only correctness bugs before**, exactly the class of thing
F-3's ABI classifier (SysV register-vs-memory aggregate classification) risks reintroducing
if hand-rolled. The request's own Known Warnings section already says to lean entirely on
LLVM's ABI lowering rather than hand-classify — this cross-check is corroborating
evidence, not new information, but confirms the caution is warranted from this codebase's
own history, not just general ABI lore.

---

## 3. Linking (F-7) and the Trident manifest (F-8)

### 3.1 The link driver today — native (non-wasm) path

`src/main.cpp:698-784` is the `--build-native` link step for native (LLVM) targets. Key
facts:

- The runtime archive (`liblvrt.a`, or the per-triple cross-compiled equivalent) is
  located via `findRuntimeArchive()`/`findRuntimeArchiveForTriple()` or a `--runtime`
  override, then handed to the system C++ linker driver (`probeLinkerDriver`/
  `probeCrossLinkerDriver`) alongside the emitted object.
- **The existing "optional native dependency contributes extra link flags" mechanism is
  already built and shipping — the exact shape F-7/F-8 needs, already proven for OpenSSL:**

  `src/main.cpp:747-775`:
  ```cpp
  // LA-2 (techdesign-tls-crypto.md §5.2): the generic link-plumbing seam. A
  // `lvrt.link` text file beside liblvrt.a lists extra link flags the
  // archive's providers need (OpenSSL's "-lssl -lcrypto" when the TLS
  // provider is lv_tls_openssl.c; empty for the none-provider and for old
  // archives without the file, which keep linking unchanged). build-triple.sh
  // writes it per-triple; host CMake writes it too.
  std::string extraLink;
  {
      std::string linkFile = runtimeLib;
      size_t slash = linkFile.find_last_of('/');
      linkFile = (slash == std::string::npos ? std::string()
                  : linkFile.substr(0, slash + 1)) + "lvrt.link";
      if (FILE* lf = std::fopen(linkFile.c_str(), "r")) {
          char lb[512];
          while (std::fgets(lb, sizeof lb, lf)) {
              std::string ln(lb);
              while (!ln.empty() && (ln.back() == '\n' || ln.back() == '\r'))
                  ln.pop_back();
              if (!ln.empty()) extraLink += " " + ln;
          }
          std::fclose(lf);
      }
  }
  std::string cmd = cxx + " \"" + objPath + "\" \"" +
      runtimeLib + "\" -lm" + (windows ? " -lws2_32" : "") +
      extraLink + " -o \"" + finalOutPath + "\"";
  ```
  `runtime/x86_64-pc-windows-gnu/lvrt.link` / `runtime/aarch64-linux-gnu/lvrt.link` are the
  real per-triple files this reads. This is a **file-based side-channel next to the
  runtime archive**, not a plan/manifest field — an important structural fact: it's driven
  entirely from `runtime/<triple>/`, independent of any given user project's manifest. A
  per-*project* native dependency (an arbitrary user's `-lz`) has no path into this
  mechanism today; `lvrt.link` only ever describes the *runtime's own* optional providers
  (OpenSSL today). F-7/F-8 need a **second, project-scoped link-input channel** — this
  `lvrt.link` mechanism is precedent for "a text side-file listing extra linker flags,"
  not a channel FFI can simply reuse as-is.

- CMake-side feature detection (the pattern F-8's Trident-side "feature detection with
  graceful absence" should mirror): `CMakeLists.txt:69-73`:
  ```cmake
  find_package(OpenSSL QUIET)
  if (OpenSSL_FOUND)
    message(STATUS "OpenSSL ${OPENSSL_VERSION} found — TLS/crypto enabled")
    target_compile_definitions(langfront PUBLIC HAVE_OPENSSL)
    target_link_libraries(langfront PUBLIC OpenSSL::SSL OpenSSL::Crypto)
  endif()
  ```
  and again for the runtime archive itself (`CMakeLists.txt:1438-1495`, conditionally
  building `lv_tls_openssl.c` vs. the none-provider and writing the matching `lvrt.link`).
  This CMake-level detection is **compiler-build-time**, not user-project-build-time — it
  decides whether the shipped `leviathan`/`liblvrt.a` itself supports OpenSSL at all. F-8's
  Trident `[native]` manifest is a different, later-binding kind of detection (does *this
  user's machine* have `libz`/`pkg-config libz` at `trident build` time) — related in
  spirit, distinct in mechanism. Don't conflate the two when writing the design.

- The wasm link lane (`src/main.cpp:633-697`, `designs/complete/hard-02-link-lane.md`) is
  a separate, parallel lane keyed on `isWasmTriple(targetTriple)` — evidence that adding a
  new linker lane beside the existing native probe, without disturbing it, is an accepted,
  already-precedented pattern (useful if F-7's dynamic-load path or a future FFI-specific
  link mode needs its own lane).

### 3.2 The frozen build-plan contract — the central F-8 tension, resolved by precedent

`designs/complete/techdesign-toolchain.md:174-237` (§3.3) is the **hard, frozen** contract
between Trident (writes the plan) and Leviathan (reads it, never re-parses the manifest).
The frozen shape:
```
plan {
    out = "..."; mode = "..."; target = "..."; optLevel = 0 | 2;
    entry { kind = "script" | "file" | "function"; target = "..."; }
    src { path = "<abs>"; moduleId = ""; origin = ""; }
    edge { from = ""; to = "lib"; }
}
```
Rule 4 states: *"The plan is the only channel. No side files, env vars, or manifest
re-reads on the compiler side."* Read naively, this looks like it forbids F-8 outright
(a `[native]` manifest section has nowhere to go). **Finding: this is resolved by direct
precedent already in the same contract** — LA-20's `asset { }` block
(`techdesign-toolchain.md:207-215`) was added to the plan grammar **after** the freeze, as
a wholly new, additive block kind:
```
// LA-20 (additive, post-freeze): declared comptime import() build inputs,
// resolved + hashed by trident from the manifest's `assets = [...]`.
asset { rel = "views/index.html"; path = "<abs>"; moduleId = "";
        hash = "sha256:<hex>"; }
…
```
with the explicit design note: *"A manifest with no `assets` key emits no `asset` rows —
an older leviathan reading such a plan sees no new grammar."* This is precisely the shape
F-8 needs: a new `link { }` (or similarly-named) block row, additive and optional,
following the same "new block kind, absent when unused, older readers unaffected" rule
that `asset` already established as acceptable within the frozen contract. **The STOP-event
rule (`techdesign-toolchain.md`/`techdesign-package-manager.md` §0.3) is about changing
*existing* frozen fields or moving dependency-resolution logic into `leviathan` — it does
not appear to forbid a new additive block, given `asset` already is one.** The design
should still treat this as an owner's-call point (per no-deferrals-in-designs) rather than
assuming it's pre-approved, but it is not an open architectural question the way it might
first appear — there's a landed precedent to point to and mirror.

### 3.3 The manifest format and PM/compiler separation (F-8)

`tools/trident/manifest.hpp:1-83` is the whole manifest schema. Current shape (annotated
in the header comment):
```
name    = "hello"
entry   = "main.lev"
sources = ["main.lev", "util.lev"]
assets  = ["views/**", "schema/*.sql", "openapi.json"]   # LA-20, optional
version = "0.1.0"
out     = "hello"

[[dep]]
path = "jsonlib"
as   = "Json"
version = "1.0.0"
dev  = false
```
```cpp
struct ProjectManifest {
    std::string name;
    std::string entry;
    std::vector<std::string> sources;
    std::vector<std::string> assets;   // LA-20
    std::vector<Dependency> deps;
    std::string version;
    std::string out;
};
```
A `[native]` table (F-8: link libraries, `pkg-config` names, include/link search dirs,
vendored source, feature-detection) is a new top-level manifest key, structurally the same
kind of addition `assets` already was. **`leviathan` never parses this file — the whole
point of the PM/compiler split** (`designs/complete/techdesign-toolchain.md` header, and
enforced by a standing grep proof at `techdesign-package-manager.md:60-62`:
`grep -rnE 'parseManifest|Dependency|trident\.toml|project\.mf' src/` must stay empty).
Trident resolves `[native]` (feature-detect, `pkg-config` lookup, vendor build) entirely
inside `tools/trident/`, then emits the resolved result as `link { }` plan rows per §3.2;
`leviathan`'s compiler-side changes are limited to reading those new rows and passing them
to the linker (§3.1) — it does no detection, no `pkg-config` invocation, nothing.

### 3.4 Dynamic loading (F-7b) and libffi (F-13a) — fully greenfield

`grep -rn "dlopen|dlsym|libffi|ffi_call"` across `src/*.cpp`, `runtime/*.c`, and
`CMakeLists.txt` returns **nothing**. There is no existing dynamic-loading code, no
existing libffi integration, and no partial work to build on for either F-7's
`Library.load(path)`/`sym(name)` runtime-plugin surface or F-13(a)'s
"interpreters use libffi" option. Both are net-new subsystems; the design should not
assume any scaffolding exists.

---

## 4. The capability gate and interpreter-reach precedents (F-13, F-15)

### 4.1 The wasm per-target capability gate — closest existing precedent for a *gate*, not a package-capability opt-in

`designs/complete/hard-03-capability-gate.md` is the landed mechanism gating
platform-absent natives (files, process spawn, raw sockets, etc.) on the wasm-browser
target. Two tiers, both implemented at the LLVM backend:
```
1. Reachable-from-user -> compile-time diagnostic. Pre-emission, walk the IR
   call graph from the user program's roots... A gated native reachable from
   user code diagnoses: `wasm-browser: 'File' is not available on this target
   (no filesystem in a browser)`.
2. Unreachable prelude bodies -> trap stub. Gated natives in functions emitted
   anyway compile to a call to `lvrt_unsupported(const char* what)`.
```
This is a **per-target** capability gate (what a *platform* lacks), not a **per-package**
capability opt-in (what a *dependency* is allowed to touch) — F-15's ask is closer to the
latter. There is no existing precedent for a package-level capability-declaration-and-
enforcement mechanism in this codebase; the closest conceptual sibling the request itself
names is the DI capability model (`[Injectable]`-style, `techdesign-04-di-config.md`), but
that's an application-wiring concept, not a security gate — worth the designer reading
both before deciding F-15's shape, but neither is a ready-made answer.

### 4.2 The DOM host-bridge — evidence the wasm `@extern` proposal did NOT land as proposed

**Finding, important for F-1/F-16's "shared `@extern` family" plan:** `@extern` does not
exist anywhere in the compiler (`grep -rn '"extern"|@extern|Extern'` across `src/*.cpp`,
`src/*.hpp` returns nothing but unrelated LLVM `ExternalLinkage` hits). The wasm frontend's
DOM bridge, which `proposal-wasm-frontend.md` §7 sketched as generating
`@extern("dom")`-attributed import declarations per DOM method, **did not ship that way**.
What actually landed (`designs/complete/hard-06-hostbridge-seam.md`, 2026-07-18) is a fixed,
small, hand-written reflective seam — five natives behind three C entry points:
```
| native (prelude decl)                                          | C entry            |
|------------------------------------------------------------------|--------------------|
| int sysHostI(string op,int h0,int h1,string a,string b)          | lvrt_hostcall      |
| string sysHostS(string op,int h0,int h1,string a,string b)       | lvrt_hostcall      |
| void sysHostV(string op,int h0,int h1,string a,string b)         | lvrt_hostcall      |
| int sysHostCloReg((int) => void cb)                              | lvrt_host_clo_reg  |
| string sysHostEcho<T>(T v)                                       | lvrt_hostecho      |
```
with the explicit reasoning: *"doc 05 as-built... replaced the one-wasm-import-per-method
model with a single reflective `lv.dom_call(op, ...)`."* This is precisely what
`request-bindgen-metaprog-scope.md` (2026-07-19, the day before this FFI request) records
as **open and blocked** — its whole ask is about generating per-method `@extern`-shaped
stubs from this fixed reflective seam, and it found the metaprog rules engine currently
can't do that (interface/method reflection + cross-class injection gaps).

**Consequence for the FFI design:** there is no live, working `@extern` implementation to
share or reconcile with. "Unify with the wasm `@extern`" is currently two *aspirations*
(this ticket and the still-open bindgen ticket) pointing at the same not-yet-built
attribute name, not an existing mechanism FFI must not collide with. The design is free to
define `@extern(abi, lib, symbol?)` on its own merits; if the bindgen ticket's path (a) or
(b) is later chosen, that work should adopt whatever shape FFI defines for the `"C"` ABI
case, not the reverse. Worth stating this explicitly in the FFI design so it isn't blocked
waiting on a sibling ticket that is itself waiting.

### 4.3 Interpreter native dispatch today (F-13 baseline)

Both interpreters share one dispatch function, `nativeCall`
(`src/RuntimeValue.hpp:339`, implemented `src/RuntimeNatives.cpp:621`), a single big
string-match chain (`cls == "..."` then `m == "..."`, or free-function `name == "..."`)
called from `src/Eval.cpp:922` (oracle) and `src/IrInterp.cpp:126` (IR interpreter). There
is no per-target subsetting inside this file today (unlike the wasm gate, §4.1) — every
native it implements is available on both interpreters uniformly (POSIX-only assumptions
throughout, e.g. `termios`, `sys/socket.h`). **This is the file F-13(b)'s "clean
interpreter deferral" would extend** (add an `@extern` case that raises the equivalent of
`"native FFI not available on this engine"`), and **the file F-13(a)'s libffi path would
extend instead** (add an `@extern` case that resolves the library via `dlopen`, the symbol
via `dlsym`, and invokes via `ffi_call` using a signature built from the Leviathan
declaration's parameter/return types). Either path is a new case in this one file/pair of
call sites — there's no separate "interpreter native registry" to also touch.

---

## 5. Attributes and the metaprogramming substrate (F-1, F-3, F-9, F-12)

### 5.1 The attribute system (what carries `@extern(...)`/`@repr(C)`/`@export("C")`)

`docs/reference.md:2010-2030` (Layer A):
```
attribute Route { string method; string path; }   // fields ARE the arguments
attribute Column { string name = ""; }             // defaulted -> bare @Column

@Route("GET", "/users")                            // positional, comptime-const
Array<User> list() => ...;
@App::Tag(3) void f() { }                          // qualified form
```
Constraints that bound what `@extern`/`@repr`/`@export` can look like:
- `attribute Name { fields }` — fields must be `int`/`float`/`bool`/`string` only. No
  methods/ctors/accessors inside an attribute (checker-enforced). So `@extern(abi: "C",
  lib: "z", symbol: "compress2")` is directly expressible (three string fields, the last
  defaultable to bare `@extern` cases); a `Ptr<Tag>` generic parameter is not an attribute
  concern at all (it's a type, orthogonal to this).
- Attributes attach to classes, structs, interfaces, members, functions, globals, and
  namespaces — a free function (the common FFI declaration shape) is already a valid
  attachment point structurally.
- **Attributes do nothing until a rule reads them** — an attribute alone is inert
  metadata. For `@extern` to actually change codegen, either (a) the backends read the
  attribute directly off the declaration (bypassing the rule-injection layer entirely,
  since this is a compile-time codegen decision, not source-to-source AST rewriting), or
  (b) a rule + comptime reflection generates something from it. Given `@extern`'s job is
  "tell the linker/codegen what to call," **(a) is almost certainly the right shape** —
  it's analogous to how the checker/resolver already reads other structural markers
  (`mutating`, `readonly`) directly rather than through the rules engine. The rules engine
  (Layer B, §16.5) is for *source-level* code generation (injecting a method body), which
  is a different problem than "this empty-body function's linkage came from here."
- Per-file scoping (`docs/reference.md:2025-2030`): unqualified `@Name` resolves through
  the declaring file's imports; ambiguity is an error, qualify with `@Web::Route`-style
  paths. `@extern`/`@repr`/`@export` would presumably be `std`-namespace built-ins (like
  `@Route` is user-namespace but a hypothetical `@extern` would need to be globally
  available without an explicit `uses`) — an open syntax question the request already
  flags as owner's-call.

### 5.2 Rules engine (F-12's bindgen substrate)

`docs/reference.md:2032-2047` (Layer B) — matcher + quasiquoted injection, namespace-
scoped, running AST→AST before lowering so injected code is cost-identical to hand-written
code. This is the substrate `request-bindgen-metaprog-scope.md` found insufficient for the
*wasm* DOM bindgen (needs interface/method reflection + cross-class injection, neither
shipped). The same gaps would block a native-C header bindgen (F-12) for the identical
reason — reflecting a C function's signature and generating a matching `@extern`
declaration is exactly the "reflect signatures, inject generated declarations" shape that
ticket found blocked. **F-12 inherits that open ticket's blocker whole-cloth; the FFI
design's job is only to make sure whatever `@extern` shape it defines is friendly to that
future bindgen (e.g., a flat, fully-positional/keyword attribute the rules engine could
one day emit programmatically), not to unblock the metaprog gap itself** (out of scope per
the request's own §9).

---

## 6. Cross-cutting/target facts (F-16)

### 6.1 `target::*` comptime constants — already landed, ready to use

`docs/reference.md:2114-2120`:
```
comptime if (target::os == "windows") { uses App::WinConsole; }
```
`target::os` (`"linux"`/`"windows"`/`"macos"`/`"wasm"`/`"unknown"`), `target::arch`
(normalized — `arm64` reads as `aarch64`), `target::triple` (the exact `--target` string).
These are the mechanism F-16's cross-compilation story keys off — no new comptime surface
needed, just consumption.

### 6.2 Calling-convention lowering is LLVM's job, already proven at arbitrary triples

`LlvmGen::emitObject(path, triple, optLevel)` already takes an arbitrary target triple as
a parameter and has been proven (the wasm spike, `proposal-wasm-frontend.md` §14, W-M0,
2026-07-17) to retarget cleanly with zero LLVM-backend-internal changes — the triple flows
straight to LLVM's `TargetMachine`. This is direct evidence F-16's "lean on LLVM's ABI
lowering, don't hand-roll" is not just advisable but already how this backend treats
target-specific codegen everywhere else it matters.

---

## 7. Summary table — what's genuinely new vs. what's precedented

| Feature | Existing precedent | What's actually new |
|---|---|---|
| F-1 empty-body decl → native call | Fully general today (`Lower.cpp:1428`), any source file | The `@extern` attribute itself; backends reading it instead of failing closed |
| F-2 marshaling table | `LvValue`/tag model (§2.2), `Block` (§2.3) | Everything not already a language value: raw scalar/pointer C-ABI calling convention (§2.1 finding) |
| F-3 `@repr(C)` structs | None — columnar opt-out is a global flag, not per-struct (§2.5 finding) | A genuinely new per-declaration layout override |
| F-4 pointers/pin contract | `Block`'s `dataPtr` (§2.3); scope/escape analysis (info.md §15) | The pin-vs-move/COW/gather-on-escape interaction; no existing "don't move this" marker |
| F-5 callbacks/trampolines | Closure representation (`RuntimeValue.hpp` `Closure`); `lvrt_callclosure` (hard-05, referenced by hard-06) | A C-function-pointer-shaped trampoline generator; nothing generates a raw fn ptr from a closure today |
| F-6 ownership/lifetime | Ids-not-handles thread rule (§2.4); ARC model (info.md §15) | Explicit per-binding free/borrow/adopt declarations — new surface |
| F-7 static link | `lvrt.link` file mechanism (§3.1) — but runtime-scoped, not project-scoped | A project-scoped link-input channel |
| F-7 dynamic load | None (§3.4 finding) | Fully new |
| F-8 Trident `[native]` | Manifest extension pattern (`assets`, §3.3); plan `asset{}` block precedent (§3.2) | The `[native]` schema itself + trident-side pkg-config/vendor logic |
| F-9 `@export("C")` | None | Fully new (reverse-direction ABI + artifact emission) |
| F-10 errors across boundary | `errno`-adjacent patterns not centralized anywhere found | New, per-binding policy |
| F-11 variadics | None | New, likely narrow/deferred-shaped |
| F-12 bindgen | Rules engine (§5.2) — but already blocked on the same gap the wasm bindgen ticket found | Inherits that open blocker |
| F-13 interpreter reach | `nativeCall` dispatch (§4.3) as the extension point | libffi integration (a) or deferral diagnostic (b) — both new |
| F-14 C-ABI-only scope | N/A (a scope statement, not a mechanism) | N/A |
| F-15 gate/capability | Per-*target* gate precedent (§4.1) — not per-*package* | A new capability-opt-in mechanism; DI's `[Injectable]` is a weak sibling at best |
| F-16 portability | `target::*` constants (§6.1) landed; LLVM triple retargeting proven (§6.2) | Just consumption — least novel item on the list |

---

## 8. Files a designer will likely want open while writing

- `src/Lower.cpp` (empty-body → `CallNativeFn` decision, ~1420-1650)
- `src/LlvmGen.cpp` (native dispatch chain ~2450-2920; `Gen` ctor's `rt*` declarations
  ~180-320; `emitObject` triple/link handling)
- `src/CGen.cpp` (native dispatch chain ~1000-1100; inline `rt_*` helpers ~250-500)
- `src/RuntimeNatives.cpp` (interpreter native cores, shared oracle+IR)
- `runtime/lv_abi.h` (the normative ABI contract — tag enum, all `lvrt_*` signatures)
- `src/main.cpp:560-820` (the whole link-driving block, all four output modes)
- `docs/reference.md:2010-2200` (attributes/rules/metaprog syntax reference)
- `tools/trident/manifest.{hpp,cpp}`, `designs/complete/techdesign-toolchain.md` §3.3,
  `designs/techdesign-package-manager.md` (PM-side manifest/plan mechanics)
- `designs/complete/hard-03-capability-gate.md`, `hard-06-hostbridge-seam.md` (closest
  gate/bridge precedents, both Track W)
- `designs/requests/proposal-wasm-frontend.md`, `request-bindgen-metaprog-scope.md`
  (the sibling tickets sharing `@extern` in name only, per §4.2's finding)
