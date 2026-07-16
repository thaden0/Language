# Tech Design: Stream Unsubscribe / Dispose (`InStream<T> : IDisposable`)

**Status:** PROPOSED — ready for implementation. **Date:** 2026-07-12. **Feature ID:** SU-1.
**Request:** `designs/requests/accepted/request-stream-unsubscribe.md`.
**Research substrate:** `docs/stream-unsubscribe-research.md` (the dossier; every `file:line` below
was verified there against `agent2` @ 2026-07-12 — re-verify at implementation, lines drift).
**Depends on:** nothing un-landed (terminal floor F2 landed; `sysUnwatch`/`sysSignalClose` exist on
all three active engines). **Unblocks:** Sonar bounded-window signal subscriptions (modal/overlay
Ctrl-C capture); closes `techdesign-terminal-floor.md` §8 open question #1.
**Difficulty:** S/M — prelude-only (Resolver.cpp) + corpus; one checker *verification* task (§7 M4),
no new native, no new IR op, no ABI change.
**Risk:** LOW-MEDIUM — the code is small; the risk is concentrated in the three reentrancy/race
hazards (§6), each of which this design pins with a corpus test.

**Engine lanes:** full behavior on oracle / IR / LLVM. emit-C++ compiles the surface but the signal
stream stays loop-bound-rejected there (unchanged). **ELF/X64Gen: no work, no gate, ever**
(frozen; the v1 leak on ELF is permanent by policy).

---

## 1. Motivation

The stream substrate (info.md §13) can subscribe but never *unsubscribe*: `signal::on(sig)`
(`Resolver.cpp:2845`) returns a bare `InStream<int>` whose backing `StreamBuffer` sits in the
process-lifetime `signal::SignalState` fanout registry forever, keeping the per-signal `signalfd`
and its loop watch alive until program end. Consequences today:

1. A component that wants a signal for a *bounded window* (a Sonar modal that captures Ctrl-C only
   while open) cannot release it — the subscription, the fd, and the watch leak for the run.
2. **A live signal watch pins the event loop** (`RuntimeLoop.cpp:49`, `lv_loop.c:352`): a one-shot
   program that ever called `signal::on` can never exit by loop-drain.
3. The resource world is inconsistent: `Timer.cancel()`, `TcpStream.close()`, `Channel.close()`,
   `TaskGroup.close()` all exist, but the *general* stream read view has no dispose — so every new
   demand-driven system stream reinvents teardown ad-hoc, or leaks.

This design gives the stream family the uniform dispose operation the rest of the resource world
already has, riding the existing `using`/`IDisposable` machinery: a subscription is a resource,
and resources get deterministic release.

```lev
using InStream<int> winch = signal::on(signal::WINCH);   // auto-closes on scope exit
```

## 2. Current state (grounded — dossier §2)

- `StreamBuffer<T>` (`Resolver.cpp:2522`): single-consumer queue; `push` runs a claimed
  subscriber's callback **synchronously inline** (`:2533-2535`); **no closed flag, no close()**.
- `InStream<T>` (`:2560`): thin wrapper over `buf` and *nothing else* — the back-reference gap: it
  has no pointer to producer/registry/fd, so as written it cannot implement `close()`.
- `IOStream<T> : InStream<T>, OutStream<T>` (`:2577`): diamond, one collapsed buffer; inherits
  whatever lands on `InStream`.
- `signal::SignalState` (`:2799`): parallel arrays `sigs`/`fds`/`subs` keyed by distinct signo; one
  signalfd + one watch per signo, fanned to N buffers. **The `sysWatch` return id is discarded**
  (`:2855`) — the single concrete bookkeeping gap; `sysUnwatch(id)` needs it.
- The floor already has every native teardown needs, on oracle/IR/LLVM alike: `sysUnwatch`
  (`Resolver.cpp:1436`), `sysSignalClose` (unblocks the mask + closes the fd on both the interp
  `RuntimeNatives.cpp:124-130` and LLVM `lv_plat_posix.c:251` lanes). **No new floor work.**
