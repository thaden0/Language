# Proposal — WebAssembly Front-End Target (Leviathan in the browser, DOM access)

**Status:** proposal — **deferred to post-Gate-E** (after `techdesign-09-web-foundations`
lands and the framework-enabling refactors settle). Not scheduled into the active track
queue. Two near-term actions only: a de-risking **spike** (§9) and a standing
**syscall-floor discipline** (§8).
**Date:** 2026-07-08.
**Depends on (for the real work):** a settled async-suspension model (info.md §14, §19 #5),
per-target stdlib packaging (the "ship stdlib as files" ruling flagged in
`techdesign-09-web-foundations.md` §0), LLVM-backend runtime ABI landed (`liblvrt.a`, per
`docs/techdesign-portable-backend-2.md` Track B), and metaprog Phase 3+ for generated
bindings (info.md §16.5). **Depends on (for the spike):** nothing — the current LLVM
backend suffices.
**Source:** the WebAssembly front-end question (use Leviathan on Atlantis's client side,
compiled to wasm with a JS glue layer for DOM access); reconciled against info.md §7/§9/§13/
§14/§15/§17 and the engine-coverage policy in `techdesign-00-overview.md` §4.3.
**Owns (when built):** a new backend column (IR → wasm), a browser implementation of the
§13 Layer-0 floor, and a capability-gated platform library + JS glue. **Touches the language:
nothing.** **Touches the IR: nothing.** This is the central claim and the main de-risking —
the same shape as Track 09's zero-native claim.

---

## 0. Verdict (the decision this doc records)

**The type-system concern is misplaced, and it is not the blocker.** The instinct — "Leviathan
erases like Java, so it can't hand concrete typed values to a `wasm-bindgen`-style marshaler" —
rests on a wrong premise (§1) and a wrong requirement (§2). Leviathan carries *more* runtime type
information than Rust or Java-erased generics, and the wasm↔JS boundary needs a **stable ABI**,
not monomorphization. Every dynamic-runtime language ships on wasm with full DOM access over a
uniform representation (C#/Blazor, Kotlin/Wasm, Dart, Pyodide); monomorphization is a *workaround
for having no runtime*, not a prerequisite.

**The real work is a backend, a runtime-floor retarget, and a bindgen layer — none of which
touches the language.** The bulk is **cheaper to build after the queue** (§11), because building
it now means building on four substrates still in flux (§4) and blocking on an async-suspension
design decision that is not yet made (§3.2). Doing it after **Gate E** — this doc's shorthand for
the end of the pre-framework robustness pass (Phase E in `techdesign-00-overview.md` §5: Track 09 /
web-foundations landed, ~Aug 21) — turns it into a contained, track-sized effort riding settled
ground.

**Do now (cheap, high-value):**
1. **A 1–2 day spike (§9):** retarget the existing LLVM backend to `wasm32`, run a
   pure-computation program in `wasmtime`/a browser. Validates the whole type-system claim
   empirically for near-zero cost, commits to nothing.
2. **A standing discipline (§8):** keep the §13 Layer-0 floor enumerated, wrapped, and
   per-backend-diagnosed — the house style already, now also named as the wasm-portability
   guardrail. Costs Track 08 nothing.

---

## 1. The premise, corrected — Leviathan is not Java erasure

The framing "closer to Java-style erasure than to Rust or C++" is inaccurate in the way that
matters. Leviathan is the **C# hybrid**, and — unlike Java-erased generics — it carries runtime
type information. Two facts from the runtime, not the prose:

- **Every value is self-describing by tag.** `RuntimeValue.hpp` — `enum class VKind { Void, Int,
  Float, Bool, String, Object, Closure, Array, Map, None, Char }`; the native backends carry the
  same `[tag(8)][payload(8)]` 16-byte pair (`X64Gen.hpp`, and the LLVM ABI's `LvValue* {tag,
  payload}` in `LlvmGen.hpp`).
- **Every object knows its class/shape at runtime.** `RuntimeValue.hpp` — `struct Object { Symbol*
  cls; ... }`. The `cls` pointer is the §7 shape; the shape holds the name→offset slot table. A
  marshaler can recover the class and enumerate typed slots from any live object.

| axis | Java erasure | Rust / C++ | **Leviathan** |
|---|---|---|---|
| generic type args at runtime | erased, lost | monomorphized, no RTTI needed | erased from *codegen*, but **tag + classId survive** |
| primitive in a generic slot | **boxed** (`Integer`) | monomorphized inline | unboxed tagged payload (§9 object-mask) |
| value-type generic (`Array<Point>`) | still boxed | monomorphized, dense | **reified & dense** (§9 `struct` → flat run) |
| runtime type info | mostly gone | none (static only) | **present** (tag + shape) |

The bottom row is the whole story. Rust *needs* per-instantiation `wasm-bindgen` glue precisely
because it has **no runtime and no RTTI** — the static call-site type is the only handle it has on
a value. Leviathan is the opposite: it can marshal **reflectively**, off the tag and shape it
already carries.

Java erasure specifically means (a) type params gone, (b) primitives forced to box, (c) no runtime
reification. Leviathan matches only (a), and only for codegen. It does **not** box primitives on
the fast path (§9), it **does** reify value types (`struct` → dense/columnar, §9), and it **does**
retain per-value/per-object type info. Calling that "erasure" imports the wrong intuitions.

---

## 2. Why the uniform representation is an advantage at the wasm boundary

The wasm↔JS boundary does **not** require monomorphization. WASM core types are `i32/i64/f32/f64`
plus `externref`; everything else — strings, objects, arrays, structs — crosses as *a pointer into
linear memory + a length*, or as an opaque handle. The glue marshals against an agreed
representation. That agreement is a **stable ABI**, which Leviathan already has, and because it is
*uniform*, the marshaler is written **once**, table-driven off the tag and shape — instead of
regenerated per instantiation the way `wasm-bindgen` must.

- **Reference generics are erased/uniform** (like C# reference types, like every VM): `Array<string>`
  and `Array<DomNode>` share one runtime type and one marshaling path. The type argument was a §9
  compile-time slot, checked and gone; the container holds tagged `Value`s.
- **Value generics are reified** (like C# value types): `Array<Point>` is a flat run of `Point`
  records (§9), which is exactly the layout a JS `TypedArray` view reads **zero-copy**. The one
  place concrete layout genuinely matters — sharing a specific typed record across the boundary
  without a copy — Leviathan already covers, *on demand*, without making everything reified.

This is the proven-wasm-friendly model (C#/.NET on wasm, Kotlin/Wasm, Dart), not the
Java-pure-erasure model the premise feared. The uniform representation is a feature here: one
marshaler, not N.

---

## 3. What actually changes — and what does not

Everything lowers to **one IR** (info.md §17); the IR is the semantic truth and each backend only
diverges on "emit for an op." So the wasm target is exactly three things, none of them the
language:

1. **A new backend:** IR → wasm (§5).
2. **A browser implementation of the §13 Layer-0 floor** (§6).
3. **A capability-gated platform library + JS glue** (§7).

The **language, the type system, and the IR are unaffected.** "It's really just libraries + a
backend" is correct.

### 3.1 Per-target capability subsetting already exists

The front end does **not** need a language subset (a dialect fork would be a mistake — two
languages to maintain). It needs a *platform-capability* subset, and that is already a first-class
concept:

- **Engine-coverage policy** (`techdesign-00-overview.md` §4.3): oracle+IR mandatory; emit-C++
  everything-except-system-layer; ELF whole-language with **explicit diagnostics for deferred
  items, never silence**; LLVM scalar-core, extend-or-report.
- **The clean-diagnostic pattern in practice** (`techdesign-08-system-natives.md` §6, F6 DNS): the
  pure ELF backend has no libc resolver, so `sysResolve` emits *"resolve: not on the pure backend
  yet"* rather than faking it.

A wasm-browser backend is **another column in that matrix**. It implements the browser-relevant
floor (console write, `sysNow`, `sysRandom` via WebCrypto, event-loop hooks) and cleanly diagnoses
the rest — `File`, `sysSpawn`/fork/execve, `getdents`, raw TCP `connect`, `isatty` — as
unsupported-on-this-target. Referencing `File` in browser-targeted code is a compile-time
diagnostic, exactly like ELF-DNS today. Note how much of Track 08's floor is *inherently absent* in
a browser (processes, directories, ttys, raw sockets) — the subset falls out naturally; it is not
something to hand-carve.

Two layers, not to be conflated. A capability **present on both** targets (console, time, random,
networking) is the *same stream surface wired to a different floor at runtime* (§6) — the retarget,
invisible to the program. A capability **absent** on a target (files, processes in a browser) is
what the **capability gate** catches at *compile time*, with a diagnostic. The gate only ever
concerns the second kind; the first is a silent floor swap. And it is strictly **per target** —
native Leviathan keeps everything; see §3.4.

### 3.2 The two genuine runtime exceptions (not "just libraries")

Two items are language-*runtime*-coupled, and they are the real reason to defer:

- **async/await suspension.** Today `await` **pumps-until-ready** on Leviathan's own single-threaded
  loop (info.md §13/§14). In a browser the host owns *the* event loop and forbids pumping it
  synchronously from inside a wasm call, so `await`/blocking reads must become **true suspension**
  (JSPI — JS Promise Integration; Asyncify; or a compiler CPS transform). info.md already defers
  this ("true stackful/CPS suspension… needed for the native backend") and §19 #5 (async coloring)
  is an **open decision**. Build the browser bridge before that settles and you rebuild it.
- **threads** (`request-threads.md`). Browser concurrency is Web Workers + `SharedArrayBuffer` — a
  message-passing model. §14's isolation-by-default maps *well* onto Workers (capture-by-copy,
  return-a-result), but the mapping is runtime work, not a library.

These two are why "wait" wins: both have unmade design decisions upstream of any wasm code.

### 3.3 Async and threads keep their surface; only the mechanism changes

Neither is a language change — the surface stays, the runtime beneath it swaps, exactly as the
floor does (§3.1).

- **async/await.** The surface — a function returns `Promise<T>`, `await p` unwraps it, **no
  coloring** (any function may await, §14) — is unchanged. Only *suspension* changes. Today `await`
  **pumps-until-ready** on Leviathan's own single-threaded loop (§13); a browser owns the loop and
  forbids pumping it from inside a wasm call, so `await` must yield to the host and resume.
  - **The no-coloring commitment (§14) points at *stackful* suspension.** Stackless/CPS async
    (C#/Rust/JS) splits each async function into a state machine and therefore *needs* coloring to
    know which functions to split — the exact contagion §14 rejected. Stackful suspension
    (fibers/green-threads) suspends the whole stack, so *any* function can await with no per-function
    transform. That is the model Leviathan already chose.
  - **Recommendation: stackful, realized per target — and on wasm-browser that primitive is JSPI.**
    JSPI (JavaScript Promise Integration) is a WebAssembly **engine feature** — a standards-track
    proposal implemented inside the VM (V8/etc.), **not a library you bundle**; from JS you touch
    only a thin wrapper (`WebAssembly.Suspending`/`promising`), and the stack switch itself is the
    engine's. It suspends the entire wasm stack when a suspending import returns a JS promise and
    resumes it on resolve — browser-native stackful suspension, near-zero cost on the non-suspending
    path, composing directly with the promises `fetch`/DOM APIs already return. Being an engine
    feature, its only cost is a *platform-support* dependency (why Asyncify is the fallback). On the
    native backend the same model is stackful coroutines / a switchable stack (the "true suspension"
    info.md already flags as needed). One *model* (stackful), two realizations — because
    stack-switching is inherently target-specific.
  - **Asyncify is the bring-up fallback only** — a Binaryen/Emscripten **build-time tool** that
    delivers stackful suspension via a whole-program binary rewrite needing no engine support, but at
    a real code-size and speed cost (the instrumentation ships in the module) plus an Emscripten
    dependency, so it is a crutch (same category as emit-C++→Emscripten, §5), not the shipping path —
    useful while JSPI support settles across engines.
  - **A stackless CPS transform is *not* recommended for Leviathan** — it reintroduces
    coloring-shaped whole-program transformation, fighting the §14 decision.
  - §19 #5 (the async-coloring *surface* decision) should settle first, so the model is built once.

- **threads.** The surface — `spawn`/high-tier promises, isolation-by-default (§14) — is unchanged.
  OS threads become **Web Workers** (each its own wasm instance, message-passing); the gated
  shared-mutable path becomes **SharedArrayBuffer + Atomics**. §14's safety model maps almost 1:1:
  "a worker captures its inputs by copy or ownership… runs… returns a result" *is* the
  Worker/`postMessage` model; "immutable shares freely / shared-mutable is the gated exception" *is*
  SAB. The one casualty is the low-tier raw clone-style `fork` with a shared address space — but §14
  already gates that as Unix-specific, so a browser simply omits that gate (a subset). The high tier
  — what people actually use — maps cleanly.

### 3.4 What each target loses, keeps, and gains — the losses are the *browser* target's, not the language's

**This is strictly per-target. Native Leviathan (AOT/ELF/interpreter) loses nothing** — files,
processes, raw sockets, argv/env, OS threads all remain. The "Lost" column below is the
*wasm-browser* target's smaller floor only, the same way Rust's `wasm32` target has no `std::fs`
while Rust-native keeps it. Same language, same source for the portable parts; a target only
narrows which platform capabilities are in scope.

| | capabilities (wasm-browser target) |
|---|---|
| **Lost** (compile-time gated on *this target*, §3.1) | filesystem (`File`, dirs), process spawning (`sysSpawn`/`Process`), raw TCP/UDP + `sysResolve` DNS, argv/env, tty/`isatty`, synchronous blocking reads, raw OS threads / shared-address `fork` |
| **Kept** (present; floor retargeted at runtime, §6) | the **entire language + every pure library** (JSON, DateTime, encoding, digests, collections, strings, math — target-independent); console; time; randomness; the event loop; async/await + high-tier threads (§3.3); networking **reshaped** — HTTP over `fetch`, sockets over `WebSocket`, as stream endpoints (§13) |
| **Gained** (new, via the §7 bridge) | the DOM, `fetch`, `WebSocket`, WebCrypto, Canvas/WebGL, storage (localStorage/IndexedDB/OPFS) |

The losses are exactly the OS-privileged capabilities meaningless inside a browser sandbox —
catching them at compile time beats a runtime crash. You lose *raw* sockets, not *networking*.

### 3.5 The same seam reaches bare metal — Leviathan is not foreclosed from OS work

The floor-retarget (§6) is a *general* seam, not a wasm trick. The `syscall`-based floor (§13,
info.md §17) is merely the **Linux-userspace implementation** of Layer 0 — it assumes a kernel
exists beneath it. The floor retargets in two directions:

- **Sideways → the browser** (this proposal): `syscall` → imported JS functions.
- **Downward → bare metal** (a future OS target): `syscall` → *nothing beneath* — the program **is**
  the bottom. I/O goes to a serial port / framebuffer via MMIO; the §15 allocator bootstraps from
  physical page frames instead of `mmap`; there is no kernel to call because you are writing it.

Leviathan is unusually well-positioned for this — more than most languages — because its philosophy
already reserves the pieces an OS needs: the **gate/bazooka-room** (§16: raw memory, raw pointers,
`unsafe` — a kernel lives almost entirely here), the **pure backend** that already emits its own
freestanding machine code with no libc/linker and talks to the CPU directly, and the
**self-hosting endgame** that bottoms out on unsafe code over the literal `syscall` instruction (a
kernel bottoms out one rung lower, on privileged instructions).

Genuinely missing today — all *below* the language, none *in* it: (1) an **inline-asm /
privileged-instruction intrinsic** (the ultimate gate) to emit `in`/`out`, `rdmsr`/`wrmsr`,
`lgdt`/`lidt`, `cli`/`sti`, `hlt` — §19 #12 ("raw pointers, decide last") taken to its limit;
(2) a **bare-metal boot/link target** (a multiboot/UEFI image, not a Linux-loaded ELF) with
**interrupt-handler codegen** (naked/interrupt calling conventions for IDT entries); (3) the floor's
**downward retarget** (framebuffer/serial I/O + a physical-memory/page-table allocator bootstrap).
Same *shape* as the wasm work — a backend column + a floor retarget — and, like wasm, **the language
core and IR are untouched.** So "does our syscall floor foreclose writing an OS?" is **no**: the
syscall floor is one *implementation* of the seam, and a kernel is another (the empty one). This is
also why §8's discipline pays double — every capability kept behind the floor abstraction is one
reimplementable on bare metal; every place library code reaches past it to assume "a Linux is here"
is a place OS work would break.

---

## 4. The build-away-from risk — four moving substrates

Building the target now stacks it on four things still in flux; each is a rebase you would pay
twice:

| substrate | state today | cost of building on it now |
|---|---|---|
| LLVM backend + runtime ABI | just reached parity (Gate G1); `liblvrt.a` **not landed** — `LlvmGen.hpp` still links the temporary `tests/support/llvm_stub_runtime.c` | target an unfinished runtime ABI; rebase on every change |
| async model | pump-until-ready; true suspension unbuilt; §19 #5 open | rebuild the entire browser async bridge |
| stdlib packaging | monolithic `kPrelude` string; "ship stdlib as files" awaits an owner ruling | no per-target library seam exists — hack one, then redo it |
| metaprog rules | Phases 1–2 built; splices / `meta.*` reflection (Phase 3+) unbuilt (§16.5) | hand-write bindgen stubs you would later regenerate from rules |

Deferring past Gate E settles the two big ones (async, per-target packaging), turning the wasm
target into "add a backend column + a platform library + glue" — bounded and well-understood.

---

## 5. The wasm backend (three paths, in build order)

- **Primary: LLVM → `wasm32`.** The LLVM backend already emits objects via `TargetMachine` for
  arbitrary triples (`LlvmGen::emitObject(path, triple, optLevel)` — the triple is already a
  parameter) and hit full IR parity at Gate G1. `wasm32-unknown-unknown` / `wasm32-wasi` is just
  another triple. Consistent with the LLVM-primary portable pivot. The runtime `lvrt_*` functions
  need a wasm build (see §6); once `liblvrt.a` lands, this is a relink against a wasm-compiled
  runtime, since the ABI is shared.
- **Fast bring-up: emit-C++ → Emscripten.** `CGen.cpp` already emits freestanding C++; piping it
  through Emscripten yields a working browser build quickly, and **dissolves the original "no
  libc++" worry** because Emscripten supplies the shims. Cost: an Emscripten toolchain dependency
  and larger output. A proof-of-concept lane, not the shipping answer (it reintroduces exactly the
  dependency the portable pivot shed).
- **Self-host analog (later): our own wasm emitter.** Mirrors the pure-ELF backend's "own
  machine-code + ELF writer" — a `.wasm` module writer with no external toolchain. The
  philosophically-aligned endgame, not a first step. **Do not route this through `X64Gen.cpp`/
  `X64.hpp`** (frozen); it is a new emitter alongside the LLVM path.

**Recommendation:** LLVM → `wasm32` for the spike (§9) and for the real target; keep
emit-C++→Emscripten in reserve as a bring-up lane if the LLVM runtime ABI is late.

---

## 6. The runtime-floor retarget (the real runtime work)

§13 Layer 0 bottoms out on the `syscall` instruction. A browser has no syscalls. The retarget:

- **Syscalls → wasm imports.** `sysWrite`/`sysRecv`/`sysNow`/… become functions imported from the
  JS host (`env` module). Clean because §13 already funnels *all* I/O through a thin floor — you
  replace one floor implementation, not the streams above it. This is the §16 bazooka-room bottom
  shelf relocated from `syscall` to host-import.
- **Allocator ports nearly free.** The §15 ARC + per-frame arena + free-list allocator runs over a
  heap region; point it at a `WebAssembly.Memory`, and `mmap`-growth becomes `memory.grow`.
  Leviathan owns its allocator, so no host call is needed to allocate. The whole §15 memory story
  (retain/release, arena reset at return, COW-on-refcount) is target-independent and comes along.
- **The hard one — async in the browser.** `await`/blocking reads must become true suspension.
  **Recommended: JSPI** — browser-native *stackful* suspension, which is the model the §14
  no-coloring commitment already implies (full reasoning in §3.3); **Asyncify** is the works-today
  bring-up fallback; a stackless **CPS transform is not recommended** (it fights no-coloring). The
  same stackful model serves the native backend as fibers/coroutines, so this is the "true
  suspension" info.md already flags, scheduled earlier. Single largest line item, and the reason the
  target waits for the §19 #5 async decision.

---

## 7. The JS/DOM bridge (the "wasm-bindgen equivalent")

Mostly a **library + rules** deliverable, not a compiler subsystem:

- **Handle table for JS objects.** DOM nodes/events are opaque JS values held as `externref`
  (reference-types, widely shipped) or integer indices into a JS-side `heap[]`. A Leviathan
  `class DomNode { handle h; }` wraps one; DOM APIs (`getElementById`, `setAttribute`,
  `addEventListener`) are imported functions over handles.
- **One reflective marshaler, off tag + shape.** Numbers unwrap the tag; **strings** pass `ptr+len`
  and transcode **UTF-8↔UTF-16** at the boundary via `TextEncoder`/`TextDecoder` (Leviathan strings
  are UTF-8 handles, §10; this is where §9/§10's open encoding question lands); objects marshal by
  reading the shape's slot table through the `cls` pointer; **value structs** pass as a pointer the
  JS side reads as a `TypedArray` view (the zero-copy path, §2). Uniform representation → this is
  written once.
- **Closure trampoline for events.** Export `wasm_invoke_closure(closureHandle, eventHandle)`;
  register Leviathan closures in a table, hand JS the index, JS calls the trampoline per DOM event.
  Rides the existing closure-with-captures representation (`RuntimeValue.hpp` `struct Closure`).
- **Generate stubs with the §16.5 rules engine.** An `@extern("dom")` attribute on a declared DOM
  interface → generated import declarations + marshaling stubs (`Rules.cpp`). The bindgen is a
  rules/library artifact on top of existing metaprogramming, not a new compiler — but it wants
  Phase 3+ splices/reflection for the ergonomic version (§4).
- **DOM events as streams.** §13 already makes streams the system boundary (sockets/timers are
  stream endpoints; `EventEmitter` is Layer-2 fan-out). A DOM event source → a stream of events is
  idiomatic, not bolted on — the browser event loop replaces the native one under the same surface.

---

## 8. Standing discipline — keep the syscall floor portable (do now)

The single most valuable cheap-now move, aimed precisely: **do not restrict Track 08 from adding
the syscalls it needs.** argv/exit, time/random, dirs, non-blocking connect, DNS, `sysSpawn` are
essential for the CLI/server/package-manager story — Trident and the Atlantis backend cannot be
built without them. Instead, hold the discipline that is *already the house style* (§13, Track 09's
zero-native claim), which costs ~nothing:

1. **Every floor native stays in the enumerated §13 Layer-0 list**, each with a one-line capability
   tag. The floor is a fixed capability interface, not an ever-growing native grab-bag.
2. **Portable library code never calls a floor native directly** — it goes through the prelude
   wrappers (`env`, `Process`, `TcpStream`, …). Track 09 proves this scales (an entire track,
   zero new natives, everything in-language over the floor). That wrapper layer **is** the
   portability seam: the library surface is portable; the floor is what a target swaps.
3. **Each backend declares its covered subset and cleanly diagnoses gaps** — already the ELF-DNS
   pattern (`techdesign-08` F6). A new target implements its subset and diagnoses the rest.

Hold these three and a wasm backend never "builds away from" the floor — it implements the
browser-relevant handful and diagnoses the OS-only remainder. This is a guardrail for Track 08 (and
every future floor-touching track), not new work.

---

## 9. The near-term action — a de-risking spike (1–2 days)

One throwaway spike, and nothing more, until post-Gate-E:

- Retarget the existing LLVM backend to `wasm32` (`emitObject(path, "wasm32-unknown-unknown")` —
  the triple is already a parameter).
- Compile a **pure-computation** Leviathan program — objects, generics, `struct`s, arrays,
  closures, arithmetic; **no I/O, no async, no DOM** — and run it in `wasmtime` (or a browser via a
  trivial loader), linking a stub for whatever `lvrt_*` the pure-compute subset references
  (allocator over linear memory; the existing `llvm_stub_runtime.c` is the starting point).
- **Success = it runs and differential-matches the oracle** on a small corpus subset.

This empirically validates the §1/§2 claim — the uniform tagged-value representation compiles and
runs on wasm and needs no monomorphization — for near-zero cost, without touching the runtime
floor, async, or bindgen. A clean spike (expected) buys the confidence to schedule the real work
post-queue with the type-system question closed. A *failed* spike is a cheap, early signal that
surfaces before any real investment. Either way it is the correct next step; everything else waits.

---

## 10. Foreseeable problems & strategies

| # | problem | strategy |
|---|---|---|
| 1 | **Async cannot pump the browser loop** — the core mismatch (§3.2/§3.3/§6). | The target waits on the §19 #5 suspension decision. Recommended mechanism: **JSPI** (browser-native stackful suspension, fits the §14 no-coloring model, §3.3); Asyncify as the works-today bring-up fallback; a stackless CPS transform is not recommended. Do not ship a browser async bridge on the pump-until-ready model — it will be rebuilt. |
| 2 | **String encoding boundary** — Leviathan UTF-8 handles vs JS UTF-16 (§7). | Transcode at the marshaler with `TextEncoder`/`TextDecoder`; make the wasm target the forcing function that finally rules on §9/§10's open encoding question (what `subStr` counts). Pin round-trips in a corpus. |
| 3 | **Reference cycles never collected** (info.md §15/§19 #10) — a DOM node ↔ Leviathan closure handler is exactly the Timer→handler cycle shape. | The escaping tier's cycle asterisk is pre-existing, not wasm-specific. Follow the churn corpus discipline (bind before holding); flag `weak`-ref-vs-cycle-collector (§19 #10) as *more* pressing once DOM handler graphs exist. Do not let the bridge invent its own leak. |
| 4 | **Emscripten reintroduces the dependency the portable pivot shed.** | emit-C++→Emscripten is a *bring-up lane only* (§5), explicitly not the shipping path; the LLVM→wasm32 + own-runtime route is the portable answer. Keep the two lanes labeled; never let the Emscripten lane become load-bearing. |
| 5 | **Monolithic prelude has no per-target seam** (§4). | Blocked on the "ship stdlib as files" ruling (`techdesign-09` §0). The wasm target is a *consumer* of that refactor, not the place to hack a one-off seam — escalate if it is still unruled when the target is scheduled. |
| 6 | **Bindgen wants metaprog Phase 3+** for the ergonomic generated-stub story (§7). | Hand-writable stubs are possible earlier but are throwaway; sequence the ergonomic bridge after Phase 3 splices/reflection land, or accept known-throwaway hand stubs for a proof-of-concept and log them as such. |
| 7 | **Value-struct dense layout beyond plain structs is "next major work"** (info.md §11/§17) — the zero-copy path (§2) leans on it. | The reference-generic (boxed) path works without it; zero-copy typed sharing is an *optimization* that matures with the dense-layout work, not a blocker for a first DOM bridge. Ship boxed first, zero-copy when dense lands. |

---

## 11. Sequencing & effort

- **Now, against moving substrates (§4):** effective cost inflated ~1.5–2× by churn, and *blocked*
  on the unmade async-suspension decision — realistically a months-long thing that keeps rebasing.
  **Not recommended.**
- **The spike (§9):** 1–2 days, now, no dependencies. **Recommended.**
- **The discipline (§8):** zero marginal cost, now, standing. **Recommended.**
- **The real target, post-Gate-E:** a track-sized effort like the others (a backend column + a
  capability-gated platform library + marshaler + closure trampoline + rules-generated stubs),
  because it rides settled ground. Rough shape once scheduled: LLVM→wasm32 backend + floor imports
  (small–medium) → async suspension integration (medium–large, the pacing item) → the DOM bridge
  (medium, rules-assisted) → a "hello DOM" then a real Atlantis-client demo as the gate.

Gate to schedule the real work: **Gate E complete** (Track 09 landed) **and** the §19 #5 async
decision made **and** the per-target stdlib ruling made. Until all three, only the spike and the
discipline proceed.

---

## 12. STOP conditions

- The spike (§9) reveals the LLVM backend cannot emit working `wasm32` without runtime-ABI work
  that is really Track B's — coordinate with the portable-backend owner, do not fork the runtime.
- Scheduling the real target while the §19 #5 async model is still open — that decision is upstream
  and above this proposal's pay grade (info.md §14/§19); escalate, do not improvise a suspension
  model here.
- Any temptation to fork the *language* into a browser subset (§3.1) — the subset is
  platform-capability, expressed through the existing engine-coverage matrix, never a dialect.
- Any temptation to make the Emscripten lane (§5) the shipping path — it is bring-up only.

---

## 13. Reference-doc duty (when built)

info.md: a new §20 (or §13 addendum) on the wasm target — the capability-subset framing, the
floor-as-imports retarget, the async-suspension requirement; §19 additions (per-target stdlib
ruling wanted; async suspension model as a wasm blocker). `docs/reference.md`: a target/backend
matrix row for `wasm32`; the `@extern` binding surface once the bridge exists. Atlantis
`designs/atlantis/techdesign-09-views.md` (the client-facing track) is the *consumer* — cross-link
when the bridge is real.

---

## 14. Implementation log

### W-M0 — the de-risking spike (2026-07-17): **PASS**

LLVM→wasm32 ran a pure-computation corpus subset and differential-matched the oracle,
byte-for-byte, with **zero compiler edits and zero runtime-ABI changes**. The §1/§2 claim
holds empirically: the uniform tagged 16-byte `LvValue` compiles and runs on wasm32 and
needs no monomorphization.

- **Triple:** `wasm32-wasi`. `wasm32-unknown-unknown` also *emits* an object fine (the
  first probe used it), but the runtime C (`lv_runtime.c` → `snprintf`/`strtod`/`memcpy`/
  `fmod`) needs a libc, so the spike settled on `wasm32-wasi` + wasi-libc for the runtime
  and linked as a WASI command module (crt1 → `_start` → `lv_entry.c`'s `main`). No
  compiler change was needed for either triple — `emitObject(path, triple, optLevel)` took
  it as a parameter, exactly as §5/§9 predicted.
- **Toolchain:** clang 18.1.3, wasm-ld (LLD) 18.1.3, wasmtime 46.0.1, wasi-libc
  `0.0~git20230113` (Ubuntu noble/universe), compiler-rt builtins
  `libclang_rt.builtins-wasm32-wasi-24.0` (wasi-sdk-24 release — needed for `__multi3`,
  which wasi-libc's `strtod`/`strtoll` pull in). Compiler under test: master @ `e6bec34`.
- **Harness:** `tests/spike-wasm/run.sh` + throwaway `tests/spike-wasm/lv_plat_spike.c`
  (the spike floor — write/map/exit real over WASI, everything else `abort()`; plus dead
  stubs for the task scheduler and TLS provider, since `lv_task.c`/`lv_tls_none.c` aren't
  wasm-buildable and `lv_tasks_enabled()→0` sends `lv_entry.c` down the pump path). It
  compiles `lv_runtime.c`+`lv_loop.c`+`lv_entry.c` for wasm once, then per corpus file:
  `--native-obj --target wasm32-wasi` → `wasm-ld` → `wasmtime run` → `diff` vs `--ir`.
  Not wired into CTest (spike §3).
- **Corpus (29/29 matched):** math, structs, structs_array, generics, collections, loops,
  match, match_nested, classes, oo, floats, literals, const, sieve, strcmp, bitops,
  optional, exceptions, cow, maps_set, namespaces, qualified, use, seq, iterator, readonly,
  class_dispatch, method_refs, named_defaults — i.e. objects, generics, boxed structs,
  arrays, maps, closures, strings, arithmetic, control flow, exceptions, print. Byte-
  identical stdout on every one. (`math.ext` alone already exercises int64 min, big-hex
  radix formatting, transcendentals, and caught exceptions.)
- **Corroborating signal — the capability gate already works.** Two candidates I first
  mislabeled pure-compute, `using.ext` (`File`) and `generic_iface.lev` (`Channel`), were
  rejected by the compiler at emit time with clean `"... is not available on this target"`
  diagnostics — the hard-03 per-target subset (§3.1) doing its job, not a spike failure.
- **Known-inert hazards (spike §4) — verified, none bit:** the TLS-model pin
  (`LlvmGen.cpp` hard-01) produced a linkable object; wasm-ld raised no TLS-reloc
  complaint. The `[heap]` escaping-tier meter prints to **stderr**, so it never perturbs
  the stdout differential (the one thing to know when reading a raw `wasmtime` run).
- **STOP conditions (§12 / doc-00 §8): none triggered.** No LLVM-backend edit, no
  runtime-ABI change, no runtime fork. Note the spike ran *after* the W-M1 HARD packets
  (hard-01/02/03/05) had already landed on master, so it also serves as a regression check
  that those edits keep pure-compute wasm green.

**Verdict:** the type-system question is closed in the affirmative — proceed to W-M1
scheduling as written. The spike deliverable lives on a throwaway branch (`tests/spike-wasm/`),
pushed, not merged; it changes no compiler or runtime source. The only real toolchain gap a
CI lane would need to fill is a wasm sysroot + compiler-rt builtins (both fetched here without
root), which is the doc-02/03 `build-triple.sh --target wasm32` concern, not the spike's.
