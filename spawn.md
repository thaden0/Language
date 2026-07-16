# Spawn / Process on LLVM — Research Findings

**Type:** research document (findings only — not a tech design, not an implementation plan).
**Purpose:** capture everything currently known about the `sysSpawn`/`Process` floor and the
LLVM backend so that a tech design for bringing process-spawn to LLVM can be written from this
document alone. Every claim carries a `file:line` citation into the tree as it stands today.
**Date:** 2026-07-16.

This document describes **what is**, not what should be built. Where the research surfaced a
choice, it is recorded as an open question (§13), not a recommendation.

---

## 1. The question, and the short answer the research gives

Process spawn (`std::sysSpawn` and the in-language `Process` class) runs today on the **oracle and
IR interpreters only**; the compiled backends (LLVM `--build-native`, emit-C++ `--build`) defer the
spawn natives. This is gated as **G-LANG-2**.

The research finding is that the deferral is narrow. The feature is built in layers, and **only the
lowest layer is missing on LLVM**:

| Layer | Where it lives | LLVM status today |
|-------|----------------|-------------------|
| `Process`, `TcpStream`, `Channel` (in-language wrappers) | prelude string in `src/Resolver.cpp` | **already compiles to LLVM** — ordinary Leviathan code, engine-agnostic |
| event loop, `sysWatch`/`sysWatchWrite`/`sysUnwatch`, timers | `runtime/lv_loop.c` | **already ships on LLVM** (used by sockets/TLS) |
| `sysSend`/`sysRecv`/`sysClose` with pipe (`ENOTSOCK`) fallback | `runtime/lv_plat_posix.c` | **already ships on LLVM** |
| the four process primitives: `sysSpawn`, `sysPidfdOpen`, `sysReap`, `sysKill` | `src/RuntimeNatives.cpp` (interpreter only) | **absent from the C runtime and from `LlvmGen.cpp`** |

So the LLVM gap is four native functions plus their wiring. Everything they plug into already
exists and runs on LLVM. Sections 2–12 document each row of that table in full.

---

## 2. The frozen contract (the four natives)

### 2.1 Language-level signatures — `src/Resolver.cpp:1477-1482`

Declared bodiless in the `std::` prelude (the resolver emits `Op::CallNativeFn` with `sname` = the
name); the same declarations are seen by every backend:

```
// Track 08 F7: spawn floor. [pid, stdinFd, stdoutFd, stderrFd], [] = spawn
// failure (exec failure instead arrives as exit code 127 via sysReap).
Array<int> sysSpawn(string path, Array<string> args);
int sysPidfdOpen(int pid);   // pollable fd, ready when pid exits
int sysReap(int pid);        // -1 running; else code (128+sig if signaled)
int sysKill(int pid, int sig);  // 0/-1; pid <= 0 refused
```

### 2.2 Authoritative design record

`designs/complete/techdesign-08-system-natives.md` (Track 08, status **COMPLETE**). §7 is the F7
spec; §9 the numbered "problems"; §12 the STOP conditions; §13 the landed log. Per §13 the doc is
now historical record — **the living contract is the prelude code** (`Resolver.cpp`) and the
interpreter reference implementation (`RuntimeNatives.cpp`, §4 here).

### 2.3 Semantics and invariants that any implementation must preserve

Sourced from `designs/complete/techdesign-08-system-natives.md` §7 (`:128-152`) and the interpreter
oracle (`src/RuntimeNatives.cpp:1850-1933`):

- **`sysSpawn` return shape:** `[pid, stdinFd, stdoutFd, stderrFd]` (4-element `Array<int>`) on
  success; `[]` (empty array) on **spawn failure** (a `pipe`/`fork` failure only).
- **exec failure ≠ spawn failure.** A bad path is *not* a spawn failure: the child `execve` fails,
  the child `_exit(127)`s, and `127` arrives later via `sysReap`. (`Resolver.cpp` maps
  `pid <= 0` → resolve `127` at the `Process` level, `Resolver.cpp:1846-1849`.)
