# Tech Design: Terminal Floor — Winsize + Signals-as-Streams

**Status:** design, pre-implementation. **Date:** 2026-07-12. **Feature ID:** F2 (Sonar prerequisite).
**Supersedes:** `designs/complete/terminal-winsize.md` + `designs/complete/signals.md` (both absorbed and moved to complete/ with this landing; their pre-decided rulings stand verbatim unless restated here). **Depends on:** none. **Unblocks:** Sonar T09 (resize + raw-mode safety).
**Difficulty:** S/M. **Risk:** LOW — additive platform-floor natives on the established `lv_plat` pattern.

Frozen surface: anchor §4/F2. Ground truth: infodemp §3. Raw mode is ALREADY LANDED on all four backends with crash-safe restore (`lv_plat_posix.c:85/:99`, `lvrt_term_shutdown` `lv_runtime.c:2211`, exit hook `lv_entry.c:68`, `term::enableRaw()` `Resolver.cpp:2558`) — the `designs/complete/terminal-raw-mode.md` "proposal" header is stale; do not redesign it.

---

## 1. Motivation

Sonar's `App` must (a) size its root at startup and re-layout on terminal resize, and (b) guarantee terminal restore on external kill. The floor today has neither a size op nor any signal surface (`lv_plat.h` inventory ends at poll — infodemp §3). Two pieces, both already fully proposed; this design promotes them jointly as the "terminal floor completion."

Axis-mapping flag for consumers: `WinSize` is rows/cols; Sonar `Size` is w=cols/h=rows — the mapping is Sonar's job, noted so nobody "fixes" the floor.

## 2. Design — Winsize

