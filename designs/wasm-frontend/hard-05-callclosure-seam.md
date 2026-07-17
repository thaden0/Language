# Track W — HARD 05: `lvrt_callclosure` seam (CONTINGENT)

**[HARD]** — would add one `lvrt_*` symbol to the emitted-against ABI surface in
`runtime/lv_runtime.c`. **Status:** CONTINGENT — verify the trigger **before scheduling
W-M3**. **Context:** `techdesign-05-dom-bridge.md` §4.

> **Audit 2026-07-16 (with the W-M1 HARD packets): trigger does NOT fire — NOT NEEDED.**
> The design bet holds: closure invocation is NOT LlvmGen-only. The runtime already
> invokes `LV_CLO` values C-side through the registered trampoline (`lv_rt_dispatch_fn`):
> `runtime/lv_loop.c` ("invokes `clo(argVal)` through the registered dispatch trampoline",
> fnIndex read from the closure body's word0, closure passed as its own args[0]), and
> `runtime/lv_thread.c` (`lv_worker_body_task`) does the same for thread bodies. The DOM
> trampoline can reuse that exact pattern — pending-throw flag included, since dispatch
> surfaces throws via the flag already.

## Trigger

The closure trampoline (`lv_invoke_closure`) needs a C-callable way to invoke a Leviathan
closure (`LV_CLO {fnIndex, captureHead}`, `lv_abi.h:157-166`). Design bet: the seam already
exists — the loop dispatches closure callbacks today, and `lvrt_await`'s continuations run
them. If audit confirms a reusable runtime path, this doc closes as NOT NEEDED. If closure
invocation exists **only** as LlvmGen-emitted `CallValue` sequences with no C entry point,
this doc fires.

## The edit (if triggered)

Add `lvrt_callclosure(LvValue* out, const LvValue* clo, const LvValue* args, int nargs)`
to `lv_runtime.c`, mirroring exactly what generated `CallValue` code does (dispatch through
the registered `LvDispatchFn` / fn table by `fnIndex`, captures via `captureHead`). All
targets get it (one source); only the wasm glue calls it in v1.

## Constraints

- ABI addition → portable-backend owner review; the symbol's contract documented in
  `lv_abi.h` alongside the closure body layout.
- No LlvmGen edits — generated code keeps its own `CallValue` emission; this is a parallel
  C entry, not a replacement.
- Must respect the pending-throw flag model (a throw inside the closure surfaces as the
  flag, checked by the trampoline caller — no unwinding across the boundary).

## Verification

- Four-lane differential on the full suite (no generated-code change expected at all).
- A wasm-lane pin: JS → trampoline → closure with captures → mutates state → JS observes;
  plus a throwing closure surfacing cleanly.
