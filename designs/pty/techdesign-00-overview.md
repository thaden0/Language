# Tech Design 00 — PTY Floor (G-LANG-2, terminal half): Overview & Rulings

**Status:** design, ready for staged implementation. **Date:** 2026-07-16.
**Scope:** the pseudo-terminal floor — allocate a PTY, spawn a child attached to it as its
controlling terminal, stream bytes both ways through the existing event loop, resize it, reap it —
on **oracle + IR + LLVM**, POSIX-first with a chartered **Windows ConPTY** lane. Closes the
terminal half of gate **G-LANG-2** and unblocks Helm H10 (the integrated terminal,
`designs/helm/techdesign-00-overview.md` §8).
**Ground truth:** `docs/pty-floor-research.md` (the research dossier — every OS/precedent claim in
this design is anchored there; its §-references are cited as `R§…`), plus the landed process-spawn
design `designs/complete/techdesign-spawn-llvm.md` (conventions D1–D7 there are inherited wholesale).
**Out of scope (frozen):** VT parsing (in-language, Helm-side — the floor moves bytes, R§E);
X64Gen/ELF; emit-C++ (keeps its deliberate system-layer deferral); winpty (ratified out, D-P8);
TIOCPKT packet mode (R§A.3 — rejected, noise for our scope); a general per-fd "descriptor bag"
spawn (research Candidate 3 — recorded as the eventual shape if ever needed, not built).

**Labeling convention:** sections that touch the machine-language LLVM backend
(`src/LlvmGen.cpp` rows, `lv_abi.h` ABI wiring, `runtime/lv_*.c` marshaling linked into native
binaries) are tagged **HARD**. They demand the most care: an ARC or ABI mistake there is a silent
memory bug, not a test failure.

---

## 0. What and why

`Process` (pipes) landed on all three lanes on 2026-07-16. A pipe child is not a terminal child:
Helm's integrated terminal must host a **shell** and full-screen TUIs, which demand a controlling
tty (`isatty`, job control, `SIGWINCH`, line discipline). The research (R§0) established the POSIX
gap is narrow — a variation on the existing spawn with a different child prologue and one resize
ioctl — while Windows is a genuinely new subsystem (ConPTY + a poll bridge).

The staged shape (3 stages, one doc each):

| doc | stage | contents |
|---|---|---|
| `01-interpreter-lane.md` | **S1** | frozen `sysPty*` natives in the oracle (`RuntimeNatives.cpp`, shared by oracle+IR), the `Pty` prelude class, the EIO-collapse fix, goldens green on oracle=IR |
| `02-llvm-native.md` | **S2** | plat floor (`lv_plat_pty_*`), `runtime/lv_pty.c`, **HARD** LlvmGen rows, build wiring, selftest, goldens promoted to three-lane byte-identity |
| `03-windows-conpty.md` | **S3** | ConPTY win32 floor, the reader-thread↔socketpair bridge, win32 reap/kill, **HARD** codegen gating changes, the Windows behavioral test lane |

Consumer forcing the landing (the per-consumer precedent): **Helm H10**. Gate G-H6 ships either an
embedded terminal (this floor green) or the external-terminal fallback (this floor red) — this
design exists to make it the former.

---

## 1. Frozen contract (binding on every lane)

### 1.1 Language surface — two new natives (prelude block, beside the spawn floor, `src/Resolver.cpp:1477-1482`)

```
// G-LANG-2 terminal half: pty floor. [pid, masterFd] on success; [] on any
// allocation/spawn failure (exec failure is NOT spawn failure: the child
// _exit(127)s, code via sysReap — the F7 convention). masterFd is ONE fd,
// read AND write (a pty fuses stdout/stderr; there is no separate stderr).
// rows/cols <= 0 is refused ([]). flags bit0 = deterministic termios (goldens).
Array<int> sysPtySpawn(string path, Array<string> args, int rows, int cols, int flags);
int sysPtyResize(int masterFd, int rows, int cols);   // 0/-1; kernel SIGWINCHes the child
```

