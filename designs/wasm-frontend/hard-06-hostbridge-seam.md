# Track W — HARD 06: the `lvrt_host*` DOM/JS bridge seam (W-M3)

**[HARD]** — adds emitted-against `lvrt_*` symbols to the ABI surface (`LlvmGen.cpp`
`CallNativeFn` rows + `lv_abi.h` declarations). **Status:** LANDED (2026-07-18, with the
W-M3 DOM bridge). **Context:** `techdesign-05-dom-bridge.md` §2–§4. One HARD packet =
one commit.

> **Why this packet exists — the doc-05 "HARD content: none" bet did NOT hold.**
> Doc 05's header bet that the bridge is "JS + prelude + corpus" with the trampoline's
> only codegen contingency being hard-05 (`lvrt_callclosure`, LANDED). The bet missed one
> mechanism: **a hand-written `Dom` prelude method cannot reach a JS host import without a
> native seam, and every new native needs a `CallNativeFn` row in `LlvmGen.cpp` — a HARD
> edit by the overview §0 definition** ("adding/changing what generated code calls").
> The dossier §8's picture ("DOM APIs are imported functions over handles") assumed the
> doc-06 bindgen would emit one wasm import per method; but the marshaler §3 wants *one*
> reflective routine "written once," which means the prelude funnels every DOM op through
> a small fixed set of generic seams, not N generated imports. Those seams are the ABI
> addition here. Kept minimal per §0: **three C entry points, three `CallNativeFn`
> branches**, no drive-by refactors, four-lane differential green.

## The seam (five natives, three C entry points)

The prelude `Dom` surface (a `kPreludeWasm` segment, shipped in the shared prelude for
now per overview §5) calls these `std::` natives; nothing else does. On non-wasm targets
they are reachable only if user code touches `Dom` (the existing corpus does not), so the
native four-lane differential is byte-identical; if reached natively they raise cleanly
(`lvrt_host*` native stubs in `lv_runtime.c`).

| native (prelude decl) | C entry | result | used by |
|---|---|---|---|
| `int sysHostI(string op,int h0,int h1,string a,string b)` | `lvrt_hostcall` | handle/int | createElement, getElementById, createTextNode, documentBody, cloCount |
| `string sysHostS(string op,int h0,int h1,string a,string b)` | `lvrt_hostcall` | string | getAttribute, getText |
| `void sysHostV(string op,int h0,int h1,string a,string b)` | `lvrt_hostcall` | void | setAttribute, setText, appendChild, addEventListener, removeEventListener, release |
| `int sysHostCloReg((int) => void cb)` | `lvrt_host_clo_reg` | closure-table index | events()/on() — registers the handler as a §5 **root** |
| `string sysHostEcho<T>(T v)` | `lvrt_hostecho` | string | marshaler round-trip pins (§8, every tag) |

- **`op`** selects the operation; **`h0`** is the primary node handle (0 = document/none);
  **`h1`** is a secondary handle (child for appendChild; closure-table index for
  add/removeEventListener); **`a`/`b`** are string args (`""` when unused). Arity 5 covers
  every DOM v1 op with a fixed signature — no per-op import, one reflective JS marshaler.
- **`sysHostCloReg`** is the one closure-taking native: it retains the closure C-side and
  stores it in the bridge's closure-table **root** (§5), returning its index. `addEventListener`
  then rides `sysHostV` with `h1 = cloIdx`; `removeEventListener` rides `sysHostV` and releases
  index `h1`; `cloCount` rides `sysHostI("cloCount")` for the leak pin. So the whole
  register/attach/detach/release/count lifecycle needs exactly one extra native.
- **`sysHostEcho`** is the reflective round-trip probe: it marshals its single `LvValue` arg
  to JS via the §3 marshaler and returns a host-side rendering, exercising every tag path for
  the §8 pins. Generic native (`<T>`), one `LvValue` arg.

The two async legs (`fetch`/WebSocket, doc 05 §6) are **not** in this packet's minimum: the
W-M3 gate (overview §4, doc 05 §8) awaits a *timer*, not a fetch. `fetch` lands as a fourth
C entry (`lvrt_hostawait`, a `WebAssembly.Suspending` seam) in a follow-up within W-M3/W-M4;
the sync bridge here is the gate-critical surface.

## The edit

1. `lv_abi.h`: declare `lvrt_hostcall`, `lvrt_host_clo_reg`, `lvrt_hostecho` next to
   `lvrt_callclosure`, with the contract above.
2. `LlvmGen.cpp`: three `CallNativeFn` branches (member `FunctionCallee`s + `fn()` inits +
   the dispatch rows). Result retained (`retainDst`) uniformly — fresh string/handle
   results transfer +1; retain is immediate-safe (`lv_is_counted` gate), so int/void
   results no-op. **NOT gated** by the hard-03 table: the bridge is a wasm-*gained*
   capability, not a *lost* one.
3. `lv_runtime.c`: native stubs raising `"host bridge: available on the wasm-browser
   target only"` (unreachable in the existing corpus → differential byte-identical).
4. `runtime/lv_bridge_wasm.c` (new, wasm-only): real implementations over the `lv.dom_call`
   import + the closure-table root + the `lv_dom_dispatch` trampoline export (rides
   `lvrt_callclosure`, hard-05, and `lv_wasm_dispatch_run`, lv_task_wasm.c, for a
   suspendable per-event activation).

## Constraints

- ABI addition → portable-backend owner review; contract on the `lv_abi.h` declarations.
- Minimal diffs, no drive-by refactors (§0). The three branches sit at the end of the
  `CallNativeFn` else-chain, before the final `fail(...)`; no existing native name changes.
- The marshaler footgun rule (doc 00 §6 / doc 05 §3) is a JS-side property — the JS
  marshaler never reads `payload-16`; the C entries pass borrowed `LvValue*`s only.

## Verification

- Four-lane differential on the full pre-existing suite (no generated-code change for any
  existing program; new native names only).
- `corpus_wasm` green (the bridge natives are unreached by that lane).
- The W-M3 DOM lane (`tests/run_wasm_dom.sh`, out of default CTest per doc 05 §8): the
  marshaler round-trip pins (every tag), the §7 string pins, the §5 leak pin, and the
  "hello DOM" click-awaits-timer-mutates-text demo — all against the node DOM-stub host.
