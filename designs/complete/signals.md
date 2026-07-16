# Signals as system streams (SIGWINCH & friends) — Technical Design

**Status:** proposal (nothing implemented). **Date:** 2026-07-07.
**Motivating case:** kilo’s **live resize** — redraw when the terminal window
changes size (SIGWINCH) — plus the safety obligation that an external `kill`
(SIGTERM/SIGINT) restores the terminal instead of leaving a raw, echo-less
shell. **Related:** `terminal-raw-mode.md` (the restore this doc protects),
`terminal-winsize.md` (what a SIGWINCH tick re-queries). Consistent with
`argv.md` (calls not bindings; comptime `sys*` deny) and with the language’s
existing stream model.

> **Supersedes a deliberate omission.** `designs/complete/techdesign-08-system-natives.md`
> **avoids** signals on purpose — “signals + the loop is a can of worms
> deliberately avoided” (§7 note) and “Any signal-handler temptation (SIGCHLD et
> al.)” (§12 STOP). That caution is correct *about running language code in a
> signal handler*. This design removes the can of worms by **never running
> language code in signal context** — signals become ordinary event-loop streams
> via `signalfd`/self-pipe, the exact mechanism timers and sockets already use.

Verified against this worktree (`agent3`, Jul 7).

---

## 1. Current state (verified)

- **No signal handling of any kind.** `grep signal|sigaction|signalfd|
  self.?pipe runtime/ src/RuntimeNatives.cpp` finds only `MSG_NOSIGNAL`
  (suppressing SIGPIPE on `send`, `lv_plat_posix.c:131`). No signal is caught,
  blocked, or observed; a SIGWINCH is ignored, a SIGTERM kills the process with
  default disposition.
- **But the delivery machinery already exists.** The runtime has a poll-driven
  event loop with fd-watches: `sysWatch(fd, cb)` “fire cb when fd is read-ready”
  (`Resolver.cpp:776`), backed by `lv_plat_poll` (`lv_plat.h:70`,
  `lv_plat_posix.c:138`) and consumed by `TcpStream.onData` (`Resolver.cpp:784`).
  And the **system-event-as-stream pattern is already blessed**: a timer “is a
  SYSTEM STREAM (§13): the loop pushes tick numbers into a real `StreamBuffer`,
  so subscribe/pull are the ordinary stream machinery” (`Resolver.cpp:749–762`,
  the `Timer` class over `sysTimerStart`).

So a signal has an obvious, already-paved home: **another readable fd feeding a
`StreamBuffer`.** No new event-loop concept is required — only a way to turn a
signal into a readable fd.

## 2. Design: a signal is a system stream (never a handler)

The unsafe thing is running an allocator/ARC/language callback inside an async
signal handler. We don’t. Two equivalent floor realizations, both making the
signal a **readable fd** the existing watch delivers:

- **Linux (primary): `signalfd`.** Block the signals in the process mask, open a
  `signalfd(2)` for them. There is **no handler at all** — the kernel queues
  `signalfd_siginfo` records the loop reads like any socket. Cleanest possible.
- **Portable POSIX fallback: self-pipe.** A tiny `sigaction` handler whose whole
  body is `write(pipe_wr, &signo_byte, 1)` — async-signal-safe (`write` is on
  the safe list), no allocation, no language code. The read end is the watched
  fd.

Either way, the language sees a stream of signal numbers pushed by the loop —
identical in shape to `Timer.ticks()`. This is the “§13: system events are
streams” rule applied to signals, and it is what defuses techdesign-08’s
objection: the objection is to *handlers*, and there is no language-level
handler here.

## 3. Native + language API

### 3.1 Floor (`runtime/lv_plat.h` + `lv_plat_posix.c`)

```c
/* Block `sigs` in the process mask and return a readable fd that becomes ready
 * when any of them is pending. Linux: signalfd. Portable: self-pipe + sigaction.
 * Returns the fd (>=0) or -1. The fd is set non-blocking and is meant to be
 * handed straight to the existing poll/watch loop. */
int lv_plat_signal_open(const int* sigs, int n);

/* Drain one pending signal number from the fd (the loop calls this when the fd
 * is read-ready); returns the signo, or -1 if none/would-block. Signalfd:
 * read one struct signalfd_siginfo. Self-pipe: read one byte. */
int lv_plat_signal_next(int fd);

void lv_plat_signal_close(int fd);   /* close fd; unblock/restore default disposition */
```

