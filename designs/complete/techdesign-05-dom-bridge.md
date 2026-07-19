# Track W — doc 5 of 6: the JS/DOM bridge (W-M3, part 1)

**Status:** LANDED (2026-07-18) — see §9 for the as-built shape. **Depends on:** W-M2 (doc
04 — event handlers await; the bridge assumes suspension works).
**HARD content:** the design bet below did NOT hold — reaching a JS host import from a
hand-written prelude method needs a native seam, and every new native is a `CallNativeFn`
row in `LlvmGen.cpp` (a HARD edit). Landed as the minimal packet
**`hard-06-hostbridge-seam.md`** (three C entries, three rows, four-lane differential
green). §4's trampoline rides `hard-05-callclosure-seam.md` (LANDED), as designed.

> **Original bet (kept for the record):** "HARD content: none *by design bet* — §4's
> trampoline is runtime-side; its contingency is its own packet,
> `hard-05-callclosure-seam.md`. Everything else is JS + prelude + corpus." The trampoline
> half held; the marshaler/DOM-op half did not — see §9 item 1.

## 1. Deliverables

- The **reflective marshaler** in `lv_host.js` (§3) — written once, table-driven off tag +
  shape.
- The **handle table** for opaque JS values (§2).
- The **closure trampoline** for DOM events (§4).
- A hand-written v1 `Dom` prelude surface (`kPreludeWasm`-candidate text, but shipped as
  ordinary prelude code until the packaging ruling; doc 06 §2 replaces hand stubs with the
  rules bindgen).
- DOM events as streams + `fetch`/`WebSocket` endpoints (§6).

## 2. Handle table

DOM nodes/events/JS objects are opaque: JS keeps `heap = [undefined]`, hands wasm integer
indices; Leviathan wraps them (`class DomNode { handle h; }` — an `Int` slot, nothing
more). Free-list reuse on release; a `lv.drop(handle)` import wired to the wrapper's
release path so handles don't leak (see §5 for the cycle caveat). v1 uses integer indices,
not `externref` — indices keep the ABI plain-`i64` and debuggable; `externref` is a later
optimization with zero surface change.

## 3. The marshaler (normative — and the two footguns)

One routine, switch on tag (`lv_abi.h:38-53`), exactly the dossier §8.2 table:

- immediates (`LV_INT/BOOL/CHAR`) → payload; `LV_FLOAT` → reinterpret the payload bits as
  f64 (never a numeric cast, `lv_abi.h:58-64`); `LV_NONE/VOID` → null/undefined;
- `LV_STR` → `TextDecoder.decode(mem[payload+8 .. +8+len])`; **key off `len`, never NUL**
  (`lv_abi.h:91-93`); JS→wasm strings via `TextEncoder` into a fresh runtime string
  (allocate through an exported `lvrt_` string constructor, never raw memory);
- `LV_OBJ` → `classId = mem[payload]`, walk `LvClassInfo.slotNames[i]` at
  `payload + 16 + 16*i` (`lv_abi.h:95-107, 227-246`) → plain JS object; handle-wrapper
  classes (a `DomNode`) short-circuit to `heap[h]`;
- `LV_ARR` → v1 **boxed only**: array of `marshal(elem)`. DENSE/COLUMNAR zero-copy
  `TypedArray` views are deferred until the dense-layout work matures (dossier risk #7;
  `IrModule::columnar` stays off) — ship boxed first;
- `LV_CLO` → closure-table index (§4); `LV_MAP` → object over entries; `LV_BLOCK` → a
  `Uint8Array` view (copy on hand-off, don't alias linear memory into user JS).

**Footgun rule (binding, from doc 00 §6):** never touch `payload-16` (the `LvHeader`)
without the value-class / region-registry gate — literal strings and inline dense records
carry **no header** (`lv_abi.h:61-63, 85-93`). The JS marshaler never reads headers at all
(it has no business with `rc`); any *runtime-side* helper added for the bridge must gate.

The class-info table reaches JS via the existing `lvrt_register(classes, nclasses, dispatch)`
seam (`lv_abi.h:262`): export a tiny runtime getter (`lvrt_classinfo(classId)` returning
the struct pointer) and read `slotNames` through linear memory. Runtime-side addition, one
accessor, no codegen.

## 4. The closure trampoline

Rides the frozen closure body `{ fnIndex, captureHead }` (`lv_abi.h:157-166`). Runtime-side
export:

```
lv_invoke_closure(cloPtr /* retained LV_CLO payload */, argTag, argPay)
```

implemented in `lv_task_wasm.c` (it must run as a dispatched task — wrap the export
`promising`, doc 04 §3.2, so handlers may await): build the one-arg call and invoke through
the registered dispatch fn / the runtime's existing call-a-closure path (the same machinery
`lvrt_await`'s continuations and the loop's callback dispatch already use). JS side:
`addEventListener("click", cb)` → glue retains the closure (exported `lvrt_retain`),
stores it, installs a real DOM listener calling
`lv_invoke_closure(clo, LV_OBJ, wrap(eventHandle))`. Captures are already in linear memory
via `captureHead` — nothing extra crosses.