Everything else is **reuse, not new surface**: the master fd rides `sysWatch`/`sysRecv`/`sysSend`/
`sysClose` (the pipes-take-the-same-call ENOTSOCK fallback, `RuntimeNatives.cpp:1614-1615`,
`lv_plat_posix.c:437-450`); exit rides `sysPidfdOpen`/`sysReap`/`sysKill` unchanged. No new watch
kinds (the F5 rule — new watch *registrations* are in-bounds, new *kinds* are a STOP).

### 1.2 Prelude class (in-language, `Resolver.cpp`, beside `Process` at `:1808`)

```
class Pty {                          // one merged stream — onData, not onStdout/onStderr
    new Pty(string path, Array<string> args, int rows, int cols);          // flags = 0
    new Pty::Deterministic(string path, Array<string> args, int rows, int cols); // flags = 1
    bool ok();                       // pid > 0
    void write(string s);            // queue-and-drain via TcpStream (backpressure = D-P9)
    void onData((string) => void cb);
    void onClose(() => void cb);     // master closed (child side gone, output drained)
    int  resize(int rows, int cols);
    void kill();                     // sysKill(pid, 15); ConPTY tree-kill on Windows
    Promise<int> exitCode();         // drain-then-resolve, D-P5 ordering
}
```

There is **no `closeStdin()`**: a pty has one channel. The EOF protocol on a pty is the line
discipline's `VEOF` (`^D`, canonical mode) — `write("\x04")` — documented on the class, not a
method (R§D.2's cat-round-trip shape).

### 1.3 The platform floor (S2; declared here so every stage codes to one shape)

```c
/* --- pty floor (G-LANG-2 terminal half, designs/pty/) ----------------------
 * lv_plat_pty_spawn: allocate a pty, stamp the frozen termios profile selected
 *   by flags (bit0 = deterministic/golden, D-P3), seed rows×cols, fork, child
 *   does setsid/TIOCSCTTY/dup2(slave→0,1,2)/execve (async-signal-safe only,
 *   D-P2), parent closes the slave and returns pid > 0 with *master an
 *   O_CLOEXEC + O_NONBLOCK fd (read AND write). rows/cols <= 0 or any
 *   allocation failure: -1, no fd leaked. Exec failure: child _exit(127).
 * lv_plat_pty_resize: TIOCSWINSZ on the master / ResizePseudoConsole; the
 *   KERNEL delivers SIGWINCH to the child's foreground group — callers never
 *   signal by hand (R§A.4). 0/-1. */
int lv_plat_pty_spawn(const char* path, char* const argv[],
                      int rows, int cols, int flags, int* master);
int lv_plat_pty_resize(int master, int rows, int cols);
```

`reap`/`kill`/`pidfd_open`/`poll`/`recv`/`send`/`close` are the existing floor, unchanged in shape
(S3 gives `reap`/`kill` real win32 bodies — shape-identical, so not a STOP).

---

## 2. Design rulings (D-P1 … D-P10)

**D-P1 — Surface: research Candidate 1, unified spawn-with-pty.** One call allocates and spawns;
the caller never sees a slave fd. Candidate 2 (open-then-spawn) is rejected for v1: it exposes the
slave-lifetime footgun (parent must close it at exactly the right moment or child-exit EOF never
fires, R pitfall #7) and has no Windows analog (ConPTY hides the slave entirely) — the abstraction
would leak OS shape (R§C). Ratified: if a future need arises for spawning onto explicit fds, that
is Candidate 3's own design, not a mutation of this surface.

**D-P2 — `posix_openpt` family; `forkpty` is banned.** All non-async-signal-safe work
(grantpt/unlockpt/ptsname_r/open-slave/tcsetattr/TIOCSWINSZ) happens in the **parent, before
fork**; the child body is exactly `setsid(); ioctl(TIOCSCTTY); dup2×3; close; execve; _exit(127)`
— every call async-signal-safe (R§A.1-A.2). This is the spawn floor's D7 discipline extended by
two ioctls. LLVM programs are multithreaded; `forkpty`'s in-child `ptsname`/`login_tty` can
deadlock on the malloc lock (R pitfall #1).

**D-P3 — The floor owns the termios; two frozen profiles selected by `flags` bit0.** Stamped
explicitly on the slave before fork (never trusted to kernel defaults — they differ cross-OS):

- **DEFAULT (flags 0)** — a stock interactive terminal, what a shell expects:
  `c_iflag = ICRNL | IXON`; `c_oflag = OPOST | ONLCR`; `c_cflag = CS8 | CREAD`;
  `c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN`; `VMIN=1, VTIME=0`; B38400.
- **DETERMINISTIC (flags bit0, `LV_PTY_DETERMINISTIC = 1`)** — DEFAULT with
  `ECHO | ECHOE | ECHOK` cleared, nothing else. Echo is the one timing-dependent interleaving
  source (host writes bouncing back, R§D.1 / pitfall #5); everything else in DEFAULT is already
  deterministic. `ONLCR` stays ON in both profiles — goldens assert post-translation bytes
  (`\r\n`), the realistic terminal behavior Helm's VT parser will see anyway.

Two profiles rather than research's single frozen struct because the consumers genuinely differ:
Helm's terminal needs a real tty (echo on — a bare `cat` must show typing), goldens need echo
dead. Both are frozen constants **in the floor**; the language never sees a termios. The `stty`
-in-the-child alternative is rejected as racy (R§D.1).

**D-P4 — The EIO collapse (the one behavioral fix to an existing path).** Linux signals
child-gone on a pty master as `read() == -1/EIO`, macOS/BSD as `0` (R§A.3, CPython bpo-26228).
Today both recv fallbacks map any `read() < 0` to *would-block* — on a pty master that means
**the closed stream never closes and the read-watch busy-spins forever**. Fix in exactly two
places, additive semantics ("EIO on the read fallback = orderly close"):
- oracle: `src/RuntimeNatives.cpp:1632-1634` and `:1647-1650` (both sysRecv forms);
- floor: `lv_plat_recv` (`lv_plat_posix.c:446-451`).
Pipes are unaffected (a pipe read does not produce EIO in normal operation); sockets are
unaffected (the fallback only runs after ENOTSOCK). Writes need **no** change: `EIO` on a write
already lands in the existing fatal `-2` bucket (`lv_plat_posix.c:441-442`), exactly right for
"slave closed" (R§A.3).

**D-P5 — Exit retirement: drain, then reap, then resolve.** Two exit signals exist (master
EOF/EIO = "output done"; pidfd/reap = "status available") and their order is nondeterministic
(R§A.3/A.5, pitfall #11). Ruling, mirrored from `Process.tryReap`'s pumpAll-before-close
(`Resolver.cpp:1863-1879`): `Pty.tryReap` on a reap success **pumps the master dry
(`io.pumpAll()`), closes it, then resolves** — trailing output is never lost. Master-EOF alone
never resolves `exitCode()` (a pty can EOF while the status is still unreaped); it fires
`onClose` via the normal TcpStream close path.

**D-P6 — Reuse over invention.** The master fd is carried by the existing `TcpStream` (send
queue-and-drain, read-watch delivery, stale-fd guards, `pumpAll`); reaping is the existing
pidfd-watch/20ms-poll-fallback pair, verbatim from `Process` (`Resolver.cpp:1844-1879`). No new
loop machinery, no new watch kinds, no SIGCHLD handler — the spawn design's D6 restated.

**D-P7 — Symbol placement: new `runtime/lv_pty.c`.** The ownership pattern: `lv_loop.c` sockets,
`lv_thread.c` threads, `lv_proc.c` processes, **`lv_pty.c` ptys**. Marshaling conventions are
`lv_proc.c`'s verbatim (alias LvValue strings for argv, never copy; the inline 16-byte
`st_val` store; `retainDst()` array-return parity — spawn design D1/R7).

**D-P8 — Windows: ConPTY (1809+), reader-thread↔socketpair bridge, no winpty.** The bridge is the
node-pty shape (R§B.3): hidden floor-internal threads pump ConPTY's un-pollable anonymous pipes
into a loopback socketpair the WSAPoll loop *can* poll. Sanctioned under the signal self-pipe
precedent (`lv_plat.h:82-88`): **no language code ever runs on a bridge thread** — they move
bytes, nothing else. winpty is ratified OUT (R open-q #7): 1809 is the CI floor (Server 2019),
winpty is a heavy scraping dependency. Pre-1809 (probe `CreatePseudoConsole` via GetProcAddress):
`lv_plat_pty_spawn` returns `-1` → language sees the ordinary `[]` spawn failure. Runtime
degrade, not compile-time reject — same binary runs on 1809+ and older.

**D-P9 — Backpressure = the TcpStream policy, unchanged.** A full master (child not draining) or
full ConPTY input buffer surfaces as the send `-1` retryable; `TcpStream.send`'s queue-and-drain
write-watch absorbs it (`Resolver.cpp:1513-1519`). No new policy (R§E left it to us; the answer
is the one the codebase already has).

**D-P10 — Codegen gating (HARD, S2+S3).** `sysPtySpawn`/`sysPtyResize` lower on **all** targets
including Windows triples (S3 gives them a real floor; pre-S3 the win32 stubs return the failure
sentinels). `sysReap`/`sysKill` **lose** their Windows compile-time reject in S3 (they gain real
win32 bodies keyed by the pid registry, D-W3 in doc 03). `sysSpawn`/`sysPidfdOpen` **keep** the
Windows reject — pipes-spawn on Windows remains future work, and the prelude's pidfd→-1 fallback
already covers `sysPidfdOpen`'s absence on Windows ptys. The exact row surgery is in docs 02/03.

---

## 3. Testing doctrine (gates every stage)

- **POSIX three-lane byte-identity is required** (R§D.5): oracle/IR/LLVM on the same kernel with
  the frozen DETERMINISTIC termios produce identical accumulated master streams. One `.expected`
  per golden, three lanes diff it — the project doctrine, unchanged.
- **Accumulate, never chunk** (R§D.3, pitfall #12): every golden accumulates `onData` until close
  and asserts the total; chunk boundaries are nondeterministic.
- **The golden children** (R§D.2): `/bin/echo` (spawn+drain+exit), `/bin/cat` (write → VEOF `^D`
  → echo-back → exit 0), `/bin/stty size` (winsize seed + resize), `/bin/sh -c 'test -t 0 && echo
  tty'` (controlling-tty proof), a bad path (`_exit(127)` convention). All fixed-output under the
  DETERMINISTIC profile.
- **Windows is a separate behavioral lane** (R§B.2/D.5, pitfall #9): ConPTY re-renders — its
  bytes are ConPTY's VT, not the child's. Never cross-OS byte goldens; Windows asserts
  post-VT-strip content ("output contains `hi`", "exit code 0"). Doc 03 §6.
- **CI reality** (R§D.4): `posix_openpt` failing (`-1`, container without devpts) must skip/xfail
  loudly, never hang.

---

## 4. Gates

| gate | contents | proves | depends |
|---|---|---|---|
| **G-PTY1** | doc 01 (LANDED, `designs/complete/techdesign-01-pty-interpreter-lane.md`): oracle natives + EIO collapse + `Pty` prelude + `sys_pty` goldens, oracle=IR | the contract works end-to-end on the interpreters | — |
| **G-PTY2** | doc 02 (LANDED, `designs/complete/techdesign-02-pty-llvm-native.md`): plat floor + `lv_pty.c` + **HARD** LlvmGen rows + build wiring + selftest; goldens byte-identical oracle=IR=LLVM | **G-LANG-2 terminal half GREEN on POSIX** | G-PTY1 |
| **G-PTY3** | doc 03: ConPTY floor + bridge + win32 reap/kill + **HARD** gating changes; MinGW triple compiles; behavioral script checked in | the Windows charter is met to the same standard as the existing win32 floor (written-to-spec; executed when a Windows host is available) | G-PTY2 |
| **G-PTY4** | docs: `reference.md` Pty section; Helm §14 append (H10 unblocked); G-LANG-2 closed in the gate tables; research dossier marked consumed | the paper trail | G-PTY2 (POSIX) |

G-PTY3 and G-PTY4 are parallel after G-PTY2; Helm H10 needs only G-PTY2 to start (Helm's v1
golden lane is POSIX).

---

## 5. Risk register

| # | risk | mitigation |
|---|---|---|
| K1 | fork-in-threaded-runtime deadlock via pty setup | D-P2: all unsafe work pre-fork; child body async-signal-safe only; selftest spawns from a threaded context (spawn design R1 precedent) |
| K2 | EIO collapse regresses pipes/sockets | collapse is scoped to the read-*fallback* path + EIO only; full corpus + sysnatives suite must stay green (doc 01 §5) |
| K3 | echo/termios nondeterminism in goldens | DETERMINISTIC profile frozen in the floor (D-P3); goldens all use `Pty::Deterministic` |
| K4 | trailing output lost at exit (macOS may discard, R pitfall #11) | D-P5 drain-then-resolve; golden 1 asserts full output + exit code together |
| K5 | ARC mistake on the `[pid, master]` array (**HARD**) | sysArgs/sysSpawn `retainDst()` parity, settled empirically by valgrind selftest + memcheck lane (doc 02 §5) |
| K6 | ConPTY shutdown deadlock (pre-24H2) | drain-on-bridge-thread until pipe break; `ClosePseudoConsole` never called from the drainer; flags=0 (no INHERIT_CURSOR) — doc 03 §3 |
| K7 | Windows pid-reuse race on reap/kill | pid→HANDLE registry owned by the floor; reap/kill hit the registry, never OpenProcess-by-pid — doc 03 §4 |
| K8 | master fd leaks into sibling spawns | `posix_openpt(O_CLOEXEC)` atomic, fcntl fallback documented (R§A.6); fd-churn ±0 test extended to pty rounds |
| K9 | cross-triple archives silently missing `lv_pty.c` | both build lists + archive rebuild are one commit (spawn design R4 precedent) |

**STOP conditions (house rules, unchanged):** any existing `lv_abi.h` shape or native-signature
change; any new watch kind; any SIGCHLD handler; any X64Gen/ELF work; any emit-C++ system-layer
implementation; any weakening of the three-lane byte-identity doctrine. Hitting one =
stop and escalate, not improvise.

---

## 6. Implementation log (append-only)

- 2026-07-16 — design created from `docs/pty-floor-research.md`. Rulings D-P1..D-P10 recorded;
  all eight research open questions decided (surface = Candidate 1; termios = two frozen
  profiles, flags bit0; merged single stream ratified; retirement = drain-then-reap;
  kill sentinel + registry = doc 03 D-W3/D-W4; bridge ownership = per-pty, doc 03 §3;
  winpty = out; backpressure = TcpStream policy). Staged S1 (interpreters) → S2 (LLVM, HARD)
  → S3 (Windows ConPTY, HARD gating). Consumer: Helm H10 / gate G-H6.
- 2026-07-16 — **S1 LANDED, gate G-PTY1 green** (doc 01). Oracle `sysPty*` natives +
  the D-P4 EIO collapse, the `Pty` prelude class, and `tests/corpus/sys_pty/` all in;
  oracle = IR byte-identical, full regression sweep green. G-PTY2 (LLVM floor, HARD)
  is next and unblocks Helm H10 on POSIX. See doc 01 §7 for the landing detail.
- 2026-07-19 — **S2 LANDED, gate G-PTY2 green — the G-LANG-2 terminal half is GREEN
  on POSIX** (doc 02). The `lv_plat_pty_*` floor + the floor-half EIO collapse,
  `runtime/lv_pty.c`, the HARD `LlvmGen.cpp` rows, build wiring across all three
  source lists + both prebuilt triple archives, the `test_pty_floor` selftest case,
  and `tests/corpus/sys_pty/` promoted to three-lane byte-identity
  (oracle = IR = LLVM on one `.expected`). Per D-P8 the pty natives lower on **every**
  target including Windows and degrade at runtime (`[]`), rather than taking spawn's
  compile-time reject. Two findings recorded in doc 02 §9: §3.3's 5-arg arity question
  came back clean, and a latent S1 race (read-EOF closing the pty master SIGHUPs a
  session-leader child mid-exit) was diagnosed and fixed via `TcpStream.keepFdOnEof()`.
  Helm H10 can now start against `Pty` on oracle + IR + LLVM. S3 (Windows ConPTY,
  HARD gating) remains.
