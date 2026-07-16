# Terminal raw mode (termios) — Technical Design

**Status:** proposal (nothing implemented). **Date:** 2026-07-07.
**Motivating case:** a kilo port. A full-screen TUI cannot exist until the
terminal stops line-buffering and echoing — this is the **#1 blocker** for
interactive terminal programs. **Related:** `argv.md` (same floor-native +
top-level-namespace shape, and the same comptime-hermeticity reasoning applies
verbatim); `terminal-winsize.md` and `signals.md` (the other two kilo runtime
gaps) share the `term` namespace and the restore-on-exit epilogue defined here.

Everything in §1 was verified against this worktree (`agent3`, Jul 7) at the
cited `file:line`.

---

## 1. Current state (verified)

- **No terminal control anywhere.** A grep for `termios|tcgetattr|tcsetattr|
  ICANON|ioctl` over `runtime/` and `src/RuntimeNatives.cpp` finds only
  socket-level matches (`MSG_NOSIGNAL`, Win32 `ioctlsocket(FIONBIO)`); there is
  no terminal primitive in the platform floor and no `sys*` native for it. The
  floor's declared interface (`runtime/lv_plat.h:22–70`) is map/unmap, write/
  read, now_ns, exit, open/close/stat, tcp, and poll — no `tcgetattr`/`ioctl`.
- **stdin is read line-at-a-time.** The only stdin path is `sysReadLine(fd)`
  (`src/RuntimeNatives.cpp:213`, `runtime/lv_runtime.c:lvrt_readline`), which
  blocks on a newline — cooked mode. `sysRead(fd, max)` (`:268` /
  `lvrt_sysread`, `runtime/lv_runtime.c:1264`) reads raw bytes, but with the
  kernel still in canonical mode it only returns after a line is submitted, and
  echo/signals/flow-control are all still on. There is no way to get
  keystroke-at-a-time input.
- **Output is a straight write.** `sysWrite(fd, s)` → `lv_plat_write` (unbuffered,
  `runtime/lv_plat_posix.c:29`). Escape sequences (cursor moves, screen clears)
  therefore already work as ordinary writes — the missing half is purely
  *input* discipline plus disabling `OPOST`.

Consequence: kilo's first two calls — `tcgetattr` to save the terminal, then
`tcsetattr` with the raw flags — have nowhere to bind.

## 2. This is a floor operation, not a stream

Raw mode is process/terminal *state*, set once and restored once — like argv
(`argv.md §2`), not like the timers and sockets that ride the event loop as
streams. It belongs in the same family as `sysStat`/`OpenMode`: a plain floor
native returning a plain result. Keystrokes that *arrive over time once raw mode
is on* are a stream (and read via `sysRead` in a poll loop, or later an
`InStream` over stdin), but the mode toggle itself is not.

The termios struct is platform-specific, so — following the floor rule that OS
types never escape a `lv_plat_*.c` file (the `LvPollFd`/`WSAPOLLFD` precedent,
`lv_plat.h:54–69`) — the save/restore state lives **inside the floor**, and the
language never sees a `termios`. The floor exposes an intent (“go raw” /
“restore”), not the mechanism.

## 3. Native + language API

### 3.1 Platform floor (`runtime/lv_plat.h` + `lv_plat_posix.c`)

Two new primitives; POSIX is the only place `<termios.h>` is included:

```c
/* raw mode: save the current terminal state (once) and switch fd to
 * character-at-a-time, no-echo, no-signal, no-flow-control, no-output-post-
 * processing. Returns 0 on success, -1 if fd is not a tty (ENOTTY) or the
 * ioctl fails. Idempotent: a second call while already raw is a no-op success
 * (the ORIGINAL state stays saved — never overwritten with the raw state). */
int lv_plat_term_raw(int fd);

/* restore the state saved by the matching lv_plat_term_raw. No-op (0) if raw
 * mode was never entered on this fd. Safe to call from the exit epilogue and
 * from a fatal path (async-signal-safe: a single tcsetattr, no allocation). */
int lv_plat_term_restore(int fd);
```

POSIX implementation (sketch, byte-for-byte the canonical kilo recipe):

```c
static struct termios g_saved; static int g_raw_active = 0; static int g_raw_fd = -1;
int lv_plat_term_raw(int fd) {
    if (g_raw_active) return 0;                       /* idempotent; keep first save */
    if (tcgetattr(fd, &g_saved) != 0) return -1;      /* ENOTTY if not a tty */
    struct termios r = g_saved;
    r.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);   /* IXON: ^S/^Q flow off */
    r.c_oflag &= ~(OPOST);                                     /* no \n -> \r\n xlate */
    r.c_cflag |= (CS8);
    r.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);            /* no echo/line/^C^Z */
    r.c_cc[VMIN] = 1; r.c_cc[VTIME] = 0;                       /* block for >=1 byte */
    if (tcsetattr(fd, TCSAFLUSH, &r) != 0) return -1;
    g_raw_active = 1; g_raw_fd = fd; return 0;
}
int lv_plat_term_restore(int fd) {
    if (!g_raw_active) return 0;
    int rc = tcsetattr(fd, TCSAFLUSH, &g_saved);
    g_raw_active = 0; return rc;
}
```