- Both event loops snapshot the ready set before firing callbacks, so cancelling a watch from
  inside its own callback is loop-safe by construction (`RuntimeLoop.cpp:53`, `lv_loop.c:362-365`).
  The reentrancy hazards are entirely in the *prelude's own* registry bookkeeping (§6).
- `using` (checker `Checker.cpp:2652-2668`) requires the declared type to be a reference class
  implementing `IDisposable` with a zero-arg `close()`. Adding `: IDisposable` to a prelude class
  is a **zero checker/Lower/backend-cost** change (established by the D-B `IIterable` finding and
  F6; interface dispatch is ordinary runtime-class dispatch).

## 3. Design decisions (the rulings)

These resolve dossier §6's ten open questions. Each ruling is normative.

**R1 — Surface shape: Option A.** `InStream<T>` itself becomes `: IDisposable` and carries an
*optional* teardown closure attached by the producer. No `Subscription` handle class (Option B —
clunkier under `using`, two objects), no per-producer subtype (Option C — not uniform for future
system streams). Rationale: Option A is the request's stated ideal, `using`-compatible in one
line, zero-backend-cost, and it is the exact `TcpStream.onClose`/`hasCloseCb` field pattern
already proven in the prelude (`Resolver.cpp:1493/1495/1552`). A plain in-memory `InStream` has no
closure attached, so `close()` degrades to a clean no-op + buffer close — the uniform "I am done"
call works on *every* stream, doing real teardown only where there is something to tear down.

**R2 — Membership token: assigned int ids, never identity `==`.** Each subscription gets a
monotonic `int` id from a `SignalState.nextId` counter; removal is by int equality
(`indexOf` over a parallel `Array<Array<int>> subIds`). This follows the TaskGroup "ids, not
handles" idiom (`Resolver.cpp:1231`) and the loop's own watch-id model, and avoids any dependence
on cross-engine object-identity `==` for `StreamBuffer` handles (unverified on LLVM — a STOP
condition if reached for, §9).

**R3 — `SignalState` bookkeeping.** Two new parallel arrays: `Array<int> watchIds` (store the
`sysWatch` return so `sysUnwatch` becomes possible — closes the one concrete gap) and
`Array<Array<int>> subIds` (parallel to `subs`), plus `int nextId = 0`.

**R4 — `deliver` re-resolves per drain iteration.** `signal::deliver` currently caches `idx` once;
its safety after a mid-delivery last-close depends on a subtle close-order invariant (the closed
fd makes the next `sysSignalNext` return −1 before the stale `idx` is re-read — dossier §5.1).
Rule: move `findFd(fd)` to the **top of each drain iteration** and return on −1. One line;
eliminates the reasoning burden instead of documenting it.

**R5 — Strict delivery cutoff via `StreamBuffer.closed`.** The substrate gains `bool closed` +
`void close()`, and **`push` on a closed buffer is a silent drop**. This upgrades Known Warning #1
from "at-most-one trailing delivery" to a **strict cutoff**: even when a fanout snapshot still
holds a just-closed buffer, the push hits the closed flag and drops. Full substrate semantics in
§4.1; D-B coordination in §8.

**R6 — Teardown order and idempotency.** On last-subscriber removal: `sysUnwatch(watchId)` first
(stop the loop from dispatching into an fd about to die), then `sysSignalClose(fd)` (unblock →
default disposition, acceptance criterion #2), then drop the registry row (all five parallel
arrays). `signal::off` is idempotent by construction: a second call misses in `findSig` or
`indexOf` and no-ops — no double-`sysSignalClose` (the `TcpStream` stale-fd footgun,
`Resolver.cpp:1503-1508`, cannot occur). The registry row is removed synchronously inside `off`
with no await/park point anywhere in the function.

**R7 — `signal::off` is a free function**, declared beside `on`/`deliver`/`findSig` — never a
method on the `const SignalState st` eager global. A method on that global would drag the signal
natives into every emit-C++ compile (the exact regression `techdesign-terminal-floor.md:96-102`
fixed). Preserving the plain-data-holder-global + demand-compiled-free-functions shape is a hard
constraint (STOP condition, §9).

