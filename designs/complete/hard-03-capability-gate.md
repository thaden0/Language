# Track W — HARD 03: the capability gate at native dispatch

**[HARD]** — edits `src/LlvmGen.cpp` (CallNativeFn dispatch, `:2457ff`) and adds one
`lvrt_*` symbol to the emitted-against ABI surface. **Status:** scheduled (W-M1).
**Context:** `techdesign-02-backend-column.md` §5. **Depends on:** hard-01.

## Why two tiers (the grounded constraint)

The whole prelude lowers into every IrModule (`Resolver.cpp:5193-5201` concatenates all
segments), so prelude bodies referencing gated natives (File wrappers, `sysSpawn`, DNS…)
are present in every wasm compile even when unreachable. A blanket compile-time `fail()`
— the shape of the Windows precedents at `LlvmGen.cpp:2542,2565` — would brick every build.

## The edit

1. **Reachable-from-user → compile-time diagnostic.** Pre-emission, walk the IR call graph
   from the user program's roots (everything not from the `<prelude>` file,
   `Resolver.cpp:5194`). A gated native reachable from user code diagnoses:
   `wasm-browser: 'File' is not available on this target (no filesystem in a browser)` —
   named after the capability wrapper; include the reaching path if cheap. (ELF-DNS
   pattern, `techdesign-08-system-natives.md` §6.)
2. **Unreachable prelude bodies → trap stub.** Gated natives in functions emitted anyway
   compile to a call to the new helper `lvrt_unsupported(const char* what)`: prints
   `<what>: not on the wasm-browser target` to fd 2 via `lv_plat_write`, then
   `lv_plat_exit(134)`. Added to `lv_runtime.c` for **all** targets (a no-op-until-called
   helper) so the archive stays one source.

Gated set v1 (= overview §3 "Lost" row): file/dir natives, process spawn, raw TCP/UDP +
resolve, argv/env, tty/termios, signals, `spawn`/threads (until the Workers leg). Keep it
as **one table** in `LlvmGen.cpp` next to the dispatch, commented as the wasm column's
declared subset.

## Constraints

- Gate logic active **only** when the target triple is wasm; native emission paths
  byte-identical.
- `lvrt_unsupported` is the only ABI addition; portable-backend owner reviews it.
- No checker/resolver edits — the gate lives entirely at the backend, per the
  engine-coverage policy (`techdesign-00-overview.md:187-190`).

## Verification

- Four-lane differential on the full suite (native lanes untouched).
- `tests/corpus/wasm/gated_file.lev` pins the compile-time diagnostic text.
- One contrived pin reaches `lvrt_unsupported` at runtime; asserts message + exit 134.
- Full wasm corpus still compiles (proves tier 2 keeps prelude bodies buildable).
