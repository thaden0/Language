# Track W — doc 4 of 6: async on wasm — JSPI as the stackful realization (W-M2)

**Status:** PROPOSED. **Depends on:** W-M1 complete (docs 02, 03).
**HARD content:** none *by design goal* — the one contingency is its own packet,
`hard-04-await-routing.md` (§4).

## 1. The one idea

The landed task substrate (LA-30) already does exactly what JSPI does: park a real call
stack at an unready `await`, run other work, resume in completion order. Natively the
switch is `lv_ctx_switch` (`lv_ctx_x86_64.S:35`) under a per-thread scheduler
(`lv_task.c:932-962`). On wasm-browser the *engine* performs the identical operation:
`WebAssembly.Suspending` imports + `WebAssembly.promising` exports suspend/resume a whole
wasm activation on a JS promise. **One model (stackful, uncolored `await` — locked in,
`info.md:1513-1515,1634-1648`), two realizations.** The surface changes zero; the prelude
changes zero; the scheduler *moves into the host event loop*.

Inversion to hold onto: natively, Leviathan's scheduler owns the loop and polls parked
tasks on quiescence. On wasm-browser the **host owns the loop**; every runnable unit enters
wasm as its own `promising` export call (its own suspendable activation), and "quiescence
polling" becomes a host-side turn-end scan (§3.3). This is the same "each dispatched
callback runs in a task" rule, with the browser supplying the task stacks.

## 2. Deliverables

- `runtime/lv_task_wasm.c` — realizes the `lv_task.h` interface over host imports; replaces
  doc 02 §6's `lv_task_stub.c` in the wasm archive. `lv_tasks_enabled()` → 1.
- The wasm leg of the loop registry (timers/watches) — either inside `lv_task_wasm.c` or a
  thin `lv_loop_wasm.c`, whichever keeps `lv_loop.c` untouched; **never** edit the shared
  loop for wasm-only behavior.
- Host-side additions to `lv_host.js` (`lv.park`, `lv.timer_start/cancel`, settle scan,
  `promising`-wrapped exports).
- Async corpus wired to the wasm lane.

## 3. Mechanism (normative)

### 3.1 Awaiting a Leviathan promise

`lvrt_await` (`lv_runtime.c:2985`) is unchanged: not-ready + tasks-enabled →
`lv_task_park_on(obj)`. The wasm realization of `lv_task_park_on`:

1. calls the **suspending import** `lv.park(promPtr)`;
2. the host creates `new Promise(resolve => parked.set(promPtr, resolve))` and returns it;
   JSPI suspends the whole current activation;
3. when the host later learns `promPtr` settled (§3.3), it calls the stored `resolve`; the
   engine resumes the activation inside `lv_task_park_on`, which returns 0 (parked-and-
   resumed), and `lvrt_await` re-reads the promise fields exactly as the native resume does.

The G11 invariant carries over verbatim: an activation never suspends with the pending-throw
flag set (there is no Itanium EH anywhere — the flag model makes JSPI safe the same way it
made fibers safe).

### 3.2 Timers and dispatched callbacks

When Leviathan registers a timer, the wasm loop leg imports `lv.timer_start(id, dueMs)`;
the host `setTimeout`s and, on fire, calls the **`promising`-wrapped export**
`lv_dispatch(id)` — the existing runtime entry that runs one dispatched callback in a task
context (whatever symbol `lv_task.c`'s scheduler uses to run a callback; the wasm leg
exports that seam). Each dispatch is its own activation, so a callback that awaits suspends
independently of every other — in-flight concurrency is no longer bounded by anything but
the parked map. Dispatch **order** for simultaneously-due work must preserve the registry's
due/creation order (RuntimeLoop.hpp:60-64 parity): the host fires strictly through its own
event loop, which preserves `setTimeout` ordering per due-time; pin it in the corpus.

### 3.3 Settlement — turn-end scan, no settle hook

