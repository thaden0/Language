# Tech Design 01 — PTY Stage S1: Oracle Natives, EIO Collapse, `Pty` Prelude (gate G-PTY1)

**Status:** design, ready for implementation. **Parent:** `techdesign-00-overview.md` (rulings
D-P1..D-P10 are binding here; research citations `R§…` = `docs/pty-floor-research.md`).
**Scope:** everything needed to run a pty child on **oracle + IR**: the two `sysPty*` natives in
`src/RuntimeNatives.cpp` (shared by both interpreter lanes — the spawn precedent: no
Resolver/Checker/Eval/IrInterp changes are needed for a `sys*` free function), the EIO collapse in
the oracle's `sysRecv`, the prelude `sysPty*` signatures + `Pty` class in `src/Resolver.cpp`, and
the `tests/corpus/sys_pty/` goldens green with oracle = IR byte-identical.
**No LLVM, no Windows, no runtime/ C in this stage** (docs 02/03). Nothing in this stage is HARD
per the labeling convention — it is interpreter C++ and prelude Leviathan.

---

## 1. Oracle natives — `src/RuntimeNatives.cpp` (append beside the F7 spawn block, `:1869-1962`)

### 1.1 `sysPtySpawn` — the D-P2 sequence, transcribed

The block is the F7 `sysSpawn` (`:1879-1930`) with the pipe plumbing replaced by the R§A.2
sequence. Shape rules restated: `[]` on any allocation/spawn failure (empty path, bad rows/cols,
openpt/grant/unlock/ptsname/open/fork failure); child `_exit(127)` on exec failure; parent-built
argv; the SIGPIPE-once disposition is shared with sysSpawn (same static, or a second idempotent
ignore — either is fine, `SIG_IGN` is process-global).

```cpp
// --- G-LANG-2 terminal half: pty floor (designs/pty/) ---------------------
// sysPtySpawn(path, args, rows, cols, flags) -> [pid, masterFd] | [] on
// failure. ONE master fd, read+write (a pty fuses stdout/stderr). flags bit0
// = deterministic termios profile (D-P3). Child discipline is D-P2: ALL
// non-async-signal-safe work (openpt/grant/unlock/ptsname/open/tcsetattr/
// TIOCSWINSZ) happens here in the parent BEFORE fork; the child body is
// setsid/TIOCSCTTY/dup2/close/execve/_exit only.
if (name == "sysPtySpawn") {
    const std::string& path = args.size() > 0 ? args[0].s : std::string();
    long long rows = args.size() > 2 ? args[2].i : 0;
    long long cols = args.size() > 3 ? args[3].i : 0;
    long long flags = args.size() > 4 ? args[4].i : 0;
    std::vector<Value> fail;
    if (path.empty() || rows <= 0 || cols <= 0) { out = varr(std::move(fail)); return true; }
    static bool sigpipeIgnored2 = false;   // disposition, not a handler (F7 note)
    if (!sigpipeIgnored2) { ::signal(SIGPIPE, SIG_IGN); sigpipeIgnored2 = true; }

    std::vector<std::string> argvStore;              // argv BEFORE fork (D7/D-P2)
    argvStore.push_back(path);
    if (args.size() > 1 && args[1].arr)
        for (const Value& a : *args[1].arr) argvStore.push_back(a.s);
    std::vector<char*> argv;
    argv.reserve(argvStore.size() + 1);
    for (std::string& s : argvStore) argv.push_back(s.data());
    argv.push_back(nullptr);

    int mfd = ::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);   // atomic cloexec (R§A.6)
    if (mfd < 0) { out = varr(std::move(fail)); return true; }
    char sname[64];
    if (::grantpt(mfd) != 0 || ::unlockpt(mfd) != 0 ||
        ::ptsname_r(mfd, sname, sizeof sname) != 0) {
        ::close(mfd); out = varr(std::move(fail)); return true;
    }
    int sfd = ::open(sname, O_RDWR | O_NOCTTY);      // NOT cloexec: child keeps it past exec
    if (sfd < 0) { ::close(mfd); out = varr(std::move(fail)); return true; }

    // D-P3: stamp the frozen profile on the SLAVE, parent-side, pre-fork.
    struct termios tio;
    std::memset(&tio, 0, sizeof tio);
    tio.c_iflag = ICRNL | IXON;
    tio.c_oflag = OPOST | ONLCR;
    tio.c_cflag = CS8 | CREAD;
    tio.c_lflag = ISIG | ICANON | ECHOE | ECHOK | IEXTEN;
    if ((flags & 1) == 0) tio.c_lflag |= ECHO | ECHOE | ECHOK;  // DEFAULT profile
    else tio.c_lflag &= ~(tcflag_t)(ECHOE | ECHOK);             // DETERMINISTIC: echo family off
    tio.c_cc[VMIN] = 1; tio.c_cc[VTIME] = 0;
    // control chars: take the platform defaults for the rest (VEOF=^D, VINTR=^C…)
    tio.c_cc[VEOF] = 4; tio.c_cc[VINTR] = 3; tio.c_cc[VSUSP] = 26;
    tio.c_cc[VERASE] = 0x7f; tio.c_cc[VKILL] = 21; tio.c_cc[VQUIT] = 28;
    ::cfsetispeed(&tio, B38400); ::cfsetospeed(&tio, B38400);
    struct winsize ws; std::memset(&ws, 0, sizeof ws);
    ws.ws_row = (unsigned short)rows; ws.ws_col = (unsigned short)cols;
    if (::tcsetattr(sfd, TCSANOW, &tio) != 0 || ::ioctl(sfd, TIOCSWINSZ, &ws) != 0) {
        ::close(sfd); ::close(mfd); out = varr(std::move(fail)); return true;
    }

    pid_t pid = ::fork();
    if (pid < 0) { ::close(sfd); ::close(mfd); out = varr(std::move(fail)); return true; }
    if (pid == 0) {                                  // async-signal-safe ONLY (D-P2)
        ::setsid();                                  // session leader FIRST (R pitfall #8)
        ::ioctl(sfd, TIOCSCTTY, 0);                  // slave = controlling tty
        ::dup2(sfd, 0); ::dup2(sfd, 1); ::dup2(sfd, 2);
        if (sfd > 2) ::close(sfd);
        ::execve(path.c_str(), argv.data(), environ);
        ::_exit(127);
    }
    ::close(sfd);                    // parent keeps ONLY the master (R pitfall #7)
    int fl = ::fcntl(mfd, F_GETFL, 0);
    ::fcntl(mfd, F_SETFL, fl | O_NONBLOCK);          // rides the loop like every fd
    std::vector<Value> r{vint(pid), vint(mfd)};
    out = varr(std::move(r));
    return true;
}
```

