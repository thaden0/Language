# Track 10 — True Threads M3/M6: the execution leg (completes `techdesign-threads-2.md`)

**Status:** completing design, 2026-07-11. This document **completes** — does not
supersede — `techdesign-threads-2.md` (doc-2): D1′ (pure isolation, copy-always
flatten/rebuild boundaries) stands unchanged, as do the surface, the backend
matrix, and the SPSC substrate. What this doc adds is the piece doc-2
under-specified and the M1/M2/M4 implementation proved needs its own design: how
one engine-shared prelude `spawn` drives a **cooperative task on the
interpreters and a real pthread on LLVM** without either a silent fallback or an
engine-forked prelude. It also **amends doc-2 §6 on one point** (Worker/Promise
handles are NOT transferable — §1 A-1, a v1 soundness fix), and records three
runtime facts the implementation established that the M3 build must respect.

**Prior art trail:** `techdesign-threads.md` (move model — refuted 2026-07-11) →
`techdesign-threads-2.md` (copy-always — M1/M2/M4/M5 landed from it; see its §14
implementation log) → this doc (M3/M6). All three move to `designs/complete/`
together when M6 lands.

---

## 0. Position and file ownership

Same track, same owner as doc-2. M3/M6 touches, disjoint from other tracks:

- `runtime/lv_thread.c` (**new**) — WorkerCtx, pthread trampoline, join/reap.
- `runtime/lv_runtime.c` — the C flatten/rebuild engine (a new §-block beside
  `lv_recursive_free`, whose per-tag layout walk it mirrors), the transfer
  counters, and one guard in `lv_rt_init` (§5.4).
- `runtime/lv_abi.h` — seam declarations; `runtime/lv_loop.c` — none expected
  (POLLNVAL auto-cancel already exists — verified, `lv_loop.c:215-233`).
- `src/LlvmGen.cpp` — lowering for the seam natives; the Await amendment (§3.3).
- `src/RuntimeNatives.cpp` — interpreter seam implementations; the §1 A-1
  predicate change. `src/Eval.cpp` / `src/IrInterp.cpp` — the Await amendment.
- `src/Resolver.cpp` — prelude `spawn`/`Worker`/`Channel` rewrites (§3).
- `tests/corpus/threads_serial/` (amend per A-1), `tests/corpus/threads_native/`
  (**new**, LLVM lane), `fuzz/thread_leak.py` (**new**).
- **Not touched:** `X64Gen.cpp`/`X64.hpp` (frozen; ELF already rejects the
  surface loudly), the churn harness.

---

## 1. What the implementation established, and two amendments

### The landed substrate this doc builds on (doc-2 §14 log has commit refs)

- Runtime hot state is thread-local (`LV_TLS`); **a fresh pthread lazily mmaps
  its own heap/arena on first allocation** via `lv_ensure_init` — per-worker
  heaps largely fall out. Zero-spawn corpus + wine lane green; ELF and the /2
  listen overload reject loudly; comptime `sys*` deny is automatic.
- `sysThreadTransfer<T>(T) -> T` is the one boundary mechanism, landed on the
  interpreters (deep copy, seen-map, cycles preserved) with the §6 predicate as
  a runtime backstop; on LLVM it currently **aborts loudly if reached** and is
  dead code otherwise (verified: spawn program exits 1 with a clear message;
  `corpus_llvm_full` green).
- The generic-native pattern `T sysX<T>(T)` is verified through the checker and
  BOTH lowerers (a sibling track hit an IR-lowering failure on a
  generic-return-with-generic-arg native shaped differently; the landed
  `sysThreadTransfer` shape is the proven one — new seam natives follow it or
  return `int`).

### Three runtime facts M3 must respect (verified in source this session)

- **F-i.** `lv_rt_init` unconditionally writes `g_argc/g_argv`
  (`lv_runtime.c`, first line of the function). A worker thread entering the
  lazy path (`lv_ensure_init -> lv_rt_init(0, NULL)`) would **clobber the
  process-global argv to NULL** — `sysArgs` would silently degrade for the whole
  process after the first spawn. The worker bootstrap therefore must NOT call
  `lv_rt_init`; §5.4 specifies a dedicated `lv_thread_ctx_init()` that
  initializes only the TLS members (heap, arena, freelist, accounting, throw
  state) and leaves process-globals alone.