**Contingency:** if the runtime has no C-callable invoke-closure seam (closure calls exist
only as LlvmGen-emitted `CallValue` sequences), the fix is the contingent HARD packet
**`hard-05-callclosure-seam.md`**. Design bet: the seam exists (the loop dispatches
closures today); verify before scheduling W-M3 and close the packet as NOT NEEDED if it
holds.

## 5. Lifetime & cycles (the standing hazard, made explicit)

A DOM node ↔ Leviathan closure handler is exactly the Timer→handler cycle shape the
escaping-tier ARC does not collect (pre-existing, not wasm-specific — dossier §10). Rules:

- the glue's closure table is a **root** — entries retained on register, released on
  `removeEventListener`/node drop; symmetric, audited in one place;
- the `Dom` prelude follows the churn-corpus discipline (bind before holding); handler
  fields that point back at nodes should use the existing `weak` slot substrate
  (`LV_WEAK_PROXY`, `Symbols.hpp:52` `isWeak`) in the library surface where a cycle is
  structural;
- the bridge must not invent its own leak: one pin in the corpus that registers/unregisters
  N handlers and asserts the closure table returns to size 0.
- escalate visibility: once DOM handler graphs exist, the §19 #10 weak-vs-cycle-collector
  decision gets more pressing — note it in the W-M3 log; do not solve it here.

## 6. Events and I/O as streams

§13 already makes streams the system boundary; the browser slots in under the same surface:

- **DOM events** — `node.events("click")` returns a stream endpoint; the glue installs one
  listener per subscription feeding the trampoline; `EventEmitter` fan-out stays Layer-2,
  in-language, untouched.
- **HTTP** — the prelude `HttpClient` surface gets a wasm floor lane over a `Suspending`
  `lv.fetch(...)` import (doc 04 §3.5): request out via the marshaler, response body in as
  a stream endpoint (chunks from a `ReadableStream` reader, each arrival resuming an
  awaiting read). Raw sockets stay gated; **networking is reshaped, not lost.**
- **WebSocket** — same pattern, message events as a stream endpoint; defer to first real
  consumer (Atlantis demo) if W-M3 runs long — `fetch` is the gate-critical one.

## 7. Strings — the forcing function

The wasm boundary transcodes UTF-8↔UTF-16 at the marshaler. This is where the open
info.md §9/§10 encoding question (what `subStr` counts) stops being deferrable: file the
ruling request when W-M3 opens, and pin round-trip corpus (ASCII, multibyte, astral,
lone-surrogate-from-JS → replacement policy) whatever the ruling says. The marshaler
itself is encoding-ruling-neutral (it moves bytes + lens); only the pins care.

## 8. Verification (W-M3 gate, joint with doc 06)

"hello DOM": a Leviathan program that builds a DOM subtree, sets attributes, installs a
click handler that awaits a timer then mutates text — asserted via a headless-browser
harness (the `lv_host_page.html` loader + playwright-or-equivalent, whatever is already on
the box; keep it out of default CTest, one script `tests/run_wasm_dom.sh`). Marshaler
round-trip pins for every tag; the §5 leak pin; the §7 string pins.

## 9. As-built (2026-07-18) — what shipped and where it diverged

1. **The "no HARD" bet was falsified → `hard-06-hostbridge-seam.md`.** A hand-written
   `Dom` prelude method cannot reach a JS host import without a native seam, and every
   native is a `CallNativeFn` row in `LlvmGen.cpp` — a HARD edit by overview §0. The dossier
   §8's "DOM APIs are imported functions" assumed the doc-06 bindgen would emit one wasm
   import per method; the §3 "written once" marshaler instead funnels every op through a
   fixed generic seam. Landed minimal: **three C entries** (`lvrt_hostcall` generic sync
   call, `lvrt_host_clo_reg` closure-root register, `lvrt_hostecho` reflective probe) +
   **three rows**, NOT gated (DOM is a wasm-*gained* capability; native stubs raise). The
   four-lane differential is byte-identical (no existing program calls them).
