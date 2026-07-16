# Track 10 — True Threads / Workers, competing design (responds to `request-threads.md`, LA-1)

**Status:** competing design to `techdesign-threads.md`, produced by adversarial review
2026-07-11. **This document supersedes the original's transfer model**; it keeps the
original's surface, backend matrix, per-worker loops, SPSC channel substrate, and
SO_REUSEPORT floor, all of which survived review. Read §1 for exactly what was refuted
and why; read from §2 onward as the design to implement.

> **COMPLETED-BY (2026-07-11): `techdesign-threads-3.md`** — M1/M2/M4/M5 landed
> from this document (§14 log); the M3/M6 execution leg (true pthreads on LLVM,
> the C flatten/rebuild engine, the leak harness) is specified in doc-3, which
> resolves this doc's latent §3.1-prelude-vs-§4.2-lowering tension via a native
> **capability-probe seam** and **amends §6 on one point**: `Worker<T>`/
> `Promise<T>` handles are NOT transferable across a thread boundary (a
> cross-thread `resolve` would invoke the promise's continuation on the wrong
> thread — the exact D1′ race); `Channel<T>` remains the one portal.
> **Implement M3/M6 from doc-3.**

**Requested by:** Atlantis framework (Tracks 01 + 05), 2026-07-06. **Priority:** P1, wanted
by ~AG-6 (2026-12). **Substrate:** info.md §14 (isolation by default), §13 (streams/loop),
§15 (ownership + refcount tiers). **Backend reality:** X64Gen/ELF frozen; LLVM is the true
target; oracle/IR are the serial semantic reference.

---

## 1. Adversarial findings — why the original's transfer model does not survive contact with the runtime

The original design's Decision D1 has two halves: **(a)** per-worker heaps, nothing counted
shared across threads; **(b)** values cross boundaries by **ownership move**, reparenting the
refcount prefix into the receiving thread's allocator, with use-after-move rejected at compile
time. Half (a) is correct and this design keeps it. Half (b) is refuted on three independent
grounds, each fatal on its own:

### F-1. The ARC gate makes a cross-heap "moved" value invisible to the receiver

Every retain/release first passes `lv_is_counted` → `lv_in_region`
(`runtime/lv_runtime.c:190-206`, `:80-88`): a pure **address-range check against the
process-global heap/arena bounds** (`g_heap_base/end`, `g_arena_base/end`,
`lv_runtime.c:70-78`). This gate is load-bearing — it is what lets data-segment string
literals and arena values carry no refcount prefix (info.md §15). Make the heap per-thread
and the gate becomes "is this pointer in *my* region": a value whose bits live in worker W's
mmap **fails every other thread's check**, so on the receiving thread retain/release
**silently no-op** on it — the "moved" value is unmanaged, its graph can never be released,
and `lv_free_raw` (`lv_runtime.c:139-148`) silently drops any free of it (out-of-region
early-return) or, if the check were widened, would push a W-region block onto the receiver's
free-list — recycling memory that dies when W's mmap is unmapped. There is no cheap
"reparent the prefix" operation: the prefix lives in a page owned by the source allocator,
and ownership of *pages* is what the region check encodes.

### F-2. The original frees the worker's allocator while "moved" values still point into it

Original §7#4: "`WorkerCtx` (loop, arenas, allocator) is freed by `pthread_join` inside the
join handler." But the worker's **result** — the value the whole spawn exists to produce —
was allocated by the worker, in the worker's heap. A "move at +1" hands the awaiter a
pointer into pages that the join handler then unmaps. The same applies to every channel
message still in flight when a sender exits. The original's move model and its teardown
model contradict each other; only deep copy into receiver-owned memory (before teardown)
reconciles them.

### F-3. Use-after-move cannot be enforced without alias analysis the language rejects

Objects are references; `MyClass b = a;` aliases freely; the language deliberately has the
Rust *mechanism* (scope + refcount) and not the Rust *discipline* — no borrow checker
(info.md §18). The original's compile-time "abandoned after spawn/send" check tracks the
**variable**, and §15's return-liveness machinery it claims to reuse works precisely because
a returned local's *other aliases keep their own +1 and remain valid* — ownership transfer
at +1 is additive there, not exclusive. Move-to-another-thread is exclusive:

```
MyClass b = a;
Worker<int> w = spawn(() => a.compute());   // "moves" a — compile check marks `a` dead
b.field = 7;                                 // b is the same object: two threads, one
                                             // non-atomic refcount. The race D1 exists
                                             // to prevent, reintroduced by its own
                                             // enforcement gap.
```

Catching this requires whole-program may-alias analysis — a borrow checker in all but name.
The isolation *guarantee* cannot rest on an unenforceable check.

### F-4 (scope finding, not fatal). "One documented hook in `lv_loop.c`" misstates the TLS surface