`resolve()` stays pure language (fields on the promise object); the native design polls
parked tasks on quiescence rather than hooking settle (LA-30 G10). Mirror it: after every
export call into wasm returns (dispatch, trampoline, main), the host scans `parked`,
calling the exported `lvrt_promise_state(promPtr)` (already in the runtime — `lv_runtime.c`
uses it in `lvrt_await`; export it via the link flags) for each entry; ready → resolve and
delete. O(parked) per turn, same complexity stance as native; the settle-hook import is the
documented escape hatch if it ever measures hot — do not build it speculatively.

ARC note: the parked map holds `promPtr` across turns — the C side must retain the promise
object when parking and release on resume (the native parked-set already does exactly this;
keep the retain on the C side of `lv_task_park_on`, never trust the JS map with lifetime).

### 3.4 Program lifetime

`lv_main` is wrapped `WebAssembly.promising`; the "drain the loop after the program body"
rule becomes: the host's `run()` resolves when `lv_main`'s promise settles **and** the
parked map and timer set are empty. Loop-drained-with-tasks-parked reproduces the flip-mode
loud diagnostic (LA-30 doc 5 C3): if timers and parked are empty but parked tasks remain
suspended, print the same message the native flip mode prints.

### 3.5 Host promises (`fetch` et al.)

Free by construction: a floor/bridge import that returns a JS promise is declared
`Suspending`, and the awaiting Leviathan frame suspends on it directly — this is the path
doc 05 §6 uses for `fetch`/`WebSocket` stream endpoints. No Leviathan-side promise
plumbing needed beyond wrapping the result.

## 4. The zero-codegen claim (and its contingency)

Design goal: **no `src/LlvmGen.*` edits in W-M2.** Post-LA-30, the LLVM leg's `Await`
bottoms out in the runtime call `lvrt_await`, and everything above changes only in
`lv_task_wasm.c` + JS. Audit this at W-M2 start; if any path still bypasses `lvrt_await`
with an inline pump/park BB graph, the fix is the contingent HARD packet
**`hard-04-await-routing.md`** — otherwise close that packet as NOT NEEDED in the log.

## 5. JSPI availability & the fallback lane

- Feature-detect `typeof WebAssembly.Suspending === "function"` in `lv_host.js`; fail loud
  with a clear message when absent, naming the flag/browser floor. Headless CI: Node ≥ 24
  or Chrome headless with JSPI enabled — pick whichever the CI box already has; record it
  in `tests/run_wasm.sh`.
- **Asyncify fallback:** prove the async corpus once through a Binaryen `wasm-opt
  --asyncify` build to de-risk engine-coverage complaints, record sizes/timings in the log,
  then **shelve it** — bring-up lane, never CI-required, never load-bearing (STOP
  condition 4). A stackless CPS transform is **not recommended, ever** — it reintroduces
  the coloring §14 foreclosed (`techdesign-00-overview.md:181-186`).

## 6. Verification (W-M2 gate)

- Async corpus (await, then-chains, timers, interleaving order pins, throw-across-await)
  through the wasm lane in a JSPI host, diffed against `--ir`. Failure-routing pins from
  LA-30 doc 5 (C2) reused verbatim — completion-order semantics must match native tasks.
- The C3 loud-drain diagnostic pinned.
- One `fetch`-shaped end-to-end (can stub the URL with a data: URI) proving Suspending
  imports compose with `lvrt_await` above them.

## 7. Threads — explicitly deferred (not in W-M2)

The surface maps cleanly later (Workers = thread-pinned tasks + structured-clone ≙ the
landed flatten/rebuild copy boundary, `lv_thread.h:23-25`; SAB+Atomics = the gated
shared-mutable tier), but it is its own leg with its own doc when scheduled. Until then
`spawn`/`Channel` on wasm stays compile-time gated (doc 02 §5), exactly like Windows/ELF
today (`info.md:1606-1607`). Revisit doc 02 §3's LocalExec TLS pin when this leg opens.
