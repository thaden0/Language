# Stream unsubscribe / dispose — technical research dossier

**Status:** pre-design research. Not a tech design. This is the complete technical
substrate a tech design for the stream-unsubscribe track
(`designs/requests/request-stream-unsubscribe.md`) must build on: what exists today, exactly
how streams and the signal registry are represented on every engine, what machinery a
"dispose a subscription" surface touches, the hard problems it introduces, and the questions
the design has to answer. Every claim below is anchored to code (`file:line`) verified against
the current tree (`agent2`, 2026-07-12), not to prose in `info.md`/`reference.md`.

Audience: whoever writes the tech design (a Fable-class author per
`[[feedback_techdesign-conventions]]`), and the Sonnet-class implementers after them.

---

## 0. The one-paragraph orientation

The stream substrate (`info.md` §13) is a **push queue with an optional single consumer** and
**no teardown surface**: `StreamBuffer<T>` can be pushed to, pulled from, and have its consumer
end claimed, but it cannot be *closed*, and `InStream<T>` — the read view a producer hands back
— holds nothing but a pointer to its buffer, so a holder of an `InStream` has no way to say "I
am done, release whatever is feeding me." Every fd-/loop-bound producer in the prelude solved
its *own* teardown ad-hoc — `Timer.cancel()` (`Resolver.cpp:1411`), `TcpStream.close()`
(`Resolver.cpp:1591`), `Channel.close()` (`Resolver.cpp:1387`) — but `signal::on(sig)`
(`Resolver.cpp:2845`) did **not**: it returns a bare `InStream<int>` whose backing
`StreamBuffer` sits in a process-lifetime fanout registry (`signal::SignalState`,
`Resolver.cpp:2799`), keeping the per-signal `signalfd` + `sysWatch` alive until the program
ends (its own `techdesign-terminal-floor.md` §8 open question #1). The request is to give the
stream family the **uniform dispose operation** the rest of the resource world already has
(`using`/`IDisposable`, `Resolver.cpp:2416`) so that (a) a subscriber can drop out of a fanout
and (b) when a fanout empties, the underlying OS resource is torn down. The load-bearing facts:
**the floor already has every native this needs** (`sysUnwatch`, `sysSignalClose`), the
teardown is entirely a **prelude + registry-bookkeeping** change, and the signal case is the
*only* current producer that leaks — but the surface must generalize to any future
demand-driven system stream, which is why it belongs on `InStream`/the substrate, not bolted
onto `signal`.

---

## 1. Terminology, pinned

| term | meaning here |
|---|---|
| **subscription** | one consumer's standing interest in a source: concretely, one `StreamBuffer<T>` registered in a producer's fanout list, wrapped in the `InStream<T>` handed to the consumer. |
| **fanout** | a producer holding N per-subscriber buffers and broadcasting each event to all of them. `signal::SignalState.subs` (`Resolver.cpp:2802`) is the only one in the tree; `info.md` §13 calls this a Layer-2 "EventEmitter" reshaping (no such class exists yet). |
| **dispose / unsubscribe / close** | the missing operation: drop this subscriber's buffer from the fanout, and if it was the last, tear the source down. |
| **standing pull / claim** | `subscribe(cb)` seizes a buffer's single consumer end (`StreamBuffer.setHandler`, `Resolver.cpp:2547`); a later `pull` throws. Distinct from *fanout* (one buffer per subscriber). |
| **source resource** | what a fanout must reclaim on last-close. For signals: the `signalfd` (`sysSignalClose`) and its loop watch (`sysUnwatch`). |
| **loop-bound carrier** | a value whose meaning is an fd + a live event-loop registration (`TcpStream`/`Timer`/…). `reference.md:1517` lists these as **non-flattenable** — they cannot cross a `spawn` boundary. A subscription is one. |
| **the back-reference gap** | `InStream<T>` (`Resolver.cpp:2560`) stores only `StreamBuffer<T> buf`. It has no pointer to the producer/registry/fd, so as written it *cannot* implement `close()`. Closing the gap is the core design problem. |

---

## 2. The current state — grounded

### 2.1 The stream substrate (`Resolver.cpp:2516-2582`)

`StreamBuffer<T>` is a single-consumer queue, Array-backed, with **no close/EOF state**:

```
class StreamBuffer<T> {                     // Resolver.cpp:2522
    Array items;
    (T) => void handler;
    bool hasHandler = false;
    void push(T v) {                        // :2532  if claimed, call handler INLINE; else enqueue
        if (hasHandler) { var cb = handler; cb(v); }
        else            { items = items.add(v); }
    }
    T pull() {                              // :2540  throws if claimed OR empty
        if (hasHandler) throw RuntimeException("consumer end is claimed by a subscriber");
        if (isEmpty())  throw RuntimeException("stream is empty");
        ...
    }
    void setHandler((T) => void cb) {       // :2547  claims consumer end; drains backlog to cb
        if (hasHandler) throw RuntimeException("consumer end is already claimed");
        ...
    }
}
```

The two facts that matter most for this design:

- **`push` runs the subscriber's callback synchronously** (`:2533-2535`). In a fanout,
  broadcasting an event *re-enters user code*, which may in turn call `close()`. Every
  mid-delivery hazard (§5.1) flows from this one line.
- **There is no `closed` flag and no `close()`** — the substrate cannot express end-of-stream
  or "producer detached." (The parallel `techdesign-http-and-streams-maturity.md` D-B §2.3 already
  proposes adding `bool closed` + `void close()` here for the *EOF* problem; this request wants
  the same field for the *unsubscribe* problem — §7 and §12 argue they should land coherently.)

The typed views (`:2560-2582`):

```
class InStream<T>  { StreamBuffer<T> buf;  T pull(); bool hasData(); void subscribe(cb); }  // :2560
class OutStream<T> { StreamBuffer<T> buf;  OutStream<T> (<<)(T v); }                          // :2567
class IOStream<T> : InStream<T>, OutStream<T> { ... }   // diamond: ONE collapsed buf (:2577)
```

`InStream<T>` is a **thin wrapper over `buf`** and nothing else. This is the back-reference gap:
`close()` on an `InStream` would have no idea what to tear down. Note also the diamond
(`IOStream`) — whatever `close()` shape lands on `InStream` is inherited by `IOStream`, and must
mean something coherent there (an `IOStream` is both ends of one buffer).

### 2.2 The signal registry — the one leaking producer (`Resolver.cpp:2770-2861`)

```
namespace signal {
    const int HUP=1; INT=2; QUIT=3; USR1=10; TERM=15; WINCH=28;   // :2779-2783
    class SignalState {                       // :2799  PARALLEL ARRAYS keyed by distinct signo
        Array<int> sigs = [];                 //   sigs[i]  = signal number
        Array<int> fds  = [];                 //   fds[i]   = its open signalfd
        Array<Array<StreamBuffer<int>>> subs = [];   // subs[i] = fanout list for sigs[i]
    }
    const SignalState st = signal::SignalState();     // :2806  process-lifetime global
    int findSig(int sig);  int findFd(int fd);        // :2808 / :2813  linear scans, -1 if absent

    void deliver(int fd) {                    // :2825  loop calls this when the signalfd is ready
        int idx = signal::findFd(fd);
        if (idx < 0) return;                  // :2827  guard — fd no longer registered
        int signo = std::sysSignalNext(fd);
        while (signo >= 0) {
            Array<StreamBuffer<int>> list = signal::st.subs.at(idx);   // :2830  VALUE snapshot
            int i = 0;
            while (i < list.length()) {
                OutStream<int> w = OutStream(list.at(i));
                w << signo;                   // :2834  -> buf.push -> subscriber cb INLINE
                i = i + 1;
            }
            signo = std::sysSignalNext(fd);   // :2837  drain (WINCH coalescing)
        }
    }

    InStream<int> on(int sig) {               // :2845  the public entry
        StreamBuffer<int> b = StreamBuffer();
        int idx = signal::findSig(sig);
        if (idx < 0) {                        // FIRST subscriber for this signo:
            int fd = std::sysSignalOpen([sig]);
            if (fd < 0) throw RuntimeException(...);
            signal::st.sigs = signal::st.sigs.add(sig);
            signal::st.fds  = signal::st.fds.add(fd);
            signal::st.subs = signal::st.subs.add([b]);
            std::sysWatch(fd, (ready) => signal::deliver(fd));   // :2855  ***RETURN ID DISCARDED***
        } else {                              // Nth subscriber: append to the fanout
            signal::st.subs = signal::st.subs.with(idx, signal::st.subs.at(idx).add(b));
        }
        return InStream(b);                   // :2859  bare InStream, no closer
    }
}
```

Four registry facts drive the design:

1. **The `sysWatch` id is thrown away** (`:2855`). `sysUnwatch` takes that id
   (`Resolver.cpp:1436`), so **teardown is impossible without first storing it** — `SignalState`
   must gain an `Array<int> watchIds` (or the design must reach for `lvrt_loop_cancel_fd`, which
   is *not* a native — see §2.3). This is the single concrete gap behind the request's "only …
   fan-out bookkeeping" claim.
2. **One `signalfd` + one watch per distinct signal number**, fanned to N buffers (the comment
   at `:2792-2795` explains why: two signalfds for one signal would race for the single pending
   delivery). So "last subscriber for a source leaves" = `subs[idx]` becomes empty → unwatch +
   `sysSignalClose(fds[idx])` + drop the `idx` row.
3. **The subscriber's identity is its `StreamBuffer` `b`.** To remove it, the design needs to
   *find* `b` in `subs[idx]` — by identity (`indexOf`, §2.7) or by an assigned id (§4). The
   `InStream` returned at `:2859` is the only handle the consumer holds, and it wraps `b`.
4. **`deliver`'s snapshot** (`:2830`, `list = subs.at(idx)`) is a pure-value copy (arrays are
   immutable values, `info.md` §11), so iterating `list` is crash-safe even if a callback
   rewrites `st.subs`. But `idx` is cached across the drain loop — see the §5.1 analysis for why
   that is *subtly* safe only because `sysSignalClose` makes the next `sysSignalNext` fail.

