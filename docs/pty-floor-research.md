# PTY floor — technical research dossier

**Status:** pre-design research. **Not** a tech design. This is the complete
technical substrate a tech design for a `lv_plat_pty_*` floor surface (allocating a
pseudo-terminal for a **child** process) must build on: what the process/event floor
already provides, exactly how a pty must be set up on POSIX and Windows, the hard
determinism problem that gates golden testing, what every serious precedent
(node-pty, libuv, portable-pty/WezTerm, Alacritty) actually does, and the questions
the design has to answer. Every runtime claim is anchored to code (`file:line`)
verified against this tree (`agent1`, 2026-07-16); every OS claim is anchored to a
man page section or an MSDN page.

Audience: whoever writes the tech design, and the implementers after them. Primary
target is **Linux**; **macOS** divergences are called out explicitly; **Windows** is
chartered (ConPTY) but POSIX-first.

Charter constraints treated as frozen (not relitigated): the process floor
(`lv_plat_spawn`/`pidfd_open`/`reap`/`kill`), the single-threaded poll(2) event
model (`LvPollFd`), signals-as-streams (`lv_plat_signal_*`), host-terminal raw/
winsize floors (`lv_plat_term_*`), and the Windows spawn/thread compile-time reject
that this effort is chartered to lift for the pty path. See
`runtime/lv_plat.h:124-157` for the interface these must compose with.

---

## 0. One-paragraph orientation

A pty is a **kernel-provided bidirectional character device pair** — a *master*
(what the host holds) and a *slave* (what the child sees as its controlling
terminal). The floor's job is narrow and mechanical: **allocate the pair, wire the
slave onto the child's 0/1/2 as its controlling tty, hand the master back as a
single pollable fd, and expose a resize knob** — nothing above the byte level (VT
parsing is in-language, Helm-side, and explicitly out of scope). On POSIX this is
almost entirely a **variation on the spawn floor we already have**
(`lv_plat_posix.c:462`): the only new machinery is opening the pty master
(`posix_openpt`), a different child-side dup2/`setsid`/`TIOCSCTTY` prologue, and one
`ioctl(TIOCSWINSZ)`. The master is an ordinary fd — it drops straight into the
existing `poll(2)` loop and the existing `pidfd`/`reap` exit machinery still works
unchanged. On **Windows** the story is genuinely different: ConPTY
(`CreatePseudoConsole`) is not a byte-transparent pipe — **it re-renders the child's
output into VT sequences of its own**, its pipes are **anonymous and not pollable
with WSAPoll**, and closing it can **deadlock** if the output pipe isn't being
drained. The two hard problems the design must solve are therefore (1) a
**poll-compatible bridge** for ConPTY's un-pollable pipes (the precedents all use a
hidden reader thread — acceptable here under the same rule as the signal self-pipe:
*no language code runs on it*), and (2) **deterministic three-lane golden testing**,
which is achievable on POSIX (same kernel, so oracle/IR/LLVM are byte-identical
given a fixed child termios) but **not** cross-OS (ConPTY's re-rendering means
Windows needs its own expected files or behavioral asserts).

---

## TL;DR — findings that change the design