Single saved slot (not keyed by fd): a process drives one controlling terminal;
a second fd wanting raw mode is out of scope and documented, exactly as the
allocator ships one region per tier and logs multi-region as a future extension
(`lv_runtime.c:54–63`). `g_raw_active`/`g_raw_fd` are also what the exit epilogue
(§3.4) consults to decide whether a restore is owed.

`VMIN=1, VTIME=0` gives kilo's default blocking single-byte read; a
`term.rawTimed(ds)` variant (`VMIN=0, VTIME=ds`, tenths of a second) is noted for
non-blocking polls but not required for v1.

### 3.2 Floor native (`sysTerm*`)

Two `sys*` natives, wrapping the floor exactly like `sysStat`/`sysOpen`:

```
namespace std {
    int sysTermRaw(int fd);       // native: enter raw mode; 0 ok, -1 not-a-tty/fail
    int sysTermRestore(int fd);   // native: restore; 0 (also when never raw)
}
```

- Interpreters: `nativeFreeCall` arms calling `tcgetattr`/`tcsetattr` directly
  (`<termios.h>`), holding the same file-static saved slot — the pattern
  `sysStat` uses with `::stat` (`RuntimeNatives.cpp:253`).
- LLVM/emit-C++: `lvrt_systermraw`/`lvrt_systermrestore` in the C runtime routing
  to `lv_plat_term_raw`/`_restore`, declared+called like `lvrt_sysstat`
  (`LlvmGen.cpp:265`, `CGen.cpp`).

Because the name is `sys*`-prefixed, comptime evaluation **denies it
automatically** (the hermeticity gate, `Eval.cpp:358`; `reference.md:908`) — a
build must never toggle the build machine's terminal. Zero special cases, same
as `argv.md §3`.

### 3.3 Language surface — top-level `namespace term`

Mirrors `namespace env`/`math` (`argv.md §4`; `Resolver.cpp:1264`), a flat
top-level namespace so `term::` resolves on every engine without the nested-
namespace gap:

```
namespace term {
    bool enableRaw()  => std::sysTermRaw(0) == 0;    // stdin; true if it took
    void restore()    { std::sysTermRestore(0); }
    bool isRaw()      => ...                          // reads a prelude flag (below)
}
```

Functions, not bindings — I/O deferred to the call site (`argv.md §3`). `term`
is where `terminal-winsize.md` adds `size()` and `signals.md`’s SIGWINCH stream
is consumed; one namespace for the terminal.

Idiomatic usage (kilo's entry, and the reason restore-on-exit matters):

```
void main() {
    if (!term.enableRaw()) { console.writeln("not a terminal"); return; }
    // raw mode is on; a `try/finally`-free language means the runtime's
    // exit epilogue (§3.4) is the guarantee the terminal is restored even on
    // an uncaught throw or a mid-draw crash — not this function.
    Editor ed = Editor.open(env::args().skip(1));   // argv.md
    ed.run();                                        // read keys via sysRead, draw via sysWrite
    term.restore();                                  // normal-path restore
}
```

### 3.4 Guaranteed restore-on-exit (the degradable extra, folded in)

A TUI that dies in raw mode leaves the user's shell unusable (no echo, no line
editing) — so restore must be **guaranteed by the runtime, not the program**.
The floor already tracks `g_raw_active`; the runtime must call
`lv_plat_term_restore` on *every* exit path:

1. **Normal exit** — `runtime/lv_entry.c:main` after the loop drains, before
   `return lv_rt_exit_code()` (`lv_entry.c:29–39`): `lvrt_term_shutdown()` which
   restores iff `g_raw_active`.
2. **Uncaught exception** — the top-level uncaught reporter (`lvrt_uncaught`,
   the path `lv_rt_exit_code`’s comment cites, `lv_runtime.c:1393–1397`) restores
   *before* it writes the traceback, so the message lands in a sane terminal.
3. **Explicit exit** — `env.exit(code)` (`exit-codes.md`) restores in its
   epilogue before `lv_plat_exit`.
4. **Fatal `lv_die`** — `lv_die` (`lv_runtime.c:49`) gains a restore call before
   its `lv_plat_exit(1)`.

The restore is a single `tcsetattr` with no allocation — async-signal-safe, so
it is also what `signals.md`’s SIGINT/SIGTERM handling invokes. This is the
*only* new runtime wiring beyond the native; it composes with, and is a peer of,
the `exit-codes.md` epilogue (same four sites).

## 4. Per-backend coverage

| engine | primacy | raw-mode source |
|---|---|---|
| **LLVM** (`--build-native`) | **primary** | `lvrt_systermraw` → `lv_plat_term_raw` (POSIX `tcsetattr`); restore wired into `lv_entry.c` epilogue |
| tree-walk (`--run`) | interp | `nativeFreeCall "sysTermRaw"` → `tcgetattr`/`tcsetattr` directly |
| IR (`--ir`) | interp | same shared `nativeFreeCall` |
| emit-C++ (`--build`) | port | `rt_systermraw` in the generated runtime (`<termios.h>`); restore in generated `main`'s epilogue |
| pure ELF (`--emit-elf`) | **frozen** | **deferral diagnostic** — not extended |