- **F-ii.** `lv_loop.c` already auto-cancels watches on closed fds
  (`LV_POLLNVAL`, fire-first-cancel-after — `lv_loop.c:215-233`), matching the
  interpreters' `RuntimeLoop.cpp:84-99`. The join-fd teardown in §5.3 leans on
  this: closing the eventfd after the result is consumed retires its watch on
  both engine families with no new mechanism.
- **F-iii.** argv/env and the class-info table are process-global and
  written-once-then-read — correct to share; only the mutable hot state is TLS.
  Nothing else needs converting.

### Amendment A-1 (to doc-2 §6): Worker/Promise handles are NOT transferable

Doc-2 §6 listed `Channel<T>` / `Worker<T>` handles together as "portal values."
For `Channel` that is right (§6 here keeps it — its record is designed for two
threads, §6.1). For `Worker`/`Promise` it is **unsound under D1′**, and the M1
implementation made the reason concrete: `Promise.resolve(v)` **invokes the
stored continuation inline** (`cont` — the prelude's `then`/await wiring). A
worker resolving a promise owned by the spawner would execute a spawner-heap
closure **on the worker's thread** — cross-thread closure invocation against
non-atomic refcounts, i.e. exactly the race D1′ exists to prevent. It stayed
latent in M1 only because the serial engines have one thread. The rule, now
uniform across ALL engines so dev behavior never diverges from native:

> **A `Worker<T>` or `Promise<T>` value may not cross a thread boundary** —
> capturing one in a spawn body or sending one through a channel is a loud,
> catchable error naming the type ("pass a Channel"). `Channel<T>` remains the
> one sanctioned cross-worker conduit (that is R-4's whole point).

Mechanical change: in `RuntimeNatives.cpp`, `lvThreadIsPortal` keeps **Channel
only**; Promise-derived classes move to the reject set (reusing the same
base-walk). The landed `threads_serial/spawn_join.lev` case 4 (worker awaits a
captured sibling Worker) is **amended**: the capture becomes the loud error this
rule prescribes, and the no-deadlock property it demonstrated is re-demonstrated
through a channel (`channel.lev`'s ping-pong already does; a chained variant is
added). This is a v1-soundness fix blessed here — updating the `.expected` is
not test-relaxation. (v2 may add a true cross-thread join handle; it needs a
resolve-marshalling design that does not exist in v1.)

### Amendment A-2 (resolving doc-2 §3.1 vs §4.2): the capability-probe seam

Doc-2 said both "spawn is prelude-authored" (§3.1) and "spawn lowers to
`lv_thread_spawn`" (§4.2) — a latent contradiction, since one prelude body
lowers identically on every backend (the M1 lesson: prelude code cannot branch
per engine, and prelude METHODS lower eagerly on LLVM even when unused, so every
native they reference needs a lowering on every active backend). The resolution
is neither an engine-forked prelude nor an intrinsic: **one prelude body over a
native capability probe** (§2). `sysThreadStart` returns a real join fd where
true threads exist (LLVM/POSIX) and `-1` where they don't (the interpreters);
the prelude takes the cooperative path on `-1`. This is not a silent fallback —
serial-cooperative IS the designed interpreter semantics (doc-2 §5's matrix),
and every backend that must reject (ELF, Windows §7) rejects at compile time.

---

## 2. The native seam

Three natives complete the floor (all `sys*` → comptime-denied automatically;
all follow the verified generic shape or return `int`):

| native | signature | oracle + IR | LLVM (POSIX) | ELF / Windows-target |
|---|---|---|---|---|
| `sysThreadTransfer` | `T sysThreadTransfer<T>(T v)` | **landed** — deep copy, seen-map, §6-predicate backstop | §4: the C flatten/rebuild engine (replaces the landed loud abort) | compile-time reject (already loud) |
| `sysThreadStart` | `int sysThreadStart<T>(() => T body)` | returns **-1** (capability probe: no true threads here) | §5: flatten captures, create eventfd, register nothing yet, `pthread_create`, return the **join eventfd** | compile-time reject |
| `sysThreadResult` | `T sysThreadResult<T>(int joinFd)` | never called (the `-1` path never registers a watch) | §5.3: drain eventfd, **rebuild result into the calling thread's heap**, `pthread_join`, munmap worker regions, close fd, free ctx; **throws** the rebuilt exception if the worker failed | compile-time reject |

Two properties the seam guarantees by construction:

- **Resolve-on-spawner-thread.** The worker thread's only outputs are the
  flattened buffer in its `WorkerCtx` slot and one eventfd write. The `Worker`
  object, its `cont` continuation, and the user's loop callbacks are only ever
  touched by the thread that owns them — A-1's race is structurally absent.
- **Loud everywhere threads don't exist.** ELF and Windows targets reject at
  compile time (the natives have no lowering there — the existing
  `fail("native floor function ...")` path, plus §7's explicit diagnostic); the
  interpreters never claim to be threaded (the probe). There is no configuration
  in which a worker silently runs serial while presenting as parallel.

M3a's first step is a floor probe corpus file exercising each new native on
`--run`/`--ir` (and the reject on `--emit-elf`) before anything is built on
them — the cheap de-risk the sibling track's IR-lowering surprise argues for.

---

## 3. Prelude rewrites and the Await amendment

### 3.1 `spawn` — one body, two execution legs

```
Worker<T> spawn<T>(() => T body) {
    Worker<T> w = Worker();
    var snap = std::sysThreadTransfer(body);      // capture snapshot NOW (acceptance #1)
    int jfd = std::sysThreadStart(snap);          // -1 = no true threads on this engine
    if (jfd < 0) {
        // Cooperative leg (oracle/IR): an immediate loop task, spawn-ordered.
        std::sysTimerStart(0, 0, (n) => {
            try { w.resolve(std::sysThreadTransfer(snap())); }
            catch (IException e) { w.reject(e); }
        });
    } else {
        // True-thread leg (LLVM): the body is ALREADY running on its pthread.
        // The watch fires on THIS loop when the worker signals; result/throw
        // rebuild + reap happen inside sysThreadResult, on this thread.
        std::sysWatch(jfd, (fd) => {
            try { w.resolve(std::sysThreadResult(fd)); }
            catch (IException e) { w.reject(e); }
        });
    }
    return w;
}
```

Notes. The join-fd watch **is** live loop work, so an un-awaited worker keeps
the process alive and auto-reaps when it fires (doc-2 F-9, unchanged). The
watch retires itself: `sysThreadResult` closes the fd and F-ii's POLLNVAL
auto-cancel drops the watch — no watch-id capture needed (a closure snapshots
its captures, so the `wid = sysWatch(...)`-then-use-`wid`-inside pattern would
capture a stale 0; this design avoids it entirely). On the cooperative leg the
`try/catch` is what turns an uncaught body throw into a rejection instead of
killing the loop driver.

### 3.2 `Worker` — the minimal reject slot (doc-2 §3.2's promise, kept)

```
class Worker<T> : Promise<T> {
    bool failed = false;
    string failMessage = "";       // v1 carrier; see the reconciliation note
    new Worker() { }
    void reject(IException e) {
        failed = true;
        failMessage = e.message;
        ready = true;              // wakes the awaiter; Await checks failed first
    }
}
```

v1 carries the exception's **message** and rethrows at the join as a
`RuntimeException(failMessage)`. Carrying the concrete exception *object* is
deliberately deferred: the object crosses the boundary fine (it flattens like
any statically-shaped object), but re-typing the rethrow by dynamic class is
`request-promise-rejection.md`'s design space — when that lands, `failMessage`
becomes `IException error` and the two reconcile exactly as doc-2 anticipated.
(A `Promise`-typed or optional-generic field is avoided on purpose: bug.md #1.)

### 3.3 The Await amendment (engine change, one rule)

The Await pump in each active engine (`Eval.cpp` ~:951, `IrInterp.cpp` ~:366,
`LlvmGen.cpp` ~:2689) gains one check, after `ready` reads true and before
yielding `value`:

> if the promise object has a `failed` field that is `true`, **throw** a
> `RuntimeException` carrying its `failMessage` field (ordinary catchable
> unwind — `await` is a rethrow point, doc-2 §3.2 / info.md §12.6).

Plain `Promise`s have no `failed` field → zero behavior change for every
existing program (field-absent = not failed; the engines already read promise
fields by name, so absence is cheap). X64Gen is frozen and never sees a
`Worker` (spawn rejects on ELF at compile time) — exempt. The reject path gets
its own corpus case on all three active engines (cooperative leg on the
interpreters, true leg on LLVM, byte-identical message).

### 3.4 `Channel` — same probe, native ring behind the same surface

`Channel<T>` keeps its landed prelude surface and adds the native leg (§6):
construction calls `int sysChannelNew(capacity, policy)`; on `-1` (interpreters)
every method runs the landed in-process queue path; on an id ≥ 0 (LLVM),
`send`/`receive`/`close` delegate to `sysChannelSend(id, v)` /
`sysChannelReceive(id) -> T?`-shaped natives over the §6 record. The Channel
*object* then carries only scalars (the id + policy) — flattening a Channel
handle degenerates to copying the id, and **portal semantics fall out of plain
field copy** (both rebuilt handles name the same process-global record). On the
interpreters the object itself stays the shared portal (landed behavior,
unchanged).

---

## 4. The C flatten/rebuild engine (`lvrt_systhreadtransfer`, real)

Replaces the landed loud abort. One engine, all four crossings (spawn capture,
join result, worker exception, channel message).

### 4.1 Buffer format — a self-contained relocatable snapshot

```
[ header: totalBytes | recordCount | rootOffset ]
[ record ]* — each 16-byte aligned:
    tag      (8B — the LvValue tag of the node)
    size     (8B — this record's byte length)
    payload  — per-tag, all internal references as BUFFER-RELATIVE OFFSETS
```

Per-tag payload encoding mirrors exactly the layout knowledge already encoded in
`lv_recursive_free` (`lv_runtime.c:240-317`) — that function is the layout
oracle; the flatten walk is its non-destructive twin:

| tag | flatten payload | rebuild |
|---|---|---|
| scalars, `LV_NONE` | inline 16-byte value | bit copy |
| `LV_STR` | length + bytes | fresh prefixed heap string via `lvrt_halloc` |
| `LV_OBJ` (statically shaped) | classId + static slot count + slot records (offsets) + dyn-list `(nameId, value-offset)` pairs | `lvrt_obj_new`, store rebuilt slots; replay dyn-list |
| `LV_OBJ` (value struct) | same as object (structs are flat by §9 — no refs possible) | `lvrt_copyval`-shaped rebuild |
| `LV_ARR` boxed | length + element offsets | fresh boxed array, elements rebuilt |
| `LV_ARR` dense | raw memcpy of the record run (value-structs hold no references, doc-2 §4.4) | memcpy into a fresh dense array |
| `LV_MAP` | length + (key-offset, value-offset) pairs | fresh map, insertion order preserved |
| `LV_CLO` (root only) | fn pointer/index **verbatim** (code is process-shared) + capture list (name, value-offset) | fresh closure, capture chain rebuilt into the receiving heap |
| `LV_CLO` (non-root), fd-carriers, `Promise`-derived (A-1), dynamic-shape, `LV_BLOCK` | **reject** — buffer aborted, error names the type | — |

The walk is an **explicit work-stack DFS with a seen-map** (payload address →
record offset): shared substructure flattens once and rebuilds shared, cycles
terminate, no C-stack recursion on adversarial depth (doc-2 problem #8).
Erased-generics reality (memory note): the predicate re-checks on the concrete
walk and **throws** a catchable `RuntimeException` naming the offending type —
identical message text to the interpreters' `lvThreadCopy`, so the differential
holds on error paths too.

### 4.2 The transfer allocator + counters

Buffers are plain `malloc`/`free` (the LLVM/threads path links libc via
pthreads) — thread-safe, process-global, and **outside every `lv_in_region`
range**, so a leaked pointer can never be mistaken for a counted value. Three
process-global C11 atomic counters — `transfers_out` (malloc'd, not yet freed),
`thread_spawns`, `thread_reaps` — are diagnostics off the hot path. Exposed the
same way the escaping-tier meter is: an env-gated stderr line at exit
(`LANG_THREAD_STATS=1` → `[threads] spawns=N reaps=N transfer-outstanding=0`),
which is what M6's harness parses (§8) — no new language surface.

### 4.3 Ownership rule

Flatten reads the source graph without mutating it (the sender keeps its value —
copy-always); rebuild allocates every node through the **receiving thread's**
`lvrt_halloc`, so every rebuilt node is a first-class counted value in its
region, and D1′'s invariant — a counted allocation is touched by exactly one
thread from birth to free — holds at every crossing.

---

## 5. `lv_thread.c` — spawn, trampoline, join, reap

### 5.1 `WorkerCtx`

```c
typedef struct LvWorkerCtx {
    pthread_t     tid;
    int           joinFd;        /* eventfd; level-triggered */
    void*         captureBuf;    /* flattened captures (owned until trampoline consumes) */
    void*         resultBuf;     /* flattened result-or-exception (worker writes, joiner frees) */
    int32_t       failedFlag;    /* body threw */
    /* worker-region bookkeeping for reap-time munmap: */
    uint8_t      *heapBase, *arenaBase;
} LvWorkerCtx;
```

`sysThreadStart` (spawner thread): flatten captures → `eventfd(0, 0)` → allocate
ctx → `pthread_create` → return `joinFd`. The **prelude registers the watch in
the same expression sequence, before any await can run** — and eventfd is
level-triggered, so even a worker that finishes before the watch registration
leaves the fd readable: the lost-wakeup ordering (doc-2 problem #3) is closed
from both sides.

### 5.2 Trampoline (worker thread)

1. `lv_thread_ctx_init()` — **not** `lv_rt_init` (F-i): mmap heap+arena, zero
   freelist/accounting, clear throw state; TLS-install. Record region bases in
   the ctx for reap.
2. Rebuild captures from `captureBuf` into this heap; free the buffer.
3. Invoke the body through the registered dispatch trampoline (the same
   `lv_invoke`-shaped entry the loop uses for every closure).
4. Drain **this thread's** loop until no work remains (R-3: a worker may
   register its own timers/watches; doc-2 §4.2's order — the result is published
   only when the worker is genuinely done).
5. If `lv_g_throwing` (this thread's): flatten the thrown value, set
   `failedFlag`; else flatten the return value. Store in `resultBuf`.
6. `write(joinFd, 1)`; increment `thread_spawns` was done at start — nothing
   else; return. (The thread does NOT free its heap — reap does, §5.3; it holds
   flattened, region-external data only.)

### 5.3 `sysThreadResult` (spawner thread, inside the watch callback)

read/drain eventfd → rebuild `resultBuf` into the calling thread's heap → free
buffer → `pthread_join(tid)` → munmap the worker's heap+arena regions →
`close(joinFd)` (F-ii retires the watch) → free ctx → increment `thread_reaps`
→ if `failedFlag`, **throw** the rebuilt exception's message as a
`RuntimeException`; else return the rebuilt value. **Rebuild strictly precedes
munmap** — doc-2 F-2 stays closed. Nothing outside a worker heap ever points
into it, so the unmap is unconditionally safe.

### 5.4 `lv_rt_init` guard (F-i)

`lv_thread_ctx_init` is a new function initializing only TLS state.
Belt-and-braces, `lv_rt_init` additionally guards the argv capture
(`if (argv) { g_argc = argc; g_argv = argv; }`) so no future lazy-init path can
regress `sysArgs` even if misrouted.

---

## 6. Channel — the LLVM leg (doc-2 §4.3, refined by §3.4's probe)

- **Record** (transfer-allocator-resident, process-global): capacity, policy,
  closed flag, the ptr-slot SPSC ring (head/tail as C11 acquire/release
  atomics — still the only atomics on any hot path), and **two eventfds**:
  `dataReady` (producer→consumer) and `spaceReady` (consumer→producer;
  `block`-policy backpressure — doc-2 F-7). A 2-count endpoint tally freed when
  both ends drop.
- `sysChannelSend(id, v)`: flatten `v`; on full — `block`: register a
  `spaceReady` watch on THIS loop and park (true backpressure, the §13 forced
  pairing); `drop`: free buffer, return; `error`: free buffer, throw. Else
  enqueue pointer, release-store tail, write `dataReady` if the consumer is
  parked.
- `sysChannelReceive(id)`: if empty and open — park on `dataReady` via this
  loop; if empty and closed — `None`. Else acquire-load head, take the buffer,
  rebuild into this heap, free it, advance head, write `spaceReady` if a
  blocked producer waits. `close(id)`: set flag, write `dataReady`.
- **The recorded serial-engine asymmetry stands** (doc-2 §14 log): `block`
  degrades to enqueue on the interpreters (a running cooperative task cannot
  park mid-body). Values and output are unaffected (sends are copies; D1′ makes
  results interleaving-independent); the difference is memory-shape only, and
  the corpus never encodes it.

---

## 7. Backend matrix (final)

| engine | worker execution | boundary |
|---|---|---|
| tree-walk oracle | cooperative loop task (probe = -1) | landed interpreter deep copy |
| IR interpreter | same — differential twin | same walk, byte-identical |
| LLVM, POSIX targets | **true pthreads**, per-worker TLS heap + loop, eventfd join | §4 C engine |
| LLVM, **Windows target** | **compile-time reject** — `LV_TLS` is deliberately non-TLS under `_WIN32` (mingw emulated-TLS ≠ LLVM COFF TLS lowering; doc-2 §14 log), so true threads there would share what must be isolated. `--target *windows*` + any `sysThread*`/`sysChannel*` emission = "threads: unsupported on Windows (v1)". The CreateThread + native-TLS twin is the deferred v2 item (doc-2 §12). |
| ELF / X64Gen | **frozen; compile-time reject** (landed) | — |

---

## 8. Corpus & differential plan

- `threads_serial/` (landed, amended per A-1): spawn/join, capture snapshots,
  loud non-flattenables — now including a captured `Worker` — and the channel
  suite; chained cooperation demonstrated via channels. Oracle + IR lanes,
  byte-identical.
- `threads_native/` (**new**, `run_native_llvm.sh` lane): the same
  spawn/join/channel programs the serial dir runs (shared subset — LLVM output
  must be **byte-identical to the oracle's expected files, never sorted or
  normalized**; a drift is an isolation bug, doc-2 §7/§11), plus LLVM-only
  programs with no serial counterpart: **blocked-worker isolation** (a
  `while(true)` worker while a sibling serves — meaningless serially, where it
  would hang; acceptance #4), the reject-path rethrow at `await`, and the
  SO_REUSEPORT two-worker accept test (joins the `corpus_net_ports`
  RESOURCE_LOCK group).
- Determinism discipline (doc-2 §8) unchanged and load-bearing: workers compute
  and return/send; only the spawning thread prints, at join/receive points.
- The M2 perf-gate residue: an arena save/restore microbench (a `.lev` tight
  loop timed via `sysMonotonic`, run before/after on the same host) pins the
  initial-exec TLS cost as within noise — measured once, recorded in the log,
  not assumed.

## 9. M6 — `fuzz/thread_leak.py`

Sweeps, per program, N in {small, large}, parsing the existing `[heap]` stderr
meter plus §4.2's `[threads]` stats line (`LANG_THREAD_STATS=1`):

1. **spawn/join churn** — N workers spawned+awaited: main-thread live-at-exit
   +0B; `reaps == spawns`; `transfer-outstanding == 0`.
2. **channel churn** — ≥1M messages at 16B/1KB/64KB payloads: live flat,
   outstanding 0; msgs/sec + MB/s recorded in the implementation log (doc-2
   problem #6's benchmark gate — v2's zero-copy immutable tier gets argued from
   these numbers).
3. **adversarial graphs** — deep chains, diamonds, cycles, shared substructure
   through spawn captures and channel sends: rebuild-equality asserted via
   program output; no growth across N.
4. **fd flatness** — the program samples `sysListDir("/proc/self/fd").length()`
   at start and end (after all joins) and prints both; the harness asserts
   equality (no leaked eventfds). Spinning-worker programs are excluded from the
   reap assertion by construction (they exit via `env.exit`, the sanctioned way
   out with a live worker — reaps intentionally < spawns there, and only there).

## 10. Foreseeable problems (delta over doc-2 §9)

| # | problem | strategy |
|---|---|---|
| 1 | Worker's lazy init clobbers argv (F-i) | dedicated `lv_thread_ctx_init`; argv guard in `lv_rt_init`; a corpus program asserts `sysArgs` is intact after a spawn |
| 2 | A prelude method referencing a seam native breaks unrelated LLVM programs (the M4 lesson — eager prelude-method lowering) | every seam native lands WITH its LLVM lowering in the same commit; `corpus_llvm_full` is the gate, as it was for `sysThreadTransfer` |
| 3 | Cross-thread `Promise.resolve` re-emerges via some new path | A-1's predicate rejects Promise-derived at every crossing; the resolve-on-spawner rule is structural (§2); code review checklist item on every M3 commit |
| 4 | eventfd exhaustion / leak under churn | one eventfd per live worker + two per channel, all closed at reap/teardown; M6 asserts fd flatness |
| 5 | The watch callback throwing (rebuild raises) unwinding the loop driver | the prelude wraps the resolve in try/catch and routes to `reject` (§3.1) — the loop survives, the awaiter gets the rethrow |
| 6 | Flatten meets a type added after this design (new tag) | the walk's default case is reject-with-type-name, never bit-copy — unknown tags are loud by construction |

## 11. Milestones & timeline

Target window stays ahead of AG-6 (doc-2 set Dec 2026; work is running early —
dates below are effort, not calendar):

| M | deliverable | accept | est |
|---|---|---|---|
| M3a | seam natives (probe form) + A-1 predicate change + prelude spawn/Worker/Await amendment, **interpreters only** | `threads_serial` green with amended expectations; floor-probe corpus green on --run/--ir; ELF rejects | 1d |
| M3b | the C flatten/rebuild engine (§4) replacing the loud abort; unit-probed via `sysThreadTransfer` round-trips on LLVM **without threads** (main thread → main thread) | round-trip corpus (deep/shared/cyclic) byte-identical to interpreters on LLVM | 3d |
| M3c | `lv_thread.c` + `sysThreadStart`/`sysThreadResult` lowerings; true spawn/join end-to-end | `threads_native` spawn/join green, byte-identical; blocked-worker isolation; reject-rethrow | 3d |
| M3d | Channel LLVM leg (§6) | channel corpus green on LLVM incl. ping-pong; REUSEPORT two-worker accept | 2d |
| M6 | `fuzz/thread_leak.py` + stats line + arena microbench record | §9's four assertions green at both sweep sizes | 2d |

## 12. STOP conditions (all of doc-2 §11's, plus)

- Any temptation to let a `Worker`/`Promise` cross a thread boundary "just this
  once" (A-1) — that is the cross-thread resolve race; STOP.
- Any temptation to have the worker thread touch ANY spawner-owned object
  beyond the eventfd write — STOP (the seam exists so this never happens).
- `lv_thread_ctx_init` growing a call into `lv_rt_init` or any process-global
  write — STOP (F-i).
- A differential that only passes with sorted/normalized output — isolation
  leaked; STOP, fix flatten (unchanged, restated because M3c is where the
  temptation will appear).

## 13. Reference-doc duty & completion

On M3c: reference.md §6.6.66 drops the "stops loudly / remaining milestone"
caveats for LLVM and documents the Windows compile-time reject; info.md §14's
status note updates to "true threads live on LLVM." On M6: move
`techdesign-threads.md`, `techdesign-threads-2.md`, and this doc to
`designs/complete/`, implementation log filled (including the M6 benchmark
numbers); Atlantis LA-1 flips to landed-in-full.

---

## 14. Implementation log — LANDED IN FULL (2026-07-11)

All milestones landed on master in §11's order; full `ctest` stayed green at each
commit (baseline 122/122, growing to 131/131 as thread lanes were added).

**M3a — capability-probe seam + A-1 + Await rethrow (interpreters).** The
`sysThreadStart`/`sysThreadResult` seam (§2), `spawn` rewritten to one body over
two legs behind the probe (§3.1), `Worker`'s reject slot (§3.2), and the Await
amendment (§3.3, `Eval.cpp` + `IrInterp.cpp`). A-1 (§1) landed:
`lvThreadIsPortal` keeps Channel only, Promise-derived moves to the reject set.
Corpus: `threads_serial/spawn_join.lev` case 4 amended to the loud A-1 error, the
no-deadlock property re-shown through a Channel (`channel.lev` `workerConsumes`);
`probe.lev` exercises the seam on `--run`/`--ir`; ELF rejects loudly.

**M3b — the C flatten/rebuild engine (`lv_runtime.c`).** Replaced
`lvrt_systhreadtransfer`'s loud abort with the real engine (§4): flatten into a
self-contained 16-byte-aligned relocatable buffer (offsets, not pointers), seen-
map hash so shared substructure and cycles flatten once, rebuild through the
receiving thread's `lvrt_halloc` at +1. Per-tag layout mirrors `lv_recursive_free`
(str/obj+dyn/boxed+dense arr/map/root-closure). New `lv_thread.h` (internal
contract), the F-i argv guard + `lv_thread_ctx_init`/`_ctx_unmap` (§5.4), the
§4.2 transfer counters. Corpus `threads_transfer/roundtrip.lev` (deep/shared-
diamond/cycle/map) byte-identical across treewalk/IR/LLVM; ASan+UBSan clean.

**M3c — true OS threads (`lv_thread.c`, new).** WorkerCtx, the pthread trampoline
(bootstrap via `lv_thread_ctx_init`, rebuild body, run, drain loop, flatten
result-or-exception-message, signal eventfd), `sysThreadStart`/`sysThreadResult`
(§5.1/§5.3: flatten captures → eventfd → `pthread_create`; drain → rebuild-before-
munmap → `pthread_join` → `munmap` → close → reap). The eventfd write/read pair is
the whole handoff's happens-before edge (no ctx field needs its own atomic; 500-run
stress, 0 mismatches). LlvmGen: the two lowerings, the §3.3 Await rethrow (via
`lvrt_raise_str`), the §7 Windows compile-time reject. Corpus `threads_native/`
(LLVM lane): `spawn_join` + `reject_rethrow` byte-identical to the serial oracle,
`blocked_worker` (acceptance #4). **A verified-hazard fix beyond the design:** a
reaped worker's join `eventfd` is reused by the next `eventfd()` before the next
poll, so F-ii's POLLNVAL loses that race — `sysThreadResult` now explicitly
cancels its join watch (`lvrt_loop_cancel_fd`) before close, the watch firing
retains the cb across its invoke so a self-cancel is safe, and `add_watch` reuses
retired slots (bounds the registry under churn). Recorded deviation: bug.md #3
(the tree-walk oracle over-captures the whole closure env — a spawn body diverges
from LLVM when a non-flattenable local is merely in lexical scope; the corpus
follows the discipline of not having one in scope at body creation).

**M3d — Channel native ring (`lv_thread.c`).** `LvChannel` — a `capacity`-slot
SPSC ring of flattened-message pointers, head/tail as C11 acquire/release atomics,
two eventfds; `sysChannelNew`/`Send`/`Receive`/`Close`. The Channel object carries
only the id scalar, so a captured handle relocates by plain field copy (§3.4).
Corpus `threads_native/channel.lev` byte-identical to the serial in-process queue
(cross-thread ping-pong + worker-consumes + same-thread struct/overflow/closed);
200-run stress, 0 mismatches/hangs; ASan+UBSan clean. Recorded deviations: a
parked endpoint blocks its thread on the eventfd rather than pumping its loop's
other work (a v2 refinement, unobservable in the corpus); the channel record is
not reclaimed on endpoint drop (bounded per-channel, invisible to the counted-heap
meter). bug.md #4 (a pre-existing LLVM `int? == None` payload-vs-tag defect: a 0
value reads as None) was surfaced by the M6 channel churn and worked around.

**A leak fix that came out of M6.** The `spawn` prelude originally pre-snapshotted
the body on main (`sysThreadTransfer(body)`) even on the LLVM leg, which copied a
captured **cyclic** graph into main's heap per spawn — ARC cannot free a cycle, so
the adversarial churn leaked ~512B/iter. Fixed by flattening the captures inside
`sysThreadStart` on the true-thread leg (acceptance #1 holds there) and snapshotting
only on the cooperative leg — +0B after.

**M6 — leak/reap/fd harness + stats + microbench.** The §4.2 `[threads]` stats
line (`LANG_THREAD_STATS=1`, wired into `lv_main`'s exit). `fuzz/thread_leak.py`
over `fuzz/thread_churn/{spawn_join,channel,adversarial}.lev`: at N in {200, 2000}
every program holds live-at-exit **+0B**, reaps == spawns, transfer-outstanding ==
0, output == oracle, and passes fd flatness (a low-`RLIMIT_NOFILE` re-run — a
leaked join eventfd per worker would exhaust the table; this replaces the design's
`sysListDir('/proc/self/fd')` sampling, which has no LLVM lowering). Wired as the
`thread_leak_llvm` ctest lane.

**Benchmark numbers (recorded, §8/§9.2 — the v2 zero-copy tier argues from these):**
- **spawn/join churn:** ~18.6K workers/sec (2000 sequential spawn+await), +0B.
- **channel churn:** ~520K int-messages/sec at 1M messages (1.92s, RSS 2.2 MB),
  +0B, transfer-outstanding 0.
- **adversarial-graph churn:** ~14K graph-transfers/sec (a diamond+cycle
  captured+rebuilt per worker), +0B.
- **arena/allocator microbench (§8):** 20M heap alloc+free through the TLS
  hot-state cursors in ~7580 ms, **within ~1% run-to-run** — the initial-exec TLS
  cost of `lv_g_arena_cursor`/`lv_g_throwing` sits in the noise (M2's STOP-gate
  perf check, measured not assumed).
