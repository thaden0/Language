# Track W — HARD 02: wasm-ld link lane in the driver

**[HARD]** — edits the LLVM link driving in `src/main.cpp:600-673`. **Status:** scheduled
(W-M1). **Context:** `techdesign-02-backend-column.md` §4. **Depends on:** hard-01 in the
same milestone; the archive from doc 02 §6.

## The edit

The driver probes native/gnu/clang cross linkers; a wasm triple currently falls into that
probe. Add an early `if (isWasmTriple)` lane — parallel to, not a rewrite of, the probe:

- key: normalized triple starts `wasm32`;
- linker: probe `wasm-ld`, `wasm-ld-18`; absent → clean diagnostic naming the package;
- inputs: emitted object + `runtime/wasm32-unknown-unknown/liblvrt.a` (honor the existing
  `--runtime <path>` override);
- flags: `--no-entry --export=<entry>` (the same entry symbol `lv_entry.c` exposes on
  native — reuse, don't invent) + `--import-undefined` (imports resolve from the JS host,
  `techdesign-03-floor-wasm.md` §2);
- output: `<out>.wasm`.

## Constraints

- The native probe path must be byte-identical in behavior for non-wasm triples.
- No new CLI flags; `--target`/`--runtime`/`--build-native` semantics unchanged.

## Verification

- Four-lane differential on the full suite (native lanes untouched).
- `tests/run_wasm.sh` end-to-end: `--build-native out.wasm --target
  wasm32-unknown-unknown` → runs under `wasmtime`, stdout matches `--ir`.
- Missing-`wasm-ld` diagnostic pinned once (negative test).
