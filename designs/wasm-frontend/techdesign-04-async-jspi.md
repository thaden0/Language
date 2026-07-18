# Track W — doc 4 of 6: async on wasm — JSPI as the stackful realization (W-M2)

**Status:** LANDED (2026-07-17) — see §8 for the as-built deltas. **Depends on:** W-M1
complete (docs 02, 03).
**HARD content:** none *by design goal* — the one contingency is its own packet,
`hard-04-await-routing.md` (§4, closed NOT NEEDED). As-built, ONE `LlvmGen.cpp` edit
happened anyway — a hard-03 GATE refinement, not an Await-path change (§8 item 2).

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

## 8. As-built (2026-07-17) — what the mechanism sections didn't anticipate

1. **The C shadow stack is the real task-stack problem.** §1's "the browser supplying
   the task stacks" covers the ENGINE's wasm stack only: JSPI does not save
   `__stack_pointer`, the module global clang's C lowering uses for address-taken
   locals in linear memory, so two live activations on one shadow stack clobber each
   other. `lv_task_wasm.c` enforces a four-rule discipline (its file header):
   per-activation pooled 256 KiB stacks (the lv_task.c mmap-pool analog, malloc'd),
   save/restore of `__stack_pointer` + the current-task pointer around every
   suspending import, one static service stack for non-suspending host-entry work
   (the settle scan, wrapper bookkeeping), and main pinned to the wasm-ld default
   stack as the FIRST export entered (lv_host.js's `run()` guarantees the order; the
   probe driver learned this the hard way). The sp accessors are the wasi-libc
   `.globaltype __stack_pointer` inline-asm idiom — data movement, not a context
   switch; doc 02 §6's "no .S files" stance stands.
2. **One `LlvmGen.cpp` edit after all — hard-03's gate, not Await.** The §4 audit
   re-confirmed clean routing (hard-04 stays CLOSED NOT NEEDED; `Op::Await` is still
   the single `lvrt_await` call). But the async corpus found a hard-03 tier-1 FALSE
   POSITIVE: the user-reach walk reused the emission walk's by-name CallDyn fallback,
   so any generic `close()` dispatch (every `Timer` — its ctor builds
   StreamBuffer/OutStream) marked `Channel.close` → `sysChannelClose` and bricked the
   compile. Fixed by dropping the name-fallback from the GATING walk only (the
   emission walk is untouched): user-instantiated classes already mark all their
   members at `NewObject`, and under-marking only shifts a case to the tier-2
   `lvrt_unsupported` trap — loud at runtime, never wrong emission. Native lanes
   byte-identical (`userReach` is only consumed under `targetWasm`); full suite green.
3. **Park key = the task record, scanned via a new `lv_park_poll` export** — not §3.3's
   "export `lvrt_promise_state` via the link flags". One scan export covers all three
   park shapes (promise / any2 / join — the B2 surface §2 didn't enumerate but
   "realizes the `lv_task.h` interface" requires: `sysTask*` is NOT gated on wasm, so
   TaskGroup/cancel/shield/joinall and the b2_* corpus all run on this lane). Zero
   `main.cpp` edits: `export_name` attributes on always-pulled archive members make
   `--export` link flags unnecessary. The G11 invariant and the §3.3 ARC retain landed
   as specified; `lv_task_stub.c` is deleted, superseded.
4. **`lv_loop_wasm.c` REPLACES `lv_loop.c` in the wasm archive** (§2's "thin
   `lv_loop_wasm.c`" option — the shared file's registries are file-static and its
   step blocks in poll, so interposing wasn't possible; `lv_loop.c` is untouched, per
   the §2 rule, and stays on every native triple). Its one behavioral invention:
   `lvrt_loop_step` = "suspend until the host completes one dispatch" (`lv.step`),
   which keeps `lv_main`'s EMITTED tail drain — pumped from inside task 0 in both
   modes since LA-30 — correct with zero codegen edits, C2 throw routing included.
   Repeating timers re-arm from C after each fire (fixed-delay vs native fixed-rate;
   the corpus pins tick shape/order, not phase — `repeat_cancel.lev`). FD watches
   register/cancel but never arm or count as loop work (every fd source is gated).
5. **`lv_plat_map` had to stop bump-allocating** (`lv_plat_wasm.c`): the W-M1 cursor
   from `__heap_base` could overlap wasi-libc dlmalloc's sbrk region once both had
   grown the memory — latent in W-M1, real the moment task stacks made malloc traffic
   (§8 item 1). Now page-granular `memory.grow` from the live end; two end-growers
   are disjoint by construction.
6. **§3.4's "print the same message" lands runtime-side:** the host resolves each
   parked task `LV_PARK_DRAINED` (park order, matching `drain_wake_all`) and the
   RUNTIME raises the native flip mode's own C3 message — which is what keeps
   `drained_await_throws` byte-identical against `--ir`, exit 1 included. The corpus
   lane now diffs exit codes as well as stdout.
7. **Verification as run (the §6 gate):** the corpus_wasm lane is 44 files, 0
   skipped — W-M1's 28 pure-compute+console + the async cluster (LA-30 doc-5 pins
   verbatim: C1 `order_two_awaits`, C2 `throw_uncaught_callback`, C3
   `drained_await_throws`, C4 `ready_no_yield`, plus `cancelled_exception` and the
   three `b2_*` files) + four new wasm pins (`order_timers` — §3.2's simultaneous-due
   pin, `repeat_cancel`, `then_chain` — then-chains + throw-across-await, and
   `probe_async`). The §6 fetch e2e is `lv_probe_fetch`: one extra activation
   suspending on a GENUINE `fetch('data:text/plain,42')` through a Suspending import
   plus a yield-lane suspension while corpus activations sit parked (run_wasm.sh's
   `probe_*` mode; in-stack fetch-under-`lvrt_await` arrives with doc 05's bridge).
   Hosts: Node 24.14 with `--experimental-wasm-jspi` (detected in run_wasm.sh;
   pure-compute still passes without it, async fails loud naming the floor) and
   headless Chrome 145 (JSPI on by default since 137) via `lv_host_page.html` —
   `then_chain` byte-identical, C3 drain banner + exit 1 verbatim.
8. **Asyncify fallback NOT exercised:** no Binaryen `wasm-opt` on the build host, and
   the engine-coverage complaint it was de-risking has shrunk (JSPI default-on in
   Chrome, behind one flag in Node). Recorded here per §5 and shelved unbuilt —
   bring-up reserve only, never CI-required, never load-bearing (STOP condition 4);
   the no-CPS stance stands.