**R8 — `Timer`/`TcpStream` do not fold in (v1 scope).** They keep their existing
`cancel()`/`close()`. Noted follow-up (out of scope, trivially additive later): `TcpStream`
already has a conformant zero-arg `close()` and could simply *declare* `: IDisposable` to become
`using`-able.

**R9 — Disposition gap is intended behavior.** After last-close the signal returns to default
disposition (criterion #2 *wants* this); for `TERM`/`INT`/`QUIT` under a raw-mode TUI the
raw-mode safety handlers (terminal-floor §3 M4) cover the gap independently of `signal::on`.
Criterion #2 is pinned as a **Linux-signalfd guarantee**; on the non-primary self-pipe POSIX
fallback the mask unblocks but the `sigaction` handler stays installed
(`lv_plat_posix.c:236-239`) — documented asterisk, not a work item.

**R10 — No prelude flow-narrowing.** All new prelude code uses −1-sentinel ints (`findSig`,
`indexOf` style), never `T?`/union narrowing (`[[leviathan-prelude-no-narrowing]]` — LLVM
misreads it).

## 4. The design (normative)

All code changes live in the prelude (`Resolver.cpp`). Natives unchanged. Checker unchanged
(verification-only task in M4).

### 4.1 Substrate: `StreamBuffer<T>` close semantics

```lev
class StreamBuffer<T> {
    Array items;
    (T) => void handler;
    bool hasHandler = false;
    bool closed = false;                       // NEW

    void push(T v) {
        if (closed) return;                    // NEW: silent drop — strict fanout cutoff (R5)
        if (hasHandler) { var cb = handler; cb(v); }
        else            { items = items.add(v); }
    }
    T pull() {
        if (closed)     throw RuntimeException("stream is closed");     // NEW, distinct message
        if (hasHandler) throw RuntimeException("stream: consumer end is claimed by a subscriber");
        if (isEmpty())  throw RuntimeException("stream is empty");
        ...unchanged...
    }
    void setHandler((T) => void cb) {
        if (closed)     throw RuntimeException("stream is closed");     // NEW
        if (hasHandler) throw RuntimeException("stream: consumer end is already claimed");
        ...unchanged...
    }
    void close() {                             // NEW: idempotent
        closed = true;
        hasHandler = false;                    // detach any claimed consumer
    }
}
```

Normative rulings, v1:

- **`push` on closed = silent drop.** Rationale: a closed buffer has, by definition, no consumer —
  enqueueing is a leak and throwing would punish the *producer* for a benign race that is inherent
  to fanout removal-mid-delivery (the fanout's snapshot may legitimately still hold a buffer whose
  owner closed it one callback ago). This is consumer-initiated close; see §8 for the
  producer-initiated (EOF, D-B) coordination.
- **`pull`/`setHandler` on closed = throw `"stream is closed"`** (distinct from
  `"stream is empty"`). The closer pulling its own closed stream is a caller bug and fails loud.
  Buffered-but-undelivered items are dropped with the buffer — the closer said "done"; v1 does not
  offer drain-after-close (Channel-style closed-and-drained semantics are D-B's producer-EOF
  question, §8).
- `close()` is idempotent (sets flags, nothing else) and never throws — mandatory for `using`
  (`close()` during exception unwind must not replace the in-flight exception,
  `reference.md:781`). `isEmpty()`/`hasData` behavior unchanged (a closed buffer stops growing, so
  this is benign).

### 4.2 Read view: `InStream<T> : IDisposable`

```lev
class InStream<T> : IDisposable {
    StreamBuffer<T> buf;
    () => void onDispose;                      // NEW: optional producer-attached teardown
    bool hasDispose = false;                   // NEW  (TcpStream onClose/hasCloseCb pattern)

    new InStream(StreamBuffer<T> b) { buf = b; }

    T pull() => buf.pull();
    bool hasData() => !buf.isEmpty();
    void subscribe((T) => void cb) buf.setHandler(cb);

    void close() {                             // NEW: IDisposable; idempotent; never throws
        if (hasDispose) {
            hasDispose = false;                // flip BEFORE running — reentrancy/double-close safe
            var d = onDispose;
            d();
        }
        buf.close();
    }
}
```

- The producer attaches teardown by **field write after construction** (`s.onDispose = ...;
  s.hasDispose = true;`) — same as `TcpStream.onClose(cb)` sets `onClosed`/`hasCloseCb`. No new
  constructor overload (keeps the nullary-closure auto-construct question out of scope; the
  guard bool, not the closure slot, carries the truth).
- `close()` flips `hasDispose` *before* invoking the closure, so a teardown that reenters (or a
  second `close()`, or manual-close-then-`using`-exit) no-ops — the TaskGroup self-safe-under-
  unwind lesson.
- `close()` transitively never throws: the closure body is `signal::off` (§4.4), which is
  throw-free; `buf.close()` is throw-free.
- **`OutStream<T>` is unchanged.** Dispose is a consumer-side operation; the write view carries no
  teardown (a producer that wants to end a stream is D-B's EOF, not this design).
- **`IOStream<T>` coherence:** inherits `close()`; for an in-memory `IOStream` (both ends, one
  buffer) it closes the shared buffer and no-ops the absent closure. Ruling: `close()` on an
  `IOStream` disposes the *read view's* subscription teardown only — there is no separate
  write-side resource in the current model. Subsequent `<<` pushes silently drop (§4.1). Document
  in reference.md.

### 4.3 Signal registry: bookkeeping + hardened `deliver`

```lev
class SignalState {
    Array<int> sigs = [];
    Array<int> fds  = [];
    Array<int> watchIds = [];                  // NEW (R3): sysWatch ids, enables sysUnwatch
    Array<Array<StreamBuffer<int>>> subs = [];
    Array<Array<int>> subIds = [];             // NEW (R3): parallel per-sub int ids (R2)
    int nextId = 0;                            // NEW: monotonic subscription id source
}
```

```lev
void deliver(int fd) {
    int signo = std::sysSignalNext(fd);
    while (signo >= 0) {
        int idx = signal::findFd(fd);          // R4: re-resolve EVERY iteration
        if (idx < 0) return;                   // registry row torn down mid-drain: stop
        Array<StreamBuffer<int>> list = signal::st.subs.at(idx);   // pure-value snapshot
        int i = 0;
        while (i < list.length()) {
            OutStream<int> w = OutStream(list.at(i));
            w << signo;                        // push: closed buffers silently drop (R5)
            i = i + 1;
        }
        signo = std::sysSignalNext(fd);
    }
}
```

`signal::on` — three changes: keep the watch id, assign a sub id, attach the closure:

```lev
InStream<int> on(int sig) {
    StreamBuffer<int> b = StreamBuffer();
    int myId = signal::st.nextId;
    signal::st.nextId = signal::st.nextId + 1;
    int idx = signal::findSig(sig);
    if (idx < 0) {                             // first subscriber for this signo
        int fd = std::sysSignalOpen([sig]);
        if (fd < 0) throw RuntimeException("signal: cannot watch signal " + sig.toString());
        int wid = std::sysWatch(fd, (ready) => signal::deliver(fd));   // id KEPT now
        signal::st.sigs     = signal::st.sigs.add(sig);
        signal::st.fds      = signal::st.fds.add(fd);
        signal::st.watchIds = signal::st.watchIds.add(wid);
        signal::st.subs     = signal::st.subs.add([b]);
        signal::st.subIds   = signal::st.subIds.add([myId]);
    } else {                                   // Nth subscriber: append to the fanout
        signal::st.subs   = signal::st.subs.with(idx, signal::st.subs.at(idx).add(b));
        signal::st.subIds = signal::st.subIds.with(idx, signal::st.subIds.at(idx).add(myId));
    }
    InStream<int> s = InStream(b);
    s.onDispose = () => signal::off(sig, myId);
    s.hasDispose = true;
    return s;
}
```

Registry writes stay in the established rebuild style (`st.subs = st.subs.with(idx, ...)`) —
**bare-global-write only through the `st` holder's fields**, never a namespace-qualified global
write (`[[leviathan-prelude-backend-gotchas]]`: ns-global writes don't lower; `st`'s fields are
object field writes, which do — this is what `on` already does today).

### 4.4 `signal::off` — the new free function (R6, R7)

```lev
void off(int sig, int subId) {
    int idx = signal::findSig(sig);
    if (idx < 0) return;                       // signal already fully torn down: idempotent no-op
    Array<int> ids = signal::st.subIds.at(idx);
    int k = ids.indexOf(subId);
    if (k < 0) return;                         // this sub already removed: idempotent no-op
    signal::st.subs   = signal::st.subs.with(idx, signal::st.subs.at(idx).removeAt(k));
    signal::st.subIds = signal::st.subIds.with(idx, ids.removeAt(k));
    if (signal::st.subs.at(idx).length() == 0) {          // LAST subscriber: reclaim the source
        std::sysUnwatch(signal::st.watchIds.at(idx));     // 1. stop loop dispatch first
        std::sysSignalClose(signal::st.fds.at(idx));      // 2. unblock -> default disposition
        signal::st.sigs     = signal::st.sigs.removeAt(idx);   // 3. drop the row (all 5 arrays)
        signal::st.fds      = signal::st.fds.removeAt(idx);
        signal::st.watchIds = signal::st.watchIds.removeAt(idx);
        signal::st.subs     = signal::st.subs.removeAt(idx);
        signal::st.subIds   = signal::st.subIds.removeAt(idx);
    }
}
```

Properties, each pinned by a §7 test: idempotent (double-close, close-after-teardown);
broadcast-isolating (non-last removal touches only `subs`/`subIds`, the fd and watch survive);
throw-free; no await/park anywhere, so no reentry window between removal and teardown.

## 5. Acceptance criteria → mechanism map

| # | criterion (from the request) | delivered by |
|---|---|---|
| 1 | subscriber disposes; stops receiving | `close()` → `off` removes the buffer; `closed` flag drops any in-flight fanout push (strict cutoff, R5) |
| 2 | last-out: fd unwatched + closed, signal back to default disposition | `off`'s empty-check → `sysUnwatch` + `sysSignalClose` (mask unblock is in the native, both lanes); Linux-signalfd guarantee, self-pipe asterisk (R9) |
| 3 | `using` works | `InStream<T> : IDisposable` + zero-arg never-throwing `close()` satisfies the checker gate `Checker.cpp:2652-2668` |
| 4 | differential green oracle/IR/LLVM | prelude-only change over natives already present on all three lanes; §7 test matrix |

## 6. Hazards and their dispositions

1. **Mid-delivery removal** (request Known Warning #1). The inner walk is crash-safe (pure-value
   snapshot). Semantics: strict cutoff via the closed-flag drop (R5) — a subscriber closed from
   inside any callback receives **zero** further deliveries. Last-sub-closed-mid-drain: `deliver`
   re-resolves `idx` each iteration and returns on −1 (R4); belt-and-suspenders, the closed fd
   also ends the drain. Pinned: §7 T5.
2. **Unblock-vs-queued-delivery race** (Known Warning #2). A signal pending in the signalfd at
   teardown is dropped with the fd — *correct* under the established coalescing contract
   ("at least one tick after the last change, not one-per-signal", `signals.md:178`). Unwatch
   happens before close so the loop never dispatches `deliver` into a dying fd. The
   post-teardown disposition gap is intended (R9).
3. **Broadcast isolation** (Known Warning #3). Teardown is gated strictly on the fanout list
   emptying; a non-last removal cannot touch `fds`/`watchIds`. Pinned: §7 T3.
4. **Double-close / stale-fd reuse.** `close()` flips `hasDispose` first; `off` misses and no-ops
   on the second call; each fd is `sysSignalClose`d at most once. Pinned: §7 T6.
5. **`using` + throwing dispose.** Entire dispose path is throw-free by construction (§4.2/§4.4),
   so the no-`finally`-chaining rule (`reference.md:781`) never bites.
6. **Loop pinning.** Removing the last watch lets `hasWork()` drain — a one-shot program that
   subscribes and disposes now exits. Pinned: §7 T4 (the anti-pin).
7. **Thread boundary.** A disposable `InStream` is a loop-bound carrier (fd + live watch behind
   it) and must not cross `spawn`. M4 verifies how the flatten walk classifies it and closes the
   gap **without** breaking plain in-memory `InStream` copy-across-spawn (decision tree in M4).
8. **emit-C++ regression.** All new signal code is free functions + fields on the plain-data
   holder global (R7); `InStream`'s new closure field follows the `TcpStream` precedent, which
   emit-C++ already compiles. M3 includes an emit-C++ compile-only check of a non-signal program
   (the M3-trap regression guard).

## 7. Implementation plan

Single track, one implementer. All source ownership: `Resolver.cpp` (prelude) +
`tests/corpus/floor/` + `tests/run_terminal_floor.sh`. No file conflicts with any open track
(checked against open designs 2026-07-12; see §8 for the one coordination seam).

| M | scope | detail | difficulty | target |
|---|---|---|---|---|
| M1 | Substrate + view | §4.1 `StreamBuffer.closed`/`close()` + §4.2 `InStream : IDisposable`; corpus T6 + an in-memory smoke (close a plain stream; `using` over it; pushes drop; pull throws distinct message) | S | 2026-07-13 |
| M2 | Signal registry | §4.3 `SignalState` fields + hardened `deliver` + `on` changes; §4.4 `signal::off`; corpus T1–T3, T5 | M | 2026-07-13 |
| M3 | Lane matrix + churn | T4 (`using` × 3 exit edges + loop-drain exit), T7 churn/fd-leak, differential oracle/IR/LLVM green, emit-C++ compile-only regression guard, ELF deferral unchanged | S | 2026-07-14 |
| M4 | Thread-boundary check | Locate the spawn flatten walk's loop-bound-carrier rejection (reference.md §6.6.66 list). (a) If it rejects by class list: add rejection for `InStream` **only when** `hasDispose` is inspectable; (b) if field inspection is infeasible there, do NOT blanket-reject `InStream` (would break in-memory copy-across-spawn) — instead document the gap in reference.md §6.6.66 and file a bug.md entry [P2] "disposable InStream crosses spawn uncaught". Either branch is acceptable; pick, record in the impl log | S | 2026-07-14 |

Timeline: land in full (M1–M4) by **2026-07-14 EOD**; single PR-equivalent to master is fine, or
M1+M2 then M3+M4 as two pushes.

### Test surface (extends the terminal-floor harness: `tests/corpus/floor/*.lev`, CTest `terminal_floor`)

| T | pin | criterion |
|---|---|---|
| T1 | subscribe USR1, self-kill, 1 tick, `close()`, self-kill → **no** second tick (extend `usr1.lev`) | #1 |
| T2 | single sub, `close()` → registry empty; interp disposition-clean assertion (the `interpSignalCleanup` idempotency precedent, terminal-floor §7); process exits by loop drain | #2 |
| T3 | two subs (`fanout.lev`): close one → other still receives every tick, fd stays open | #3 / KW#3 |
| T4 | `using InStream<int> w = signal::on(WINCH);` — close runs on fall-through, `return`, and throw-unwind (3 variants); one-shot program **exits** after scope | #3 |
| T5 | callback closes (a) itself, (b) another sub, (c) the last sub, from inside `deliver` → no crash, strict cutoff (zero trailing deliveries), loop exits | KW#1 |
| T6 | `close()` twice; manual close then `using`-exit; `off` after full teardown → all no-op, no throw | R6 |
| T7 | N× subscribe/close churn → fd count at baseline, `+0B` net (the standard churn assertion; extend the `floor_pty.py`/`thread_leak.py` pattern) | hygiene |
| T8 | full differential: oracle/IR/LLVM identical on T1–T6; emit-C++ compiles a non-signal program (M3 guard) + still rejects the signal stream; ELF deferral diagnostic unchanged | #4 |

### Reference-doc duty (lands with M3, not before)

- reference.md §6.6 streams: `StreamBuffer.closed`/`close()` semantics (push-drop, distinct pull
  error), `InStream.close()`, the `IOStream` read-view ruling.
- reference.md §6.6.65: add `InStream` to the `IDisposable` conformer list beside `File`/`TaskGroup`.
- reference.md `signal` section: `signal::off`, last-subscriber teardown, default-disposition
  guarantee + self-pipe asterisk, the strict-cutoff delivery contract.
- `techdesign-terminal-floor.md` §8 open question #1: mark closed, pointer here.
- info.md §13: one line — the substrate now has deterministic dispose (`using`/`IDisposable`
  maturity counterpart).

## 8. Coordination — the D-B (streams-maturity EOF) neighbor

`deferal-http-and-streams-maturity.md` D-B wants `StreamBuffer.close()` for **producer-side EOF**
(so `for..in`/`toArray` over a live stream can terminate); this design lands the **same field and
method** for consumer-side detach. Convergence rulings so the two never collide:

- **One `closed` field, one `close()`** — this design lands them; D-B builds on them, it does not
  re-add them (the b2d147f duplicate-work lesson: **fetch master and re-read this section before
  starting D-B**).
- v1 semantics here are consumer-close-shaped: push-on-closed drops silently (required by the
  fanout race, §4.1). D-B's producer-EOF story wants loud push-after-close and
  closed-and-drained pulls (`pullOrNone`, Channel-shaped). **Those are refinements D-B owns**:
  when D-B lands, it may split loudness by role (e.g. keep drop for fanout producers, throw for
  owned single-producer streams) or introduce `pullOrNone` beside the throwing `pull` — but it
  must keep `close()` idempotent and never-throwing (the `using` contract this design relies on),
  and must keep the strict-cutoff drop reachable for fanout use. This paragraph is the recorded
  hand-off.
- `Timer`/`TcpStream` adoption of `IDisposable` stays with D-B or a later follow-up (R8).

## 9. STOP conditions (Sonnet implementers: stop and escalate, do not improvise)

1. **Any new native or IR op** seems needed. The premise, validated by the dossier §2.3/§9, is
   that `sysUnwatch` + `sysSignalClose` suffice. If they don't, STOP.
2. **Any X64Gen/ELF work**, including tests. Frozen; the ELF leak is permanent by policy; never
   gate anything on an ELF finding (`[[feedback_x64gen-frozen]]`).
3. **Teardown as a method on the `st` global** (or any new eager-global-instance method). Free
   functions only (R7). If the free-function shape hits a lowering wall, STOP.
4. **Reaching for identity `==` on `StreamBuffer`** to locate a subscriber (R2 forbids), or for
   `lvrt_loop_cancel_fd` (LLVM-only, not a native).
5. **Any `T?`/union flow-narrowing in prelude code** (R10). Use −1 sentinels.
6. **`using` gate fails** on `using InStream<int> w = signal::on(...)` for a checker-side reason
   (generic-class + interface interaction). Expected to pass (interface satisfaction is
   slotless; the D-B finding says zero checker work) — if it doesn't, that is a checker bug, not
   a prelude workaround site. STOP, file it, escalate.
7. **emit-C++ breaks on a non-signal program** after the `InStream` field additions. The
   `TcpStream` closure-field precedent says it won't; if it does, STOP (do not "fix" by moving
   teardown onto the global).

## 10. Out of scope (explicit)

- Producer-side EOF semantics, `pullOrNone`, stream iteration (`IIterable`) — D-B (§8).
- `Timer.cancel()`/`TcpStream.close()` migration to `IDisposable` (R8).
- An `EventEmitter` Layer-2 class (info.md §13's fan-out reshaping) — the signal registry remains
  the only fanout; when `EventEmitter` is designed, `signal::off`'s shape (per-sub id + last-out
  source teardown) is its template.
- Any HTTP/socket teardown changes; any new `sys*`; anything ELF.

## 11. Implementation log

*(implementer appends here, per milestone)*
