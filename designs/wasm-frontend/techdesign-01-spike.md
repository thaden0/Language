# Track W — doc 1 of 6: the de-risking spike (W-M0)

**Status:** ready to run now; throwaway branch; commits to nothing.
**Depends on:** nothing — the current LLVM backend and landed `liblvrt.a` suffice.
**HARD content:** none by design — the spike makes **zero compiler edits**. If it cannot
complete without editing `src/LlvmGen.*`, that is itself the spike's answer (§4, STOP).

## 1. Purpose

Empirically close the type-system question: the uniform tagged 16-byte `LvValue`
representation (`lv_abi.h:36`) compiles and runs on `wasm32` and needs no monomorphization.
Success buys scheduling confidence for W-M1+; failure is a cheap early signal routed to the
portable-backend owner. Either way, done in 1–2 days.

## 2. Scope pins

- **In:** objects, generics, `struct`s (boxed path), arrays, maps, closures, strings,
  arithmetic, control flow. Pure computation + `print` only.
- **Out:** async/await, timers, threads, any `sys*` beyond write, DOM, JSPI, columnar
  layout (`IrModule::columnar` stays off), the link driver, `build-triple.sh`.

## 3. Runbook

1. **Emit the object** (works today, zero edits — `LlvmGen.cpp:3309-3333` initializes all
   targets for any non-empty triple):

   ```sh
   build/leviathan --native-obj /tmp/prog.o --target wasm32-unknown-unknown prog.lev
   ```

   If `wasm32-unknown-unknown` trips over anything, try `wasm32-wasi` before concluding —
   record which triple worked.
2. **Build the runtime for wasm.** Compile the real runtime, not a stub:

   ```sh
   clang --target=wasm32-unknown-unknown -nostdlib -O2 -c runtime/lv_runtime.c -o /tmp/lvrt.o \
         -D... (whatever config the file needs; start from the flags in CMake's LANG_LVRT_SOURCES target)
   ```

   Expect `lv_runtime.c` to reference `lv_plat_*` symbols and (transitively) task/loop
   symbols. For the spike, satisfy them with a **throwaway** `lv_plat_spike.c`: `lv_plat_map`
   over a static arena or `memory.grow` intrinsic (`__builtin_wasm_memory_grow`),
   `lv_plat_write` → one imported JS/WASI function, `lv_plat_exit` → imported abort,
   everything else `abort()`. Stub `lv_tasks_enabled() → 0` territory: the spike corpus has
   no `await`, so the park path never runs. This file is spike-only garbage; doc 03's
   `lv_plat_wasm.c` is the real one and does NOT grow from this.
3. **Link and run:**

   ```sh
   wasm-ld /tmp/prog.o /tmp/lvrt.o /tmp/plat_spike.o -o /tmp/prog.wasm \
           --no-entry --export=lv_main --allow-undefined
   wasmtime run --invoke lv_main /tmp/prog.wasm    # or a 10-line browser loader
   ```

   (Exact entry symbol: whatever `lv_entry.c` exposes — check how the native link lane
   names the program entry; export that.)
4. **Differential-match.** Run a pure-compute corpus subset through the wasm module and the
   IR interpreter (`--ir`); assert byte-identical stdout. A `tests/spike-wasm/run.sh` that
   loops the chosen corpus files is enough — do not wire CTest.

## 4. Success / failure semantics

- **Success** = every chosen corpus file runs and matches. Record the exact triple, flags,
  clang/wasm-ld/wasmtime versions, and any surprises in doc 00's log; proceed to W-M1
  scheduling.
- **Failure** = emission or execution requires changes to `src/LlvmGen.*` or the `lvrt` ABI.
  **STOP condition 1**: write up exactly what broke (op, IR shape, symbol), hand it to the
  portable-backend owner, do not fork or patch the runtime in the spike branch.

Known-inert hazards, expected NOT to bite (verify, don't fix): the TLS-model pin has no
wasm case and falls to GeneralDynamic (`LlvmGen.cpp:3343-3355`) — single-threaded spike
should tolerate it; if the linker rejects TLS relocs, note it as W-M1 evidence and try
`-mattr=+bulk-memory` / flattening the offending flag at the clang step, still without
touching LlvmGen.

## 5. Deliverable

A short entry appended to the proposal's §14 implementation log
(`designs/requests/proposal-wasm-frontend.md`) + the spike branch pushed (not merged):
triple, toolchain versions, corpus list, match result, and the one-paragraph verdict.
