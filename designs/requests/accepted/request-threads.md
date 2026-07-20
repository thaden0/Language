# Request: True Threads / Workers (LA-1)

**From:** Atlantis framework (designs/atlantis/, Tracks 01 + 05). **Date:** 2026-07-06.
**Priority:** P1 — the enterprise performance story; wanted by ~AG-6 (2026-12), not blocking
earlier Atlantis gates. **Owner status (2026-07-06):** confirmed coming ("we will have true
threads … if you want it, I'll get it").

## 1. Requirement

A web server on one cooperative event loop cannot use more than one core, and one slow
handler stalls every request. Atlantis needs, in the info.md §14 shape (two tiers, shared
address space, isolation by default):

- **R-1. `spawn`** — start a worker (real OS thread) running a closure; returns a handle.
  Captures are **by copy or ownership transfer**, never shared mutable reference (the §14
  safety model). Immutable/pure values (strings, pure Arrays/Maps) share freely.
- **R-2. Join as a promise.** The handle is `Promise<T>`-shaped so `await handle` is the
  join — no second suspension surface (the framework's one-model rule).
- **R-3. An event loop per worker.** Each worker can run timers/sockets/`await`
  independently; the existing single-loop program is the degenerate 1-worker case.
- **R-4. Cross-thread streams.** A `StreamBuffer<T>` variant usable as an SPSC channel
  between two workers (the §13 lock-free ring over atomic head/tail — already envisioned).
  This is the sanctioned communication path; shared mutable objects stay gated.
- **R-5. Shared listener.** N workers must be able to accept on one TCP port. Cheapest
  shape: `sysTcpListen(port, reusePort: true)` (SO_REUSEPORT) so each worker owns a full
  accept→serve loop with zero shared state. An acceptor-plus-handoff primitive is an
  acceptable alternative; SO_REUSEPORT is simpler and share-nothing.

## 2. Known hard part (flagging, not solving here)

The escaping-tier **refcounts are not atomic** today. Options seen from the framework side:
atomic retain/release; or per-worker heaps + ownership transfer at the channel boundary
(send = move, like the capture rule) so counts never race. The second preserves today's
non-atomic fast path and matches the isolation-by-default model; it constrains R-4 to
transfer-or-copy semantics, which Atlantis is happy with. Shape-table mutation across
threads (info.md §14's sharpest interaction) can stay simply forbidden for v1.

## 3. What Atlantis does with it

`HttpServer(port, workers: N)` — N event loops, each accepting on the shared port;
per-request state already lives in `Context` (no ambient globals — the framework is being
built worker-safe in shape from day one, so adoption is a config flip). The MySQL pool
gains per-worker sub-pools (connections never cross workers — no locking needed).

## 4. Acceptance

1. Corpus: spawn/join returns a value; captures are copies (mutating the original after
   spawn is not observed by the worker); `await handle` parks the spawning loop only.
2. Two workers accept on one port (SO_REUSEPORT); loopback client sees both serve.
3. Channel corpus: 1M messages through a cross-worker stream, `live-at-exit` flat (+0B
   discipline, same as the churn corpus).
4. A deliberately-blocked worker does not stall its siblings' request handling.
5. Differential: oracle/IR may run workers serially (semantic reference); LLVM runs true
   threads; corpus output identical.

## 5. Interim fallback (already designed in)

Single-loop process; scale-out = N processes behind a reverse proxy. Pipeline, pool, and
Context are being designed with no ambient mutable globals so the flip to workers is
config, not rewrite.