- **No PATH search** — explicit path only (v1 boundary).
- **Pipes are `O_CLOEXEC`** (a later sibling won't inherit the parent ends) and **parent ends are
  `O_NONBLOCK`** (reads ride the event loop; stdin writes queue-and-drain).
- **`SIGPIPE` is ignored once**, lazily, at first spawn — so a write to a dead child's stdin
  surfaces as `EPIPE` (→ `-2` from `sysSend`) rather than killing the host process. The design notes
  this is "a disposition, not a handler — §12's no-signal-handler rule is about SIGCHLD-style
  reentrancy" (`RuntimeNatives.cpp:1854-1859`).
- **`sysReap` return:** `-1` = still running (or not our child); `0..255` = normal exit
  (`si_status & 0xFF`); `128 + signal` if signal-terminated (shell convention). **Never blocks** —
  `waitid(..., WNOHANG)`.
- **`sysKill` refuses `pid <= 0` (and `sig < 0`)** at the floor: the `kill(2)` group/broadcast forms
  (`0`, `-1`, `-pgid`) are never exposed to programs. `Process.kill()` uses `SIGTERM` (15); a pending
  `exitCode()` then resolves `143` (`128+15`).
- **No SIGCHLD handler anywhere.** Reaping is poll-driven via `pidfd_open` + `waitid(WNOHANG)`. This
  is a hard design rule (§12 STOP condition: "Any signal-handler temptation (SIGCHLD et al.) — the
  design's pidfd route exists precisely to avoid it").
- **Reap flushes buffered output before closing.** The child's last stdout/stderr can race pidfd
  readiness, so the reap path pumps the pipes to EOF before closing them; a spawn-churn loop leaves
  the fd table at ±0 (`Resolver.cpp:1874`; asserted by a test, §10).

### 2.4 Naming caution — two unrelated "spawn"s

Greps for "spawn" hit two distinct features. The research repeatedly had to separate them:

- **Process spawn (Track 08 F7)** — `std::sysSpawn` / `Process`, an external OS process
  (`fork`+`execve`+pipes). **This document's subject.** Interpreter-only today.
- **Thread spawn (Track 10)** — `std::spawn` / `Worker<T>` / `Channel<T>`, cooperative tasks and OS
  threads inside one process. **Already fully landed on LLVM** (`info.md:1589`; `runtime/lv_thread.c`).
  The `lv_spawn_global_check` / `lvrt_register_spawn_check` / `LvSpawnCheckFn` machinery
  (`lv_abi.h:271-285`, `LlvmGen.cpp:853-864`, `lv_runtime.c:1005-1007`) belongs to *this* feature and
  is unrelated to child processes (child processes share no heap, so nothing is flattened). Do not
  conflate them.

---

## 3. The reference implementation (the interpreter oracle)

The byte-exact behavioral spec lives in `src/RuntimeNatives.cpp:1840-1933`. Both interpreters reach
it through the free-function dispatcher `nativeFreeCall(name, args, out, err, sink)`
(`RuntimeNatives.cpp:1183`), a linear `if (name == "...")` chain. (Class *methods* use a separate
`nativeCall(...)` at `RuntimeNatives.cpp:592` — the spawn quartet and all fd-I/O natives are free
functions, not methods.)

Dispatch entry points:
- Oracle (`Eval.cpp`): free natives → `nativeFreeCall(...)` at `Eval.cpp:759`.
- IR (`IrInterp.cpp`): free natives → `nativeFreeCall(in.sname, ...)` at `IrInterp.cpp:279`.

### 3.1 `sysSpawn` — `RuntimeNatives.cpp:1850-1901` (verbatim, abbreviated)

```cpp
if (name == "sysSpawn") {
    const std::string& path = args.size() > 0 ? args[0].s : std::string();
    if (path.empty()) { out = varr({}); return true; }           // [] spawn failure
    static bool sigpipeIgnored = false;
    if (!sigpipeIgnored) { ::signal(SIGPIPE, SIG_IGN); sigpipeIgnored = true; }
    // argv built BEFORE fork: argv[0] = path, then args[1] elements, NULL-terminated
    int inP[2], outP[2], errP[2];
    if (::pipe2(inP, O_CLOEXEC) != 0) { out = varr({}); return true; }   // + close cascade
    if (::pipe2(outP, O_CLOEXEC) != 0) { /* close inP; */ out = varr({}); return true; }
    if (::pipe2(errP, O_CLOEXEC) != 0) { /* close inP,outP; */ out = varr({}); return true; }
    pid_t pid = ::fork();
    if (pid < 0) { /* close all six; */ out = varr({}); return true; }
    if (pid == 0) {                       // child: dup2 + exec only; nothing allocates
        ::dup2(inP[0], 0); ::dup2(outP[1], 1); ::dup2(errP[1], 2);
        ::execve(path.c_str(), argv.data(), environ);
        ::_exit(127);
    }
    ::close(inP[0]); ::close(outP[1]); ::close(errP[1]);          // parent closes child ends
    for (int fd : {inP[1], outP[0], errP[0]}) {                   // parent ends non-blocking
        int fl = ::fcntl(fd, F_GETFL, 0); ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }
    out = varr({vint(pid), vint(inP[1]), vint(outP[0]), vint(errP[0])});
    return true;
}
```

Syscalls used: `signal`, `pipe2`, `fork`, `dup2`, `execve`, `_exit`, `close`, `fcntl`.

### 3.2 `sysPidfdOpen` — `RuntimeNatives.cpp:1904-1909`

```cpp
if (name == "sysPidfdOpen") {
    long long pid = args.empty() ? -1 : args[0].i;
    if (pid <= 0) { out = vint(-1); return true; }
    out = vint((long long)::syscall(SYS_pidfd_open, (pid_t)pid, 0));   // Linux ≥ 5.3
    return true;
}
```

### 3.3 `sysReap` — `RuntimeNatives.cpp:1913-1924`

```cpp
if (name == "sysReap") {
    long long pid = args.empty() ? -1 : args[0].i;
    if (pid <= 0) { out = vint(-1); return true; }
    siginfo_t si; si.si_pid = 0;                                  // WNOHANG zero-out idiom
    if (::waitid(P_PID, (id_t)pid, &si, WEXITED | WNOHANG) != 0 || si.si_pid == 0) {
        out = vint(-1); return true;                             // still running / not ours
    }
    out = vint(si.si_code == CLD_EXITED ? (long long)(si.si_status & 0xFF)
                                        : 128 + (long long)si.si_status);
    return true;
}
```

### 3.4 `sysKill` — `RuntimeNatives.cpp:1927-1933`

```cpp
if (name == "sysKill") {
    long long pid = args.size() > 0 ? args[0].i : -1;
    long long sig = args.size() > 1 ? args[1].i : SIGTERM;
    if (pid <= 0 || sig < 0) { out = vint(-1); return true; }     // group/broadcast never exposed
    out = vint(::kill((pid_t)pid, (int)sig) == 0 ? 0 : -1);
    return true;
}
```

### 3.5 Companion fd-I/O natives the feature rides (already on LLVM)

`Process` wraps each pipe fd in a `TcpStream`, so it uses the socket natives, which fall back to
`read(2)`/`write(2)` on `ENOTSOCK` (pipes are not sockets):

- **`sysSend(fd, data) -> int`** (`RuntimeNatives.cpp:1547-1581`): `>=0` bytes written; `-1`
  retryable (`EAGAIN`/`EWOULDBLOCK`/`EINTR`); `-2` fatal (`EPIPE`/peer gone). Uses
  `send(..., MSG_NOSIGNAL)` then `write(2)` on `ENOTSOCK`. Zero-copy `Block` overload exists.
- **`sysRecv(fd, max) -> string?`** (`RuntimeNatives.cpp:1582-1625`): `None` = EOF/closed; `""` =
  would-block; non-empty = data. `recv(...)` then `read(2)` on `ENOTSOCK`. Default `max` 4096. `Block`
  overload returns `int?` (`None`/`0`/`>0`).
- **`sysClose(fd) -> int`** (`RuntimeNatives.cpp:1313-1321`).
- **Watches:** `sysWatch`/`sysWatchWrite`/`sysUnwatch` (`RuntimeNatives.cpp:1626-1642`);
  `sysTimerStart`/`sysTimerCancel` (`RuntimeNatives.cpp:1330-1341`).

### 3.6 Portability flags observed in the oracle

| Feature | Call | Portability |
|---|---|---|
| CLOEXEC pipes | `pipe2(fd, O_CLOEXEC)` | Linux/BSD; POSIX fallback = `pipe` + `fcntl(FD_CLOEXEC)` |
| Non-blocking parent ends | `fcntl(F_GETFL/F_SETFL, O_NONBLOCK)` | POSIX |
| Pidfd reap | `syscall(SYS_pidfd_open, pid, 0)` | **Linux ≥ 5.3 only**; prelude has a 20 ms poll-reap fallback when it returns `-1` (§5.2) |
| Non-blocking reap | `waitid(P_PID, pid, &si, WEXITED\|WNOHANG)` + `si.si_pid=0` pre-clear | POSIX API; the zero-out idiom is the Linux WNOHANG signal |
| No-death writes | `SIGPIPE` `SIG_IGN` + `send(MSG_NOSIGNAL)` + `ENOTSOCK`→`write` | `MSG_NOSIGNAL` Linux/BSD; macOS uses `SO_NOSIGPIPE` |
| Event loop | `poll(2)` | POSIX; portable |

---

## 4. The in-language layer (engine-agnostic; already on LLVM)

All of this is prelude Leviathan code in `src/Resolver.cpp`. It calls only `std::sys*` floor
functions, so it compiles to whatever the backend provides — no per-backend code exists here.

### 4.1 `Process` — `src/Resolver.cpp:1808-1880`

Fields (`:1809-1818`): `pid=-1`, `pidfd=-1`, `reapWatchId=-1`, `reapTimerId=-1`, `bool reaping`,
`bool exited`, three `TcpStream` (stdin/stdout/stderr, each `TcpStream(-1)`), `Promise<int> exitP`.

Constructor (`:1820-1828`) calls `std::sysSpawn(path, args)`; only when the result length is 4 does
it set `pid` and wrap the fds in `TcpStream`s:

```
new Process(string path, Array<string> args) {
    Array<int> r = std::sysSpawn(path, args);
    if (r.length() == 4) {
        pid = r.at(0);
        stdinS = TcpStream(r.at(1));
        stdoutS = TcpStream(r.at(2));
        stderrS = TcpStream(r.at(3));
    }
}
```

Surface: `ok()` (`pid > 0`), `write(s)` → `stdinS.send`, `closeStdin()`, `onStdout(cb)`/`onStderr(cb)`
→ `stdoutS.onData`/`stderrS.onData`, `kill()` → `sysKill(pid,15)` + close stdin, `exitCode() ->
Promise<int>` (§5.2), `tryReap()` (§5.2). The pid drives reaping; the fds become streams. **No new
event machinery beyond the pidfd reap watch.**

### 4.2 `TcpStream` (the pipe wrapper) — `src/Resolver.cpp:1489-1598`

- `send(s)` → `std::sysSend`; short write buffers the tail + registers a write-watch
  `std::sysWatchWrite(fd, ()=>drain())`; `-2` drops the buffer.
- `onData(cb)` → stores `cb`, registers a read-watch `std::sysWatch(fd, ()=>pump())`.
- `pump()` → `std::sysRecv(fd, 4096)`; `None` → close + fire `onClosed`; else deliver the chunk.
- `pumpAll()` → synchronous drain to EOF/would-block (used by reap to flush trailing output).
- `close()` → idempotent; `stopDrain`, `sysUnwatch`, `sysClose`, `fd=-1` (stale-fd discipline).
- Uses the `TcpStream self = this;` capture idiom (lambdas don't close over the implicit receiver).

### 4.3 `Channel<T>` — `src/Resolver.cpp:1338-1393`

Not part of the spawn floor, part of the same concurrency surface. Dual-mode: `sysChannelNew`
returns `>=0` (native SPSC ring + eventfds, the LLVM/POSIX leg) or `-1` (interpreters → in-language
queue). Documented here only because it is the sanctioned cross-thread portal referenced by the
flattenability rule (§6).

---

## 5. The event loop (the substrate the reap plugs into; already on LLVM)

Two behavior-identical implementations: `src/RuntimeLoop.cpp` (C++, interpreters) and
`runtime/lv_loop.c` (C, linked into `liblvrt.a` for LLVM). **LLVM uses `runtime/lv_loop.c` only.**
`runtime/lv_loop.c:361-365` cites `RuntimeLoop.cpp`'s `nextBatch` as ground truth.

### 5.1 Mechanism and registries

- **`poll(2)`, level-triggered**, via the platform seam `lv_plat_poll` (`lv_loop.c:432`;
  `lv_plat_posix.c:447-474`). A fresh `pollfd` set is rebuilt every tick — no persistent kernel
  registration, no epoll_ctl. **Adding a child fd is just a watch; no new loop plumbing.**
- Two thread-local registries (`lv_loop.c:36-47`): `Timer*` and `Watch { id, fd, write, cb, active }`.
  Both are `LV_TLS` — a child's fds must be watched on the thread that owns the pumping loop.
- **fd-watch API already present** (`lv_loop.c:329-344`, declared `lv_abi.h:718-723`):
  `lvrt_syswatch` (read), `lvrt_syswatchwrite` (write), `lvrt_sysunwatch`. The callback receives the
  fd as an `LV_INT` argument (`lv_loop.c:461`) and does the `sysRecv`/`sysRead`. **A pipe or pidfd
  registers exactly like a socket fd — no new C API is needed.**
- **C-internal cancel:** `lvrt_loop_cancel_fd(int fd)` (`lv_loop.c:321-327`, prototype
  `lv_thread.h:44`) cancels every watch on an fd before its number is closed and reused. Exists
  precisely because eventfd/pidfd numbers get closed+reused; a stale watch would fire on the wrong
  fd (the documented `POLLNVAL`-alone race, `lv_loop.c:314-320`).
- **Tick model:** `lvrt_loop_has_work()` (`:352-356`) + `lvrt_loop_step()` (`:366-528`, one batch:
  compute timeout from earliest timer → build pollfd set → `lv_plat_poll` → snapshot ready ids → fire
  watch callbacks with fd arg → retire `POLLNVAL` dead fds → fire due timers oldest-first). Ready ids
  are snapshotted before firing because callbacks may add/cancel watches mid-batch.
- **Program lifetime** = loop runs while the registry has live work. A live pidfd watch keeps the
  program alive until the child exits and the watch retires — the correct behavior for a child.
- **EOF/half-close:** `lv_plat_poll` folds `POLLHUP|POLLERR` into read-readiness (`lv_plat_posix.c:463-470`);
  a child closing stdout surfaces as `POLLHUP` → read watch fires → `sysRecv` returns `None`. Same
  path as socket half-close (`lv_loop.c:598-614`); a pipe fd rides it unchanged.
- **Timers:** `lvrt_systimerstart(delayMs, intervalMs, cb)` / `lvrt_systimercancel` (`:265-273`);
  `intervalMs 0` = one-shot. Timers set the poll timeout, so a kill-after-N-ms is naturally a
  one-shot timer racing the pidfd watch (the `lvrt_sysawaitany2` multi-park at `:213-233` is the
  existing shape).

### 5.2 How exit-code delivery works (`Process`, no new C code)

`Process.exitCode()` (`Resolver.cpp:1844-1862`) and `tryReap()` (`:1863-1879`):

```
Promise<int> exitCode() {
    if (exited || reaping) return exitP;
    if (pid <= 0) { exited = true; exitP.resolve(127); return exitP; }   // spawn failure
    reaping = true;
    pidfd = std::sysPidfdOpen(pid);
    Process self = this;
    if (pidfd >= 0) reapWatchId = std::sysWatch(pidfd, (ready) => self.tryReap());     // primary
    else            reapTimerId = std::sysTimerStart(20, 20, (n) => self.tryReap());    // 20ms poll fallback
    return exitP;
}
```

`tryReap()`: `code = std::sysReap(pid)`; `< 0` → spurious wake, return. Else set `exited`;
unwatch/cancel; close pidfd; `stdinS.close()`; **`stdoutS.pumpAll()` + `stderrS.pumpAll()` BEFORE
closing** (output races pidfd readiness); close stdout/stderr; `exitP.resolve(code)`.

The pidfd-watch reap is **structurally identical to the already-shipping thread-join** in
`runtime/lv_thread.c`: `lvrt_systhreadstart` creates a level-triggered eventfd as the join fd
(`:223`), the prelude `sysWatch`es it, and `lvrt_systhreadresult` (`:247-279`) is the callback that
drains, collects the result, and does **`lvrt_loop_cancel_fd(fd)` before `close(fd)`** (`:266-267`).
A pidfd is pollable identically to an eventfd; `sysReap` is the `lvrt_systhreadresult` analogue.

---

## 6. The flattenability / spawn-boundary rule (why `Process` stays on one thread)

A `Process` is an **fd-/loop-bound carrier**: its meaning is a set of fds plus live event-loop watch
registrations bound to one thread's loop. It cannot cross a `std::spawn` (thread) boundary.

Reference — `docs/reference.md:1651-1661` (§6.6.66 Flattenability):

> What may cross a boundary: primitives, `char`, `None`, ranges, `struct` values, strings, pure
> `Array`/`Map` of flattenable elements, and statically-shaped `class` objects. Rejected — a loud,
> catchable error naming the type: a nested closure (only the spawn body itself may cross), an
> **fd-/loop-bound carrier** (`TcpStream`/`TcpListener`/`Timer`/`Process`, and a disposable
> `InStream`), and a `Block`. … Keep loop-bound carriers on their owning thread; pass a `Channel<T>`.

Reference — `docs/reference.md:1490-1516` (§6.6.59 Process): documents the `Process` API, the
`ok()`/`exitCode()`/`kill()` surface, the 127/143 conventions, the pidfd-no-SIGCHLD reap, the
"delivers buffered output then closes every owned fd; spawn-churn loop leaves the fd table at ±0"
guarantee, and closes with: *"All `sys*`-prefixed → comptime-denied automatically. Interpreters only
(oracle + IR); compiled backends defer cleanly."*

Interpreter enforcement backstop — `RuntimeNatives.cpp:903-906`:

```cpp
static bool lvThreadNonTransferable(std::string_view cls) {
    return cls == "TcpStream" || cls == "TcpListener" || cls == "Timer" ||
           cls == "Process" || cls == "TcpConnector";
}
```

The flatten/rebuild walk (`lvThreadCopy`, driven by `sysThreadTransfer`) rejects these with "value of
type <T> cannot cross a thread boundary (v1); pass a Channel" (`RuntimeNatives.cpp:981-982, 1068`).
Every `sys*`-prefixed name is comptime-denied (`reference.md:1515-1516`; guards at `Eval.cpp:742-743`).

---

## 7. How a system native reaches the LLVM backend (existing pipeline)

LLVM-compiled programs do **not** call `RuntimeNatives.cpp` (that is compiled into the interpreter).
They call `lvrt_*` symbols resolved by the linker against `liblvrt.a` (`runtime/lv_*.c`). The
normative contract is `runtime/lv_abi.h`.

### 7.1 The ABI boundary — `runtime/lv_abi.h`

- **Boundary rule** (`lv_abi.h:6-11`): every value crosses as an `LvValue*` pointer, **out-param
  first, then inputs**. Nothing passes/returns `LvValue` by value (clang lowers 16-byte structs
  differently per target; the pointer convention sidesteps it). This holds even for scalar-int args
  (fd, pid, signal) — they still cross as `const LvValue*`.
- **The value** (`lv_abi.h:36`): `typedef struct LvValue { int64_t tag; int64_t payload; } LvValue;`
  (16 bytes). Tags (`:38-40`): `LV_VOID=0, LV_INT=1, LV_FLOAT=2, LV_BOOL=3, LV_STR=4, LV_OBJ=5,
  LV_ARR=6, LV_MAP=7, LV_NONE=8, LV_CLO=9, LV_CHAR=10, LV_BLOCK=11, LV_WEAK_PROXY=12`.
- **int:** immediate — `tag=LV_INT`, `payload` holds the value.
- **None:** `tag=LV_NONE`, `payload=0`.
- **string** (`:85-90`): `payload -> { int64_t len; char bytes[len]; char nul; }` → C bytes at
  `payload + 8`, length at `payload + 0`.
- **boxed array** (`:109`): `payload -> { int64_t len; LvValue elems[capacity] }`; `word0=len`, header
  `meta=capacity`; element `i` at byte offset `8 + 16*i`.
- **`lv_abi.h` is STOP-protected** (`:1-10`): "the normative contract … contract changes are a STOP
  event (doc §0.2)." Adding new `lvrt_*` function declarations is compatible; changing an existing
  shape, or renumbering the `LV_OP_*` enum (`:377-380`, frozen/append-only, unrelated to spawn), is a
  STOP.

### 7.2 How `LlvmGen` declares and lowers a native — `src/LlvmGen.cpp`

- **Extern decl.** A helper `fn(name, ret, argTypes)` = `module->getOrInsertFunction(...)`
  (`:220-223`); the linker binds it to the `lv_*.c` definition. Each native has a `FunctionCallee`
  member (list at `:149-191`) initialized in the ctor. Existing rows for reference:
  ```cpp
  rtSysTcpConnect = fn("lvrt_systcpconnect", voidTy, {ptrTy, ptrTy, ptrTy});   // :345
  rtSysArgs       = fn("lvrt_sysargs",      voidTy, {ptrTy});                  // :328
  rtSysAccept     = fn("lvrt_sysaccept",    voidTy, {ptrTy, ptrTy});          // :357
  ```
- **Call lowering.** A free `std::sysFoo(...)` lowers via IR op `Op::CallNativeFn` (`:2457`), a giant
  `if (n == "sysFoo") ... else if ...` chain keyed on `in.sname`. `regs[in.a]` = dest (out-param),
  `arg(k) = regs[in.c + k]` = inputs, `in.d` = argc. Example:
  ```cpp
  } else if (n == "sysTcpConnect") {
      b.CreateCall(rtSysTcpConnect, {regs[in.a], arg(0), arg(1)});             // :2523-2524
  } else if (n == "sysArgs") {
      b.CreateCall(rtSysArgs, {regs[in.a]});
      retainDst();               // fresh heap Array<string> -> +1              // :2495-2497
  }
  ```
  `retainDst()` (`b.CreateCall(rtRetain, {regs[in.a]})`, `:2470`) is applied **only** to natives that
  return a fresh heap value (strings, arrays); scalar-int returns get no retain.
- **Naming convention:** `sysFoo` (camelCase) → runtime symbol `lvrt_sysfoo` (all-lowercase). e.g.
  `sysTcpConnect` → `lvrt_systcpconnect`, `sysCpuCount` → `lvrt_syscpucount`.
- **Unimplemented-native behavior.** The `CallNativeFn` chain's terminal `else` is
  `fail("native floor function '" + n + "'")` (`:2680-2682`); `fail` (`:475-478`) emits `sink.error`
  and sets `ok=false`. **So `--emit-llvm` of a program calling `std::sysSpawn` hard-fails at compile
  time with a clean diagnostic** — not a link error, not a silent miss. (This is a different guard
  from `nativeMethodCovered` at `:1966-1979`, which covers instance-method natives; the `sys*` free
  functions are gated by this `else { fail() }`.)
- **Windows gating** (`:115`, `:3305`): `bool targetWindows = Triple(t).isOSWindows();`. Linux-only
  natives guard their rows with `if (targetWindows) fail(...)`. Existing precedent for exactly the
  shape spawn would need (`:2564-2574`):
  ```cpp
  if (targetWindows) {
      fail("threads: unsupported on Windows (v1) — '" + n + "' has no Windows lowering");
  } else if (n == "sysThreadStart") { ... }
  ```

### 7.3 An end-to-end example already on LLVM: `sysTcpConnect` (fd-returning) + `sysArgs` (Array-returning)

Together these are the closest existing analogues to the spawn natives (an fd result, and an array
result). All four layers:

**Layer 1 — prelude (`Resolver.cpp`):** bodiless `std::` declaration (spawn's already at
`:1477-1482`).

**Layer 2 — LlvmGen extern decl:** `rtSysTcpConnect = fn("lvrt_systcpconnect", voidTy, {ptrTy,ptrTy,ptrTy});`
(`LlvmGen.cpp:345`); `rtSysArgs = fn("lvrt_sysargs", voidTy, {ptrTy});` (`:328`).

**Layer 3 — LlvmGen call row:** the `else if` rows shown in §7.2.

**Layer 4a — runtime C (`lv_loop.c`, socket natives live here):**
```c
void lvrt_systcpconnect(LvValue* out, const LvValue* host, const LvValue* port) {
    const char* ip = (const char*)(const void*)(intptr_t)(host->payload + 8);  // string bytes
    out->tag = LV_INT;
    out->payload = lv_plat_tcp_connect(ip, (int)port->payload);   // scalar arg via ->payload
}   // lv_loop.c:535-539
```

**Layer 4b — platform floor (`lv_plat_posix.c`), where the syscalls live:**
```c
int lv_plat_tcp_connect(const char* ip, int port) {
    ... int fd = socket(...); if (connect(fd, ...) < 0) { close(fd); return -1; }
    lv_set_nonblock_fd(fd); return fd;
}   // lv_plat_posix.c:320-330
```
Prototype in `lv_plat.h` (`:98`); a Windows-reject counterpart in `lv_plat_win32.c`.

**Array construction (`sysArgs`'s `lvrt_sysargs`, the `Array<int>` template) — `lv_runtime.c:2828-2837`:**
```c
void lvrt_sysargs(LvValue* out) {
    int64_t n = ...;
    lvrt_arr_new(out, n);                       // fresh boxed array, rc 0, length n
    for (int64_t i = 0; i < n; i++) {
        LvValue s; lvrt_str_new(&s, a, strlen(a));   // build element
        lv_st_val(out->payload, 8 + 16 * i, &s);      // store into buffer
        lvrt_retain(&s);                              // buffer owns element
    }
}
```
Supporting helpers: `lvrt_arr_new(out, len)` (`lv_runtime.c:1786-1791`) allocates a fresh boxed array;
`lv_st_val(payload, off, &v)` (`:48-51`) writes an element pair; `lvrt_arr_fill` (`:1910`) stamps
`HDR(payload)[0] = 1` for the owned/rc-1 convention. For an `Array<int>`, each element is
`{ .tag=LV_INT, .payload=fd }` (a retain on an immediate int is a no-op). **Note:** two ARC
conventions appear in the tree — `lvrt_sysargs` returns rc-0 and relies on `retainDst()` in codegen,
whereas `lvrt_arr_fill` stamps `HDR[0]=1`. Which one `sysSpawn`'s return must follow is an
implementation detail to be pinned against the `sysArgs` precedent (recorded as an open question,
§13).

Reading an `Array<string>` *argument* (spawn's `args`): follow `lvrt_systaskjoinall`
(`lv_loop.c:189-200`) — `rawlen` at base `+0`, element payload at `base + 8 + 16*i + 8`; each element
is `LV_STR`, C string at `elemPayload + 8`.

Scalars/None: int → `out->tag=LV_INT; out->payload=v;`; None → `out->tag=LV_NONE; out->payload=0;`
(`lv_runtime.c:260`, `:552`; `lv_loop.c:608`).

---

## 8. The platform layer — the OS seam, and what process support exists

### 8.1 The seam: `runtime/lv_plat.h`

"The ONLY place the runtime touches an OS" (`lv_plat.h:1-4`). `lv_runtime.c`/`lv_loop.c` never call a
raw OS primitive; everything routes through `lv_plat_*` (`lv_loop.c:10-12`). A new target = a new
`lv_plat_*.c`, nothing else (`lv_plat.h:2-6`). The interface is a flat `int fd` space
(`lv_plat_win32.c:16-29` folds console HANDLEs / CRT fds / Winsock SOCKETs into one int space).

### 8.2 Primitives that already exist (`lv_plat.h:22-142`)

Memory (`map`/`unmap`), fd I/O (`write`/`read`, `open`/`close`, `set_nonblock`), sockets
(`tcp_connect`/`tcp_connect_nb`/`connect_result`/`tcp_listen`/`accept`/`send`/`recv`/`sock_buffer`),
poll (`lv_plat_poll` + `LvPollFd`/`LV_POLLIN/OUT/NVAL`), clock (`now_ns`/`now_realtime_ms`),
misc (`cpu_count`/`random`/`exit`), terminal (`term_raw`/`restore`/`size`/`israw`), signals-as-streams
(`signal_open`/`next`/`close`), stat/fs (`stat_size`/`stat_mtime`/`stat_isdir`/`mkdir`).

### 8.3 Process / fork / exec / pipe / waitpid support: **NONE**

A grep across `runtime/` for `fork|execv|posix_spawn|waitpid|waitid|pidfd|CreateProcess|pipe2|dup2|WNOHANG`
returns **zero hits** in `lv_plat_*.c`, `lv_runtime.c`, `lv_loop.c`, `lv_task.c`, `lv_thread.c`. The
only occurrences are:
- `lv_plat_posix.c:217` — a `pipe()` inside the self-pipe **signal** fallback (not process-related).
- `runtime/selftest.c` — `fork`/`waitpid`/`pipe` in the **test harness** only (`:1051, 1073, 1222-1237`),
  to run children that check crash-reporting; not a runtime facility.

So the fork/exec/pipe/pidfd/waitid floor is **greenfield** in the runtime. There is no `lvrt_sysspawn`
/ `lvrt_sysreap` / `lvrt_syskill` / `lvrt_syspidfdopen` symbol anywhere in `runtime/`, and no matching
row in `LlvmGen.cpp`. (Grep for `sysspawn|syspipe|sysexec|sysproc` in `runtime/` is empty; the only
hits are the C++ oracle and the `.lev`-prelude signatures.)

### 8.4 POSIX vs Win32 split, and the Windows precedent

Per-file, triple-selected: `build-triple.sh:107-111` picks `lv_plat_win32.c` for `*windows*`/`*mingw*`,
else `lv_plat_posix.c`. Prebuilt per-triple archives exist under `runtime/aarch64-linux-gnu/` and
`runtime/x86_64-pc-windows-gnu/`.

There is an established **Windows-reject precedent** for exactly this kind of POSIX-only feature:
threads are POSIX-only — `lv_thread.c` is `#ifndef _WIN32` and the whole TU is empty on Windows, "the
LLVM codegen rejects the thread/channel seam natives for a Windows target at compile time"
(`lv_thread.c:10-18`); the Windows signal floor is an always-fail stub citing "the spawn/Channel
Windows-reject precedent" (`lv_plat_win32.c:227-231`; also `lv_plat.h:88-89`). `lv_plat_win32.c` is
itself "PRE-LAND, COMPILE-UNVERIFIED" (`:2-9`). A real Windows child-process story would need
`CreateProcess` + anonymous pipes + `WaitForSingleObject`/job objects — none of which exist.

### 8.5 Signals in the runtime today

- Terminal restore-on-kill: `sigaction` with `SA_RESETHAND` on `SIGTERM/SIGHUP/SIGINT/SIGQUIT`, only
  when raw mode is entered (`lv_plat_posix.c:99-111`).
- SIGSEGV crash reporter in the task substrate (`lv_task.c:414-423`).
- Signals-as-streams: Linux `signalfd` (`lv_plat_posix.c:177-195`) or self-pipe fallback
  (`:210-248`) — signals become readable fds; user code never runs in signal context.
- SIGPIPE ignored once in the TLS provider (`lv_tls_openssl.c:63`) and in the oracle's `sysSpawn`.
- **No SIGCHLD handler exists, and the design forbids one** (§2.3, §6). A pidfd registered via
  `sysWatch` replaces it — see §5.2.

---

## 9. Build / link wiring

### 9.1 The `liblvrt.a` target — `CMakeLists.txt:1179-1213`

```cmake
add_library(lvrt STATIC
  runtime/lv_runtime.c runtime/lv_plat_posix.c runtime/lv_loop.c
  runtime/lv_thread.c runtime/lv_entry.c
  ${LV_TASK_SRCS} ${LVRT_TLS_SRC})
target_include_directories(lvrt PUBLIC runtime)
target_link_libraries(lvrt PUBLIC m pthread)   # + OpenSSL when found
```

Produces host-triple `liblvrt.a`, which LLVM-compiled programs link. `runtime_selftest` links it and
runs under CTest + valgrind (`:1219-1246`).

### 9.2 The per-triple cross build — `runtime/build-triple.sh`

Builds `runtime/<triple>/liblvrt.a`. Source list (`build-triple.sh:107-111`):
```bash
case "$triple" in *windows*|*mingw*) floor=lv_plat_win32.c ;; *) floor=lv_plat_posix.c ;; esac
srcs=(lv_runtime.c "$floor" lv_loop.c lv_thread.c lv_entry.c "${task_srcs[@]}" "$tls_src")
```
`-std=gnu17 -O2`, archived with `ar rcs`; a `lvrt.link` sidecar carries extra link flags. Triples
built: `aarch64-linux-gnu` (default) and `x86_64-pc-windows-gnu`. `lv_plat_win32.c` is **not** in the
default CMake target — only selected by a Windows triple.

### 9.3 Where new runtime symbols would be picked up

Any new `.c` file added to the runtime must be added to **both** the CMake `add_library(lvrt STATIC …)`
list (`CMakeLists.txt:1180-1186`) **and** the `build-triple.sh` `srcs=(…)` array (`:111`). New
`lvrt_*` symbols must be declared in `lv_abi.h` (existing net/thread natives at `:648-743`) and their
extern in `LlvmGen.cpp` (`fn("lvrt_…")` block, `:224-251`). (Placement of the new symbols — a new
`runtime/lv_proc.c` mirroring how `lv_loop.c` owns socket natives and `lv_thread.c` owns thread
natives, versus appending to an existing file — is an open question, §13.)

---

## 10. How backends are differentially tested (existing infrastructure)

- **One golden per program.** Every engine lane diffs byte-for-byte against a single `.expected`, so
  byte-identity across oracle/IR/LLVM is enforced by all lanes sharing one golden.
  - Interpreters: `tests/run_corpus.sh <bin> --run|--ir <dir>` (`run_corpus.sh:1-18`).
  - LLVM: `tests/run_native_llvm.sh <bin> <dir> <runtime-sources…>` → `--native-obj` → `cc -O2` link
    against `${LANG_LVRT_SOURCES}` (= `lv_runtime.c` + `lv_plat_posix.c` + `lv_loop.c`, defined
    `CMakeLists.txt:819-829`) → run → diff the same `.expected` (`run_native_llvm.sh:20-38`).
  - Whole top-level corpus on LLVM: `corpus_llvm_full` (`CMakeLists.txt:998-1001`).
- **Engine-gating is directory placement, not an in-file annotation.** There is no `@skip`. A corpus
  dir gets a lane only if a matching `add_test` names it. Deferred-native dirs are deliberately
  excluded from the full LLVM scan (`CMakeLists.txt:359-362`: "its natives defer with a clean
  coverage-error on the compiled backends, so … `corpus_llvm_full` must not compile it").
- **The existing spawn test (interpreter-only):** `tests/corpus/sys_natives/sys_spawn.lev` (+`.expected`)
  — sequenced `/bin/echo` argv+onStdout, `/bin/cat` stdin-EOF, bad-path→127. Header comment: "this dir
  is interpreter-only — the compiled backends defer the spawn natives cleanly." Wired interpreter-only
  at `CMakeLists.txt:363-368` (`corpus_sys_natives_treewalk` + `_ir`; **no `_llvm` sibling**).
- **Deferral is positively asserted** in `tests/run_sysnatives.sh` (CTest `sys_natives`,
  `CMakeLists.txt:356-358`): §10 (`:271-286`) compiles a `sysSpawn` program via emit-C++ and LLVM and
  asserts each **fails cleanly** ("clean spawn deferral"); §8/§9 cover `kill→143`, 8-round fd-churn ±0
  via `/proc/self/fd`, and comptime `sysSpawn` hermeticity denial.
- **Precedent for flipping a native from "defers" to "covered":** the F5/`sysMonotonic` landing —
  "§5's LLVM sysMonotonic check flipped from 'defers' to 'covered'" (`techdesign-08-…:377`).

---

## 11. emit-C++ (CGen) — separate deferral policy

emit-C++ defers spawn too, and **by explicit policy defers the entire system layer.** Track 08 owns
"`CGen.cpp` sys stubs policy (emit-C++ skips the system layer by design — keep its clean
coverage-errors)" (`techdesign-08-…:18-20`). Mechanism: free natives are an `if/else` chain in
`CGen.cpp:1004-1053`; the terminal `else` at `:1053` is `sink_.error({}, "native backend: native '" +
in.sname + "'"); ok_ = false;`. emit-C++ uses a small inline `rt_*` preamble (e.g. `rt_syswrite`,
`rt_sysnow`), **not** the `liblvrt.a`/`lvrt_*` runtime LLVM links — a different runtime path. (Recorded
because it is easy to assume the two compiled backends share a runtime; they do not.)

---

## 12. Constraints and frozen contracts surfaced by the research

- **`lv_abi.h` is STOP-protected** (`:1-10`). Adding new `lvrt_*` declarations is compatible;
  changing an existing `LvValue*` shape, an existing native's signature/return convention, or the
  `LV_OP_*` enum (`:377-380`, frozen/append-only) is a **STOP event** (doc §0.2).
- **The F7 native signatures and return shapes are frozen** (`Resolver.cpp:1479-1482`), and the
  interpreter (`RuntimeNatives.cpp:1840-1933`) defines byte-identity. A compiled leg mirrors them; it
  does not get to redefine `[pid,in,out,err]`/`[]`, `sysReap` `-1`/code/`128+sig`, or the `pid<=0`
  refusal.
- **X64Gen / ELF is frozen and out of scope** (`techdesign-08-…:275-278`: "no new work goes on
  `X64Gen.cpp`/`X64.hpp`"). Nothing gates on ELF.
- **emit-C++'s system-layer deferral is a deliberate policy** (§11) — its clean coverage-error should
  stay green, per Track 08's own stubs policy.
- **`.lev` / runtime split house rule.** Per the Helm/project conventions, application code never
  touches `src/**` or `runtime/**`; a language gap is a STOP escalated to a language-side design. This
  spawn-on-LLVM work is itself the language-side item (G-LANG-2), so it operates in `src/`+`runtime/` —
  but under the same STOP discipline for anything touching a frozen contract above.
- **The event-loop-surgery STOP does not apply here.** Track 08 problem #2's STOP "concerned the
  frozen X64Gen-emitted loop, which the pivot removed from scope"; F5 proved `lv_loop.c` can take new
  watch registrations with a "no new watch *kinds* ⇒ bit-identical poll set" guarantee
  (`techdesign-08-…:298-307`). A pidfd read-watch is an ordinary read-watch — no new watch kind.
- **The `lvrt_register_spawn_check` machinery is unrelated** (§2.4) — it guards thread-spawn Promise
  flattening (bug #35), not child processes.

---

## 13. Open questions surfaced by the research (not decided here)

> **RESOLVED 2026-07-16** by `designs/techdesign-spawn-llvm.md` (landed): #1 → the
> `sysArgs` rc-0 + `retainDst()` convention (D1); #2 → a new `runtime/lv_proc.c` (D2);
> #3 → Linux-first with the prelude poll-reap fallback, macOS/BSD recorded as a seam,
> not code (its §6.3); #4 → the Windows-reject precedent, compile-time `fail` +
> always-fail win32 stubs (D4); #5 → a dedicated `tests/corpus/sys_spawn/` dir with
> oracle/IR/LLVM lanes over one `.expected`, and the `run_sysnatives.sh` LLVM
> assertion flipped from "defers" to "covered" (its §7).

1. **ARC convention for `sysSpawn`'s array return.** Two patterns coexist: `lvrt_sysargs` returns
   rc-0 and relies on `retainDst()` in the `LlvmGen` row; `lvrt_arr_fill` stamps `HDR(payload)[0]=1`.
   Which one the spawn return must follow needs pinning against the `sysArgs` precedent (§7.3).
2. **Placement of the new `lvrt_*` symbols** — a new `runtime/lv_proc.c` (mirroring `lv_thread.c`'s
   ownership of thread natives) versus appending to `lv_loop.c`/`lv_thread.c` (§9.3). Either requires
   the two-place build wiring (CMake + `build-triple.sh`).
3. **Non-Linux POSIX portability.** `pidfd_open` is Linux ≥ 5.3 and `pipe2`/`MSG_NOSIGNAL` are
   Linux/BSD. The oracle is Linux-first; the in-language prelude already has a 20 ms poll-reap
   fallback when `sysPidfdOpen` returns `-1` (§5.2), but `pipe2`/`SIGPIPE`/`waitid` portability for
   macOS/other Unix is unaddressed (§3.6).
4. **Windows.** Whether to follow the threads/signals Windows-reject precedent (compile-time reject +
   always-fail stubs) or attempt a real `CreateProcess` story (§8.4). No Windows child-process
   primitive exists today.
5. **Test-lane placement.** The interpreter-only `sys_spawn.lev` currently lives in a dir excluded
   from `corpus_llvm_full`; enabling an LLVM lane implies either a dedicated dir or splitting it out,
   plus flipping the two "clean spawn deferral" assertions in `run_sysnatives.sh:271-286` to "covered"
   (§10). The exact restructuring is undecided.

---

## 14. File index (load-bearing sources for a future design)

| Area | Files |
|------|-------|
| Interpreter oracle (byte-exact spec) | `src/RuntimeNatives.cpp:1840-1933` (spawn quartet), `:1547-1642` (send/recv/watch) |
| Language-level signatures + `Process`/`TcpStream`/`Channel` prelude | `src/Resolver.cpp:1477-1482`, `:1489-1598`, `:1808-1880`, `:1338-1393` |
| Interpreter dispatch | `src/Eval.cpp:759`, `src/IrInterp.cpp:279`; `src/RuntimeLoop.{hpp,cpp}` |
| LLVM codegen (native decls + dispatch + gating) | `src/LlvmGen.cpp:149-191`, `:224-404`, `:2457-2684`, `:2680-2682` (fail), `:115`/`:2564-2574`/`:3305` (Windows) |
| ABI contract | `runtime/lv_abi.h` (`:36` LvValue, `:38-40` tags, `:109` array, `:648-743` sys decls, `:1-10` STOP) |
| C runtime | `runtime/lv_runtime.c` (`:48-51`, `:1786-1791`, `:2828-2837` value/array build), `runtime/lv_loop.c` (`:279-344` watches, `:321-327` cancel_fd, `:366-528` step, `:535-539` socket native), `runtime/lv_thread.c:199-279` (eventfd-join reap analogue) |
| Platform seam | `runtime/lv_plat.h`, `runtime/lv_plat_posix.c` (`:320-330`, `:406-417` ENOTSOCK fallback, `:447-474` poll), `runtime/lv_plat_win32.c:227-231` (reject precedent) |
| emit-C++ (separate deferral) | `src/CGen.cpp:1004-1053` |
| Build/link | `CMakeLists.txt:356-368`, `:819-829`, `:998-1001`, `:1179-1213`; `runtime/build-triple.sh:107-134` |
| Tests | `tests/corpus/sys_natives/sys_spawn.lev`, `tests/run_sysnatives.sh:271-286`, `tests/run_native_llvm.sh`, `tests/run_corpus.sh` |
| Design record | `designs/complete/techdesign-08-system-natives.md` §7/§9/§12/§13; `docs/reference.md:1490-1516` (§6.6.59), `:1651-1661` (§6.6.66); `designs/helm/techdesign-00-overview.md:585-591` (G-LANG-2 note) |