Signal *numbers* cross the boundary as plain ints — no `siginfo` struct escapes
the floor (the `LvPollFd` opacity rule).

### 3.2 Native surface

```
namespace std {
    int sysSignalOpen(Array<int> sigs);   // native -> watchable fd, -1 on failure
    int sysSignalNext(int fd);            // native -> next signo, -1 if none
    int sysSignalClose(int fd);
}
```

These compose with the **existing** `sysWatch`/`sysUnwatch` (`Resolver.cpp:776`)
— no new loop primitive. `sys*`-prefixed ⇒ comptime-denied automatically
(`Eval.cpp:358`): a build never touches the build machine’s signal disposition.

### 3.3 Language surface — top-level `namespace signal`

Signal-number constants (Linux values; a `Win32` map lives in its floor) as
prelude globals, and a `Signal`-stream built exactly like `Timer`:

```
namespace signal {
    const int INT  = 2;   const int QUIT = 3;   const int TERM = 15;
    const int HUP  = 1;   const int USR1 = 10;  const int WINCH = 28;

    // A system stream of ticks (the signal number) — subscribe/pull are the
    // ordinary stream machinery, like Timer.ticks() (Resolver.cpp:749). One
    // signalfd per subscribed set, its readable fd registered with sysWatch;
    // the watch callback drains sysSignalNext and pushes into a StreamBuffer.
    InStream<int> on(int sig) { ... }     // built over sysSignalOpen + sysWatch
}
```

kilo’s live-resize loop — the motivating use, and note it reuses
`terminal-winsize.md` with zero new glue:

```
InStream<int> resizes = signal.on(signal.WINCH);
resizes.forEach((sig) => editor.onResize(term.size()));   // re-query on each tick
```

Because the tick arrives on the event loop, `onResize` runs as ordinary language
code between keystroke reads — no reentrancy, no async-signal-safety burden on
the program at all.

### 3.4 The safety obligation: restore the terminal on external kill

This is the *reason signals are not purely optional* for a TUI. With raw mode on
and `ISIG` cleared (`terminal-raw-mode.md §5`), Ctrl-C is a byte — but an
external `kill` (SIGTERM), a closed SSH session (SIGHUP), or `kill -INT` still
terminates with default disposition, **skipping the exit epilogue** that restores
the terminal (`terminal-raw-mode.md §3.4`) — leaving a broken shell.

Fix, and it is small and async-signal-safe: **when raw mode is entered**, the
runtime installs handlers for `SIGTERM/SIGHUP/SIGINT/SIGQUIT` whose entire body
is `lv_plat_term_restore(fd)` (one `tcsetattr`, on the safe list) followed by
re-raising the signal with the default handler so the process still dies with the
right status. This is runtime-internal (not a language-visible `signal.on`) — it
guarantees the raw terminal is always restored, closing the last gap in
`terminal-raw-mode.md §3.4`. A program that *also* `signal.on(signal.TERM)`s for
graceful shutdown composes: its stream tick fires first (via the self-pipe/
signalfd path), and if it doesn’t exit, the default disposition eventually does.

## 4. Per-backend coverage

| engine | primacy | signal source |
|---|---|---|
| **LLVM** (`--build-native`) | **primary** | `lvrt_syssignalopen` → `lv_plat_signal_open` (`signalfd`); fd rides the existing `lv_loop.c` poll/watch |
| tree-walk / IR (`--run`/`--ir`) | interp | `nativeFreeCall` + `RuntimeLoop` fd-watch (the same `addTimer`/watch used by `TcpStream`) — see §5 interpreter note |
| emit-C++ (`--build`) | port | `rt_syssignalopen` (`signalfd`/self-pipe) + the generated loop |
| pure ELF (`--emit-elf`) | **frozen** | **deferral diagnostic** — not extended |

**Frozen ELF:** `signalfd`/`rt_sigprocmask` are new syscalls that would require
new `X64Gen` emission — forbidden. `sysSignalOpen` hits the native fallthrough →
clean `native floor function 'sysSignalOpen'` deferral (`argv.md §5.4`;
`X64Gen.cpp:3854`). An ELF TUI simply doesn’t live-resize; the terminal-restore
safety of §3.4 is likewise LLVM/interpreter-only on the frozen backend (an ELF
kilo relies on the normal-exit epilogue only). **Do not edit `X64Gen.cpp`.**
**Win32:** no POSIX signals; `lv_plat_signal_open` maps `WINCH` to a
`SetConsoleCtrlHandler` / window-buffer-size-event fd-shim, `INT`/`TERM` to
`CTRL_C_EVENT`/`CTRL_CLOSE_EVENT`; same fd-to-loop shape.