### 2.3 The natives — the "no new floor work" claim, validated (with one caveat)

Everything the teardown calls **already exists on every non-frozen lane**:

| native | decl | interp (oracle/IR) | LLVM |
|---|---|---|---|
| `sysWatch(fd,cb) -> id` | `Resolver.cpp:1435` | `RuntimeNatives.cpp:1488` → `RuntimeLoop::addWatch` | `lv_runtime`/`lvrt_syswatch` (`lv_loop.c:329`) |
| `sysUnwatch(id)` | `Resolver.cpp:1436` ("cancels read AND write watches") | `RuntimeNatives.cpp:1500` → `RuntimeLoop::cancelWatch` | `lvrt_sysunwatch` (`lv_loop.c:342`) → `lv_loop_cancel_watch` |
| `sysSignalOpen(Array<int>) -> fd` | terminal-floor §3 | `RuntimeNatives.cpp:1255` (`interpSignalOpen:94` — block mask + `signalfd`) | `lvrt_syssignalopen` (`lv_runtime.c:2379`) → `lv_plat_signal_open` (`lv_plat_posix.c:177`) |
| `sysSignalNext(fd) -> signo` | terminal-floor §3 | `RuntimeNatives.cpp:1262` (`interpSignalNext:113`) | `lv_runtime.c:2390` → `lv_plat_signal_next:190` |
| `sysSignalClose(fd)` | terminal-floor §3 | `RuntimeNatives.cpp:1266` (`interpSignalClose:124` — **unblock mask + close**) | `lv_runtime.c:2394` → `lv_plat_signal_close:251` (**unblock + close, swap-remove registry**) |

So the request's scope claim — *"composes with the existing `std::sysUnwatch` floor call and
the terminal-floor `sysSignalClose` native — no new floor work"* — **is correct**. The only
addition is prelude/registry bookkeeping: store the watch id (§2.2 fact 1) and add the
language-level dispose surface.

