# Threads corpus — native (LLVM true OS threads)

Track 10 M3c (`designs/complete/techdesign-threads-3.md`). The **LLVM lane** for `spawn`/
`Worker<T>`/join, where a worker is a real pthread with its own TLS heap+arena+
loop and the result rebuilds back on the spawner's thread through an eventfd join
(`runtime/lv_thread.c`). This directory has **no interpreter lane** — it is the
true-thread counterpart of `threads_serial/`.

- `spawn_join.lev` — a byte-identical copy of `threads_serial/spawn_join.lev`:
  the SAME program, the SAME `.expected`, run on true threads. A drift from the
  serial oracle's output is an isolation/flatten bug, never a tolerance (§7/§11).
- `reject_rethrow.lev` — a worker body throws; the true-thread leg flattens its
  exception's message, the spawner rebuilds and rethrows at `await` (§3.3). Also
  byte-identical to the serial run.
- `blocked_worker.lev` — **LLVM-only** (acceptance #4): a `while(true)` worker
  while a sibling computes and returns. Meaningless serially, where the single
  cooperative loop would hang. The spinner is never awaited; the program exits
  via `env::exit` — the sanctioned way out with a live worker (reaps < spawns,
  here and only here).
- `channel.lev` — a byte-identical copy of `threads_serial/channel.lev` on the
  **native lock-free ring** (§6): cross-thread ping-pong (producer worker ->
  main consumer) and worker-consumes (main producer -> worker consumer) over the
  process-global record + two eventfds, plus the same-thread struct-copy /
  overflow / closed-send cases. Byte-identical to the serial in-process queue.

Output-determinism discipline (§8) is load-bearing: workers compute and
return/send; only the spawning thread prints, at join points. Never print from a
worker body — that would race stdout ordering across real threads.