**Frozen ELF (X64Gen): out of scope, by the freeze.** Raw mode needs the
`TCGETS`/`TCSETS` ioctls; adding them would mean new `X64Gen` codegen, which the
portable pivot forbids. `sysTermRaw` therefore hits the existing native
fallthrough and emits the clean `native floor function 'sysTermRaw'` deferral
(the exact mechanism `argv.md §5.4` relies on, `X64Gen.cpp:3854`) — a
compile-time error, never a miscompile. A TUI targets LLVM; ELF is the pre-pivot
reference backend and never gains a terminal. **Do not edit `X64Gen.cpp`.**

**Win32 floor** (`lv_plat_win32.c`): `lv_plat_term_raw`/`_restore` map to
`GetConsoleMode`/`SetConsoleMode` clearing `ENABLE_LINE_INPUT |
ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT` and enabling
`ENABLE_VIRTUAL_TERMINAL_INPUT` — same two-function interface, no caller change
(the `LvPollFd` portability precedent).

## 5. Edge cases

- **Not a tty** (piped stdin, CI): `tcgetattr` → `ENOTTY`; `sysTermRaw` returns
  -1, `term.enableRaw()` returns `false`. The program degrades (kilo prints
  “not a terminal” and exits) — never crashes. This is also what makes the
  feature safe to run in the test harness.
- **Double enable / enable-then-enable**: idempotent (§3.1) — the *original*
  cooked state is saved once and never clobbered by a raw-over-raw save.
- **Restore without enable**: no-op success (`g_raw_active == 0`).
- **`OPOST` off ⇒ `\n` no longer becomes `\r\n`.** Documented contract: a raw-
  mode program must emit `\r\n` itself (kilo does). This is a behavior of raw
  mode, surfaced in the `term` doc-comment, not a bug.
- **`ISIG` off ⇒ Ctrl-C/Ctrl-Z/Ctrl-\ are bytes, not signals.** The program
  reads `0x03` and decides. If the program *wants* SIGINT to still fire (rarer
  for a full TUI), it leaves `ISIG` — a future `term.rawKeepSignals()` flag,
  noted not built. Interaction with `signals.md`: with `ISIG` cleared there is
  no SIGINT to catch, so the restore-on-SIGTERM path (kill from another
  terminal) is what guards against an orphaned raw terminal.
- **Crash between enable and restore**: covered by §3.4’s runtime epilogue on
  all four exit paths — the whole point of putting restore in the runtime.

## 6. Phased plan

- **P0 — floor.** `lv_plat_term_raw`/`_restore` in `lv_plat.h` + POSIX; a
  `selftest.c` pty test (§7). Inert until a native calls it.
- **P1 — interpreters.** `sysTermRaw`/`sysTermRestore` in `nativeFreeCall` +
  the `term` prelude namespace. `--run`/`--ir` drive a real terminal.
- **P2 — LLVM primary.** `lvrt_systermraw`/`_restore` + `LlvmGen` wiring +
  the `lv_entry.c` restore epilogue (§3.4). Kilo builds native.
- **P3 — emit-C++.** `CGen` `rt_systermraw` + generated-`main` restore epilogue.
- **P4 — restore hardening.** Wire restore into `lvrt_uncaught`, `lv_die`, and
  `env.exit` (needs `exit-codes.md`). Ordered last because it spans the fatal
  paths; P2 already covers the common normal-exit case.
- **(frozen) ELF** — deferral diagnostic only.

## 7. Testing — pty-based, because raw mode needs a real tty

Raw mode can't be exercised over a pipe, so the differential harness gates on a
pty rather than the corpus stdin trick:

- **Floor unit test** (`runtime/selftest.c`): `openpty()`, `lv_plat_term_raw(fd)`,
  then `tcgetattr` the slave and assert `ICANON`/`ECHO`/`ISIG`/`IXON`/`OPOST`
  are cleared and `VMIN==1`; `lv_plat_term_restore(fd)` and assert the flags
  match the pre-raw snapshot exactly. Engine-independent; runs in CI.
- **Native end-to-end** (`tests/run_term.sh`): drive the built binary under a
  pty (a small `python3 -c 'import pty; pty.spawn(...)'` or `script -qc`),
  send a byte, assert the program echoes it back character-at-a-time (proving
  no line buffering) and that after exit the pty's termios is restored.
- **Restore-on-crash**: run a program that enables raw then throws uncaught
  under the pty; assert (a) the traceback is visible and (b) the pty is back in
  cooked mode — the §3.4 guarantee.
- **Non-tty degradation**: `printf '' | binary` asserts `enableRaw()==false`
  and a clean exit (the case that lets the rest of CI stay pipe-based).
- **comptime**: `comptime term.enableRaw()` must fail with the hermeticity
  error (the `sys*` deny), same assertion as `argv.md §10`.
- **ELF**: `--emit-elf` asserts the clean `sysTermRaw` deferral, no miscompile.