1. **Use `posix_openpt` + `grantpt` + `unlockpt` + `ptsname_r`, NOT `forkpty`.**
   `forkpty` fuses pty allocation with `fork` and does non-async-signal-safe work
   (it calls `ptsname`, opens the slave by name, and on glibc runs the login_tty
   dance) *in the child between fork and exec* — the exact hazard the current spawn
   floor was written to avoid (`lv_plat_posix.c:454-459`, the D7 "child only dup2s
   and execs" discipline). The `posix_openpt` family lets us do all the unsafe work
   (grant/unlock/ptsname/open-slave) in the **parent, before fork**, keeping the
   child prologue async-signal-safe. This is the single most important structural
   ruling. (§A.1)

2. **The pty is a modification of the existing spawn, not a new subsystem.** The
   child prologue changes from `dup2(pipe) ; execve` to
   `setsid() ; ioctl(TIOCSCTTY) ; dup2(slave→0/1/2) ; close(master,slave) ; execve`.
   Everything else — argv-before-fork, `_exit(127)` on exec failure, O_CLOEXEC
   hygiene, `pidfd`/`reap`/`kill`, dropping master into `poll(2)` — is reused
   verbatim. The recommended surface is therefore **"open pty → spawn onto it"** as
   a spawn *variant*, not a separate orthogonal primitive (§C).

3. **The child owns one fd (the pty), not three.** Unlike the pipe spawn, stdout and
   stderr are the *same* device — a pty has no separate error channel. `fds[3]`
   collapses to a single master fd used for both reading and writing. The language
   layer must not expect a separate stderr stream from a pty child. (§A.2)

4. **Child-side termios must be set deterministically BEFORE exec, by the floor, on
   the slave.** Echo (`ECHO`) and CR/LF output translation (`ONLCR`) are the two
   nondeterminism sources that would wreck goldens. The floor must stamp a **fixed,
   known termios** on the slave before exec (echo policy explicit, `ONLCR` policy
   explicit) so the same child produces the same bytes on every run and every lane.
   Whoever owns that knob is the floor, not the language. (§D.1)

5. **Child-exit signalling on the master differs Linux vs macOS and must be handled.**
   After the child exits, a `read()` on the Linux pty master returns **`EIO`**;
   on **macOS/BSD** it returns **0 (EOF)**. The floor's read wrapper must map *both*
   to the same language-level "stream closed" outcome, or the IR/oracle/LLVM lanes
   will diverge by errno on non-Linux. (§A.3, confirmed by CPython bpo-26228.)

6. **ConPTY is not a pipe and not byte-transparent.** It re-renders child output
   into its own VT stream, its pipes can't be polled with WSAPoll, and
   `ClosePseudoConsole` can deadlock. This forces (a) a hidden reader-thread bridge
   into a pollable socketpair (the node-pty pattern), and (b) **Windows-specific
   golden expectations** — no cross-OS byte-identity. Flag Windows goldens as a
   separate lane from day one. (§B)

7. **No manual SIGWINCH to the child.** Setting the winsize with `ioctl(TIOCSWINSZ)`
   on the **master** (or slave) makes the *kernel* deliver `SIGWINCH` to the child's
   foreground process group automatically. The parent never signals the child by
   hand — this composes cleanly with the signals-as-streams model (the host's own
   SIGWINCH handling at `lv_plat_signal_open`, `lv_plat_posix.c:186`, is unrelated
   to the child's). (§A.4)

---

## A. POSIX PTY mechanics

### A.1 `posix_openpt` family vs `openpty`/`forkpty` — and why forkpty is out

Three ways exist to get a pty on POSIX:

- **UNIX 98 / SUSv2 `posix_openpt` family** (`posix_openpt(3)`, `grantpt(3)`,
  `unlockpt(3)`, `ptsname_r(3)`): the standardized, granular path. `posix_openpt(O_RDWR
  | O_NOCTTY)` returns a master fd; `grantpt` fixes slave ownership/permissions;
  `unlockpt` clears the slave lock; `ptsname_r` yields the slave path (e.g.
  `/dev/pts/3`) which you then `open(O_RDWR | O_NOCTTY)`. Available in glibc since
  2.2.1; present on musl, macOS, the BSDs. `grantpt` on modern Linux is
  **essentially a no-op** — "with permissions configured on pty allocation, as is
  the case on Linux" (`grantpt(3)`, NOTES) — because devpts assigns correct
  ownership at allocation; it survives as an ioctl on some systems. Still call it:
  it is a no-cost portability contract and macOS *does* still need it.

- **BSD `openpty(3)`** (from `<pty.h>`, `-lutil`): opens the master/slave pair in
  one call and returns *both* fds to the parent. Convenient, but non-POSIX (a
  BSD/glibc extension; musl has it, but it is not in SUS) and it hides the slave
  path. Fine as an *internal implementation shortcut on Linux*, but the granular
  family is more portable and gives explicit control.

- **`forkpty(3)`** — **reject.** `forkpty` = `openpty` + `fork` + (in the child)
  `login_tty` (which does `setsid`, `TIOCSCTTY`, and dup2s the slave onto 0/1/2).
  The problem is **async-signal-safety** (`fork(2)` NOTES; `signal-safety(7)`): in a
  multithreaded process — and LLVM programs are multithreaded, `lv_thread.c`,
  cf. the spawn comment at `lv_plat_posix.c:456` — only async-signal-safe functions
  may be called in the child between `fork` and `exec`. `forkpty`/`login_tty`
  internally call `ptsname` (not async-signal-safe: it may touch `malloc`/locale and
  on some libcs a static buffer) and `open`-by-name. A concurrent thread holding the
  malloc lock at fork time can deadlock the child. The existing floor was written
  precisely to dodge this (D7: "argv is built by the caller BEFORE fork; between fork
  and exec the child only dup2s and execs"). `forkpty` reintroduces the hazard.

  **Ruling:** open the master and slave in the **parent** with `posix_openpt`/
  `grantpt`/`unlockpt`/`ptsname_r`/`open`, then `fork`, then in the child do *only*
  async-signal-safe calls: `setsid()`, `ioctl(TIOCSCTTY)`, `dup2`, `close`, `execve`.
  All of those are async-signal-safe (`signal-safety(7)`). This mirrors the existing
  spawn exactly, just with a different set of fds and two extra ioctls.

### A.2 The correct child-side setup sequence

In the parent, before fork:

```
mfd = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);   // master, cloexec so it never
grantpt(mfd); unlockpt(mfd);                          //   leaks into OTHER spawns
ptsname_r(mfd, name, sizeof name);
sfd = open(name, O_RDWR | O_NOCTTY);                  // slave — NOT cloexec yet; the
                                                     //   child needs it past exec
// stamp deterministic termios + initial winsize on the SLAVE here (see §D.1, §A.4)
tcsetattr(sfd, TCSANOW, &fixed_termios);
ioctl(sfd, TIOCSWINSZ, &initial_ws);
pid = fork();
```

In the child (async-signal-safe only):

```
setsid();                       // new session; detaches from any inherited ctty,
                                //   makes this process a session leader (REQUIRED
                                //   before TIOCSCTTY can succeed) — setsid(2)
ioctl(sfd, TIOCSCTTY, 0);       // make the slave our CONTROLLING terminal — tty_ioctl(4)
dup2(sfd, 0); dup2(sfd, 1); dup2(sfd, 2);
if (sfd > 2) close(sfd);        // slave now lives on 0/1/2
// master fd is O_CLOEXEC, so it auto-closes at execve — but close explicitly if not
execve(path, argv, environ);
_exit(127);                     // same failure encoding as the pipe spawn
```

Ordering rules that are load-bearing (`setsid(2)`, `tty_ioctl(4)`,
`credentials(7)`):

- **`setsid()` before `TIOCSCTTY`.** `TIOCSCTTY` only works for a session leader
  with no controlling tty. `setsid()` creates a new session and makes the caller the
  leader; skipping it makes `TIOCSCTTY` a silent no-op and the child ends up with no
  controlling terminal (so `^C` / job control / `isatty` on the tty semantics
  break).
- **termios + `TIOCSWINSZ` before exec**, done by the **parent on the slave fd**
  (it can equally be done on the master — same underlying line discipline — but the
  parent already holds `sfd` open before fork, so do it there, once, deterministic).
  Setting it before exec means the child sees a fully-formed terminal at `main()`;
  no race where the child queries `TIOCGWINSZ` before the parent has sized it.
- **Close the master in the child.** O_CLOEXEC on the master handles this at
  `execve`, but if `execve` fails and we `_exit`, it's already closed on exec-attempt
  anyway. Belt-and-suspenders: the child never needs the master.
- **Close the slave in the parent** *after* fork returns (the parent holds a
  reference that would otherwise keep the slave open, defeating child-exit EOF
  detection — see §A.3). The parent keeps only the master.

### A.3 Master-side semantics: non-blocking reads, EOF vs EIO, writes, packet mode

- **Non-blocking master.** Set `O_NONBLOCK` on the master (as the pipe spawn does at
  `lv_plat_posix.c:488-491`) so it drops into the existing `poll(2)` loop with
  identical semantics: `LV_POLLIN` wakes a read, a short/`EAGAIN` read means "drained
  for now."
- **Child-exit signalling diverges by OS — the key portability fact.** When the last
  slave fd closes (child exited and its 0/1/2 are gone):
  - **Linux:** `read(master)` fails with **`EIO`** (`pty(7)`; this is the documented
    Linux behavior). It does *not* return 0.
  - **macOS / *BSD:** `read(master)` returns **0 (EOF)**, the conventional pipe-like
    signal. (Apple Developer Forums thread 663632 documents that on macOS a final
    chunk of child output can even be *discarded* on exit — a related hazard, see
    pitfall register.)
  This split is real and bit CPython: **bpo-26228** ("Fix pty EOF handling",
  cpython commit `5d44443`) rewrote `pty.spawn` to treat *both* `OSError`/`EIO` and a
  0-byte read as EOF because "some OSes signal EOF by returning an empty byte string,
  some throw OSErrors." **The floor's master-read wrapper must collapse `EIO` and 0
  to the same "stream closed" language outcome**, exactly as the socket `recv`
  wrapper already normalizes 0/<0 (`lv_plat.h:44-46`). If it doesn't, the oracle
  (POSIX read) and any macOS lane diverge on the errno.
- **Ordering hazard — drain before reap.** There can be buffered output in the line
  discipline that is still readable *after* the child has exited (Linux keeps it
  briefly; macOS may discard it, see above). The loop must **drain the master to
  EOF/`EIO` before treating the child as fully done**, otherwise trailing output is
  lost. This interacts with reaping: don't `reap` and tear down the master watch on
  the *pidfd* wake alone — keep reading until the master reports closed. Two exit
  signals now exist (pidfd-ready and master-EOF/EIO); the design must pick an
  ordering. Recommendation: **treat master-EOF/EIO as "output done" and pidfd/reap as
  "status available"; retire the child only when both have fired**, draining output
  first.
- **Writes to the master when the slave is closed** raise `EIO` (Linux) — analogous
  to `EPIPE` on a pipe. The existing `SIGPIPE`-ignore at first spawn
  (`lv_plat_posix.c:468-469`) does *not* cover this (pty write failure is `EIO`, not
  `SIGPIPE`), but it surfaces as a normal `-1/errno` from `write`, which the send
  wrapper's retryable/fatal split (`lv_plat.h:41-43`) can encode.
- **`TIOCPKT` packet mode: noise for our scope.** Packet mode prefixes master reads
  with a control byte signalling flush/flow-control/`TIOCSTOP` events — it exists so
  a remote peer (rlogin/telnetd) can mirror line-discipline state across a network.
  We move raw bytes to an in-process VT parser; we don't need to reconstruct
  line-discipline events out of band. **Do not enable `TIOCPKT`.** (Flag it as an
  option only if a future networked-terminal use case appears.)

### A.4 Resize: `TIOCSWINSZ` and automatic kernel SIGWINCH

- Set the window size with `ioctl(fd, TIOCSWINSZ, &winsize)` where `winsize` is
  `{ws_row, ws_col, ws_xpixel, ws_ypixel}`. It can be issued on **either** the master
  or the slave — same line discipline. Use the master post-spawn (the parent no
  longer holds the slave).
- **The kernel delivers `SIGWINCH` to the slave's foreground process group
  automatically** whenever the winsize changes (`tty_ioctl(4)`). The parent does
  **not** signal the child manually — confirmed. This is the clean composition the
  charter expected: host SIGWINCH (delivered to *us* and read via
  `lv_plat_signal_open`, `lv_plat_posix.c:186`) → we recompute child size → one
  `TIOCSWINSZ` on the master → kernel SIGWINCHes the child. No language code runs in
  any signal context at any step (signals-as-streams ruling, `lv_plat.h:77-89`).

### A.5 Reaping interplay with the existing pidfd/reap floor

The existing floor composes **cleanly**, with one addition:

- `lv_plat_pidfd_open(pid)` (`lv_plat_posix.c:495`) still returns a pollable exit fd —
  the pty changes nothing about how the child is a process. `lv_plat_reap`
  (`waitid(WNOHANG)`, `lv_plat_posix.c:504`) still yields the -1/0..255/128+sig
  encoding.
- The **new** signal is the master going EOF/`EIO` (§A.3). It is a *better* "output
  is finished" signal than pidfd (pidfd fires when the process dies, but output may
  still be buffered). The design should watch **both** the pidfd (or poll-reap
  fallback where pidfd is unavailable, `lv_plat_posix.c:500`) *and* the master, and
  only retire the child when output is drained AND status is reaped.
- **Orphan/SIGHUP edge case** (`setsid(2)`, `credentials(7)`): if the host dies first,
  the child is in its own session with the pty as controlling terminal. When the
  master is closed (host gone), the kernel sends **`SIGHUP`** to the child's
  foreground group — the child terminates as a real terminal hangup would. This is
  correct behavior (a shell in the pty should die when its terminal vanishes), not a
  leak. Conversely, if the *child* dies first, the master EOF/EIO tells us.

### A.6 fd hygiene / leak vectors

- **Master: `O_CLOEXEC`.** `posix_openpt` accepts `O_CLOEXEC` on Linux (glibc passes
  it through to the `open("/dev/ptmx")`); where a libc rejects it, fall back to
  `fcntl(F_SETFD, FD_CLOEXEC)` immediately after open (a two-step, but the window is
  single-threaded-parent so the race is only against a *concurrent* spawn on another
  thread — hence prefer the atomic `O_CLOEXEC` form, matching why the pipe spawn uses
  `pipe2(O_CLOEXEC)` atomically, `lv_plat_posix.c:471`). Without cloexec on the
  master, *every other child we spawn* inherits an open master fd — a classic leak
  that keeps the pty alive forever and defeats EOF detection.
- **Slave: deliberately NOT cloexec**, because the child must keep it across
  `execve` (it becomes 0/1/2). After the child dup2s it onto 0/1/2 and closes the
  original, and the parent closes its copy, no stray slave fd remains.
- **Both fds closed on every error path**, no partial leak — same discipline as the
  pipe spawn's cascade of `close()` on each failure (`lv_plat_posix.c:472-479`).

---

## B. Windows ConPTY

### B.1 The creation flow (confirmed against MSDN "Creating a Pseudoconsole session")

1. `CreatePipe` **twice**: an input pipe (`inputReadSide`, `inputWriteSide`) and an
   output pipe (`outputReadSide`, `outputWriteSide`). The parent keeps
   `inputWriteSide` (to type at the child) and `outputReadSide` (to read what the
   child "renders"); it hands the *other* two ends to ConPTY.
2. `CreatePseudoConsole(size, inputReadSide, outputWriteSide, 0, &hPC)` — creates the
   HPCON and takes the read-end of input + write-end of output.
3. **Close `inputReadSide` and `outputWriteSide` in the parent right after
   `CreateProcess`.** MSDN: "the handles given during creation should be freed from
   this process ... to properly detect a broken channel when the pseudoconsole
   session closes its copy." Not closing them defeats EOF detection (the classic
   pipe-refcount mistake, exactly analogous to closing the slave in the POSIX parent,
   §A.2).
4. `InitializeProcThreadAttributeList` (double-call: size, then fill) →
   `UpdateProcThreadAttribute(..., PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hPC,
   sizeof(hPC), ...)` → `STARTUPINFOEX` with `StartupInfo.cb = sizeof(STARTUPINFOEX)`.
5. `CreateProcessW(..., EXTENDED_STARTUPINFO_PRESENT, ..., &siEx.StartupInfo, &pi)`.
   The `EXTENDED_STARTUPINFO_PRESENT` flag is what routes the pseudoconsole handle in.
6. Resize: `ResizePseudoConsole(hPC, size)`. Teardown: `ClosePseudoConsole(hPC)` —
   which **terminates the attached child and its process tree**.

**Version gate:** `CreatePseudoConsole`/`ResizePseudoConsole`/`ClosePseudoConsole`
require **Windows 10 1809 (build 10.0.17763)** / Windows Server 2019 (MSDN
`createpseudoconsole`/`resizepseudoconsole`, "Minimum supported client"). Below that,
no ConPTY. See §B.4 on fallback.

### B.2 ConPTY is NOT a byte-transparent pipe — the consequence for testing

This is the finding that most changes the Windows design. On POSIX, the master
carries the child's bytes verbatim (subject only to the line discipline we control).
**ConPTY re-renders.** It hosts an in-memory screen buffer, lets the child draw into
it via the classic Console API *or* VT, and then **emits its own VT sequence stream**
on the output pipe describing that buffer — including cursor moves, SGR, and
repaints the child never wrote. Consequences:

- **Output is not the child's bytes.** A golden that asserts "child printed `hello\n`"
  will instead see ConPTY's rendered frame (positioning + `hello` + possibly a
  repaint). This is *why* cross-OS byte-identity is impossible and Windows needs its
  own expected files or behavioral asserts (confirmed below, §D.5).
