# Track 10 — True Threads / Workers (responds to `request-threads.md`, LA-1)

> **SUPERSEDED (2026-07-11) by `techdesign-threads-2.md`** — adversarial review refuted the
> §2 move/reparent transfer model on three grounds (ARC's `lv_in_region` gate makes
> cross-heap values invisible to the receiver; §7#4's join-time heap teardown dangles moved
> results; use-after-move is unenforceable without alias analysis the language rejects) and
> found the §4.2 "one hook in lv_loop.c" TLS scope understated. The competing design keeps
> this doc's surface, backend matrix, per-worker loops, SPSC ring, and SO_REUSEPORT floor,
> and replaces the boundary semantics with copy-always flatten/rebuild. **Implement from
> `techdesign-threads-2.md`, not from this document.** (Trail: doc-2's M1/M2/M4/M5
> landed 2026-07-11; its M3/M6 execution leg is specified in `techdesign-threads-3.md`.)

**Requested by:** Atlantis framework (Tracks 01 + 05), 2026-07-06. **Priority:** P1, wanted
by ~AG-6 (2026-12); not blocking earlier Atlantis gates.
**Substrate:** info.md §14 (two tiers, shared address space, isolation by default), §13
(streams/event loop), §15 (ownership + refcount tiers). **Backend reality:** X64Gen/ELF is
**frozen** (see `feedback_x64gen-frozen`); LLVM is the real target for true threads. Oracle/IR
stay the semantic reference and run workers *serially*.

This design answers the request's own §2 ("known hard part") by **choosing the second option it
lists** — per-worker heaps + ownership transfer at every boundary — and building the whole
surface on top of that one decision.

---

## 0. Position, scope, and file ownership

Track 10 is the concurrency execution layer. It owns:

- `src/Resolver.cpp` prelude additions (the `spawn`/`Worker`/`Channel` surface + `sysTcpListen`
  overload) — **append-only**, no edits to Track 08/09 prelude blocks.
- `src/LlvmGen.cpp` — a new lowering region for the thread/loop/channel intrinsics (guarded so a
  program that spawns nothing emits byte-identical code to today).
- `runtime/lv_thread.c` (**new file**) — pthread creation/join, per-thread loop bootstrap,
  eventfd join signalling, the SPSC ring. Disjoint from `lv_loop.c` (which it *calls into*, not
  edits, beyond one documented hook — §4.2).
- `src/RuntimeNatives.cpp` — the oracle/IR serial-worker shims (new `sysThread*` names).
- `tests/corpus/threads/` (**new dir**) + `fuzz/thread_leak.py` (**new**).

**Explicitly not owned / not touched:** `X64Gen.cpp`, `X64.hpp` (frozen — Track 10 excludes the
ELF backend entirely, §5); any Track 08/09 socket or spawn code beyond the one additive
`sysTcpListen` parameter; the churn harness (Track 10 adds a *sibling* leak harness, does not
edit `fuzz/churn_leak.py`).

