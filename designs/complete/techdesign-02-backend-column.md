# Track W — doc 2 of 6: the backend column (W-M1, part 1)

**Status:** LANDED (2026-07-17) — see §6/§7 for the as-built shape (wasi-libc-backed
archive, `lv_entry_main`/`export_name` finding, headless-Node corpus lane). **Depends
on:** W-M0 spike verdict (doc 01).
**HARD content:** extracted to their own minimal packets — `hard-01-tls-pin.md`,
`hard-02-link-lane.md`, `hard-03-capability-gate.md` (one packet = one commit, four-lane
differential green after each). §3–§5 below are context stubs only.

## 1. Scope

Turn "the triple is already a parameter" into a real, supported column: `--target
wasm32-unknown-unknown` produces a linked `.wasm` via the normal CLI, against a real
`liblvrt-wasm32.a`, with unsupported capabilities cleanly gated. No async (doc 04), no DOM
(doc 05). Exit criteria = W-M1 gate in doc 00 §4, jointly with doc 03's floor.

## 2. Non-goals

Own `.wasm` emitter (self-host analog — later, never via the frozen `X64Gen`);
emit-C++→Emscripten lane (bring-up reserve only, not built unless the LLVM lane stalls);
threads/SAB; columnar arrays (`IrModule::columnar` stays off for wasm v1).

## 3. TLS-model pin — **[HARD], see `hard-01-tls-pin.md`**

`LlvmGen.cpp:3343-3355` has no wasm case (silent GeneralDynamic fall-through). The packet
adds one explicit `Triple::isWasm()` branch pinning LocalExec for the single-threaded v1.
Revisited only when the Workers leg opens (doc 04 §7).

## 4. Link lane — **[HARD], see `hard-02-link-lane.md`**

`main.cpp:600-673` probes only native cross linkers. The packet adds an early, parallel
wasm lane (`wasm-ld`, the §6 archive, `--import-undefined` against doc 03 §2's import
module) without rewriting the native probe.

## 5. The capability gate — **[HARD], see `hard-03-capability-gate.md`**

Copies the in-file Windows precedent (`LlvmGen.cpp:2542,2565`) with one forced refinement:
because the whole prelude lowers into every module (`Resolver.cpp:5193-5201`), the gate is
**two-tier** — compile-time diagnostic when a gated native is *reachable from user code*
(ELF-DNS pattern), and a `lvrt_unsupported` trap stub for unreachable prelude bodies so
builds don't brick. The gated set (= doc 00 §3 "Lost" row) lives as one table in the
packet; other docs reference the gate as "doc 02 §5".

## 6. `liblvrt-wasm32.a` — the runtime archive (not HARD)

Extend `runtime/build-triple.sh` (branches only windows-vs-POSIX today, `:72-76`, and
always adds the `.S` context files) with a `wasm32*` branch:

- compiler: `clang --target=wasm32-unknown-unknown -nostdlib -O2`;
- sources: `lv_runtime.c` + `lv_plat_wasm.c` (doc 03) + `lv_loop.c` + `lv_entry.c` +
  `lv_task_wasm.c` (doc 04; until it exists, a `lv_task_stub.c` where `lv_tasks_enabled()`
  returns 0 and park aborts via `lvrt_unsupported("await")` — W-M1 corpus has no await);
- **no `.S` files** — there is no register context to hand-switch on wasm; suspension is
  the engine's (doc 04);
- archive: `runtime/wasm32-unknown-unknown/liblvrt.a`, found by the same convention the
  aarch64 cross archive uses.

If `lv_runtime.c` or `lv_loop.c` fail to compile under `-nostdlib` wasm32 (stray libc
include, host-only assumption), fix belongs to the *runtime*, coordinated with the
portable-backend owner — the spike (doc 01) should have surfaced any such case already.

**As-built (2026-07-17) — three findings this section didn't anticipate:**

