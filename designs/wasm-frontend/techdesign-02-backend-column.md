# Track W — doc 2 of 6: the backend column (W-M1, part 1)

**Status:** PROPOSED. **Depends on:** W-M0 spike verdict (doc 01).
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

## 7. Verification (W-M1 backend half)

- Four-lane differential on the full pre-existing suite after **each** HARD commit.
- New corpus lane: `tests/run_wasm.sh <leviathan> <file-or-dir>` — compile with
  `--build-native out.wasm --target wasm32-unknown-unknown`, run under `wasmtime`
  (`--invoke` the entry), diff stdout against `--ir`. Wire the pure-compute + console
  clusters. Browser check is doc 03's (needs the floor's JS host).
- Gate-diagnostic pins: a `tests/corpus/wasm/gated_file.lev` (uses `File`) asserting the
  compile-time diagnostic text — the checker-test style (`ERROR_HAS`) or a run_wasm.sh
  negative mode, whichever is cheaper.