**Caveat / the road-not-taken.** There *is* a lower-level teardown primitive on the LLVM lane:
`lvrt_loop_cancel_fd(int fd)` (`lv_loop.c:321`, Track 10 §5.3) cancels *every* watch on an fd and
is explicitly documented safe-to-call from inside that watch's own callback. It is tempting for
signal teardown (we have the fd; we don't currently have the watch id). But it is **not a
native** and is **LLVM-only** (the interpreter loop has no equivalent entry point). Storing the
watch id and using the portable `sysUnwatch(id)` keeps the teardown identical across all three
active engines and needs no new native — the recommended path.

**`sysSignalClose` gives Acceptance Criterion #2 for free — on the primary lanes.** Both the
interp (`RuntimeNatives.cpp:127` `sigprocmask(SIG_UNBLOCK,…)`) and the LLVM floor
(`lv_plat_posix.c:254`) *unblock the signal's mask* on close, so after the last subscriber
leaves the signal returns to **default disposition** (criterion #2, "observable: the signal
returns to default disposition"). ⚠️ **Divergence to document:** the *self-pipe* fallback
(non-Linux POSIX, `lv_plat_posix.c:196-249`) unblocks the mask but **leaves the `sigaction`
handler installed** and never reclaims the write end (`:236-239` says so explicitly). On that
path disposition does *not* return to strict default — it returns to "a handler that writes a
byte into a now-unread pipe." Primary lanes (interp Linux `signalfd`, LLVM `signalfd`) are
unaffected; the self-pipe path is a non-primary POSIX portability shim. The design should pin
criterion #2 as a Linux-signalfd guarantee and note the self-pipe asterisk.

### 2.4 The event loop — mid-dispatch cancellation is already safe by construction

Both loops **snapshot the ready set before firing callbacks**, precisely because "callbacks may
add/cancel watches":

- **Interp** (`RuntimeLoop.cpp:53` `nextBatch`): builds `readyIds` (`:117`), copies each ready
  watch's callback into the returned batch (`:138-140`), *then* the engine runs them. A
  `cancelWatch` (`:44`, linear `erase` by id) during a callback mutates `watches_` but not the
  already-built batch. POLLNVAL watches auto-cancel after batching (`:141`). `hasWork()`
  (`:49`) = `!timers_.empty() || !watches_.empty()`.
- **LLVM** (`lv_loop.c:366` `lvrt_loop_step`): "Ready ids are snapshotted before firing anything
  … callbacks may add/cancel timers or watches mid-batch" (`:362-365`); the fire path retains
  the callback across invoke (`:91-93`, `:318-320`) so a registry release during the callback
  cannot free a running closure. `lv_loop_cancel_watch` (`:304`) marks the slot inactive +
  releases; slots are reused (`:280-286`). `lvrt_loop_has_work` (`:352`) scans active
  timers/watches.

**Consequence:** calling `sysUnwatch` from *inside* `signal::deliver` (which is what a
subscriber's `close()` reentering the registry would do) is safe at the loop level on both
active engines. The hazard is not the loop — it is the *prelude's own* `st.subs`/`idx`
bookkeeping (§5.1).

Two loop facts the design must also respect:

- **A live signal watch pins the loop** (`hasWork()` stays true). This is intended for Sonar's
  lifetime WINCH sub, and it is exactly why unsubscribe matters for *bounded-window* subs: an
  un-disposed signal sub keeps a one-shot program from exiting (the same disease
  `techdesign-http-and-streams-maturity.md` §5 #4 documents for idle HTTP pools). Disposing the
  last sub must remove the watch so the loop can drain.
- **Program-end already backstops** (interp `interpSignalCleanup()` `RuntimeNatives.cpp:137`
  closes every signal fd + unblocks at end of `Eval::run`/`IrInterp::run`; a compiled process's
  exit reclaims fds and mask). So unsubscribe is a *within-run* reclaim; nothing leaks *across*
  runs today. The design must not double-free: teardown removes the fd from `SignalState`, and
  the swap-remove registries (`lv_plat_posix.c:255`, `RuntimeNatives.cpp:125-130`) tolerate a
  close of an already-absent fd, but a *double* `sysSignalClose` on the same fd number after the
  OS recycles it is the `TcpStream` stale-fd footgun (`Resolver.cpp:1503-1508`) — teardown must
  be idempotent (§5.6).

### 2.5 The `using`/`IDisposable` machinery — the surface to ride (`Resolver.cpp:2416`, Checker.cpp:2652, reference §5.2/§6.6.65)

```
interface IDisposable { void close(); }        // Resolver.cpp:2416
class File : IDisposable { ... void close() {...} }   // :2421 — the prelude's model conformer
```

`using Type name = expr;` (checker `Checker.cpp:2652-2668`) requires, at compile time:

- `Type` is a **reference class** (`declared.sym && !declared.sym->isValue`), **implementing
  `IDisposable`** (`isSubclass(declared.sym, disposable)`) — `Checker.cpp:2657-2660`;
- with a **zero-arg `close()`** resolved and stashed as `s->usingClose` (`:2662-2667`);
- as a **direct statement of a block** (`usingOkHere_` gate, `:2653-2655`) — not a loop body,
  field, global, or `for..in` binding;
- the binding is implicitly `const`; `close()` runs **once on every exit edge** (fall-through /
  `return` / `throw` unwinding / `break` / `continue`), multiple `using`s closing in **reverse
  order**; if `close()` throws while another exception unwinds, the new one **replaces** it (no
  `finally` chaining) — `reference.md:772-784`.

**The direct consequence for this design:** Acceptance Criterion #3
(`using InStream<int> winch = signal::on(signal::WINCH);`) requires the **static type returned by
`signal::on` to implement `IDisposable` and expose `close()`.** Two shapes satisfy that (§4):
make `InStream<T>` itself `: IDisposable`, or return a dedicated `Subscription`/subtype. Either
way the checker gate above is the acceptance test.

**Crucial low-cost fact:** adding `: IDisposable` (an interface with one slotless method
requirement) to a prelude class is a **prelude-only change with zero checker/Lower/backend
work** — the same finding `techdesign-http-and-streams-maturity.md` §2.3 records for adding
`: IIterable` to `InStream` ("Track 07's dispatch … lowers to plain `CallDyn`; adding `:
IIterable` to a prelude class needs zero checker, Eval, Lower, or backend work"), reinforced by
the covariant-return interface-satisfaction work (F6, `reference.md` §4.2). Interface dispatch is
runtime-class dispatch (`info.md` §6.9), and `close()` is an ordinary method.

### 2.6 Precedents in the family — four existing teardowns to be consistent with

The design is not inventing teardown; it is **generalizing four ad-hoc ones into the substrate**:

1. **`Channel<T>.close()`** (`Resolver.cpp:1387`) — the in-family close-semantics precedent. It
   has a `bool closed` flag (`:1342`), `close()` resolves parked waiters with `None` (`:1390`),
   `send` on closed throws (`:1354`), and `receive()` returns `None` "closed AND drained"
   (`:1379`). This is the shape `StreamBuffer.close()` should mirror if the design also lands the
   EOF half (§7/§12). `Channel` is the clearest evidence the language already models "a stream
   endpoint that can be closed."
2. **`Timer.cancel()`** (`Resolver.cpp:1411`) — `sysTimerCancel(id)` releases the loop work; a
   one-shot releases itself after firing. A `Timer` is *also* a `StreamBuffer`-fed system stream
   (`:1401-1406`) but exposes teardown as `cancel()`, not `close()`, and is **not**
   `IDisposable`. The design should decide whether `Timer` folds into the new uniform surface or
   stays idiosyncratic (a scope question — recommend: leave `Timer` alone in v1, note it).
3. **`TcpStream.close()`** (`Resolver.cpp:1591`) — the **idempotent, every-path-unwatches** model
   to copy verbatim: guard on the fd sentinel (`if (fd < 0) return;`), `stopDrain()`, `if
   (watching) sysUnwatch(watchId)`, `sysClose(fd)`, `fd = -1`. Its `onClose(cb)` /
   `onClosed`/`hasCloseCb` fields (`:1493/:1495/:1552`) and its reentrancy-through-`pump`
   (`:1555-1565`, where a recv of `None` calls `close()` then the close cb) are the exact
   template for an `InStream` that carries an optional teardown closure (§4/§7).
4. **`TaskGroup : IDisposable`** (`Resolver.cpp:1232`) — the structured-concurrency precedent,
   and the source of the governing idiom **"the group holds ids, not handles"** (`:1231`). Its
   `close()` (`:1255`) shields against re-cancellation during teardown so `using` + cancel can't
   livelock. Two lessons: (a) prefer **ids over handles** for registry membership (argues for a
   subscription-id design, §4), and (b) `close()` under `using` must be **self-safe** even when
   called during unwind.

### 2.7 Removal primitives — identity `==` and the Array surface

To drop a subscriber the design needs to locate it in `subs[idx]`. Two mechanisms exist:

- **Identity `==`.** A class with no `(==)` operator compares by **reference identity** on every
  active engine: oracle `Eval.cpp:907-909` (`bool same = r.kind==Object && l.obj==r.obj`), IR
  `IrInterp.cpp:715` (identical line), and the documented C3 contract "classes by identity"
  (`reference.md:1736`, used for Map keys on LLVM too). So `Array<StreamBuffer<int>>.indexOf(b)`
  (`Resolver.cpp:422`, uses `x == item`) would find `b` by identity. **But** this couples the
  design to a cross-engine identity-equality guarantee for `StreamBuffer` handles — verify on
  LLVM before relying on it (the object-`==` path there was not read in this pass).
- **Assigned id.** Give each subscription a monotonic `int` id (a counter in `SignalState`) and
  store parallel id arrays; remove by `int` equality via `indexWhere`/`indexOf`. This matches
  the TaskGroup "ids not handles" idiom and the loop's own watch-id model, and needs **no**
  identity-equality assumption. **Recommended.**

The pure-value Array surface for the rebuild: `add` (`:364`), `where`/`filter` (`:388/:393`),
`indexOf` (`:422`), `indexWhere` (`:518`), `removeAt(i)` (`:536`), `with(i,v)` (`:544`),
`slice` (`:551`). All return new arrays (COW under the hood, `info.md` §11), so registry updates
are `st.subs = st.subs.with(idx, list.removeAt(k))`-shaped, exactly like `on()` already does.

---

## 3. Per-engine architecture and coverage

The signal stream — and therefore any signal-unsubscribe teardown — lives only where the **event
loop** lives. The `close()`/`IDisposable` *surface* compiles everywhere; the signal *teardown
path* runs only on the loop engines.

| # | engine | file(s) | signal stream | what `InStream.close()` does here |
|---|---|---|---|---|
| 1 | tree-walk oracle | `Eval.cpp` + `RuntimeLoop.cpp` + `RuntimeNatives.cpp` | full (Linux `signalfd`) | full teardown (unwatch + `sysSignalClose`) |
| 2 | IR interpreter | `IrInterp.cpp` + same loop/natives | full (Linux `signalfd`) | full teardown |
| 3 | LLVM (primary AOT) | `LlvmGen.cpp` + `lv_loop.c` + `lv_plat_posix.c` | full (`signalfd`/self-pipe) | full teardown |
| 4 | emit-C++ | `CGen.cpp` | **rejected** — loop-bound; `sysWinSize` compiles, the signal *stream* does not (terminal-floor §3 lane matrix, impl-log M3) | `close()` on an in-memory `InStream` compiles; the signal path was never reachable here |
| 5 | pure x86-64 / ELF | `X64Gen.cpp` | **FROZEN** — `sysSignalOpen` hits native fallthrough → deferral diagnostic (`signals.md` §4) | not a target; **never gate the design on it** (`[[feedback_x64gen-frozen]]`) |

Consequences:

- **The teardown code is written once, in the prelude**, and runs identically on oracle/IR/LLVM
  because all three share the `sysUnwatch`/`sysSignalClose` contract (§2.3). This is the same
  "new prelude, zero interpreter divergence except output-parity" shape as the terminal-floor
  landing.
- **emit-C++** already refuses the signal stream with the generic coverage error
  (`CGen.cpp:1057`, "native backend does not yet cover this construct"). Adding `close()` to the
  `InStream`/substrate types must not *newly* break emit-C++ compilation of the *type* — the
  terminal-floor M3 log (`techdesign-terminal-floor.md:96-102`) documents the exact trap: a
  method-on-an-eager-global-instance drags its natives into *every* emit-C++ program. Mitigation
  is already in the tree — the signal registry is a **plain-data holder global + demand-compiled
  free functions** — and the design must preserve that shape (any new `signal::off`/teardown must
  be a free function, not a method on a global instance; §7).
- **ELF is frozen.** The v1 leak on the frozen lane is permanent-by-policy; do not add an ELF
  teardown lane, do not gate acceptance on ELF (Acceptance Criterion #4 correctly lists only
  oracle/IR/LLVM).

---

## 4. The core surface-design question

`InStream<T>` cannot close itself as written (§2.1 back-reference gap). Three shapes close the
gap; the design must pick one (this is *the* decision).

**Option A — `InStream<T> : IDisposable`, with an optional teardown closure.** Give `InStream`
the `TcpStream.onClosed`/`hasCloseCb` treatment (`Resolver.cpp:1493/1495/1552`):

```
class InStream<T> : IDisposable {
    StreamBuffer<T> buf;
    () => void onDispose;                 // optional teardown, captured by the producer
    bool hasDispose = false;
    new InStream(StreamBuffer<T> b) { buf = b; }
    T pull() => buf.pull();  bool hasData() => !buf.isEmpty();  void subscribe(cb) ...
    void close() {                        // IDisposable
        if (hasDispose) { var d = onDispose; hasDispose = false; d(); }   // idempotent
    }
}
```

`signal::on` then attaches the closure: `InStream<int> s = InStream(b); s.onDispose = () =>
signal::off(sig, b); s.hasDispose = true; return s;` (or a constructor overload taking the
closure). A plain in-memory `InStream` (e.g. `Timer.ticks()`) has `hasDispose == false`, so
`close()` is a clean no-op — the "uniform I am done" call the request asks for works everywhere,
doing real teardown only where there is something to tear down.

- **Pros:** one uniform surface on *every* stream (the request's stated ideal); `using
  InStream<int> w = signal::on(WINCH);` type-checks directly (criterion #3); `IOStream` inherits
  a coherent `close()`; matches the `TcpStream` precedent exactly; adding `: IDisposable` is
  zero-backend-cost (§2.5).
- **Cons:** every `InStream` grows two fields (a closure slot + bool) even when unused; the
  closure captures `(sig, b)` and thus keeps `b` alive until close (fine). Must confirm the
  closure-field-on-a-prelude-class emit-C++ behavior (the M3 global-instance trap is about
  *global* instances, not fields — but verify).

**Option B — a dedicated `Subscription : IDisposable` handle**, returned *alongside* or *instead
of* the `InStream`. `signal::on` returns a `Subscription` that carries `InStream<int> stream()`
plus `close()`. Mirrors Rx/`EventListener`-style APIs.

- **Pros:** keeps `InStream` a pure read view; the dispose capability is explicit in the type.
- **Cons:** breaks the request's "`InStream<T>` gains a `close()`" preference and the "uniform"
  goal; two objects to hand back; `using` wants the *stream* to be disposable, so callers would
  write `using Subscription s = ...; InStream<int> w = s.stream();` — clunkier than criterion #3's
  one-liner.

**Option C — an `InStream` subtype `SignalStream : InStream<int>, IDisposable`.** Only signal
subs are disposable; base `InStream` is untouched.

- **Pros:** no cost to non-disposable streams.
- **Cons:** `signal::on`'s return type narrows to a concrete class (fine), but the surface is no
  longer uniform across future demand-driven system streams — each would need its own subtype.
  The request explicitly wants the substrate-level answer ("any future demand-driven system
  stream").

**Recommendation:** **Option A.** It is the only shape that delivers the request's stated ideal
(a uniform dispose on `InStream`, `using`-compatible in one line) at
demonstrated-zero-backend-cost, reuses the exact `TcpStream` closure-teardown template, and
generalizes to future system streams for free. The id-vs-identity sub-decision (§2.7) is
orthogonal and resolves to **ids** (`signal::off(sig, subId)` with a `Subscription`-less int
token stored in the closure) to avoid the cross-engine identity-`==` dependency.

---

## 5. The hard problems

### 5.1 Removing a subscriber mid-delivery (request Known Warning #1)

`deliver` (`Resolver.cpp:2825`) broadcasts by calling `w << signo` → `buf.push` → the
subscriber's callback **inline** (`StreamBuffer.push:2533`). A callback may call `close()`, which
re-enters `signal::off` and rewrites `st.subs` **while `deliver` is iterating**. Three sub-cases:

- **Crash-safety of the inner loop:** SAFE by construction. `list = signal::st.subs.at(idx)`
  (`:2830`) is a **pure-value snapshot** (arrays are immutable, `info.md` §11); rewriting
  `st.subs` does not mutate `list`. The `i < list.length()` walk cannot fault.
- **Semantic: one trailing delivery.** If subscriber *i*'s callback unsubscribes subscriber
  *i+1* (a different, non-last buffer), the inner loop still pushes the *current* `signo` to
  *i+1* (it is already in `list`). So a just-unsubscribed buffer can receive **one more event**.
  Options: accept and document ("at-most-one delivery after `close()`"), or add a per-buffer
  `bool dead` flag checked before push. Recommend documenting; a dead-flag is cheap insurance if
  a consumer test demands strict cutoff.
- **`idx` invalidation across the drain.** `idx` is cached once (`:2826`) but `st.subs`'s outer
  array is rebuilt (`removeAt`) when a *last* subscriber leaves. The outer `while (signo >= 0)`
  re-reads `subs.at(idx)` at the **top** of the next iteration (`:2830`). This is safe **only
  because** last-subscriber teardown calls `sysSignalClose(fd)`, so the bottom-of-loop
  `sysSignalNext(fd)` (`:2837`) on the now-closed fd returns −1 → the loop exits *before* the
  stale `subs.at(idx)` is re-evaluated. **This is a load-bearing invariant, and it is subtle.**
  The design must either (a) state it explicitly and pin it with a test (subscriber closes the
  *last* sub from inside its own delivery), or better (b) make `deliver` re-resolve `idx` via
  `findFd(fd)` at the top of each drain iteration and `break` on −1, removing the dependence on
  close-order timing. Recommend (b) — it is one line and eliminates the reasoning burden.

### 5.2 The last-close / signalfd unblock race (request Known Warning #2)

"The signalfd unblock-on-last-close must not race a delivery already queued in the fd." A signal
can be pending in the signalfd at the instant teardown runs `sysSignalClose`. That queued
delivery is **lost** — which is *correct* under the stream's coalescing contract ("at least one
tick after the last change, not one-per-signal", `signals.md:178`, `techdesign-terminal-floor.md`
§3). The real hazard is the **disposition-gap**: `sysSignalClose` unblocks the signal (§2.3), so
between "decided this is the last sub" and program teardown, a newly-arriving signal reverts to
**default disposition** — for `TERM`/`INT`/`QUIT` that is process death. This is inherent to
"return the signal to default" (criterion #2 *wants* it) and is acceptable for `USR1`/`WINCH`;
for `TERM` under a raw-mode TUI it is separately covered by the raw-mode safety handlers
(`techdesign-terminal-floor.md` §3 M4, installed on `enableRaw`, independent of `signal::on`).
The design should: **unwatch before close** (stop the loop from dispatching `deliver` on an fd
about to be closed), then `sysSignalClose`, and document the disposition-gap as intended.

### 5.3 Broadcast isolation (request Known Warning #3)

"Closing one subscriber must not disturb the others sharing the fd." Direct: removing `b` from
`subs[idx]` when `subs[idx]` still has other members must **not** unwatch/close the fd — teardown
of the fd is gated strictly on `subs[idx].length() == 0` after removal. The one shared resource
(the `signalfd` + its watch) survives while any subscriber remains. Pin with a two-subscriber
test (`tests/corpus/floor/fanout.lev` is the existing two-sub fixture to extend): close one, the
other keeps receiving.

### 5.4 The back-reference gap (§2.1) — resolved by §4 Option A. The teardown closure captured at
`on()` time is what carries `(sig, subId)` back to `signal::off`; the base `InStream` stays a
thin wrapper whose `close()` no-ops when no closure is attached.

### 5.5 `signal::off` must be a free function, not a method on the eager global (§3). The
terminal-floor M3 log (`techdesign-terminal-floor.md:96-102`) documents that a method on the
`const SignalState st` global forces its natives into every emit-C++ compile. `off`/teardown must
live beside `on`/`deliver`/`findSig` as demand-compiled free functions so a non-signal program
still builds on emit-C++.

### 5.6 Idempotent close + stale-fd reuse. `close()` must be safe to call twice (`using` on an
already-closed stream; a manual `close()` then scope-exit). Copy `TcpStream.close`
(`Resolver.cpp:1591`): flip `hasDispose`/a `closed` flag first, so the second call no-ops. This
also guards the `TcpStream`-discovered stale-fd hazard (`:1503-1508`): never `sysSignalClose` an
fd number twice — after the first close the OS may recycle it for an unrelated resource. The
per-signal registry entry is removed on teardown, and `findSig`/`findFd` then miss, so a second
`off(sig, subId)` for the same signal finds nothing and no-ops — the desired idempotency, but the
design must verify the removal happens *before* any await/reentry inside `off`.

### 5.7 Thread boundary. A subscription is a **loop-bound carrier** (`reference.md:1517` lists
`TcpStream`/`TcpListener`/`Timer`/`Process`; a signal `InStream` is the same shape — an fd + a
live watch). It **cannot cross a `spawn`** and must be rejected by the flatten walk like the
others (each worker has its own loop). Add `InStream` (when loop-bound) / any `Subscription` to
that non-flattenable list, or confirm the existing carrier check already catches it. Document.

### 5.8 `using` + close-throws semantics. Under `using`, if `close()` throws while an exception
is already unwinding it **replaces** the in-flight exception (no `finally`, `reference.md:781`).
Signal teardown calls `sysUnwatch` + `sysSignalClose`, neither of which throws (both return
`int`/void). Keep it that way — a throwing dispose under `using` is a footgun the `TaskGroup`
shield (`Resolver.cpp:1256`) exists to tame; a signal sub has no equivalent need if teardown
stays throw-free.

### 5.9 `IOStream` coherence. `IOStream<T> : InStream<T>` (`:2577`) inherits `close()`. For an
in-memory `IOStream` (both ends over one buffer) `close()` is a no-op unless a producer attached
a closer — coherent. The design should state that `close()` on an `IOStream` closes the *read
view's* teardown only (there is no separate write-side resource in the current model).

---

## 6. The questions the tech design must answer

1. **Surface shape** — Option A / B / C (§4). Recommended: A (`InStream<T> : IDisposable` +
   optional teardown closure).
2. **Membership token** — identity `==` vs assigned int id for removing a subscriber (§2.7).
   Recommended: int id (no cross-engine identity-`==` dependency; matches TaskGroup "ids not
   handles").
3. **`SignalState` bookkeeping** — add `Array<int> watchIds` (to enable `sysUnwatch`, closing the
   only concrete gap, §2.2) and, if id-based, `Array<Array<int>> subIds` parallel to `subs`.
4. **`deliver` robustness** — re-resolve `idx` per drain iteration and `break` on `findFd < 0`
   (§5.1(b)), removing the reliance on close-order timing.
5. **Mid-delivery cutoff semantics** — document "at-most-one trailing delivery," or add a
   per-buffer dead-flag for strict cutoff (§5.1).
6. **Teardown order** — unwatch → `sysSignalClose` → drop registry row; idempotent; free-function
   `signal::off` (§5.2/§5.5/§5.6).
7. **Does the EOF/`closed` half land now or later?** The request is about *producer→consumer
   detach* (unsubscribe). `techdesign-http-and-streams-maturity.md` D-B is about
   *consumer-observable end-of-stream* (`StreamBuffer.close()` + `pullOrNone`). They share the
   `StreamBuffer.closed` field. Decide whether to (a) land unsubscribe alone (minimal), or (b)
   co-land the `closed` flag so a disposed sub also makes its own `pull` observably ended (§12).
   Recommended: land unsubscribe with a `StreamBuffer.closed` flag set on dispose, so the two
   maturity items converge instead of colliding.
8. **Does `Timer`/`TcpStream` fold into `IDisposable`?** They already have `cancel()`/`close()`.
   Recommended: leave them in v1 (scope discipline); note that `TcpStream.close` could later
   *declare* `: IDisposable` to become `using`-able (it already has a conformant `close()`), a
   trivial follow-up.
9. **Thread-boundary rejection** — confirm/extend the non-flattenable carrier list for a
   loop-bound `InStream`/`Subscription` (§5.7).
10. **emit-C++ / ELF** — `close()` surface compiles on emit-C++ (in-memory streams), signal
    teardown stays loop-bound-rejected there; ELF frozen, permanent leak by policy (§3).

---

## 7. A concrete recommended shape (sketch, not normative)

Prelude-only. Natives unchanged. (Reproduced to make the §6 recommendations concrete; the design
owns the final form.)

```
// substrate (Resolver.cpp:2522) — add the detach half of close-semantics
class StreamBuffer<T> {
    ...
    bool closed = false;
    void close() { hasHandler = false; closed = true; }   // detach consumer; mark ended
}

// read view (Resolver.cpp:2560) — becomes the uniform dispose surface
class InStream<T> : IDisposable {
    StreamBuffer<T> buf;
    () => void onDispose;  bool hasDispose = false;
    new InStream(StreamBuffer<T> b) { buf = b; }
    T pull() => buf.pull();  bool hasData() => !buf.isEmpty();
    void subscribe((T) => void cb) buf.setHandler(cb);
    void close() {                      // idempotent; no-op for plain in-memory streams
        if (hasDispose) { var d = onDispose; hasDispose = false; d(); }
        buf.close();
    }
}

// signal registry (Resolver.cpp:2799) — store the watch id + a per-sub id
class SignalState {
    Array<int> sigs = [];  Array<int> fds = [];  Array<int> watchIds = [];   // NEW: watchIds
    Array<Array<StreamBuffer<int>>> subs = [];
    Array<Array<int>> subIds = [];      // NEW: parallel ids, removal without identity-==
    int nextId = 0;
}

// deliver (Resolver.cpp:2825) — re-resolve idx each drain iteration (§5.1(b))
void deliver(int fd) {
    int signo = std::sysSignalNext(fd);
    while (signo >= 0) {
        int idx = signal::findFd(fd);   // re-resolve; entry may have been torn down
        if (idx < 0) return;
        Array<StreamBuffer<int>> list = signal::st.subs.at(idx);   // value snapshot
        int i = 0;
        while (i < list.length()) { OutStream<int> w = OutStream(list.at(i)); w << signo; i = i + 1; }
        signo = std::sysSignalNext(fd);
    }
}

// on (Resolver.cpp:2845) — attach the teardown closure; store watch id + sub id
InStream<int> on(int sig) {
    StreamBuffer<int> b = StreamBuffer();
    int myId = signal::st.nextId; signal::st.nextId = signal::st.nextId + 1;
    int idx = signal::findSig(sig);
    if (idx < 0) {
        int fd = std::sysSignalOpen([sig]);
        if (fd < 0) throw RuntimeException("signal: cannot watch signal " + sig.toString());
        int wid = std::sysWatch(fd, (ready) => signal::deliver(fd));   // KEEP the id
        signal::st.sigs = signal::st.sigs.add(sig);
        signal::st.fds  = signal::st.fds.add(fd);
        signal::st.watchIds = signal::st.watchIds.add(wid);
        signal::st.subs = signal::st.subs.add([b]);
        signal::st.subIds = signal::st.subIds.add([myId]);
    } else {
        signal::st.subs   = signal::st.subs.with(idx, signal::st.subs.at(idx).add(b));
        signal::st.subIds = signal::st.subIds.with(idx, signal::st.subIds.at(idx).add(myId));
    }
    InStream<int> s = InStream(b);
    s.onDispose = () => signal::off(sig, myId);   // s.hasDispose set by an on-closure ctor/overload
    return s;
}

// off (NEW, free function beside deliver) — drop one sub; tear down on empty
void off(int sig, int subId) {
    int idx = signal::findSig(sig);
    if (idx < 0) return;                          // already gone: idempotent
    Array<int> ids = signal::st.subIds.at(idx);
    int k = ids.indexOf(subId);
    if (k < 0) return;
    signal::st.subs   = signal::st.subs.with(idx, signal::st.subs.at(idx).removeAt(k));
    signal::st.subIds = signal::st.subIds.with(idx, ids.removeAt(k));
    if (signal::st.subs.at(idx).length() == 0) {  // LAST subscriber: reclaim the source
        std::sysUnwatch(signal::st.watchIds.at(idx));   // stop the loop first (§5.2)
        std::sysSignalClose(signal::st.fds.at(idx));    // unblock -> default disposition (crit #2)
        signal::st.sigs     = signal::st.sigs.removeAt(idx);
        signal::st.fds      = signal::st.fds.removeAt(idx);
        signal::st.watchIds = signal::st.watchIds.removeAt(idx);
        signal::st.subs     = signal::st.subs.removeAt(idx);
        signal::st.subIds   = signal::st.subIds.removeAt(idx);
    }
}
```

Open sub-decisions this sketch deliberately leaves to the design: the `InStream` ctor overload
that sets `hasDispose` (vs a mutable field write from `on`); whether `StreamBuffer.closed`
changes `pull`/`hasData` semantics now or defers to D-B; and the dead-flag question (§5.1).

---

## 8. Test surface

The existing terminal-floor harness is the template: `tests/corpus/floor/*.lev`
(`fanout.lev`, `usr1.lev`, `winch.lev`) driven by `tests/run_terminal_floor.sh` (CTest
`terminal_floor`), differential across `--run`/`--ir`/`--build-native`, emit-C++ compile-only for
the signal parts, ELF asserting the deferral. New pins (one corpus file each, mapped to the
acceptance criteria):

1. **Dispose stops delivery (crit #1):** subscribe `USR1`, self-`kill`, receive one tick,
   `close()`, self-`kill` again → **no** second tick. (`usr1.lev` extended.)
2. **Last-close tears down + returns default disposition (crit #2):** single subscriber to a
   *catchable-to-default-effect* signal; after `close()`, a subsequent delivery hits default
   disposition (observably: for `USR1` the process terminates by default, or assert via a probe
   that the mask is unblocked). Interp: assert `leviathan`'s own disposition is clean afterward
   (the `interpSignalCleanup` idempotency test at `techdesign-terminal-floor.md` §7 is the
   precedent).
3. **Broadcast isolation (crit #3, Known Warning #3):** two subscribers (`fanout.lev`), close
   one, the other still receives every subsequent tick; fd stays open.
4. **`using` deterministic close (crit #3):** `using InStream<int> w = signal::on(WINCH);` inside
   a block; assert `close()` runs on fall-through, on `return`, and on `throw`-unwind (three
   variants), and that a one-shot program **exits** afterward (the loop drains once the watch is
   gone — the anti-pin for §2.4's loop-pinning).
5. **Mid-delivery removal (Known Warning #1):** a subscriber whose callback closes (a) itself and
   (b) the last-remaining sub, from *inside* `deliver`; assert no crash, loop exits, at-most-one
   trailing delivery.
6. **Idempotent double-close (§5.6):** `close()` twice, and manual `close()` then `using`
   scope-exit; no double-`sysSignalClose`, no throw.
7. **Churn / leak (fd + memory):** N subscribe/close cycles → fd count returns to baseline
   (extend `fuzz/thread_leak.py`/`floor_pty.py` pattern), `+0B` net (the standard `[[leviathan
   prelude backend gotchas]]` churn assertion).
8. **Differential green on oracle / IR / LLVM (crit #4);** emit-C++ compiles the `InStream.close`
   surface but rejects the signal stream (unchanged); ELF deferral unchanged.

---

## 9. Scope validation — is "no new floor work" true?

**Yes, with one prelude-bookkeeping caveat.** The teardown needs only `sysUnwatch`
(`Resolver.cpp:1436`) and `sysSignalClose` (terminal-floor §3), both present on all three active
engines (§2.3). The single gap is that `signal::on` **discards the `sysWatch` return id**
(`Resolver.cpp:2855`) — so `SignalState` must start storing it (`watchIds`). That is a pure
prelude/registry change, no native, no IR op, no checker rule (adding `: IDisposable` to a
prelude class is zero-backend-cost, §2.5). The claim holds.

What is explicitly **out of floor scope** and must not be reached for: `lvrt_loop_cancel_fd`
(LLVM-only, not a native, §2.3); any new `sys*`; any ELF/X64Gen work
(`[[feedback_x64gen-frozen]]`).

---

## 10. STOP conditions

- **Any new native or IR op.** The request's premise is prelude-only. If the surface seems to
  need a new native (e.g. a signalfd-drain primitive, or a loop-introspection call), that is a
  STOP — escalate; the existing `sysUnwatch`/`sysSignalClose` are sufficient by this analysis.
- **Any X64Gen/ELF work.** Frozen. The v1 leak on ELF is permanent by policy. Do not add an ELF
  teardown lane, do not gate acceptance on ELF (`[[feedback_x64gen-frozen]]`, `signals.md` §4).
- **Reintroducing the emit-C++ eager-global-instance trap.** If teardown is authored as a method
  on the `const SignalState st` global (rather than a free function), it will drag signal natives
  into every emit-C++ program — the exact regression `techdesign-terminal-floor.md:96-102` fixed.
  STOP and refactor to a free function.
- **Relying on cross-engine identity `==` for `StreamBuffer` handles without verifying LLVM.** If
  the design chooses identity removal over ids (§2.7) it must first confirm object-`==` identity
  on the LLVM lane; the id approach avoids the question entirely.
- **Prelude flow-narrowing.** Any `T?`/union narrowing in the new prelude code — LLVM misreads it
  (`[[leviathan-prelude-no-narrowing]]`); use `-1`-sentinel ints (the terminal-floor fallback
  precedent).

---

## 11. Reference-doc duty (when it lands, not before)

- `reference.md` §6.6 streams (`:1254-1262`): document `InStream.close()` / `InStream :
  IDisposable` and the substrate `StreamBuffer.closed`/`close()` (coordinate with D-B if the EOF
  half co-lands, §12).
- `reference.md` §6.6.65 `IDisposable` (`:1678`): add `InStream` (and any `Subscription`) to the
  conformer list beside `File`/`TaskGroup`.
- The `signal` namespace section (terminal-floor's reference home): document `signal::off` /
  dispose semantics, the last-subscriber teardown, and the default-disposition-on-last-close
  guarantee (+ the self-pipe asterisk, §2.3).
- `techdesign-terminal-floor.md` §8 open question #1 closes; note it in the impl log.
- `info.md` §13: the streams section gains a line that the substrate now has deterministic
  dispose (the `using`/`IDisposable` maturity counterpart the request names).

## 12. Coordination — the streams-maturity neighbor

This request overlaps `techdesign-http-and-streams-maturity.md` **D-B** (`InStream<T>` iteration /
end-of-stream). They are **distinct but share `StreamBuffer.closed`**:

- **This request (unsubscribe):** producer→consumer *detach* + source teardown. Motivated by
  bounded-window signal subs (Sonar modal/overlay that wants Ctrl-C only while open); Sonar's
  `App` itself holds a *lifetime* WINCH sub (`designs/sonar/techdesign-09-runloop-terminal.md`,
  the consumer) for which the v1 leak is already acceptable.
- **D-B (EOF):** consumer-observable *end-of-stream* so `for..in`/`toArray` over a live stream
  terminate correctly; proposes `StreamBuffer.close()` + `pullOrNone()` (`D-B §2.3`).

**Recommendation:** land unsubscribe *with* the `StreamBuffer.closed` flag (set on dispose) so a
disposed subscription's own `pull` is observably ended — this makes the two maturity items
*converge* on one `closed` field instead of two teams adding it twice (the b2d147f duplicate-work
lesson, `[[leviathan-la30-doc02-llvm-leg-halted]]`). Fetch master and check whether D-B has
started before authoring, and coordinate the `StreamBuffer` change (the 09 §9 "coordinate, don't
trespass" rule the deferral register invokes throughout).

---

## 13. Implementation log

*(implementer appends here, per landing milestone)*