- **UTF-8 requirement.** ConPTY's VT stream is UTF-8; the parent must treat both
  pipes as UTF-8 byte streams (no ANSI codepage translation). Read/write raw bytes.
- **It is fine for *hosting* a TUI** (that's its purpose — Helm rendering a child), but
  **hostile for deterministic golden capture** of arbitrary programs.

### B.3 The core integration problem: poll-based loop vs un-pollable pipes

Our loop is `poll(2)`/`WSAPoll` (`lv_plat_win32.c:448`). **Anonymous pipes from
`CreatePipe` are not selectable/pollable with `WSAPoll`** — WSAPoll only works on
sockets. So ConPTY's output pipe cannot be dropped into `lv_plat_poll` the way the
POSIX master can. The realistic options, and what the precedents do:

- **Dedicated reader thread → socketpair the loop CAN poll (RECOMMENDED).** A hidden
  floor-internal thread does blocking `ReadFile` on ConPTY's `outputReadSide` and
  writes whatever it reads into one end of a **loopback `AF_INET`/`AF_UNIX`
  socketpair**; the loop polls the *other* end with `WSAPoll` and reads bytes there.
  A symmetric thread (or overlapped write) handles input. This is **exactly
  node-pty's design**: node-pty PR #415 ("Host conout socket in a worker") moved the
  conout pipe servicing onto a worker thread feeding a socket, both to make it
  pollable via libuv and to dodge the shutdown deadlock (§B.4). **This is the
  least-machinery bridge that preserves our single-threaded callback model** — no
  language code runs on the reader thread; it only shuttles bytes into a pollable fd,
  precisely analogous to the signal **self-pipe** whose handler body "is exactly
  `write(pipe_wr,&signo,1)`" (`lv_plat.h:82-88`). The callback model is untouched:
  callbacks still fire on the UI thread when the socket wakes the poll.
- **Named pipes + overlapped I/O + IOCP/`GetOverlappedResult`.** Create the ConPTY
  channels as *named* pipes in overlapped mode and integrate completion into an IOCP,
  bridging IOCP completions to the poll loop. This is what **libuv** does for pipes
  generally (`uv_pipe_t` over named pipes + IOCP); libuv's own `uv_tty`/`uv_process`
  path and **WezTerm's portable-pty** ConPTY backend build on overlapped named
  pipes. More machinery than a reader thread, and it still needs a bridge into our
  WSAPoll loop, which is not IOCP-based. **Not recommended for v1** — it rewrites the
  loop's readiness model.
- **`GetOverlappedResult` polling / manual IOCP in the loop.** Possible but invasive;
  rejected for the same reason.

**Precedent summary:** node-pty = ConPTY + **worker thread pumping conout into a
socket**; libuv = named pipes + IOCP (no native pty, but the pipe transport model);
portable-pty (WezTerm) = ConPTY with overlapped named pipes behind a `PtySystem`
trait; Alacritty (Windows) = ConPTY via a wrapper crate, reader thread feeding its
event loop. **All of them use a hidden thread or IOCP to make ConPTY output
consumable by an event loop** — none poll the raw pipe. Our recommendation (reader
thread → pollable socketpair) is the node-pty shape and fits our WSAPoll loop with
the least new infrastructure.

### B.4 ConPTY footguns (each confirmed against MS docs or terminal/node-pty issues)

- **`ClosePseudoConsole` shutdown deadlock.** MSDN (`creating-a-pseudoconsole-session`,
  "Ending the Pseudoconsole Session", Warning): closing "may emit a final frame
  update to `hOutput` which should be drained." If the same thread both closes and
  drains, it deadlocks (the close waits for the drain buffer to empty; the drain is
  blocked behind the close). microsoft/terminal issue #1810 ("ClosePseudoConsole API
  hanging") and PR #14160 ("Fix a deadlock during ConPTY shutdown") document it;
  node-pty PR #415 fixed it by draining on a worker. **Mitigation: keep the output
  reader thread draining until the pipe breaks on its own; call
  `ClosePseudoConsole` from a *different* thread than the drainer.** Note: **Windows
  11 24H2 (build 26100)** makes `ClosePseudoConsole` return immediately to avoid this
  — but we must support 1809+, so the drain-on-another-thread discipline is
  mandatory.
- **Child hangs when the parent stops reading.** If the parent stops draining
  `outputReadSide`, ConPTY's output buffer fills and the child blocks on write. The
  reader thread must **always** be draining while the child lives (same class as the
  MSDN "service each channel on a separate thread" warning).
- **`PSEUDOCONSOLE_INHERIT_CURSOR` deadlock.** If created with that flag, ConPTY
  sends a cursor-position **query** on `hOutput` and blocks until the host replies on
  `hInput`; a host that closes without answering deadlocks (MSDN Warning; terminal
  discussion #17716). **Mitigation: pass flags = 0** (we have no cursor to inherit;
  the charter's host tty is separate). Do not set the flag.
- **UTF-8 only** (§B.2) — a footgun if the parent applies codepage conversion.
- **Startup race dialog `0xc0000142`.** Closing the pseudoconsole while the child is
  still connecting, *or* handing an invalid HPCON, makes the client pop a
  localized init-failure dialog. Mitigation: don't tear down during startup; ensure
  the HPCON is valid before `CreateProcess`.
- **VT-in-the-output you didn't write** (§B.2) — a testing footgun, listed again in
  the pitfall register.

### B.5 Process exit / kill mapping on Windows

- **Exit wait:** the child is a real process object; `pi.hProcess` is **waitable**
  (`WaitForSingleObject` / register with an IOCP / a wait thread). This maps to our
  `pidfd` abstraction: `lv_plat_pidfd_open` on Windows would return a pollable handle
  bridged the same way as the output pipe (a wait thread signalling a pollable
  socket), or the loop can poll the process handle via the reader-thread bridge.
- **Exit code:** `GetExitCodeProcess(hProcess, &code)`.
- **Honest reap encoding.** The floor's contract is `-1` running / `0..255` exited /
  `128+sig` signaled (`lv_plat.h:130`). Windows has **no signals**: a normal exit
  gives a 32-bit code we truncate to `0..255`; a `TerminateProcess` gives whatever
  code was passed (conventionally `1`, or `STATUS_CONTROL_C_EXIT` etc.). There is no
  faithful `128+sig`. **Honest mapping:** exit code → `code & 0xFF`; killed-by-us →
  a chosen sentinel in the exited range (document it), never a fake signal number.
  This is a *lossy but honest* mapping the design must state explicitly — the
  `128+sig` band is POSIX-only.
- **Kill:** `TerminateProcess(hProcess, code)`; `ClosePseudoConsole` also terminates
  the whole attached tree, which is often what "kill the child" should mean for a
  shell. `lv_plat_kill`'s signal argument has no Windows analog beyond
  terminate-vs-nothing.

---

## C. Cross-platform surface candidates

House style for all: fds via out-params, `-1` on failure, **no partial fd leak on any
error path** (mirrors `lv_plat_spawn`, `lv_plat_posix.c:462`), platform-opaque types
kept inside the floor (the `LvPollFd` opacity rule, `lv_plat.h:139-155`).

### Candidate 1 — Unified "spawn-with-pty" (RECOMMENDED)

One call that allocates the pty and spawns onto it, parallel to `lv_plat_spawn`:

```c
/* Returns pid > 0 and writes the master fd to *master (read AND write; O_NONBLOCK,
 * O_CLOEXEC). rows/cols seed the initial winsize and a fixed deterministic termios
 * (echo/ONLCR policy fixed by the floor, §D.1). -1 on any failure, no fd leaked.
 * Note: ONE fd, not three — a pty fuses stdout/stderr. */
int lv_plat_pty_spawn(const char* path, char* const argv[],
                      int rows, int cols, int* master);
int lv_plat_pty_resize(int master, int rows, int cols);   /* TIOCSWINSZ / ResizePseudoConsole */
/* reap/kill/pidfd reuse lv_plat_reap / lv_plat_kill / lv_plat_pidfd_open unchanged. */
```

- **POSIX:** `posix_openpt`→grant/unlock/ptsname→open slave→set termios+winsize→fork→
  child setsid/TIOCSCTTY/dup2/exec (§A). `*master` is the pty master.
- **Win32:** CreatePipe×2→CreatePseudoConsole→attr-list→CreateProcess→spin up the
  reader-thread↔socketpair bridge; `*master` is the **pollable socketpair fd** the
  loop reads/writes (the thread hides the real ConPTY pipes). `pty_resize` calls
  `ResizePseudoConsole`.
- **What leaks through the abstraction:** the single-fd-not-three collapse (callers
  must not expect separate stderr); on Windows the bytes are ConPTY-rendered VT, not
  raw child output (callers/tests must know). Everything else is hidden.
- **Composition:** reuses reap/kill/pidfd verbatim; the master drops into
  `lv_plat_poll` exactly like a socket/pipe. **Fewest moving parts, best match to the
  existing spawn floor and the poll loop.** Precedent: libuv's `uv_process` bundles
  stdio setup with spawn; portable-pty's `slave.spawn_command` spawns *onto* an
  already-open pty (a 2-step variant, see Candidate 2).

### Candidate 2 — Orthogonal "open pty + spawn onto fds"

Split allocation from spawn:

```c
int lv_plat_pty_open(int rows, int cols, int* master, int* slave);  /* both fds out */
/* then reuse a spawn variant that dup2s a caller-provided slave fd onto 0/1/2
 * instead of pipes, does setsid/TIOCSCTTY, and returns pid. */
int lv_plat_spawn_onto(const char* path, char* const argv[], int slave);
int lv_plat_pty_resize(int master, int rows, int cols);
```

- **Pro:** matches **portable-pty's `PtySystem`/`PtyPair` trait** (open the pair, then
  `slave.spawn_command(cmd)`) and is more composable (could spawn multiple children,
  or a non-pty program, onto explicit fds).
- **Con:** exposes the **slave fd to the language/caller**, which is a footgun — the
  slave must be closed in the parent at exactly the right time (§A.2) or child-exit
  EOF never fires; a leaked slave fd is a silent hang. On **Windows there is no slave
  fd** (ConPTY hides it entirely), so `pty_open` returning a `slave` is a POSIX-only
  concept — the abstraction leaks OS shape. **Rejected for v1** on the
  Windows-asymmetry and the slave-lifetime footgun.

### Candidate 3 — Full "descriptor bag" spawn (over-general)

Generalize `lv_plat_spawn` to take a descriptor table (pipe or pty per fd, à la
libuv's `uv_process_options_t.stdio[]` with `UV_INHERIT_STREAM`/`UV_CREATE_PIPE`).
Maximally flexible; **over-engineered for our need** (we want "a child on a pty"),
and it dilutes the tight, auditable spawn floor. Note as the eventual shape if we
ever need per-fd control, not v1.

**Recommendation: Candidate 1.** It is the smallest delta over the existing spawn,
hides every OS asymmetry (single fd, ConPTY thread bridge), and composes with reap/
kill/pidfd/poll unchanged.

---

## D. Deterministic testing of a PTY — this gates everything

### D.1 Killing echo and CR/LF nondeterminism — the floor owns the termios

Two line-discipline behaviors would make goldens nondeterministic or surprising:

- **`ECHO`.** With echo on (the tty default), bytes the host *writes* to the master
  are echoed back and appear in the master read stream interleaved with the child's
  output — timing-dependent interleaving = nondeterministic goldens. **Decision the
  design must make explicitly:** spawn the child with a **fixed termios** where the
  echo policy is pinned. For deterministic goldens of *output*, the cleanest is to
  drive children that don't rely on echo and to pin `ECHO` **off** for the test
  termios (so writes never bounce back). The knob owner is the **floor**, setting
  termios on the slave before exec (§A.2) — *not* the child running `stty`, *not* the
  language. (`stty -echo` as a first child command is nondeterministic and racy;
  reject it.)
- **`ONLCR` (output NL→CR-NL translation).** On by default: the child's `\n` becomes
  `\r\n` in the master stream. This is *deterministic* but surprising for goldens.
  **Decision:** pin `ONLCR` policy in the fixed termios and **assert on the
  post-translation bytes** — i.e. the goldens capture exactly what the kernel line
  discipline produces under the known termios. Whether to keep `ONLCR` on (goldens
  contain `\r\n`, realistic terminal behavior) or off (goldens contain `\n`, simpler)
  is a design choice; **either is fine as long as it's fixed and the goldens match
  post-translation output.** Recommendation: keep a **single canonical test termios**
  constant in the floor/test harness and document it.
- **Other termios to pin for determinism:** disable `ISIG`-driven surprises only if
  needed; fix `VMIN=1/VTIME=0` (matches the host raw-mode floor's discipline,
  `lv_plat.h:53-57`) so reads aren't timer-quantized; ensure a fixed baud/`OPOST`
  policy. The point: **one frozen termios struct** = reproducible bytes.

**Ruling:** the floor stamps a fixed, documented termios on the slave before exec.
This is the pty analog of the host raw-mode floor already owning the termios struct
(`lv_plat.h:50-52`). The language never sees termios; it sees "spawn a pty child."

### D.2 Deterministic child programs for goldens

What the precedents' own suites use, and what to copy:

- **`/bin/cat` on a pty** — write bytes to the master, read them back (echoed by the
  line discipline or looped by cat); asserts the pipe wiring and termios. libuv's and
  node-pty's tests do variants of "spawn a shell/echo, write, assert the read."
- **`stty size`** — the canonical winsize test: set `rows×cols` via `TIOCSWINSZ`,
  spawn `stty size` (or a tiny program calling `ioctl(TIOCGWINSZ)`), assert it prints
  exactly the seeded dimensions. Deterministic and OS-portable in *value* (though the
  surrounding bytes differ by termios). node-pty tests `resize` this way.
- **A tiny scripted child in-language** — a Leviathan program that prints a fixed
  string then exits, or reads one line and echoes a transform. Because it's our own
  binary, it's byte-stable across lanes. **Best for three-lane goldens** (no reliance
  on the host having `/bin/cat` with identical output).
- **`isatty`/controlling-tty asserts** — a child that checks `isatty(0)` and prints
  0/1 proves the pty is actually the controlling terminal (validates
  setsid/TIOCSCTTY).

### D.3 Timing: assert on accumulated streams, never chunk boundaries

Output arrives in **nondeterministic chunk boundaries** — the kernel/ConPTY may
deliver `hel` then `lo\n`, or `hello\n` in one read, run to run. **Goldens must
accumulate the full stream until EOF/EIO and assert on the total**, never on
per-`read` chunk sizes or arrival order across the two directions. Every precedent
does this: node-pty's tests concatenate `onData` chunks before asserting; libuv's
echo tests read until EOF. The existing loop already accumulates (the stream
substrate is a push queue), so this is a test-harness discipline, not new code. The
one thing that *is* deterministic and assertable is the **final accumulated byte
sequence** under a fixed termios on the same kernel.

### D.4 CI realities

- **pty allocation in containers/CI:** normally fine. Linux CI runners mount
  `/dev/pts` (devpts); `posix_openpt` works. The rare failure is a container without
  `/dev/pts` mounted or with `--no-new-privileges` oddities — detect `posix_openpt`
  returning `-1` and skip/xfail with a clear message rather than hanging. GitHub
  Actions Linux/macOS runners both allocate ptys fine.
- **Windows CI ConPTY availability:** GitHub Actions `windows-latest` is Server 2022
  / Win11-era → ConPTY present. `windows-2019` (Server 2019 = 1809) is the **floor**
  version — present but pre-24H2, so the `ClosePseudoConsole` drain discipline (§B.4)
  is exercised there, which is good (it's the hard case). Guard the Windows pty tests
  behind a 1809+ check.

### D.5 Three-lane byte-identity — achievable on POSIX, NOT cross-OS

- **POSIX three-lane (oracle / IR / LLVM):** **achievable and required.** All three
  lanes run on the *same kernel* with the *same fixed termios* driving the *same
  child*; the line discipline is deterministic, so the accumulated master byte stream
  is byte-identical across lanes. This satisfies the testing doctrine (goldens
  byte-identical across oracle/IR/LLVM, diffing one `.expected`). The oracle
  (C++ interpreter over the same `posix_openpt` path via RuntimeNatives) and the LLVM
  runtime (this floor) must produce the same bytes — they will, given the same
  termios.
- **Cross-OS (POSIX vs Windows):** **explicitly NOT byte-identical.** ConPTY
  re-renders (§B.2), so its output stream is structurally different VT. **Windows
  needs its own `.expected` files or, better, behavioral assertions** (e.g. "the
  output, after stripping/parsing VT, contains `hello`" or "`stty size` reports
  80×24") rather than raw-byte goldens. The design must **partition pty goldens into
  a POSIX byte-identical lane and a Windows behavioral lane** from the start. This
  matches the frozen doctrine that emit-C++ defers the system layer and that lanes
  diff *one* expected file *per platform family*.

---

## E. Scope fences (flagged, not solved here)

- **VT PARSING is out of scope.** The floor moves bytes; interpreting `\x1b[...`
  sequences (cursor, SGR, mode sets) is in-language, Helm-side. This dossier assumes
  the parser lives above the floor. (On Windows, note the floor's bytes are already
  ConPTY-emitted VT — the parser sees VT on both platforms, but *different* VT.)
- **Flow control / backpressure at the floor.** Non-blocking writes to the master
  return `EAGAIN`/`-1` when the child isn't draining (POSIX) or the ConPTY input
  buffer is full (Windows). The floor surfaces this as the existing send
  retryable/fatal split (`lv_plat.h:41-43`); the *policy* (queue-and-drain, drop,
  block) is a language-layer question the design should state but this research does
  not settle.
- **Security notes.** `grantpt` historically (set-uid `pt_chown`) fixed slave
  ownership so only the allocating user could open it; on modern Linux **devpts sets
  correct ownership at allocation and `grantpt` is a no-op** (`grantpt(3)` NOTES) —
  no set-uid helper in the hot path. Still call `grantpt`/`unlockpt` for portability
  (macOS). A pty grants a real terminal to the child; don't allocate one for
  untrusted input without the same care as any spawn. Not a v1 blocker, flagged.

---

## Comparison table — precedent implementations

| | POSIX alloc | Win alloc | Win poll bridge | Exit detect | API shape | Notes |
|---|---|---|---|---|---|---|
| **node-pty** | `forkpty` (via native addon) | ConPTY (1809+), winpty fallback | **worker thread → conout socket** (PR #415) | pipe EOF + process wait | `spawn(file,args,{cols,rows})` → single data stream | Fixed ClosePseudoConsole deadlock by draining off-thread |
| **libuv** | no native pty; `uv_process_t` + `uv_pipe_t` | named pipes + **IOCP** | IOCP (loop is IOCP-based) | `uv_process` exit cb / `SIGCHLD` | `uv_spawn` + `stdio[]` descriptor bag | pty is left to consumers; transport model is the lesson |
| **portable-pty (WezTerm)** | `openpty` behind `PtySystem` trait | ConPTY, overlapped named pipes | overlapped I/O + reader thread | `Child::wait` | `openpty()→(master,slave)`, `slave.spawn_command` | **2-step open-then-spawn** (our Candidate 2) |
| **Alacritty** | `openpty`/`grantpt` family | ConPTY via wrapper crate | reader thread → event loop | child wait + master EOF | internal, tied to its event loop | reader-thread-into-loop, like our recommendation |
| **Leviathan (proposed)** | `posix_openpt` family (NOT forkpty) | ConPTY (1809+), no winpty | **reader thread → pollable socketpair** (node-pty shape) | master EOF/EIO **+** `pidfd`/`reap` | **Candidate 1: `lv_plat_pty_spawn`** single-fd | reuses reap/kill/pidfd/poll floor verbatim |

---

## Ranked pitfall register

| # | Pitfall | Concrete failure it causes | Mitigation |
|---|---|---|---|
| 1 | **`forkpty` in a multithreaded process** | Child deadlocks between fork/exec if another thread held the malloc/locale lock (non-async-signal-safe `ptsname`/`login_tty`) | Use `posix_openpt` family; do all unsafe work in parent before fork; child does only setsid/TIOCSCTTY/dup2/exec (§A.1) |
| 2 | **Linux `EIO` vs macOS `0` on child-exit master read** | oracle/LLVM/macOS lanes diverge on errno; a lane that only checks for 0 hangs forever on Linux (bpo-26228) | Master-read wrapper collapses `EIO` **and** 0 to "stream closed" (§A.3) |
| 3 | **`ClosePseudoConsole` deadlock (pre-24H2)** | Windows host UI/loop hangs on child teardown (terminal #1810, PR #14160) | Keep reader thread draining until pipe breaks; close from a *different* thread; flags=0 to avoid the cursor-query variant (§B.4) |
| 4 | **Not draining ConPTY output → child blocks on write** | Child hangs mid-run; looks like a broken program | Reader thread always draining while child lives (§B.3) |
| 5 | **Echo interleaving in goldens** | Nondeterministic byte order (host writes bounce back timing-dependent) | Fixed test termios, `ECHO` pinned; floor owns termios on slave before exec (§D.1) |
| 6 | **Master not `O_CLOEXEC`** | Every subsequently spawned child inherits the master → pty never EOFs, silent hang / fd leak | `posix_openpt(O_CLOEXEC)`; fallback `FD_CLOEXEC` immediately (§A.6) |
| 7 | **Parent keeps slave fd open (POSIX) / doesn't close ConPTY's handle copies (Win)** | Child exit never produces EOF; loop waits forever | Close slave in parent post-fork; close `inputReadSide`/`outputWriteSide` after CreateProcess (§A.2, §B.1) |
| 8 | **Missing `setsid()` before `TIOCSCTTY`** | Child has no controlling tty: `^C`/job control/`isatty`-tty semantics broken; `TIOCSCTTY` silently no-ops | setsid → TIOCSCTTY ordering, both async-signal-safe (§A.2) |
| 9 | **Treating ConPTY output as raw child bytes** | Windows goldens compare rendered VT to expected plain text → always fail; cross-OS golden reuse breaks | Separate Windows behavioral/expected lane; never cross-OS byte goldens (§B.2, §D.5) |
| 10 | **Faking `128+sig` on Windows kill** | Reap encoding lies about how the child died | Windows: exit code `& 0xFF`; killed→documented sentinel; never a fake signal (§B.5) |
| 11 | **Reaping before draining output** | Trailing child output lost (worse on macOS, which may *discard* the final chunk — Apple forums 663632) | Drain master to EOF/EIO first; retire child only when output done AND reaped (§A.3, §A.5) |
| 12 | **Asserting on read chunk boundaries** | Flaky goldens (kernel/ConPTY split reads nondeterministically) | Accumulate full stream to EOF, assert on the total (§D.3) |
| 13 | **`PSEUDOCONSOLE_INHERIT_CURSOR` set** | ConPTY blocks on an unanswered cursor query → deadlock (terminal #17716) | Pass flags = 0 (§B.4) |
| 14 | **No `/dev/pts` in a container** | `posix_openpt` returns -1; test hangs or crashes obscurely | Detect -1, skip/xfail with a clear message; don't assume a pty (§D.4) |

---

## Open questions the design must decide

1. **Surface shape:** confirm Candidate 1 (`lv_plat_pty_spawn`, unified) over
   Candidate 2 (open-then-spawn). The recommendation is Candidate 1 for the
   Windows-asymmetry and slave-lifetime reasons (§C) — but if a future use case needs
   spawning a *non-pty* program onto explicit fds, Candidate 2's `spawn_onto` may be
   worth the footgun. Decide now to avoid a churny second surface.
2. **The canonical test termios:** exact flags — is `ECHO` off, is `ONLCR` on or off,
   `OPOST` policy? Pick one frozen struct and document it; every POSIX golden depends
   on it (§D.1).
3. **Single fd vs pty-plus-separate-stderr:** a pty fuses stdout/stderr. Confirm the
   language surface accepts one merged stream (it must, on a real pty), and that no
   caller of the pty path expects the pipe-spawn's three-fd shape (§A.2, TL;DR #3).
4. **Exit-retirement ordering:** codify "drain master to EOF/EIO, then require
   pidfd/reap, then retire." Which fires first is nondeterministic; the loop must
   handle either order without losing output or leaking the watch (§A.3, §A.5).
5. **Windows reap sentinel:** what exact value represents "we `TerminateProcess`d it"
   in the `0..255` band, given there is no honest `128+sig`? (§B.5)
6. **Windows bridge ownership:** the reader-thread↔socketpair bridge is new
   floor-internal threading. Confirm it's acceptable under the signal-self-pipe
   precedent (no language code on the thread) and decide where its lifetime is owned
   (per-pty, torn down on child exit) — and how `lv_plat_pidfd_open`/`reap` are
   bridged to the poll loop on Windows (a wait thread signalling a pollable socket?).
7. **winpty fallback for pre-1809:** the charter suspects "likely no." Confirmed
   recommendation: **no winpty** — 1809 (Oct 2018) is old enough, winpty is a heavy
   hidden-console-scraping dependency, and Server 2019 (our CI floor) has ConPTY.
   Below 1809, **compile-time/runtime reject** the pty path (the existing Windows
   spawn-reject precedent, `lv_plat_win32.c:237`), don't add winpty. Design should
   ratify this.
8. **Backpressure policy** on a full master/ConPTY-input buffer: queue-and-drain (like
   the socket send path) vs surface `EAGAIN` to the language. Floor surfaces the
   retryable `-1`; the policy is the design's to set (§E).

---

## Source index

**Runtime (this tree, `agent1`):** `runtime/lv_plat.h:124-157` (spawn/reap/pidfd/kill,
poll, LvPollFd), `:77-89` (signals-as-streams, self-pipe), `:50-75` (host terminal
floor); `runtime/lv_plat_posix.c:462-515` (spawn/pidfd/reap/kill), `:186-248`
(signalfd + self-pipe fallback); `runtime/lv_plat_win32.c:237-241` (spawn reject
stubs), `:448-461` (WSAPoll + POLLNVAL). Prior floor doctrine:
`designs/complete/techdesign-spawn-llvm.md`.

**POSIX man pages:** `posix_openpt(3)`, `grantpt(3)` (no-op-on-Linux NOTES),
`unlockpt(3)`, `ptsname_r(3)`, `openpty(3)`/`forkpty(3)` (`<pty.h>`), `pty(7)`,
`tty_ioctl(4)` (`TIOCSCTTY`, `TIOCSWINSZ`, `TIOCPKT`), `setsid(2)`, `fork(2)` +
`signal-safety(7)` (async-signal-safety), `credentials(7)` (sessions/SIGHUP).
`posix_openpt` — pubs.opengroup.org/onlinepubs/9799919799/functions/posix_openpt.html.

**Windows / MSDN:** learn.microsoft.com/windows/console/creating-a-pseudoconsole-session
(full flow, drain/deadlock Warning, close-handles-after-CreateProcess),
.../createpseudoconsole, .../resizepseudoconsole, .../closepseudoconsole (all "Minimum
supported client: Windows 10 1809 / build 17763"). microsoft/terminal issue #1810
(ClosePseudoConsole hang), PR #14160 (shutdown deadlock fix), discussion #17716
(INHERIT_CURSOR deadlock), discussion #19112 (version probing); 24H2/build-26100
immediate-return note.

**Precedents:** node-pty PR #415 (conout on a worker socket) — github.com/microsoft/
node-pty/pull/415; libuv `uv_process`/`uv_pipe` + IOCP; WezTerm **portable-pty**
`PtySystem`/`PtyPair` trait (openpty → slave.spawn_command); Alacritty ConPTY reader
thread. **CPython bpo-26228** (commit `5d44443`, "Fix pty EOF handling": EIO-or-0 both
mean EOF) — the canonical primary source for the Linux/macOS master-read split. Apple
Developer Forums thread 663632 (macOS discards final pty output on child exit).
