# Request: Promise Rejection Semantics (LA-19)

**From:** Atlantis Tracks 05 (MySQL driver), 01 (kernel), 07 (clients). **Date:** 2026-07-07.
**Priority:** P0 **if** the P6 probe below fails; possibly already-working — probe first.
**Language status:** this is info.md §19 open decision **#7** ("what `await` does with a
rejected promise") — the framework is now the forcing function to close it.

## 1. The problem

A DB driver's `query()` returns `Promise<ResultSet>`. When the server replies with an ERR
packet, the awaiting caller must receive a **failure**, not hang and not get a fabricated
value. Today `Promise<T>` documents `resolve(v)`/`isReady()`/`get()`/`then(cb)` — there is
no documented failure path. Every async boundary in the framework (driver, HttpClient,
pool acquire, MCP dispatch) has this shape.

## 2. The probe (Track 05 P6 — run before designing anything)

Since `await` pumps the loop rather than truly suspending, a `throw` inside the
promise-completing callback may already propagate to the awaiter via ordinary unwinding.
Probe: a promise completed by a timer callback that throws; does the exception surface at
the `await` site, catchable by `try/catch` around it, on oracle + IR + LLVM? If YES on all
three, document the semantics in reference.md and this ticket closes as docs-only.

## 3. Requested semantics (if the probe fails)

The language's own stances point at one answer — **rejection is an exception carried by
the promise, rethrown at the await site**:

- `p.reject(e)` where `e : IException` (mirror of `resolve`) — the completing side's verb.
- `await p` on a rejected promise **throws `e`** at the await site — `catch` selects by
  type/interface exactly as if the failure were local (resolution-by-type, §12.6; no
  second error channel, no `.catch()` callback API — collapse #3).
- `p.then(cb)` on a rejected promise: does not invoke `cb`; the rejection surfaces when
  (and only when) the promise is awaited — or terminates as an ordinary uncaught
  exception at loop drain if never observed (loudness rule: an unobserved rejection must
  not vanish silently; a drained-loop report naming the exception is enough).
- Expected-outcome failures stay unions (`Promise<Row?>` etc.) — rejection is for the
  *exceptional* path only, same line §12.6 already draws.

## 4. Acceptance

1. Corpus: reject-before-await, reject-after-park, reject-never-awaited (drain report),
   catch-by-interface at the await site — identical on oracle/IR/LLVM.
2. Churn corpus: a rejected promise's exception object is released (+0B discipline).
3. reference.md §6.6.54 documents the full contract.

## 5. Interim fallback (designed in, ugly, deletable)

Result-shaped payloads at the seams that cannot wait (`Promise<QueryOutcome>` where
`QueryOutcome` is a value/error union) — contained inside the driver, not exposed through
C3, deleted the day this lands.

## 6. Status (2026-07-12, LA-30 M5 flip)

**Scheduled as the one-milestone follow-up doc 5 §7 recommends, on the
now-landed task substrate.** The M5 flip answered §2's probe P6 by design
ruling, not by experiment: a callback throw **must not** propagate to the
awaiter (correction C2, reference.md §6.6.67) — the docs-only close in §2's
YES branch is ruled out; build the `reject` path of §3. LA-30 already
delivers: precise routing (C2), the loud drained-await (C3 — the "unobserved
rejection at loop drain" report of §3 has its mechanism: the `drained_wakes`
stat counts C3 throws), and `Worker.reject`'s rethrow-at-await shape to
generalize from. Remaining work is exactly §3's carrier: store the
`IException` object (not just `failMessage` — mind bug.md #1's history on
optional fields), `then` on a rejected promise, and the never-observed drain
report line. Acceptance stays §4 verbatim.