2. **The marshaler lives in JS (`lv_host.js`), off tag+shape — exactly §3.** One `marshal()`
   routine; the header-gate footgun is honored by construction (it reads only tag/payload
   and the payload body, never `payload-16`). Object slot walk reads `slotNames` through the
   new host-facing accessor `lvrt_class_field_name` (paired with the existing
   `lvrt_fieldcount`) rather than JS parsing the `LvClassInfo` struct layout — the §3 "tiny
   runtime getter", realized as a typed accessor. Handle-wrapper short-circuit is the
   `{h}`-single-int-slot heuristic. Boxed arrays only (dense/columnar deferred, §3).
3. **The closure-table ROOT is C-side (`lv_bridge_wasm.c`), not JS.** §4/§5 sketched the JS
   glue holding the table; putting it C-side keeps the closure `LvValue` off the JS boundary
   entirely (JS only ever sees the integer index), so retain/release are direct C calls and
   the leak audit is one file. The event crosses to the handler as a bare `int` handle; the
   `Dom` prelude wraps it in a `DomEvent` (no C-side object construction). `removeEventListener`
   releases the root; `Dom::listenerCount()` (op `cloCount`) is the §5 leak meter.
4. **The trampoline (`lv_dom_dispatch`) runs on its own promising activation** via
   `lv_wasm_dispatch_run` — the same per-activation pooled-stack wrapper timer dispatch uses,
   so an awaiting handler suspends on its own shadow stack (lv_task_wasm.c rule 1). A throwing
   handler surfaces through the same C2 throw gate a throwing timer callback gets
   (`dom_throw` pin: Uncaught banner + exit 1), satisfying hard-05's verification.
5. **Self-dispatch (`node.click()` / `dispatch(type)`) is DEFERRED to its own host turn**,
   not fired re-entrantly. Firing synchronously would run the handler while the caller's
   wasm activation is still on the stack; a handler that *completes* there (throws or
   returns) leaks its pending-throw flag into the caller (the native model runs every
   dispatched callback in its own turn, doc 04 §3.2). A real user event already fires in its
   own turn from the browser loop and never hits this path. The `dom_throw` pin keeps main
   alive (parked on `await`, C2's shape) so the handler's throw is reported while task 0 is
   still live.
6. **Events-as-streams (§6) landed; `fetch`/WebSocket did NOT** — the W-M3 gate (overview
   §4, §8) awaits a *timer*, not a fetch, so the sync bridge is the gate-critical surface.
   `node.events("click")` returns an `InStream<DomEvent>` whose `IDisposable` teardown
   detaches the listener AND releases the root (the leak pin exercises it). `fetch` over a
   `Suspending lv.fetch` import is a follow-up (its result-string marshaling needs the
   resume-then-allocate shadow-stack dance; noted for W-M4 / the Atlantis demo). WebSocket
   stays deferred to first consumer, as §6 allows.
7. **JS→string results go through the exported `lvrt_str_new` + a 64 KiB bounce buffer**
   (`lv_host_scratch`), never raw memory (§3). Larger-than-64 KiB string results are a
   future grow (guarded loud). The marshaler-support exports (`lvrt_str_new`,
   `lvrt_fieldcount`, `lvrt_class_field_name`, `lv_host_scratch`, `lv_dom_dispatch`) are
   `export_name` wrappers pulled into the link only when a program references the bridge
   (uses `Dom`), so non-DOM wasm programs carry none of them.
8. **The `Dom` surface ships in the SHARED prelude (`kPreludeWasm`)** for all targets now,
   per overview §5 ("ordinary prelude code until the packaging ruling"); native builds never
   reach the raising stubs from the existing corpus, so the differential is unaffected.
   Doc 06 §2 makes it a per-target segment when the packaging ruling lands.
9. **Verification (the §8 gate), as run:** `tests/run_wasm_dom.sh` (out of default CTest,
   node DOM-stub host in `tests/wasm_node_run.mjs`) — `dom_hello` (the gate: build subtree +
   attributes + click handler that awaits a timer then mutates text), `dom_marshal` (every
   tag round-trip), `dom_strings` (§7 ASCII/multibyte/astral, echo + a full
   setAttribute/getAttribute boundary round-trip), `dom_leak` (§5: register/unregister N and
   the events-stream close, closure table back to 0), `dom_throw` (throwing handler → exit
   1). All 5 green under Node 24 + `--experimental-wasm-jspi`; `corpus_wasm` and the native
   four-lane differential unaffected.
10. **The §7 encoding ruling** (info.md §9/§10, what `subStr` counts) is filed as still-open;
    the marshaler is ruling-neutral (moves bytes + lens), and the string pins pass whatever
    the ruling decides — they assert UTF-8↔UTF-16 round-trip fidelity, not a `subStr` count.