- **Floor:** `int lv_plat_term_size(int fd, int* rows, int* cols)` — POSIX `ioctl(TIOCGWINSZ)`; a 0×0 report is treated as failure; Win32 `GetConsoleScreenBufferInfo` (srWindow extent). Added to `lv_plat.h` + both platform files.
- **Native:** `int sysWinSize(int fd, int field)` — field 0=rows, 1=cols; -1 = failure (the `sysStat` field-indexed shape; avoids a multi-return native).
- **Language:** `class WinSize { int rows; int cols; }`; `term::size() -> WinSize`: try `sysWinSize(1, …)`; on failure, if raw mode is active, the in-language cursor-report fallback — write `\x1b[999C\x1b[999B\x1b[6n`, read, parse `\x1b[<r>;<c>R` via indexOf/subStr/toInt; final default `WinSize(24, 80)`. (Fallback guarded on raw mode because cooked mode can't read the report unbuffered.)
- Engines: prelude decl + `RuntimeNatives.cpp` clause (oracle+IR) + `lv_runtime.c` native + LLVM binding/row/`kCovered[]` + CGen clause — winsize is loop-independent so **all four active lanes** get it. ELF: existing native fallthrough; not a target.

## 3. Design — Signals as streams

Ruling (from signals.md, normative): **language code NEVER runs in signal context.** A signal is a readable system stream.

- **Floor:** `int lv_plat_signal_open(const int* sigs, int n)` → readable fd (Linux `signalfd` primary; portable fallback = self-pipe whose handler body is exactly `write(pipe_wr, &signo, 1)`); `int lv_plat_signal_next(int fd)` → signo or -1; `void lv_plat_signal_close(int fd)`.
- **Natives:** `sysSignalOpen(Array<int>) -> int`, `sysSignalNext(int) -> int`, `sysSignalClose(int)` — composing with the EXISTING `sysWatch`/`sysUnwatch` loop (the fd becomes just another watched fd; the loop stays dumb).
- **Language:**
  ```lev
  namespace signal {
      const int HUP = 1;  const int INT = 2;  const int QUIT = 3;
      const int USR1 = 10; const int TERM = 15; const int WINCH = 28;
      InStream<int> on(int sig);      // Timer-shaped: watch callback drains signalfd, pushes signo into a StreamBuffer
  }
  ```
  One open fd per signal number, fanned to N subscribers (each subscriber its own StreamBuffer — broadcast is a Layer-2 reshaping per §13 doctrine).
- **Pre-decided edge rulings (normative, from signals.md):** SIGKILL/SIGSTOP rejected with a thrown RuntimeException; no pre-subscription buffering; SIGWINCH coalesces naturally ("at least one tick after the last change" — the fd read drains all pending); the interpreter must unblock + close signal fds at program end (the `leviathan` process outlives the program); with ISIG cleared in raw mode, `signal::on(INT)` fires only from external `kill`.
- **Raw-mode safety obligation:** entering raw mode installs handlers for SIGTERM/HUP/INT/QUIT whose ENTIRE body is `lv_plat_term_restore` + re-raise default (async-signal-safe; composes with `lvrt_term_shutdown`'s idempotent restore).
- **Lane matrix (honest):** `sysWinSize` — all four lanes. `signal::on` — rides the event loop: oracle/IR (RuntimeLoop) + LLVM (lv_loop); **emit-C++ compiles the decls but the stream is loop-bound** (same boundary as timers/sockets there). Windows v1: signal floor stubbed to always-fail (POSIX-first; the spawn/Channel Windows-reject precedent).

## 4. Implementation plan

| M | step | difficulty |
|---|---|---|
| M1 | `lv_plat_term_size` (posix + win32) + `sysWinSize` on all four lanes + `WinSize`/`term::size()` prelude (incl. fallback + default) | S |
| M2 | `lv_plat_signal_open/next/close` (signalfd + self-pipe fallback selected at build/runtime) + the three natives (oracle/IR + LLVM) | M |
| M3 | `namespace signal` prelude (consts + `on` over StreamBuffer + sysWatch; per-signal fd registry + fanout) | M |
| M4 | raw-mode safety handlers (install on first `enableRaw`, restore-and-reraise) + interpreter end-of-program mask/fd cleanup | S/M |
| M5 | corpus + pty harness | M |

## 5. Edge cases & failure modes

Not-a-tty fd (pipe) → sysWinSize -1 → default path; 0×0 winsize (some multiplexers) → failure per ruling; two subscribers to one signal each observe every delivery; subscribing to the same signal twice from one program; signal arriving before any subscriber (dropped — no pre-subscription buffering, pinned by test); `\x1b[6n` fallback interleaving with user input (constrained: only used pre-loop / documented — Sonar calls `term::size()` before starting its input source).

## 6. Potential issues & mitigations

1. **signalfd requires the signal be blocked** (`sigprocmask`) — and spawn workers (true OS threads on LLVM) must inherit the mask BEFORE `pthread_create`, or deliveries land on random threads. Mitigation: set the process mask in `lv_plat_signal_open`; the worker-spawn path in `lv_thread.c` inherits the creating thread's mask automatically (pthread semantics) — verify ordering at implementation; pin with a spawn+signal test.
2. **Conflict with the scheduler's SIGSEGV crash reporter** (`lv_task.c:379-384`) — disjoint signal sets; the safety handlers (TERM/HUP/INT/QUIT) never touch SEGV. Stated to prevent an implementation "cleanup."
3. **Self-pipe full under signal storms** — the handler's write may fail; dropping is CORRECT (coalescing semantics). Comment the intent in the handler so it isn't "fixed."
4. **Double-restore** — safety handlers + `lvrt_term_shutdown` + Sonar's `using` teardown all restore; `lv_plat_term_restore` must stay idempotent (it restores a saved termios — naturally idempotent; add the guard if the win32 path isn't).
5. **fd lifetime** — signal fds held by the registry keep the loop alive (`hasWork`); program end must close them or the process never exits. M4's cleanup + a dedicated exit test.
6. **Fallback parse garbage** — a terminal that answers `\x1b[6n` with junk: parser returns failure → default 24×80; never throws.

## 7. Testing plan

Corpus: winsize on a pty (python driver in `fuzz/` following `thread_leak.py`'s pattern: `openpty` + `TIOCSWINSZ` + read-back assert); winsize failure on a pipe → default; `signal::on(USR1)` self-`kill` delivery; two-subscriber fanout; WINCH coalescing (5 rapid resizes → ≥1 tick, final size correct); SIGTERM-under-raw restores termios then dies (pty driver asserts termios + exit status); interpreter-exit cleanliness (program with a signal sub ends; `leviathan` process exits). Differential oracle/IR/LLVM; emit-C++ compile-only for stream parts, full for sysWinSize.

## 8. Open questions

1. `signal::off`/unsubscribe surface (streams have no unsubscribe yet) — v1 leaks the subscription until program end; acceptable for Sonar (App holds one WINCH sub for its lifetime); revisit with stream maturity work.

## 9. Implementation log

- 2026-07-12 — design written; not started.
- 2026-07-12 — **LANDED IN FULL (M1–M5)** on the four active engines.
  - **M1 winsize.** `lv_plat_term_size` (POSIX `ioctl(TIOCGWINSZ)`, 0×0 → failure;
    Win32 `GetConsoleScreenBufferInfo` srWindow) + `lv_plat_term_israw`. Native
    `sysWinSize(fd, field)` (field 0=rows/1=cols/-1) on all four lanes
    (RuntimeNatives oracle+IR direct `ioctl`; `lvrt_syswinsize` for LLVM;
    `rt_winsize` for emit-C++). `class WinSize` + `term::size()` with the
    raw-mode-gated `\x1b[6n` cursor-report fallback → 24×80 default; `term::isRaw()`
    (the deferred §3.3, now backed by `sysTermIsRaw` on all four lanes). The fallback
    is deliberately **int-sentinel** (no `T?`/narrowing — LLVM misreads prelude
    narrowing, [[leviathan-prelude-no-narrowing]]). emit-C++ gained a real
    `sysRead(fd,max)` (`rt_read`) since `term::size()` pulls it in.
  - **M2 signal floor.** `lv_plat_signal_open/next/close` (Linux `signalfd`
    primary, self-pipe `#else` fallback; blocks the set, rejects SIGKILL/SIGSTOP,
    a per-fd sigset registry so close unblocks exactly its signals). Natives
    `sysSignalOpen(Array<int>)`/`sysSignalNext`/`sysSignalClose` on oracle/IR
    (direct signalfd) + LLVM (`lvrt_syssignal*`, boxed-array read via lv_abi). Not
    on emit-C++ (loop-bound — rejected with a clean coverage error, matching
    timers/sockets).
  - **M3 signal streams.** `namespace signal` (consts HUP/INT/QUIT/USR1/TERM/WINCH)
    over a **plain-data holder global + demand-compiled free functions** —
    critical: a methods-on-a-global-instance shape forced `sysSignalOpen` into
    *every* emit-C++ compile (a global instance compiles all its methods); the
    split keeps the eager global native-free so a non-signal program still builds
    on emit-C++. One signalfd + one `sysWatch` per distinct signal number, fanned
    to N per-subscriber StreamBuffers; the watch callback drains all pending
    (WINCH coalescing) and broadcasts.
  - **M4 safety + cleanup.** `enableRaw()` installs SA_RESETHAND restore-then-
    reraise handlers for SIGTERM/HUP/INT/QUIT (floor `lv_plat_posix.c`, interp
    `RuntimeNatives.cpp`, emit-C++ `rt_term_install_safety`) — disjoint from
    lv_task.c's SEGV reporter (issue #2), idempotent restore (issue #4). Interp
    `interpSignalCleanup()` at the end of `Eval::run`/`IrInterp::run` closes every
    signal fd and unblocks its signals (the leviathan process outlives the
    program, issue #5).
  - **M5 tests.** `tests/corpus/floor/` + `tests/floor_pty.py` + `tests/run_terminal_floor.sh`
    (CTest `terminal_floor`): winsize non-tty→24×80 (4 lanes) + real-pty TIOCGWINSZ
    read-back, USR1 delivery, two-subscriber fanout, WINCH coalescing (5 resizes →
    ≥1 tick, final 40×120), SIGTERM-under-raw termios restore, emit-C++
    sysWinSize-full/signal-rejected, and comptime hermeticity — all green on
    run/ir/build/build-native. No ELF lane (§17 X64Gen frozen; §2 "existing native
    fallthrough; not a target").
  - **Deviations from the written design:** none material. Two additions the design
    implied but didn't name: `sysTermIsRaw`/`term::isRaw()` (the §2 fallback guard
    the design's own §3.3 note promised would land here) and emit-C++ `sysRead`
    (pulled in by `term::size()`). One structural choice the design left open:
    the signal registry is a data-holder global + free functions, not a class with
    a global instance, to keep emit-C++ compiling non-signal programs.
