# Track W — HARD 04: Await routing through `lvrt_await` (CONTINGENT)

**[HARD]** — would edit the `Await` emission path in `src/LlvmGen.cpp`. **Status:**
CONTINGENT — W-M2's design goal is **zero LlvmGen edits**; this doc activates only if its
trigger fires. **Context:** `techdesign-04-async-jspi.md` §4.

> **Audit 2026-07-16 (with the W-M1 HARD packets): trigger does NOT fire — NOT NEEDED.**
> `Op::Await` emits exactly ONE runtime call, `lvrt_await` (`LlvmGen.cpp`, `case
> Op::Await`), with an in-code HISTORY note explicitly forbidding restoring the inline
> pump/park BB graph. No path bypasses the runtime call. Re-confirm cheaply at W-M2 start
> if intervening changes touch the Await case.

## Trigger

Audit at W-M2 start: post-LA-30, the LLVM leg's `Await` should bottom out in the runtime
call `lvrt_await` (`lv_runtime.c:2985`), letting all wasm behavior live in
`lv_task_wasm.c` + JS. If instead any path still emits an inline pump/park BB graph
(the pre-LA-30 shape was `LlvmGen.cpp:2811-2855`) that bypasses `lvrt_await`, this doc
fires. If the audit shows clean routing, close this doc as NOT NEEDED in the W-M2 log.

## The edit (if triggered)

Route the bypassing path through `lvrt_await` — the minimal change that makes the runtime
call the single suspension seam on the LLVM leg. No new ABI symbols; no behavior change on
native (the runtime call performs the same park the inline graph did).

## Constraints

- Minimal diff; portable-backend owner review before merge.
- Native semantics byte-identical — this is a mechanical seam consolidation, not a
  behavior change.

## Verification

- Four-lane differential on the full suite **plus** the async corpus specifically
  (completion order, throw-across-await, LA-30 doc-5 C2 failure-routing pins).
- Then the wasm async lane (JSPI host) per `techdesign-04-async-jspi.md` §6.
