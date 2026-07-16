# Summary: An unsubscribe / dispose surface for system streams

The stream substrate (§13) has no way to *stop* a standing subscription. `signal::on`
(`designs/complete/techdesign-terminal-floor.md` §8 open question #1) inherits this: a
subscriber's StreamBuffer, and the per-signal signalfd + `sysWatch` behind it, live until
program end. This is fine for a lifetime subscription (Sonar's `App` holds one WINCH sub for
its whole life) but blocks any component that subscribes to a signal for a bounded window.

## Request Details
Timers already have `cancel()`; sockets have `close()`; but a plain `InStream<T>` handed back
by `signal::on` (or any future demand-driven system stream) has no dispose operation. The
missing piece is a uniform "I am done consuming this stream" call that (a) drops the
subscriber's StreamBuffer from the fan-out, and (b) when the last subscriber for a source
leaves, tears down the underlying resource — for signals, `sysUnwatch` the fd and
`sysSignalClose` it (which unblocks the signal).

This is the stream-maturity counterpart to `using`/`IDisposable` (§12.7): a subscription is a
resource, and resources get deterministic release. It composes with the existing
`std::sysUnwatch` floor call and the terminal-floor `sysSignalClose` native — no new floor
work, only a language surface and the fan-out bookkeeping to know when a source has zero
subscribers.

## Requested Specific Feature
Preferred shape: `InStream<T>` gains a `close()` (or the subscription handle returned by
`subscribe` does), and `signal`'s `SignalState` registry drops the subscriber's buffer, then
`sysUnwatch` + `sysSignalClose` when its fan-out list for that signal empties. Ideally this
rides `IDisposable` so `using` works: `using InStream<int> winch = signal::on(signal::WINCH);`
auto-closes on scope exit.

## Known Warnings
- Removing a subscriber mid-delivery (the fan-out loop in `signal::deliver` is iterating) must
  be safe — snapshot or defer the removal.
- The signalfd unblock-on-last-close must not race a delivery already queued in the fd.
- Broadcast semantics (§13): closing one subscriber must not disturb the others sharing the fd.

## Acceptance Criteria
1. A subscriber can dispose its subscription; it stops receiving deliveries.
2. When the last subscriber for a signal leaves, the fd is unwatched, closed, and the signal
   unblocked (observable: the signal returns to default disposition).
3. `using` works over the returned stream (deterministic close on scope exit).
4. Differential green on oracle / IR / LLVM.

## Interim Fallback
`signal::on` v1 leaks the subscription until program end (documented in the design). The
interpreter's `interpSignalCleanup()` and a compiled process's exit both reclaim the fds and
unblock at program end, so nothing leaks *across* runs — only *within* a run's lifetime, which
is acceptable for the lifetime-subscription use cases v1 targets (Sonar).