## 5. Edge cases

- **Interpreter shares the `leviathan` process** (`--run`/`--ir` run inside the
  compiler). Blocking signals + a signalfd changes the whole process’s
  disposition for the duration of the run — e.g. Ctrl-C is delivered to the
  program’s stream, not killing `leviathan`. Contract: the interpreter installs
  the mask/fd at program start and **unblocks + closes on program end**
  (`sysSignalClose` in the run teardown), restoring `leviathan`’s own
  disposition. Documented, tested (§7).
- **Coalescing**: multiple SIGWINCH between two poll cycles collapse to one
  readable wakeup (self-pipe) / a bounded queue (signalfd). Fine — resize is
  idempotent (re-query `term.size()`); the stream promises “at least one tick
  after the last change,” not one-per-signal. Documented so a program never
  counts ticks.
- **Signal before subscription**: not delivered (the signalfd/pipe is created by
  `signal.on`); a program subscribes before entering its loop. Signals are not
  buffered pre-subscription — standard, documented.
- **Uncatchable signals** (`SIGKILL`, `SIGSTOP`): `sysSignalOpen` rejects them
  (they can’t be blocked); `signal.on(SIGKILL)` throws/returns an empty stream.
- **Multiple subscribers to one signal**: one signalfd per signal, its
  `StreamBuffer` fanned to each `InStream` subscriber (the existing multi-reader
  stream shape) — not N signalfds racing the same signal.
- **Raw-mode interaction**: with `ISIG` cleared, `signal.on(signal.INT)` only
  fires from an *external* `kill -INT`, not Ctrl-C (which is byte `0x03`). Called
  out so a program doesn’t wait on a stream that its own keystrokes can’t feed.

## 6. Phased plan

- **P0 — floor:** `lv_plat_signal_open/next/close` (signalfd primary, self-pipe
  fallback compiled per-platform); a `selftest.c` raise-and-read test.
- **P1 — interpreters:** `sysSignal*` natives + the run-teardown unblock (§5) +
  the `signal` prelude namespace and `Signal` stream over `sysWatch`. `--run`/
  `--ir` observe SIGWINCH/SIGUSR1.
- **P2 — LLVM primary:** `lvrt_syssignal*` + `LlvmGen` wiring; the signalfd rides
  the emitted loop drain (`LlvmGen.cpp:577` loop).
- **P3 — raw-mode restore handlers (§3.4):** install on `term.enableRaw`,
  remove on `term.restore`. Ordered after `terminal-raw-mode.md` P2/P4.
- **P4 — emit-C++:** `rt_syssignal*` + generated-loop integration.
- **(frozen) ELF:** deferral only.

## 7. Testing — self-delivered signals, no external actor

- **Floor unit** (`selftest.c`): `lv_plat_signal_open([SIGUSR1])`, `raise(SIGUSR1)`,
  assert `lv_plat_signal_next` returns `SIGUSR1`; a second read returns -1.
  Engine-independent.
- **Stream differential** (`tests/run_signal.sh`): a program subscribes
  `signal.on(SIGUSR1)`, arranges a tick (a child `kill $PPID -USR1`, or a timer
  that `raise`s), asserts exactly one stream tick with value `SIGUSR1` on
  `--run`, `--ir`, `--build-native`, `--build` (five-engine minus ELF).
- **SIGWINCH → resize** (pty, `tests/run_term.sh`): change the pty size
  (`TIOCSWINSZ`), assert the program’s `onResize` fires with the new geometry —
  ties winsize + signals + the loop together.
- **Terminal-restore-on-SIGTERM** (pty): raw-mode program, external `kill -TERM`,
  assert the pty returns to cooked mode (the §3.4 guarantee) and the process
  dies with the SIGTERM status.
- **Interpreter disposition restore** (§5): after `--run` of a program that
  subscribes SIGINT, assert a subsequent Ctrl-C to `leviathan` itself behaves
  normally (mask/fd were torn down).
- **comptime**: `comptime signal.on(...)` fails via the `sys*` hermeticity deny.
- **ELF**: `--emit-elf` asserts the clean `sysSignalOpen` deferral.