Notes:
- `<termios.h>`, `<sys/ioctl.h>` includes may already be present (the raw-mode natives); add if not.
- The DETERMINISTIC profile clears the whole echo family (`ECHO|ECHOE|ECHOK`) — `ECHOE`/`ECHOK`
  without `ECHO` still echo erase/kill responses, which would be interleaving sources.
- The explicit `c_cc` seeds keep the two interpreter lanes and (S2) the compiled floor
  bit-identical in behavior even if libc defaults drift; VEOF=4 is what makes the documented
  `write("\x04")` EOF protocol frozen rather than environmental.
- `ptsname_r` is the thread-safe form — the oracle process hosts worker threads (Track 10).

### 1.2 `sysPtyResize`

```cpp
// sysPtyResize(masterFd, rows, cols) -> 0/-1. TIOCSWINSZ on the master; the
// KERNEL SIGWINCHes the child's foreground group — never signal by hand (R§A.4).
if (name == "sysPtyResize") {
    int fd = (int)(args.size() > 0 ? args[0].i : -1);
    long long rows = args.size() > 1 ? args[1].i : 0;
    long long cols = args.size() > 2 ? args[2].i : 0;
    if (fd < 0 || rows <= 0 || cols <= 0) { out = vint(-1); return true; }
    struct winsize ws; std::memset(&ws, 0, sizeof ws);
    ws.ws_row = (unsigned short)rows; ws.ws_col = (unsigned short)cols;
    out = vint(::ioctl(fd, TIOCSWINSZ, &ws) == 0 ? 0 : -1);
    return true;
}
```

### 1.3 The EIO collapse (D-P4) — `sysRecv`, both forms

At `:1632-1634` (Block form) and `:1647-1650` (string form), the read-fallback's error mapping
gains one arm. Current string form:

```cpp
ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
if (n < 0 && errno == ENOTSOCK)
    n = ::read(fd, buf.data(), buf.size());
if (n == 0) { out = vnone(); return true; }             // orderly close
if (n < 0) { out = vstr(""); return true; }             // would-block
```