**Engine matrix (the R-5 acceptance #5 promise, made concrete):**

| engine | worker execution | why |
|---|---|---|
| tree-walk oracle | **serial** (run the closure inline, resolve the promise immediately) | semantic reference; scheduling must not change results |
| IR interpreter | **serial** (same) | differential twin of the oracle |
| **LLVM** | **true OS threads** (pthreads, per-thread event loop) | the actual deliverable |
| ELF / X64Gen | **excluded** | frozen; `spawn` on ELF is a compile-time "unsupported on this backend" diagnostic, not silent serial execution |

The differential contract (acceptance #5) holds **because** of the isolation model in §2: with
no shared mutable state, a worker's result cannot depend on interleaving, so "run them serially"
and "run them on real threads" are observationally identical. Isolation is what *lets* the oracle
stay simple.

---

## 1. Requirement → mechanism map

| # | request | mechanism (this doc) |
|---|---|---|
| R-1 | `spawn(closure) -> handle`; captures by copy/ownership, never shared mutable ref | §3.1 surface, §6 capture analysis; per-worker heap (§2) |
| R-2 | handle is `Promise<T>`-shaped; `await handle` is the join | §3.2 — join rides the existing one-shot-stream promise + an eventfd watch |
| R-3 | an event loop per worker (timers/sockets/`await` independently) | §4.2 — thread-local `RuntimeLoop`; the single-loop program is the 1-worker case |
| R-4 | cross-thread `StreamBuffer<T>` as SPSC channel; sanctioned comms path | §3.3 + §4.3 — `Channel<T>`, move-only, lock-free ring over eventfd |
| R-5 | shared listener; `sysTcpListen(port, reusePort: true)` (SO_REUSEPORT) | §3.4 + §4.4 — additive native param; share-nothing accept loops |

---

## 2. The core decision: pure isolation (per-worker heap, move-or-copy everywhere)

The request's §2 names the blocker: **escaping-tier refcounts are not atomic** (§15 — every
heap object/array/string/closure/map carries a *non-atomic* count, retain/release runs on every
slot write). Two threads touching one counted value race the count → use-after-free or leak. Two
ways out; the request lists both. **We take option 2.**

> **Decision D1 — v1 shares *nothing* counted across threads.** Each worker gets its own memory
> tier (its own scope arena + its own escaping-tier free-list/allocator, §15). Values only cross
> a thread boundary by **deep copy or ownership transfer (move)**, at exactly two places: the
> **spawn capture** (parent → worker) and the **channel/join boundary** (worker ↔ peer). A moved
> value is reparented: its refcount prefix is re-homed to the receiving thread's allocator and the
> sender abandons it (the §15 "copy the bits, delete the source" reality, applied across a thread
> boundary). Because a counted value is *live on exactly one thread at a time*, **no count is ever
> touched by two threads** — today's non-atomic fast path is preserved verbatim.

Consequences, stated honestly:

- **"Immutable values share freely" (request R-1's second sentence) is *deferred to v2*, not
  delivered in v1.** Strings and pure Arrays/Maps carry non-atomic counts too, so v1 *copies*
  them into the worker rather than aliasing. This is a real cost (a captured 1 MB string is
  copied per worker) and it is the price of not touching the refcount fast path. The request's §2
  explicitly accepts this ("constrains R-4 to transfer-or-copy semantics, which Atlantis is happy
  with"). v2 (§11) adds an atomic-count *immortal/shared* tier for genuinely-immutable values that
  want zero-copy fan-out; it is isolated to those values and does not re-atomicize the default
  path.
- **Shape-table mutation across threads is forbidden in v1** (info.md §14's "sharpest
  interaction"). Since nothing counted is shared, a worker cannot even *reach* a peer's object to
  add a dynamic property. Interceptable/dynamic-shape objects are non-transferable across the
  boundary (compile error at the capture/send site, §6). Statically-shaped objects transfer fine
  (their shape lives in the shared, read-only shape registry, never mutated after warm-up).
- **The scope-owned tier needs no change.** Scope-owned allocations never escape their frame, so
  they never cross a thread. Per-thread arenas are just N copies of the existing per-frame arena
  machinery, one root per pthread.

This one decision (D1) is what makes every later section small.

---

## 3. Language surface (all prelude-authored over a thin native floor)

Nothing here is new syntax. `spawn` is a free function; `Worker`/`Channel` are prelude classes;
join is `await`. The framework-facing shape:

### 3.1 `spawn` — start a worker (R-1)

```
// prelude, std namespace
Worker<T> spawn<T>(() => T body);
```

- Returns a `Worker<T>` handle. `Worker<T> : Promise<T>` (§3.2) so it *is* awaitable — no second
  suspension surface (the framework's one-model rule, R-2).
- `body` is a **captureless-by-reference** closure: the compiler rewrites its free-variable
  captures into a **capture record that is copied/moved into the worker heap at spawn time** (§6).
  A capture the compiler cannot copy-or-move (a live shared mutable object, an interceptable
  object) is a **compile error** at the `spawn` site — the gate (§16) made loud and local.
- Mutating a captured original *after* `spawn` is not observed by the worker (acceptance #1) —
  because the worker holds a copy, by construction.

### 3.2 Join as a promise (R-2)

`Worker<T>` derives from `Promise<T>` and adds nothing to the *await* surface:

```
Worker<int> w = spawn(() => expensive());
int result = await w;     // parks THIS loop only (acceptance #1), not the process
```

`await w` parks the spawning thread's event loop (§4.2) on the worker's completion eventfd; when
the worker's `body` returns, its result is **moved** across the boundary (§2) and the promise
resolves with the reparented value. A deliberately-blocked worker parks only its own loop and the
awaiter; siblings keep serving (acceptance #4) because each has an independent loop and heap.

Errors: an uncaught throw inside `body` resolves the worker-promise as **rejected** — carried as
the `IException` value moved across the boundary, surfacing at the `await` as a rethrow (consistent
with §12.6; ties into `request-promise-rejection.md`, which this doc depends on for the reject
channel — if that lands first, reuse it; if not, Track 10 ships a minimal reject slot and the two
reconcile).

### 3.3 Cross-thread channel (R-4)

```
class Channel<T> {                      // prelude; an SPSC StreamBuffer<T> variant (§13)
    new Channel(int capacity, OverflowPolicy policy);   // §13: block/drop/grow/error
    void send(T value);                 // MOVE: value is transferred out of this thread
    Promise<T> receive();               // parks the receiver's loop until an item arrives
    void close();                       // sender signals end; receiver sees a closed signal
}
```

- **One producer, one consumer** — the §13 lock-free ring over atomic head/tail. Multi-producer is
  *not* a substrate property (same stance as broadcast in §13): fan-in is N channels + a select,
  not a shared many-writer queue.
- `send` is **move-only** (D1): the value leaves the sender's heap and is reparented into the
  receiver's on the consuming side. This is the "send = move, like the capture rule" the request's
  §2 calls for. A non-transferable value (dynamic-shape object) is rejected at the `send` call site.
- `receive()` returns a `Promise<T>` so it composes with `await` and the loop — an empty channel
  parks the consumer rather than busy-waiting (contrast the §13 in-memory-stream "pull on empty is
  a loud error": a *cross-thread* channel blocks/parks, as §13 anticipates for system streams).
- The ring's head/tail are the *only* atomically-accessed words in the whole design (§4.3) — the
  concession is contained to the channel substrate, exactly where §13 already planned for it.

### 3.4 Shared listener (R-5)

```
int sysTcpListen(int port);                          // existing (Track 08 floor)
int sysTcpListen(int port, bool reusePort);          // NEW overload — SO_REUSEPORT
```

- Additive overload (resolution-by-type picks it; §9). `reusePort: true` sets `SO_REUSEPORT`
  before `bind`, so **N workers each open their own full accept→serve loop on the same port with
  zero shared state** (acceptance #2). This is the request's preferred "simpler and share-nothing"
  shape over an acceptor-plus-handoff primitive.
- `HttpServer(port, workers: N)` (framework side) becomes: spawn N workers, each runs
  `TcpListener(port, reusePort: true)` on its own loop. No connection ever crosses a worker; the
  kernel load-balances accepts. The MySQL pool's per-worker sub-pools fall out for free
  (connections are worker-local, so no locking — request §3).
- LLVM only. On the frozen ELF backend `sysTcpListen/2` is not emitted (§5); the existing
  `sysTcpListen/1` is untouched.

---

## 4. Runtime mechanism (LLVM backend, `runtime/lv_thread.c`)

### 4.1 Thread creation & the worker entry

`spawn` lowers to `lv_thread_spawn(body_fnptr, capture_record, result_slot*)`:

1. Allocate a `WorkerCtx` (its own escaping-tier allocator root + scope-arena root + a fresh
   `RuntimeLoop`, §4.2 — a heap-owned struct, freed on join).
2. `pthread_create` with a trampoline that: installs `WorkerCtx` as thread-local (§4.2), runs the
   `body` closure over the *reparented* capture record, writes the return value into the worker's
   result slot, then **signals the join eventfd**.
3. Return a `Worker<T>` whose promise is wired to that eventfd (registered on the *spawner's* loop).

The child never returns into shared interpreter state (contrast Track 08 F7's fork hazard — here
there is no fork, no address-space clone; a pthread shares code and the read-only shape registry
only). The trampoline is the sole entry; on `body` throw it writes the exception into the reject
slot and still signals the eventfd (never leaves the awaiter parked forever).

### 4.2 Per-worker event loop (R-3)

Today's `lv_loop.c` owns one process-global loop. Track 10 makes the loop **thread-local**:

- One documented hook in `lv_loop.c`: the global loop pointer becomes `__thread RuntimeLoop*
  g_loop` (thread-local storage). A program that never spawns has exactly one loop on the main
  thread → **byte-identical behavior** (problem #2 gate below). This is the *only* edit to
  `lv_loop.c`; all new logic lives in `lv_thread.c`.
- Each `WorkerCtx` carries its own `RuntimeLoop` (its own epoll/kqueue fd set, timer heap, watch
  registry). Timers, `sysWatch` sockets, and `await` all operate on the calling thread's loop.
  The existing single-loop lifetime rule (run while live work remains) applies per worker; a
  worker exits when *its* loop drains and `body` has returned.
- `await` on a `Worker`/`Channel`/`Promise` parks the **current** thread's loop only (acceptance
  #1, #4).

### 4.3 The SPSC ring & cross-thread wakeup (R-4)

- `Channel<T>` backs onto a fixed-capacity ring buffer of `T`-value slots (value size, §9 — a
  string/array slot is a handle, cheap). Head advances on the consumer, tail on the producer;
  both are `atomic_load/store` with acquire/release ordering — **the only atomics in the design**.
- Producer `send`: reserve a tail slot (respecting `OverflowPolicy`: block parks the sender's
  loop / drop discards / grow reallocs / error throws), memcpy the value's bits in, reparent
  ownership to the channel (the value is now owned by whichever thread consumes it — the transfer
  point), `atomic_store` the tail, then **write the consumer's wakeup eventfd** if the consumer is
  parked.
- Consumer `receive`: if empty, register the channel's eventfd on its loop and park; on wakeup,
  `atomic_load` head, take the slot, reparent into the consumer's heap, advance head. The eventfd
  is the cross-thread wakeup — no condition variables, no shared mutex, staying inside the "loop
  watches an fd, runs a callback" model (§13) the whole runtime already speaks.
- Acceptance #3 (1M messages, `live-at-exit` flat) is a `fuzz/thread_leak.py` target: because
  every send is a move and every receive a reparent, no value is double-counted or orphaned; the
  ring is fixed-size; the +0B discipline is the churn corpus's, re-proved across the boundary.

### 4.4 SO_REUSEPORT floor (R-5)

`sysTcpListen(port, reusePort)` — in `RuntimeNatives.cpp` (interpreters) and `LlvmGen.cpp`
(native): `setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, ...)` between `socket()` and `bind()`.
Everything else is unchanged from Track 08's floor. Kernel ≥ 3.9 (dev kernel 6.17 — fine; a P-probe
formality). The bare `sysTcpListen(port)` path keeps its exact current codegen.

---

## 5. Backend differential & the freeze

- **oracle/IR:** `sysThreadSpawn` shim runs `body` **inline and serially**, resolves the promise
  immediately; `Channel.send`/`receive` are an in-process queue on the single loop; `reusePort` is
  accepted and ignored (one process, loopback). These engines are the *semantic reference* — their
  serial output is the golden file the LLVM true-threaded run must match byte-for-byte (acceptance
  #5). Legal precisely because D1 makes results scheduling-independent.
- **LLVM:** true pthreads + per-thread loops + SPSC rings, as §4.
- **ELF / X64Gen:** **excluded and frozen.** `spawn`/`Channel`/`sysTcpListen/2` on the ELF backend
  emit a clean compile-time diagnostic (`"threads: unsupported on the ELF backend (use LLVM)"`),
  never a silent serial fallback (a silent fallback would be the §16 silent-distant footgun). No
  new X64Gen code, per `feedback_x64gen-frozen`. This mirrors Track 04/05's "exclude ELF, isolated
  corpus dir" precedent from the overview roadmap.

---

## 6. Compiler: capture & transferability analysis

`spawn`'s closure and `Channel.send`'s argument both cross a thread boundary, so the compiler must
classify every value that crosses:

| value kind | at the boundary |
|---|---|
| primitive (`int`/`bool`/`float`/`char`) | copied (value type, trivial) |
| `struct` value | deep-copied (already copy-semantics, §9) — free |
| heap string, pure `Array`/`Map` | **v1: deep-copied** into the receiver heap (D1); v2: shareable atomic-immortal |
| statically-shaped `class` object | **moved** (ownership transfer) — sender abandons it; a use-after-move on the sender is a compile error (reuses the §15 return/move liveness tracking) |
| interceptable / dynamic-shape object | **rejected** — compile error at the capture/send site (§2: shape mutation across threads forbidden v1) |
| a live `Channel<T>` handle | moved (so a worker can be handed its endpoint) — a channel has exactly one producer end and one consumer end, each owned by one thread |

The capture record is synthesized like a closure environment (the mechanism already exists —
§16.5 closures capture; bug 29 handled prelude self-call capture). The new work is the
**transferability check** (is every captured/sent value copy-or-move-able?) and emitting the
reparent glue. Move-liveness reuses §15's existing "deleted after return" stack-boolean machinery,
generalized to "abandoned after spawn/send."

---

## 7. Foreseeable problems & solution strategies

| # | problem | strategy |
|---|---|---|
| 1 | **Non-atomic refcount race** if any counted value is reachable from two threads. | D1: nothing counted is shared; move-or-copy at the two boundaries; move reparents the count to the receiver's allocator. The transferability check (§6) is what *enforces* it — a value that would be shared is a compile error, not a runtime race. |
| 2 | **Thread-local loop edit destabilizes the single-loop corpus** (timers/sockets/http on LLVM). | The only `lv_loop.c` change is `g_loop` → `__thread`. Guarantee: zero spawns → one main-thread loop → identical code path. Snapshot the timers/sockets/http LLVM corpus green *before* the edit; it is the regression net. If the loop can't take TLS-ification without structural rework, **STOP** (loop rework is portable-pivot/LLVM-owner territory). |
| 3 | **`await` cross-thread wakeup lost** (worker signals eventfd before spawner registers the watch → deadlock). | Create + register the join eventfd on the spawner's loop *before* `pthread_create`; the eventfd is level-triggered so a signal that precedes the park is still readable when the loop next polls. Corpus: spawn a worker that returns instantly, assert the await completes. |
| 4 | **Result/exception ownership on join** — who frees the worker's result, and the `WorkerCtx`. | Result is *moved* to the awaiter (reparented at +1, §15 return-transfer contract). `WorkerCtx` (loop, arenas, allocator) is freed by `pthread_join` inside the join handler once the eventfd fires — the join is what reaps the thread and its heap. Leak-corpus asserts +0B and no leaked pthread. |
| 5 | **Channel overflow / slow consumer** stalls or grows unbounded. | The §13 forced pairing: capacity comes *with* an `OverflowPolicy` (block/drop/grow/error) — no unbounded default. `block` parks the producer's loop (backpressure), making a slow consumer *visibly* that consumer's problem (§13 stance). |
| 6 | **Differential drift** — LLVM true-threaded output differs from serial oracle. | If it differs, isolation (D1) has a hole — treat any drift as a *correctness bug in transferability*, not a scheduling tolerance. The test never sorts/normalizes output to hide interleaving; a worker's contribution is deterministic by construction, so equality is exact. |
| 7 | **Comptime hermeticity** — new `sysThread*`/`sysTcpListen/2` natives must be denied at compile time (§16.5 oracle is hermetic). | Add every new native to the comptime deny-set **in the same commit** (the Track 08 problem #6 checklist — one forgotten name breaks hermeticity silently). |
| 8 | **Cross-thread reference cycles** (§15 tail) — a cycle spanning two heaps. | Out of scope, same asterisk as single-thread cycles (§19 #10). D1 actually *shrinks* the surface: a cycle can't span heaps (nothing is shared), so cross-thread cycles are impossible by construction; only within-worker cycles exist, identical to today. |

---

## 8. Milestones & acceptance

| M | deliverable | accept |
|---|---|---|
| M1 | `spawn`/`Worker<T>`/join surface on **oracle + IR** (serial); capture record + transferability check + reparent-on-move (compiler) | `threads/spawn_join.lev`: worker returns a value; mutating the captured original after spawn is not observed (acceptance #1); `await` yields it |
| M2 | LLVM: `lv_thread.c` pthread spawn + `__thread` loop (problem #2 gate) + eventfd join | same corpus green on LLVM, **byte-identical to oracle/IR** (acceptance #5); single-loop timers/sockets/http corpus unregressed |
| M3 | `Channel<T>` SPSC ring + move-send/park-receive on all three engines | `threads/channel.lev`: producer→consumer, values reparented; interim serial on interpreters, true ring on LLVM |
| M4 | `sysTcpListen(port, reusePort)` overload (interpreters + LLVM); framework spawns N accept loops | acceptance #2: two workers accept on one port (SO_REUSEPORT), loopback client sees both serve |
| M5 | Leak discipline: `fuzz/thread_leak.py` — 1M messages through a channel, spawn/join churn | acceptance #3: `live-at-exit` +0B, no leaked pthread/fd (assert via `/proc/self/fd` + the mem-verify oracle) |
| M6 | Blocked-worker isolation + reject channel | acceptance #4: a `while(true)` worker does not stall a sibling's request handling; an uncaught worker throw rethrows at `await` |

**Timeline (explicit, per techdesign convention):** target **Dec 1 – Dec 20, 2026** (ahead of
AG-6). M1 3d, M2 4d (the pthread/TLS-loop core — the risk), M3 3d, M4 2d, M5 2d, M6 2d. M2 is the
STOP-gate milestone; if the loop won't TLS-ify cleanly, escalate before proceeding.

---

## 9. Reference-doc duty

- reference §6.6.5 (sys floor table): add `sysTcpListen/2`, the `sysThread*` intrinsics.
- reference new §6.6.6x: `spawn`/`Worker<T>`/`Channel<T>` surface + the capture/transfer rules.
- info.md §14: annotate that the two-tier model is **implemented as pure isolation v1** (per-worker
  heap, move-or-copy), with atomic-shared-immutable flagged as v2 — keep §14's prose truthful to
  what shipped.
- `designs/atlantis/techdesign-00-overview.md`: mark LA-1 as designed; note `HttpServer(workers:N)`
  is now a config flip (request §3).
- On completion, move this doc to `designs/complete/` (the landed-designs convention) with the
  implementation log filled.

---

## 10. STOP conditions (Sonnet escalate-don't-improvise protocol)

- **Problem #2's loop-rework trigger** — if `lv_loop.c` cannot become thread-local behind the
  zero-spawn-identical guarantee without structural surgery, STOP (LLVM/portable-backend owner
  territory).
- **Any temptation to make the default refcount atomic** — that re-taxes the §15 fast path the
  whole language is built on; it is a v2 *opt-in tier* decision, not a v1 slide. STOP and escalate.
- **Any temptation to touch X64Gen/X64.hpp** — frozen; threads are LLVM-only. If a corpus seems to
  *need* ELF threads, STOP (the answer is "exclude ELF," never "unfreeze").
- **Differential output that only passes after sorting/normalizing** — that means isolation leaked;
  STOP and fix transferability, do not relax the test.

---

## 11. Deferred / open (explicitly out of v1)

- **Atomic shared-immutable tier (v2)** — zero-copy fan-out of genuinely-immutable strings/pure
  collections via an atomic-count *immortal* tier, isolated to those values so the default path
  stays non-atomic. Delivers R-1's "immutable shares freely" as an optimization once v1's copy
  cost is measured to matter.
- **Cancellation** (info.md §14 flags it cross-cutting) — a `Worker.cancel()` / cooperative
  cancellation token threads through async/stream signatures; designed with `request-promise-
  rejection.md`, not here.
- **Multi-producer / select** over channels — v1 is strict SPSC (§13 stance); fan-in is N channels
  + a loop-level select, a later library layer.
- **Work-stealing / thread pool** — v1 is explicit `spawn` per worker (the framework spawns exactly
  N for N cores). A managed pool is a library over `spawn`, not a language primitive.
- **Windows** — `lv_thread.c` is POSIX-first (pthreads, eventfd); the `lv_plat_win32.c` twin
  (CreateThread + a completion handle) is a follow-up, gated behind the same interface.

---

## 12. Implementation log

*(empty — to be filled as milestones land, per the Track 08/09 log convention)*