1. **`-nostdlib` was never viable.** `lv_runtime.c` alone calls `malloc`/`free`/`calloc`/
   `realloc`, the whole `mem*`/`str*` family, and `snprintf` (~90 call sites) — none of
   which a bare `-nostdlib` wasm32 build supplies, and reimplementing them isn't in scope.
   The archive compiles against **wasi-libc** (`clang --target=wasm32-wasi --sysroot=...`)
   for that C-level surface instead; the browser-vs-WASI split lives entirely in
   `lv_plat_wasm.c`'s imports (doc 03), never in which libc supplies `memcpy`. Toolchain
   env vars: `LVRT_WASI_SYSROOT` (default `/usr`, matching the `wasi-libc` apt package's
   own layout), `LVRT_WASI_BUILTINS` (default resolved via `clang -print-resource-dir`,
   matching `libclang-rt-<ver>-dev-wasm32`). Because `main.cpp`'s wasm-ld invocation links
   only the generated object plus one archive (no `-l` flags on wasm), the archive itself
   folds in the needed wasi-libc/compiler-rt objects — see build-triple.sh's wasm32 branch
   for the extraction discipline that needed (wasi-libc's `libc.a` carries more than one
   member with the same basename, e.g. two distinct `errno.o`s — a blanket `ar x`
   silently drops one; `ar xN <count>` per (name, occurrence) pair fixes it).
2. **`lv_thread.c`/`lv_proc.c` are dropped, not ported.** Both need `pthread.h`/`fork(2)`,
   neither in wasi-libc's sysroot — but nothing in a wasm-linked binary ever calls into
   them: the capability gate (hard-03) redirects every reachable spawn/thread native
   before codegen emits a real call, so their object code is unreferenced weight a wasm
   archive doesn't need to carry.
3. **`main.cpp`'s `--export=main` (hard-02) couldn't resolve.** Clang mangles a wasm
   `main(argc, argv)`'s *linker* symbol to `__main_argc_argv`; `--export=main` looks for a
   linker symbol literally named `main` and errors. Fixed by renaming the wasm leg of
   `lv_entry.c`'s entry point to `lv_entry_main` (a name this file controls, not an
   implementation-detail mangling) with `__attribute__((export_name("main")))`, and
   changing the wasm-ld flag to `--export=lv_entry_main` — the WASM EXPORT table entry is
   still spelled `"main"` either way, so JS hosts and `--invoke main` drivers are
   unaffected.

## 7. Verification (W-M1 backend half)

- Four-lane differential on the full pre-existing suite after **each** HARD commit.
- New corpus lane: `tests/run_wasm.sh <leviathan> <file-or-dir> [file-or-dir ...]` —
  compile with `--build-native out.wasm --target wasm32-unknown-unknown`, run headlessly,
  diff stdout (not stderr — the LLVM backend's escaping-tier meter has no `--ir`
  counterpart) against `--ir`. Wired as CTest's `corpus_wasm` against the same 28-file
  pure-compute+console cluster the W-M0 spike validated
  (`tests/spike-wasm/run.sh`'s `default_corpus`) plus `tests/corpus/wasm/`'s gated + trap
  pins and the `time_random` shape pin (doc 03 §5's as-built note) — 32 files, all green
  as of 2026-07-17. **As-built:** "run under wasmtime (`--invoke` the
  entry)" doesn't work as literally written — the wasmtime CLI only auto-supplies WASI
  imports, and this floor's real output path is the `"lv"` module (doc 03 §1), which
  nothing but our own host code can satisfy. `tests/wasm_node_run.mjs` — Node's native
  `WebAssembly` plus `runtime/lv_host.js`'s shared `makeImports()` — is the headless host
  instead (doc 03 §3's "node/wasmtime shim", concretely); it and `run_wasm.sh` both build
  their imports from that one shared function. Browser check is doc 03's (needs the
  floor's JS host) — spot-checked in real headless Chrome via `lv_host_page.html` +
  `--dump-dom`, byte-identical against `--ir`.
- Gate-diagnostic pins: a `tests/corpus/wasm/gated_file.lev` (uses `File`) asserting the
  compile-time diagnostic text — the checker-test style (`ERROR_HAS`) or a run_wasm.sh
  negative mode, whichever is cheaper. Landed as the run_wasm.sh negative mode.