becomes (comment included — this is the R pitfall #2 fix):

```cpp
ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
if (n < 0 && errno == ENOTSOCK) {
    n = ::read(fd, buf.data(), buf.size());
    // pty master, child gone: Linux says -1/EIO where macOS/BSD say 0 — both
    // ARE the orderly close (CPython bpo-26228; designs/pty/ D-P4). Without
    // this arm the read-watch busy-spins forever on a dead pty.
    if (n < 0 && errno == EIO) n = 0;
}
if (n == 0) { out = vnone(); return true; }             // orderly close
if (n < 0) { out = vstr(""); return true; }             // would-block
```

Identical arm in the Block form. Scope guard: the collapse lives **inside** the ENOTSOCK branch —
socket recv semantics are untouched; pipe reads don't produce EIO in normal operation. Writes
need nothing (D-P4: sysSend's fatal `-2` bucket already catches EIO, `:1595-1605`).

---

## 2. Prelude — `src/Resolver.cpp`

### 2.1 sys signatures (append to the F7 block at `:1477-1482`)

```
// G-LANG-2 terminal half (designs/pty/): [pid, masterFd] | [] on failure.
// One fd, read+write — a pty fuses stdout/stderr. rows/cols <= 0 refused.
// flags bit0 = deterministic termios (goldens). Resize: kernel SIGWINCHes
// the child; 0/-1.
Array<int> sysPtySpawn(string path, Array<string> args, int rows, int cols, int flags);
int sysPtyResize(int masterFd, int rows, int cols);
```

### 2.2 The `Pty` class (beside `Process`, after `:1880`)

`Process`'s shape with one stream instead of three, and the D-P5 retirement. Footgun discipline
observed: no truthiness, explicit `this`-receiver in stored handlers (bug #53), `0 - 1` sentinel
idiom, guards on stale fds.

```
// A child on a pseudo-terminal (designs/pty/ D-P1): ONE merged byte stream —
// there is no separate stderr on a pty. The master fd rides TcpStream (send
// queue-and-drain, read-watch delivery, D-P9); exit rides the pidfd-watch/
// poll-reap pair exactly like Process. EOF protocol: a pty has no closable
// write half — canonical mode's VEOF does it: write("\x04").
// Retirement (D-P5): reap success -> pumpAll -> close -> resolve; a master
// close alone (child side gone) fires onClose but never resolves exitCode.
class Pty {
    int pid = 0 - 1;
    int pidfd = 0 - 1;
    int reapWatchId = 0 - 1;
    int reapTimerId = 0 - 1;
    bool reaping = false;
    bool exited = false;
    TcpStream io = TcpStream(0 - 1);
    Promise<int> exitP = Promise();

    new Pty(string path, Array<string> args, int rows, int cols) {
        Array<int> r = std::sysPtySpawn(path, args, rows, cols, 0);
        if (r.length() == 2) { pid = r.at(0); io = TcpStream(r.at(1)); }
    }
    // The frozen deterministic termios profile (echo family off) — goldens.
    new Pty::Deterministic(string path, Array<string> args, int rows, int cols) {
        Array<int> r = std::sysPtySpawn(path, args, rows, cols, 1);
        if (r.length() == 2) { pid = r.at(0); io = TcpStream(r.at(1)); }
    }
    bool ok() => pid > 0;

    void write(string s) { io.send(s); }
    void onData((string) => void cb) { io.onData(cb); }
    void onClose(() => void cb) { io.onClose(cb); }
    int resize(int rows, int cols) {
        if (io.rawFd() < 0) return 0 - 1;            // stale-fd guard (TcpStream discipline)
        return std::sysPtyResize(io.rawFd(), rows, cols);
    }
    void kill() {
        if (pid > 0 && !exited) { std::sysKill(pid, 15); }
        // do NOT close io here: D-P5 — the dying child's last output is still
        // in the line discipline; tryReap drains it.
    }

    Promise<int> exitCode() {
        if (exited || reaping) return exitP;
        if (pid <= 0) {                              // spawn failure: 127 (F7 convention)
            exited = true;
            exitP.resolve(127);
            return exitP;
        }
        reaping = true;
        pidfd = std::sysPidfdOpen(pid);
        Pty self = this;
        if (pidfd >= 0) {
            reapWatchId = std::sysWatch(pidfd, (ready) => self.tryReap());
        } else {
            // pidfd unavailable (exotic kernel; Windows, doc 03): poll-reap.
            reapTimerId = std::sysTimerStart(20, 20, (n) => self.tryReap());
        }
        return exitP;
    }
    void tryReap() {
        if (exited) return;
        int code = std::sysReap(pid);
        if (code < 0) return;                        // spurious wake
        exited = true;
        if (reapWatchId >= 0) { std::sysUnwatch(reapWatchId); reapWatchId = 0 - 1; }
        if (reapTimerId >= 0) { std::sysTimerCancel(reapTimerId); reapTimerId = 0 - 1; }
        if (pidfd >= 0) { std::sysClose(pidfd); pidfd = 0 - 1; }
        io.pumpAll();                                // drain BEFORE close (D-P5, pitfall #11)
        io.close();
        exitP.resolve(code);
    }
}
```

Implementation notes:
- Verify `TcpStream.onClose` exists as a subscription (the class has `onClosed`/`hasCloseCb`
  fields, `:1493-1495`); if the setter is named differently, follow the existing name — do not
  add a second close-callback mechanism.
- `Pty::Deterministic` uses the labeled-constructor convention (house style; the
  `TextBuffer::FromFile` precedent). If labeled constructors reject a body identical to the
  primary, factor the common tail per prevailing prelude idiom — semantics above are the contract.
- `io.pumpAll()` after child exit is what makes the EIO collapse load-bearing: pumpAll loops
  sysRecv until would-block or close; without D-P4 it would loop on `""` forever on Linux.

---

## 3. IR lane

Nothing to do: `sys*` free functions route through `RuntimeNatives.cpp` for **both** oracle and IR
(the spawn design's §5 "no IrInterp changes" precedent). The IR lane is exercised by the goldens.

---

## 4. Goldens — `tests/corpus/sys_pty/` (oracle + IR in S1; the same files get the LLVM lane in S2)

One directory, one `.expected`, `run_corpus.sh`-driven — the `sys_spawn/` layout. All children use
`Pty::Deterministic` (D-P3); all asserts are on **accumulated** streams (R§D.3). Steps chain off
`exitCode()` resolutions so the transcript is deterministic (the `sys_spawn.lev` pattern).

`sys_pty.lev` sequence:

1. **echo**: `Pty::Deterministic("/bin/echo", ["hi"], 24, 80)` — accumulate onData to close;
   expect `hi\r\n` (ONLCR ON, D-P3), exit 0. Proves spawn/stream/drain/reap and regression-pins
   the EIO collapse (without D-P4 this step never terminates — the busy-spin turns the test red
   by timeout, not silently green).
2. **cat + VEOF**: `Pty::Deterministic("/bin/cat", [], 24, 80)`; `write("ping\n")`,
   `write("\x04")`; expect `ping\r\n` (echo off — no bounce; ONLCR translates cat's `\n`),
   exit 0. Proves the write path and the frozen VEOF protocol.
3. **winsize**: `Pty::Deterministic("/bin/stty", ["size"], 24, 80)` → `24 80\r\n`, exit 0.
   Proves the pre-exec TIOCSWINSZ seed (no query race, R§A.2).
4. **resize**: `Pty::Deterministic("/bin/sh", ["-c", "read x; stty size"], 24, 80)`;
   `resize(30, 100)`, then `write("go\n")` — the child's `read` blocks until our write, so the
   resize is ordered-before deterministically → `30 100\r\n`, exit 0. Proves TIOCSWINSZ +
   kernel SIGWINCH delivery on a live child.
5. **controlling tty**: `Pty::Deterministic("/bin/sh", ["-c", "test -t 0 && echo tty"], 24, 80)`
   → `tty\r\n`, exit 0. Proves setsid/TIOCSCTTY (R pitfall #8 — without them `test -t 0` still
   passes on a bare slave, but a `sh -c 'ps -o tty= -p $$'`-style probe is nondeterministic;
   `test -t 0` plus step 6 together pin the prologue).
6. **kill**: `Pty::Deterministic("/bin/cat", [], 24, 80)`; `kill()` → exit `143` (128+15).
   Proves sysKill on a session-leader child and the signaled-exit encoding.
7. **bad path**: `Pty::Deterministic("/no/such/binary", [], 24, 80)` → `ok()` true-shaped spawn,
   exit `127`. Proves the exec-failure convention survives the pty prologue.
8. **spawn refusals**: `sysPtySpawn("", [], 24, 80, 0)` and `("/bin/echo", [], 0, 80, 0)` →
   `[]` both; `Pty` wrapper resolves 127. Proves the D-P3/D-P1 argument guards.

CMake: `corpus_sys_pty_treewalk` + `corpus_sys_pty_ir` lanes beside the `sys_spawn` pair
(`CMakeLists.txt` — the `:363-368` pattern). The dir carries a header noting the LLVM lane
arrives with doc 02 (so `corpus_llvm_full`'s exclusion list carries `sys_pty/` for exactly one
stage — removed in S2).

---

## 5. Regression sweep (must stay green, same commit)

- Full existing corpus + `run_sysnatives.sh` (the EIO collapse touches the shared sysRecv);
- `sonar_v2` differential suite and `examples/helm/tests/run-tests.sh` (TcpStream consumers);
- fd-churn: extend the ±0 probe in `run_sysnatives.sh` with pty spawn/reap rounds — proves the
  master + pidfd never leak (K8).

---

## 6. Landing checklist

1. `RuntimeNatives.cpp`: §1.1 + §1.2 natives, §1.3 collapse (both sysRecv forms).
2. `Resolver.cpp`: §2.1 signatures + §2.2 `Pty` class.
3. `tests/corpus/sys_pty/` + two CMake lanes; exclusion-list entry for the LLVM full scan.
4. §5 sweep green; oracle = IR byte-identical on `sys_pty.expected`.
5. Append the landing entry to `techdesign-00-overview.md` §6.

---

## 7. Implementation log (append-only)

- 2026-07-16 — doc created (S1 of designs/pty/). Awaiting implementation.