There is no single "global loop pointer." The loop is six file-statics
(`lv_loop.c:41-46`: `g_timers/g_timerCount/g_timerCap/g_watches/g_watchCount/g_nextId`);
the allocator is another dozen globals in `lv_runtime.c` (heap, arena, free-list,
live/peak accounting, throw flag) — **all** must become per-thread for per-worker heaps to
exist at all; and generated LLVM code binds the exported `lv_g_arena_cursor` and
`lv_g_throwing` symbols **directly** for inline arena save/restore and throw checks
(`src/LlvmGen.cpp:339-345`, `:1082` — deliberately inlined to avoid cross-`.o` calls per
frame). TLS-ification therefore reaches into **codegen** (the globals become
`thread_local`, LLVM IR emits TLS-model loads) — a real, measurable change to every
frame's fast path, not a one-line runtime edit. §5 scopes this honestly and gates it.

### Smaller defects fixed in passing (each has a section below)

- **F-5.** Serial oracle shim ("run the closure inline at spawn, resolve immediately",
  original §5) **deadlocks on any ping-pong**: an inline-run worker that `receive()`s
  before the parent's `send()` parks a loop that can never be fed. → §7: workers are
  cooperative loop tasks on the interpreters, not inline calls.
- **F-6.** `OverflowPolicy` **`grow` is a race on a lock-free SPSC ring** (realloc moves
  the buffer under the peer's feet). → §3.3: `Channel` capacity policies are
  block/drop/error only; `grow` is rejected at construction.
- **F-7.** `block`-on-full needs a **producer-side wakeup** the original never provides
  (it has only the consumer's eventfd). → §4.3: two eventfds per channel, one per
  direction.
- **F-8.** `receive()` on a closed channel is unspecified ("receiver sees a closed
  signal"). → §3.3: `receive()` is `Promise<T?>`; `None` = closed-and-drained — the exact
  `sysRecv` precedent (info.md §13), narrowing done by *user* code (prelude itself avoids
  relying on narrowing, per the Track 09 lesson).
- **F-9.** Process lifetime for an **un-awaited** worker is unspecified. → §3.2: the join
  eventfd watch registered at spawn *is* live work on the spawner's loop, so a live worker
  keeps the process alive and auto-reaps on completion; no detached-thread kill semantics.
- **F-10.** "Differential output equality is exact because worker contributions are
  deterministic" (original §7#6) ignores that **stdout is shared**: two workers
  `console.write`-ing race on *ordering* even with perfect heap isolation. → §8: a stated
  corpus discipline (workers compute and send/return; only the spawner prints, at join
  points), enforced by construction in every corpus program — not by sorting output.
- **F-11.** Atlantis's "spawn N for N cores" (request §3) has no way to learn N. → §3.5:
  `std::cpuCount()` over a `sysCpuCount` native, one line per engine.
- **F-12.** The leak harness reads `lvrt_live_bytes()` — with per-thread accounting the
  number must be defined. → §4.5: per-thread live/peak; the harness asserts the **main**
  thread's live-at-exit +0B *and* that every worker heap was unmapped (reap counter),
  which is the stronger, well-defined form of acceptance #3.

---

## 2. The core decision, corrected: pure isolation with **copy-always boundaries**

> **Decision D1′ — v1 shares nothing counted across threads (kept from the original), and
> every value that crosses a thread boundary crosses by DEEP COPY through a self-contained
> transfer buffer ("flatten/rebuild"). Nothing is ever moved between heaps; no refcount
> prefix is ever reparented; no use-after-move analysis exists.** The sender's original
> remains fully valid and fully owned by the sender (ordinary ARC on its own thread); the
> receiver rebuilds a fresh, independently-owned graph inside its own heap on its own
> thread. A counted allocation is touched by exactly one thread from birth to free —
> today's non-atomic retain/release fast path is preserved **verbatim**, and the region
> check (F-1) stays correct because no thread ever holds a pointer into another thread's
> region.

This is the structured-clone / Erlang-message model, and it is *strictly simpler* than the
original: one mechanism (flatten/rebuild) serves all four crossings — spawn capture, channel
send, join result, worker exception — where the original needed move + reparent + deep-copy
(for strings/arrays) + compile-time liveness, four mechanisms with four failure modes.

What it costs, stated honestly:

- **Every crossing is O(size of the value graph), twice** (flatten on the sending thread,
  rebuild on the receiving thread). The request explicitly accepts this ("constrains R-4 to
  transfer-or-copy semantics, which Atlantis is happy with"), and the original *already*
  paid it for strings and pure collections — its "move" fast path applied only to
  statically-shaped class objects, the case F-1/F-3 show was unsound anyway. Web-shaped
  workloads (the driver here) send requests/responses measured in KB; a benchmark gate in
  M3 (§10) quantifies the copy cost so v2 optimizations are argued from numbers.
- **Zero-copy immutable sharing (request R-1's second sentence) is deferred to v2**, same
  as the original — an atomic-count immortal tier for provably-immutable values (§12). D1′
  makes this a pure *addition* later (a third value class the flatten step recognizes and
  passes by pointer), not a rework.
- **Big graphs are the user's problem to shard** — the same stance the language takes
  everywhere else (§16: loud and local): the cost is at the `spawn`/`send` call site you
  wrote, proportional to the value you passed, on the thread that passed it.

What it buys:

- **Soundness by construction, not by analysis.** No alias hole (F-3): the sender keeps its
  object, so aliases stay valid — there is nothing to enforce. Acceptance #1 ("mutating the
  captured original after spawn is not observed") holds trivially.
- **Teardown is trivial** (F-2): a worker's heap can be unmapped wholesale at reap because
  *nothing outside the worker ever points into it*. This is the actual payoff of per-worker
  heaps: worker memory becomes arena-shaped at the thread granularity.
- **The compiler's new obligation shrinks** to one predicate — *is this type flattenable?*
  (§6) — instead of a liveness dataflow.

---

## 3. Language surface (prelude-authored over a thin native floor)

No new syntax. Same shape as the original where it was right; deltas marked **[Δ]**.

### 3.1 `spawn` (R-1)

```
// prelude, std namespace
Worker<T> spawn<T>(() => T body);
```

- The compiler synthesizes the capture record exactly as for any closure (the existing
  §16.5 machinery), then checks every captured value is **flattenable** (§6). A
  non-flattenable capture is a compile error at the `spawn` site.
- At runtime, spawn **flattens the capture record** on the calling thread, hands the buffer
  to the new thread, and the worker trampoline **rebuilds** it into the worker's own heap
  before the body runs. **[Δ]** The parent's originals remain valid and owned by the
  parent — no move, no abandonment.

### 3.2 Join as a promise (R-2)

```
Worker<int> w = spawn(() => expensive());
int result = await w;      // parks THIS loop only
```

- `Worker<T> : Promise<T>` — no second suspension surface. The join eventfd is created and
  registered on the spawner's loop **before** `pthread_create` (lost-wakeup ordering, kept
  from the original; eventfd is level-triggered so signal-before-park is still readable).
- On body return, the worker **flattens the result** (or the uncaught `IException`) into a
  transfer buffer on its own thread, publishes the buffer pointer to the `WorkerCtx` result
  slot, signals the eventfd, and exits. The join handler on the spawner's loop **rebuilds**
  the value into the spawner's heap, resolves (or rejects → rethrow at `await`, per §12.6;
  reconciles with `request-promise-rejection.md` as in the original), then
  `pthread_join`s and unmaps the worker's heap/arena/loop. Rebuild strictly precedes
  teardown — F-2 closed. **[Δ]**
- **Process lifetime [Δ, F-9]:** the join-eventfd watch registered at spawn is ordinary
  live work on the spawner's loop (the same rule as a pending timer), so a live worker
  keeps the process alive; when it fires with no `await` parked on it, the handler still
  rebuilds-or-drops the result and reaps the thread. A worker is never detached and never
  killed; a never-terminating un-awaited worker hangs the program *visibly* — the same
  behavior as a never-firing awaited promise, and the honest one.

### 3.3 Cross-thread channel (R-4)

```
class Channel<T> {
    new Channel(int capacity, OverflowPolicy policy);  // block | drop | error   [Δ: no grow]
    void send(T value);          // flatten on this thread; copy-in            [Δ: not move]
    Promise<T?> receive();       // None = closed and drained                  [Δ: F-8]
    void close();                // idempotent; sender-side end signal
}
```

- **SPSC, kept**: one producer end, one consumer end, each endpoint owned by exactly one
  thread; fan-in is N channels + select at the library layer (v2, §12).
- `send` **flattens** `value` into a self-contained buffer and enqueues the buffer pointer
  in the ring slot; the sender's original stays owned by the sender. `receive` dequeues,
  **rebuilds** into the consumer's heap, frees the buffer. Buffers are allocated from the
  **transfer allocator** (§4.4) — the one process-shared allocation path, made safe by the
  ring's own SPSC handoff discipline, not by making the value heaps atomic.
- **`grow` is not accepted** (F-6): a lock-free ring cannot be resized under a concurrent
  peer without locking, and a locked resize path inside an otherwise lock-free channel is a
  silent-distant cliff. `block` (backpressure — parks the *producer's* loop), `drop`, and
  `error` (throw at `send`) cover the §13 policy space; constructing a `Channel` with
  `grow` is a compile-time error where decidable, else a construction-time throw.
- `receive()` on a closed, drained channel resolves `None` (F-8) — three states
  (item / nothing-yet / closed) map onto exactly the `sysRecv` `string?` precedent, and
  user code narrows with `!= None` as designed. `send` on a closed channel throws.

### 3.4 Shared listener (R-5) — kept verbatim

```
int sysTcpListen(int port);                 // existing
int sysTcpListen(int port, bool reusePort); // NEW additive overload — SO_REUSEPORT
```

Same as the original: `setsockopt(SO_REUSEPORT)` between `socket()` and `bind()`; N workers
each run a full accept→serve loop on one port, zero shared state, kernel load-balances.
LLVM + interpreters; not emitted on ELF.

### 3.5 `std::cpuCount()` **[Δ, F-11]**

`int cpuCount()` over a `sysCpuCount` native (`sysconf(_SC_NPROCESSORS_ONLN)`; interpreters
return the same; comptime-denied like every `sys*`). One line per engine; it is what makes
`HttpServer(port, workers: cpuCount())` writable.

---

## 4. Runtime mechanism (LLVM backend; `runtime/lv_thread.c` new, plus a scoped TLS pass)

### 4.1 `LvThreadCtx` — the per-thread runtime context **[Δ, F-4]**

All mutable runtime globals move into one struct, reached through one `__thread` pointer:

```c
typedef struct LvThreadCtx {
    /* allocator (lv_runtime.c:70-78 today) */
    uint8_t *heap_base, *heap_cursor, *heap_end;
    uint8_t *arena_base, *arena_cursor, *arena_end;
    void    *freelist[LV_SIZE_CLASSES];
    int64_t  live_bytes, peak_bytes;
    int64_t  throwing;                    /* lv_g_throwing twin */
    /* loop (lv_loop.c:41-46 today) */
    LvLoop   loop;                        /* timers, watches, nextId, epoll fd */
} LvThreadCtx;
extern __thread LvThreadCtx *lv_tls;
```

- The main thread's ctx is statically allocated and installed by `lv_rt_init` — a program
  that never spawns runs exactly one ctx and one loop. **Zero-spawn behavioral identity is
  the gate** (as in the original), but stated honestly: the *code path* is not
  byte-identical, because `lv_g_arena_cursor`/`lv_g_throwing` accesses in generated code
  become TLS accesses (F-4). The gate is therefore **corpus-identical output plus a
  perf-guard**: the timers/sockets/http LLVM corpus green before/after, and the arena
  save/restore microbenchmark within noise (initial-exec TLS on x86-64 Linux is one
  `mov %fs`-relative load — expected well under noise, but it gets measured, not assumed).
- `lv_in_region` now checks against `lv_tls`'s bounds — correct under D1′ because no
  pointer ever legitimately crosses regions (F-1 closed by construction).
- `lvrt_arena_save/restore`, `lvrt_halloc`, retain/release all read through `lv_tls`.
  `LlvmGen.cpp`'s inlined arena/throw fast paths re-bind their two globals as
  `thread_local` LLVM globals (initial-exec model) — the codegen diff is those two symbol
  declarations, not the emission logic.

### 4.2 Spawn, trampoline, reap

`spawn` lowers to `lv_thread_spawn(body_fnptr, flat_captures, WorkerCtx**)`:

1. Parent: flatten captures (§4.4); allocate `WorkerCtx` (holds a fresh `LvThreadCtx`
   *descriptor* — mmaps happen on the worker thread, so pages are NUMA-local); create the
   join eventfd; register the watch on the parent's loop; `pthread_create`.
2. Trampoline (worker thread): mmap heap+arena, install `lv_tls`, rebuild captures into the
   worker heap, run `body` through the standard dispatch trampoline
   (`lv_invoke1`-shaped), then run the worker's loop until it drains (a worker may register
   timers/watches — R-3); flatten result-or-exception into the `WorkerCtx` slot; signal the
   eventfd; return.
3. Join handler (parent's loop, on eventfd fire): rebuild result into parent heap; resolve
   or reject the `Worker<T>` promise; `pthread_join`; munmap the worker's regions; free
   `WorkerCtx`. Rebuild-before-unmap ordering closes F-2.

### 4.3 The SPSC ring + two eventfds **[Δ, F-7]**

- Ring slots hold **transfer-buffer pointers** (one word each), not `T`-value bits —
  simpler than the original's value-slot ring, and it makes slot handoff a single
  release-store. Head/tail are the only atomics in the design (kept).
- **Two eventfds:** `dataReady` (producer → consumer: written on enqueue when the consumer
  is parked) and `spaceReady` (consumer → producer: written on dequeue when a
  `block`-policy producer is parked on full). Both level-triggered, both registered on the
  owning thread's loop. `close()` writes `dataReady` so a parked consumer wakes and reads
  the closed flag.
- The endpoints are `Channel<T>` handle values; the ring itself and both eventfds live in a
  small process-global channel record allocated by the transfer allocator, freed when both
  endpoints have dropped (a 2-counter on the record, incremented at endpoint creation —
  touched only through the ring's acquire/release pair, so it needs no extra atomicity
  beyond one atomic decrement at endpoint death).

### 4.4 The transfer allocator + flatten/rebuild format **[Δ — the heart of D1′]**

- **Allocator:** the LLVM/threads path links libc already (pthreads); transfer buffers are
  plain `malloc`/`free` — thread-safe, process-global, and *outside* every `lv_in_region`
  range, so a transfer buffer can never be mistaken for a counted value even if a pointer
  leaks into ARC's path. (The pure-ELF backend's no-libc constraint is irrelevant here:
  threads are LLVM-only.)
- **Format:** a self-contained relocatable snapshot of one value graph. Records are the
  existing tagged 16-byte value pairs plus object/array/map bodies laid end-to-end;
  internal references are **buffer-relative offsets**, written by a DFS walk that maintains
  a seen-map so **shared substructure and cycles flatten once and rebuild shared** (a
  diamond in, a diamond out — semantics preserved, and the walk terminates on cycles).
  Strings copy their bytes; dense value-struct arrays memcpy their run (they contain no
  references by construction — §9 structs are flat). Rebuild is the inverse walk,
  allocating through the receiving thread's ordinary `lvrt_halloc` so every rebuilt node is
  a first-class counted value in the receiver's region.
- **One mechanism, four call sites:** spawn captures, `send`, join result, worker
  exception. One implementation, one fuzz surface (`fuzz/thread_leak.py` churns it), one
  place cycles/sharing/depth get tested.

### 4.5 Leak accounting **[Δ, F-12]**

`live_bytes/peak_bytes` are per-ctx. `lvrt_live_bytes()` returns the calling thread's
(the harness's main-thread reading keeps its meaning). The reap path asserts the worker's
regions were unmapped and counts reaps; `fuzz/thread_leak.py` asserts main-thread
live-at-exit +0B, reap count == spawn count, transfer-allocator outstanding == 0, and
`/proc/self/fd` flat (no leaked eventfds) — the well-defined form of acceptance #3.

---

## 5. Backend matrix & the freeze — kept, with one honesty fix

| engine | worker execution | notes |
|---|---|---|
| tree-walk oracle | **cooperative tasks on the single loop** **[Δ, F-5]** | not inline-at-spawn; §7 |
| IR interpreter | same | differential twin |
| LLVM | true pthreads, per-thread ctx/loop | the deliverable |
| ELF / X64Gen | **excluded** | frozen; `spawn`/`Channel`/`sysTcpListen/2`/`sysCpuCount` are a compile-time "unsupported on the ELF backend (use LLVM)" diagnostic — never silent serial fallback |

Comptime hermeticity: every new native (`sysThreadSpawn`, `sysThreadResult`, channel ops,
`sysTcpListen/2`, `sysCpuCount`) enters the comptime deny-set **in the same commit** (the
Track 08 checklist; kept from the original).

---

## 6. Compiler obligation: the *flattenable* predicate (replaces move/liveness entirely)

One recursive, per-type predicate at `spawn` capture sites and `send` argument positions:

| value kind | flattenable? |
|---|---|
| primitives, `char`, `None`, ranges | yes (bits) |
| `struct` values | yes (flat by construction, §9) |
| strings, pure `Array`/`Map` (of flattenable element/key/value types) | yes (deep) |
| statically-shaped `class` objects (all fields flattenable, transitively) | yes (deep; shared substructure/cycles preserved per §4.4) |
| closures **other than the spawn body itself** | **no in v1** — a closure's captures may reach arbitrarily far (loop registries, sockets); flattening one is a covert graph export. Loud error: "closures cannot cross threads (v1)". |
| interceptable / dynamic-shape objects | no — compile error (kept) |
| open-fd carriers (`TcpStream`, `TcpListener`, `File`, `Timer` handles) | no — an fd is loop-bound; error names the type and says "each worker opens its own" (R-5's share-nothing shape) |
| `Channel<T>` / `Worker<T>` handles | **portal values**: not flattened — the endpoint *relocates* (its record is transfer-allocator-resident, §4.3); each endpoint still owned by exactly one thread |

Where the static type is generic/erased (generics are erased — memory note), the predicate
is checked on the concrete flatten walk at runtime and throws a `RuntimeException` naming
the offending type — loud and local, never a silent skip. **No use-after-move analysis, no
liveness dataflow, no new checker machinery beyond this predicate** — the compiler diff
shrinks versus the original (F-3 dissolved rather than solved).

---

## 7. Serial shim on the interpreters **[Δ, F-5]**

`spawn` on oracle/IR schedules `body` as an **immediate task on the single loop** (a
timer-0-shaped work item, run in spawn order — deterministic), resolving the worker-promise
when the task completes. `send`/`receive` are loop-mediated promise handoffs on an
in-process queue. Flatten/rebuild **still runs** (into the same heap) — so copy semantics,
flattenability errors, and acceptance #1 are exercised identically on the reference engines,
and a ping-pong program interleaves cooperatively instead of deadlocking. A program that
truly deadlocks (A awaits B awaits A) hangs on both the serial and threaded engines — same
observable, honest behavior.

Differential contract: D1′ makes results interleaving-independent (kept from the original —
isolation is what lets the oracle stay simple), so oracle/IR serial output is the golden
file LLVM must match **byte-for-byte, never sorted or normalized** (kept; a drift is a
flatten/isolation bug, not a tolerance).

## 8. Output-determinism discipline **[Δ, F-10]**

Heap isolation does not serialize stdout. Corpus rule, stated where the tests live
(`tests/corpus/threads/README`): **workers compute and return/send; only the spawning
thread prints, at join/receive points.** This is a determinism *test discipline*, not a
language rule — user programs may print from workers and get real-world interleaving;
the corpus just never does, so byte-equality stays meaningful. (Runtime does guarantee
`console.write` of a single value is one `write(2)` — no torn lines — which it already is.)

---

## 9. Foreseeable problems & strategies (revised)

| # | problem | strategy |
|---|---|---|
| 1 | Non-atomic refcount race | D1′: a counted value lives and dies on one thread, by construction; crossings are copies through non-counted malloc buffers. No enforcement gap to defend (F-3 gone). |
| 2 | TLS-ification destabilizes the single-loop corpus / taxes the arena fast path | §4.1: one `LvThreadCtx` struct; main-thread ctx static-init; gate = corpus green **and** arena microbench within noise (initial-exec TLS). If generated-code TLS rebinding can't hold the perf gate, **STOP** (portable-backend owner territory). |
| 3 | Lost cross-thread wakeup → deadlock | eventfd created + watch registered before `pthread_create`; level-triggered. Corpus: instantly-returning worker awaited (kept). |
| 4 | Result/teardown ownership | Flatten-before-signal, rebuild-before-unmap (§4.2). Nothing outside a worker heap ever points into it, so unmap is unconditionally safe (F-2 closed). |
| 5 | Slow consumer / overflow | block/drop/error only; `block` parks the producer's loop via `spaceReady` eventfd (F-6, F-7). |
| 6 | Copy cost surprises Atlantis at scale | M3 benchmark gate: msgs/sec + MB/s through a channel at 1KB/64KB/1MB payloads, published in the implementation log; v2's immutable-share tier is argued from these numbers. |
| 7 | Comptime hermeticity | deny-set same-commit checklist (kept). |
| 8 | Flatten walk on adversarial graphs (deep recursion, cycles) | seen-map + explicit work-stack (no C-stack recursion); depth/size fuzzed in `fuzz/thread_leak.py`. Cycles rebuild as cycles (§4.4) — within-worker cycle leaks remain exactly today's §19#10 asterisk, unchanged. |
| 9 | A flattenable-typed value holding a non-flattenable at runtime (erased generics) | runtime flatten throws `RuntimeException` naming the type (§6) — loud, local, corpus-covered. |

---

## 10. Milestones & acceptance

| M | deliverable | accept |
|---|---|---|
| M1 | flatten/rebuild engine + flattenable predicate; `spawn`/`Worker<T>`/join on **oracle + IR** (cooperative shim, §7); `sysCpuCount` | `threads/spawn_join.lev`: value returned; mutation-after-spawn unobserved (acceptance #1); ping-pong via two tasks does not deadlock; non-flattenable capture is a loud error |
| M2 | `LvThreadCtx` TLS pass (runtime + the two LlvmGen global rebinds) — **no threads yet** | zero-spawn gate: timers/sockets/http LLVM corpus green; arena microbench within noise. STOP-gate milestone. |
| M3 | LLVM true threads: `lv_thread.c` trampoline, eventfd join, reap | spawn/join corpus byte-identical to oracle/IR (acceptance #5); blocked-worker isolation (acceptance #4); copy-cost benchmark published |
| M4 | `Channel<T>`: transfer allocator, ptr-slot SPSC ring, two eventfds, close/`None` | `threads/channel.lev` + ping-pong green on all three engines, byte-identical |
| M5 | `sysTcpListen/2`; Atlantis flip | acceptance #2: two workers accept on one port, loopback client sees both serve |
| M6 | leak discipline: `fuzz/thread_leak.py` (1M msgs; spawn/join churn; deep/cyclic/shared graphs through flatten) | acceptance #3 as §4.5: main live-at-exit +0B, reaps == spawns, transfer-outstanding 0, fd count flat |

**Timeline (explicit, per convention):** **Dec 1 – Dec 22, 2026** (ahead of AG-6).
M1 4d (flatten engine is the new core — front-loaded where the interpreters can debug it),
M2 3d, M3 4d, M4 4d, M5 2d, M6 3d. M2 is the STOP gate; M1's flatten engine de-risks
everything after it on the serial engines first.

## 11. STOP conditions (Sonnet escalate-don't-improvise)

- M2's perf/corpus gate fails and the fix isn't a local TLS-model or init-order change —
  STOP (LLVM/portable-backend owner).
- Any temptation to reintroduce **move/reparent** ("the copy is slow, let's just hand the
  pointer over") — that is F-1/F-2/F-3 again; STOP and escalate with the M3 benchmark
  numbers instead.
- Any temptation to make default refcounts atomic — v2-tier decision, STOP (kept).
- Any temptation to touch X64Gen/X64.hpp — frozen, threads are LLVM-only (kept).
- Differential passes only after sorting/normalizing output — isolation leaked; STOP, fix
  flatten, never relax the test (kept).
- A corpus program "needs" a closure or fd to cross the boundary — the answer is
  restructure the test (workers open their own), never widen §6's predicate ad hoc.

## 12. Deferred / open (v2+)

- **Atomic shared-immutable tier** — zero-copy fan-out for provably-immutable
  strings/collections; under D1′ it is a pure addition (a third case in the flatten walk:
  emit a pointer + atomic retain instead of bytes). Argued from M3's benchmark numbers.
- **Closure transfer** — flattenable closures (captures all flattenable, no fd/loop
  reachability) would let workers be handed continuations; needs a capture-transitive
  predicate; deliberately out of v1 (§6).
- **Multi-producer / select**; **cancellation** (with `request-promise-rejection.md`);
  **work-stealing pool** — all library layers over `spawn`/`Channel`, kept out of the
  primitive (as in the original).
- **Windows** — `lv_plat_win32.c` twin (CreateThread + event handle for eventfd's role);
  the `LvThreadCtx`/flatten layers are already platform-neutral.

## 13. Reference-doc duty

- reference §6.6.5: `sysTcpListen/2`, `sysThread*`, `sysCpuCount`.
- reference new §6.6.6x: `spawn`/`Worker<T>`/`Channel<T>`, the **flattenable** rules (§6
  table verbatim), the copy semantics ("a worker sees a snapshot; sends are snapshots"),
  `receive() -> Promise<T?>` with `None` = closed.
- info.md §14: annotate as **pure isolation v1, copy-always boundaries** (per-worker heap,
  flatten/rebuild), atomic-shared-immutable flagged v2.
- `designs/atlantis/techdesign-00-overview.md`: LA-1 designed; `HttpServer(workers:
  cpuCount())` is a config flip.
- On completion: move to `designs/complete/`, implementation log filled;
  `techdesign-threads.md` moves to `designs/complete/` alongside it marked superseded (the
  review trail stays discoverable).

## 14. Implementation log

### Landed (2026-07-11)

**M2/M5 floor** (`2271272`, on master). Per-worker heap isolation groundwork: the
allocator, event-loop, and throw-state runtime globals are now thread-local
(`LV_TLS` in `lv_abi.h`/`lv_runtime.c`/`lv_loop.c`), so a future pthread lazily
mmaps its own heap/arena and gets its own loop — **per-worker heaps largely fall
out of the TLS pass** (§4.1). `LlvmGen` pins the TLS model per target format:
initial-exec on ELF (the fast `%fs`-relative arena/throw path), **non-TLS on
Windows/COFF** (threads are POSIX-only in v1; mingw emulated-TLS does not match
LLVM's native COFF TLS lowering — forcing initial-exec was a link regression,
now gated). `sysTcpListen(port, reusePort)` SO_REUSEPORT overload +
`std::cpuCount()` (`sysCpuCount`) across interpreters + LLVM; the frozen ELF
backend emits a loud "unsupported" diagnostic for the /2 overload rather than
silently dropping `reusePort` (§5). Comptime hermeticity holds automatically
(`sys*` prefix deny-set).

**M1 — spawn/Worker/join on oracle + IR** (`58eea38`, on master). `class
Worker<T> : Promise<T>` and `Worker<T> spawn<T>(() => T body)` in the prelude;
`await w` is the join (the Checker's await-unwrap now walks base classes so a
`Worker<int>` unwraps to `int`). The **flatten/rebuild engine** is
`T sysThreadTransfer<T>(T value)` over `lvThreadCopy()` in `RuntimeNatives.cpp`:
on the single-heap interpreters, a deep copy with a seen-map so shared
substructure and cycles copy once (diamond in → diamond out; terminates on
cycles), shared by both engines so post-copy graphs — and all output — are
identical by construction (the acceptance-#5 differential). The §6 predicate is a
runtime backstop here: nested closures, fd-/loop-carriers, and Block are rejected
with a catchable RuntimeException; Worker/Channel/Promise handles are portal
values (shared, not copied). Corpus `tests/corpus/threads_serial/spawn_join.lev`
(treewalk + IR) covers value return, object + primitive capture snapshots
(acceptance #1), chained worker-awaits-worker (F-5 no-deadlock), and both loud
non-flattenable errors — byte-identical across oracle and IR.

**M4 — Channel<T> on oracle + IR** (`5706ecf`, on master). `class Channel<T>` as
an in-process queue on the one cooperative loop: `send` copies the value in
(`sysThreadTransfer`), hands it to a parked receiver or enqueues it; `receive()`
is `Promise<T?>` resolving with the next item or `None` once closed AND drained
(F-8). A captured Channel handle is a portal (§6). `OverflowPolicy` is carried as
int constants (`overflowBlock/Drop/Error`) — `grow` rejected (F-6); `block`
degrades to unbounded enqueue on the serial engines (a running task can't park
mid-body), `drop`/`error` enforce capacity at send. Corpus
`tests/corpus/threads_serial/channel.lev` covers ping-pong drain-to-`None`,
struct copy-in isolation, overflow-error, and closed-send — byte-identical.

**Deviations from this design, recorded honestly:**
- **`OverflowPolicy` is int constants, not the named enum** — enum desugaring
  (Track 03) runs on the user program only, not the prelude, so a prelude enum's
  member access does not resolve in user code. Reverts to the enum surface once
  prelude-enum desugaring lands.
- **`block` overflow degrades to enqueue on the serial engines** — no true
  producer backpressure without mid-body parking; output is unaffected (sends are
  copies), memory behavior differs. The true ring is the LLVM leg.
- **A `Promise<T?>` (nested-optional generic) class field segfaults construction**
  on both interpreters (pre-existing; bug.md #1). `Channel.waiters` is stored as
  raw `Array<Promise>` (instantiation-compatible) as the workaround.

### Landed — M3/M6 (true OS threads on LLVM, 2026-07-11)

The M3/M6 execution leg landed in full from **`techdesign-threads-3.md`** (see its
implementation log for the milestone-by-milestone commit refs and deviations).
`spawn`/`Worker<T>`/join and `Channel<T>` now run on **true OS threads on the LLVM
backend** — a worker is a real `pthread` with its own TLS heap/arena/loop, values
cross by a C flatten/rebuild engine, `Channel` is a process-global lock-free SPSC
ring + two `eventfd`s, and the join is an `eventfd` watch that rebuilds+reaps on
the spawner's thread. Byte-identical to the serial oracle across every shared
program. `fuzz/thread_leak.py` proves no leaked worker heaps, transfer buffers, or
eventfds as N grows (spawn/join, channel, adversarial-graph churn all +0B, reaps
== spawns, transfer-outstanding == 0). The historical escalation record below is
kept for the design trail.

> **Update (2026-07-11, same day):** the escalation below was answered by a
> completing design — **`techdesign-threads-3.md`** specifies the full M3/M6
> execution leg (the `sysThreadStart`/`sysThreadResult` capability-probe seam,
> the C flatten/rebuild buffer format, `lv_thread.c`'s trampoline/join/reap with
> the `lv_rt_init`-argv-clobber guard, the Channel native ring, the Windows
> compile-time reject, the amended corpus plan, and the M6 harness spec). The
> architecture sketch below is kept for the record; **doc-3 is the
> implementation source.**

**M3 — true OS threads on LLVM.** The blocker is structural and worth stating:
the interpreter runs a worker as a **cooperative language closure on its loop**;
LLVM must run the body on a **separate pthread** and resolve the promise back on
the *spawner's* loop via eventfd. These do not unify as shared prelude code — the
cooperative `(n) => w.resolve(...)` callback conflates body-run and resolve, but
on a real thread the resolve must happen on the spawner's thread, not the
worker's (else it races `w`'s non-atomic refcount — the very race D1′ exists to
prevent). So `spawn` must become an **engine-specific native/intrinsic** (design
§4.2's `spawn` → `lv_thread_spawn` lowering), not the shared prelude function M1
landed. Concretely the remaining work is:

1. **Native seam.** Make `spawn` a native handled directly in `Eval.cpp` /
   `IrInterp.cpp` (which have object construction + closure invocation + loop
   access the shared `nativeFreeCall` lacks) so each engine controls scheduling;
   or route through a `sysThreadSpawn(body) -> joinFd` + `sysWatch(joinFd, cb)` +
   `sysThreadResult(joinFd)` seam unified over an **eventfd** (a real fd both the
   interpreter's `poll`-loop and the LLVM loop already watch). The interpreter
   side must stay byte-identical to M1's corpus.
2. **C flatten/rebuild engine.** `lvrt_systhreadtransfer` real implementation: a
   deep copy over the runtime heap graph (object static-slots + dyn-list, closure
   capture-list + `@fn`, dense-vs-boxed arrays, maps, strings, blocks) with a
   seen-map for shared/cyclic structure, rebuilding through the receiving thread's
   `lvrt_halloc` so every node is a first-class counted value in its region. The
   layout to mirror is `lv_recursive_free`'s walk (`lv_runtime.c:240-317`).
   Currently `lvrt_systhreadtransfer` **aborts loudly** (never silent-wrong) — a
   spawn/Channel program built with `--build-native` stops with a clear "M3
   pending" message (verified: exit 1); it is dead code for any program that
   never spawns, so `corpus_llvm_full` stays green.
3. **`lv_thread.c` (new).** pthread trampoline (installs the TLS ctx — heap/arena/
   loop come free from the M2 TLS pass — rebuilds captures, invokes the body via
   the dispatch trampoline, flattens result, signals eventfd), eventfd join
   registered on the spawner's loop *before* `pthread_create` (lost-wakeup order,
   problem #3), and reap (`pthread_join` + munmap the worker regions **after** the
   result is rebuilt into the spawner's heap — problem #4/F-2).
4. **LlvmGen lowering** for the seam natives + the M2 STOP-gate perf check (arena
   microbench within noise — initial-exec TLS is one `%fs` load, expected under
   noise but measured, not assumed).
5. **Differential** (acceptance #5): LLVM true-thread output byte-identical to the
   serial corpus, never sorted/normalized; blocked-worker isolation (acceptance
   #4) — only meaningful once real threads exist, so it is an LLVM-only test.

**M6 — leak harness** (`fuzz/thread_leak.py`) depends on M3: it asserts the main
thread's `lvrt_live_bytes()` +0B at exit, reaps == spawns, transfer-allocator
outstanding == 0, and `/proc/self/fd` flat — all quantities the LLVM/native path
defines. Not meaningful on the shared_ptr-based interpreters.

**Why stopped here:** M3 is this doc's designated STOP condition (§11 — "any
temptation to reintroduce move/reparent … STOP"; the loop/TLS-rework gate). The
TLS gate itself is *passed* (M2 landed green, `corpus_win_wine` included), but the
true-thread runtime is a multi-day, high-threading-risk build that would refactor
the landed, tested M1/M4. Shipping a half-working pthreads runtime — or a silent
cooperative-on-LLVM fallback — would violate the no-silent-wrong / no-improvised-
design discipline. Escalated with the plan above rather than improvised.
