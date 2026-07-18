# Tech Design — Process Spawn on the LLVM Backend (G-LANG-2, process half)

**Status:** design, ready for implementation. **Date:** 2026-07-16.
**Scope:** land the four process natives (`sysSpawn`, `sysPidfdOpen`, `sysReap`, `sysKill`) on the
LLVM (`--build-native`) backend, closing the process half of gate **G-LANG-2**. The PTY floor
(the terminal half of G-LANG-2) is explicitly out of scope. X64Gen/ELF is frozen and out of scope.
emit-C++ keeps its deliberate system-layer deferral (unchanged).
**Ground truth:** `spawn.md` (root — the research doc this design is built from; every `file:line`
below is verified there), `designs/complete/techdesign-08-system-natives.md` (the frozen F7
contract), `src/RuntimeNatives.cpp:1840-1933` (the byte-exact interpreter oracle).

---

## 0. What and why

`std::sysSpawn`/`Process` run today on oracle + IR only; LLVM hard-fails at compile time with
`native floor function 'sysSpawn'` (`src/LlvmGen.cpp:2680-2682`). The research (`spawn.md` §1)
established that the deferral is narrow: the in-language `Process`/`TcpStream` layer, the event
loop, the fd watches, and the send/recv path with the pipe (`ENOTSOCK`) fallback **all already ship
on LLVM**. The gap is exactly four native functions and their wiring.

This design specifies that wiring in full: a new platform floor (`lv_plat_*`), a new runtime
translation unit (`runtime/lv_proc.c`), four additive `lv_abi.h` declarations, four `LlvmGen.cpp`
rows with Windows gating, build wiring, and the test flips. **No architectural changes.** Every
semantic decision is inherited from the frozen F7 contract; where the tree offered two conventions
(ARC on the array return, symbol placement), this design picks one and records why.

Consumer forcing the landing (the `sysNow` per-consumer precedent,
`techdesign-08-system-natives.md:273-274`): **Helm**. Its build/run/test/diagnostics features are
interpreter-only solely because of this gap (`designs/helm/techdesign-00-overview.md:585-591`).

**Non-goals:** PTY floor; Windows child processes (rejected at compile time, §6); PATH search; cwd/
env parameters on `sysSpawn` (the floor signature is frozen — callers fold cwd via
`/usr/bin/env -C`, per the Helm G-H0 log); emit-C++ coverage; any change to the four natives'
semantics; macOS/BSD portability beyond what the shared POSIX source already gives (§6.3).

---

## 1. Frozen contract (restated, binding)

From `src/Resolver.cpp:1477-1482` and the oracle (`src/RuntimeNatives.cpp:1850-1933`). The LLVM leg
must be **byte-identical** to the oracle — the differential suite enforces it (§7).

```
Array<int> sysSpawn(string path, Array<string> args);
    // [pid, stdinFd, stdoutFd, stderrFd] on success; [] on SPAWN failure (pipe/fork only).
    // Empty path -> []. exec failure is NOT spawn failure: child _exit(127)s; 127 via sysReap.
    // No PATH search. Pipes O_CLOEXEC; parent ends O_NONBLOCK. SIGPIPE ignored once at first spawn.
int sysPidfdOpen(int pid);   // pidfd_open(pid, 0); -1 on failure or pid <= 0
int sysReap(int pid);        // waitid(P_PID, WEXITED|WNOHANG): -1 running/not-ours;
                             // 0..255 = si_status & 0xFF; 128+sig if signal-terminated. Never blocks.
int sysKill(int pid, int sig); // kill(pid,sig) -> 0/-1; pid <= 0 or sig < 0 refused at the floor
                               // (kill(2) group/broadcast forms never exposed)
```

Invariants the implementation must not lose (spawn.md §2.3): argv built **before** fork; between
fork and exec the child only `dup2`s and `execve`s and a failed exec `_exit(127)`s — nothing
allocates, nothing else runs; no SIGCHLD handler ever (pidfd + `WNOHANG` reaping); `SIGPIPE`
`SIG_IGN` is a disposition, not a handler, and is permitted.

Everything above the floor is untouched: `Process`, `TcpStream`, the pidfd-watch/20ms-poll reap,
`pumpAll`-before-close, and the fd-table-±0 guarantee are prelude code (`Resolver.cpp:1808-1880`)
that already compiles on LLVM and will work the moment the floor exists.

---

## 2. Design decisions (the choices spawn.md §13 left open)

**D1 — ARC convention for `sysSpawn`'s return: the `sysArgs` precedent, verbatim.**
`lvrt_sysspawn` builds the array with `lvrt_arr_new` + `lv_st_val` and returns it at **rc 0**; the
`LlvmGen` row calls `retainDst()` (+1 transfer), exactly like the `sysArgs` pair
(`lv_runtime.c:2828-2837` / `LlvmGen.cpp:2495-2497`). We do **not** stamp `HDR(payload)[0] = 1` —
that is `lvrt_arr_fill`'s transfer convention on a different call path (`lv_runtime.c:1910`), and
mixing the two on a `retainDst()` row would leak. Elements are immediate `LV_INT`s (pids/fds carry
no heap ref), so no per-element retain. Verified at landing by the memcheck lane (§7.4).

**D2 — Symbol placement: a new `runtime/lv_proc.c`.**
Mirrors the existing ownership pattern — `lv_loop.c` owns socket natives, `lv_thread.c` owns thread
natives, `lv_proc.c` owns process natives. Keeps the fork/exec discipline reviewable in one small
TU instead of growing `lv_loop.c`. Costs one entry in each of the two build lists (§5).

**D3 — Platform seam split.**
Raw syscalls live only in `lv_plat_posix.c` (house rule: `lv_plat.h` is "the ONLY place the runtime
touches an OS", `lv_plat.h:1-4`). `lv_proc.c` does LvValue marshaling and calls four new
`lv_plat_*` functions. `lv_plat_win32.c` gets always-fail stubs so the archive always links.

**D4 — Windows: the reject precedent.**
Compile-time reject in `LlvmGen` via the existing `targetWindows` guard, message shape matching the
threads row (`LlvmGen.cpp:2564-2574`), plus the always-fail win32 stubs (defense in depth — same
double gate threads use, `lv_thread.c:10-18` + `lv_plat_win32.c:227-231`). A real `CreateProcess`
story is future work, not v1.

**D5 — Linux-first, matching the oracle syscall-for-syscall.**
`pipe2(O_CLOEXEC)`, `fork`, `execve`, `syscall(SYS_pidfd_open)`, `waitid(WNOHANG)` — identical
calls to `RuntimeNatives.cpp` so behavior can't diverge. The one non-Linux accommodation we keep is
the one that already exists in the prelude: when `sysPidfdOpen` returns `-1`, `Process.exitCode()`
falls back to the 20 ms poll-reap timer (`Resolver.cpp:1852-1858`) — so a kernel without
`pidfd_open` degrades gracefully with zero new code. Broader macOS/BSD portability (`pipe2`,
`MSG_NOSIGNAL`) stays an open item recorded in §9, not silently half-done here.

**D6 — No new loop machinery, no new watch kinds.**
The pidfd is watched by the *prelude* via the existing `sysWatch` (`lv_loop.c:329-344`); the reap
teardown is prelude `sysUnwatch` + `sysClose`. The F5 ruling applies: new watch *registrations* are
in-bounds, new watch *kinds* would be a STOP — we add none (`techdesign-08-…:298-307`).

**D7 — Fork-in-a-threaded-runtime discipline (new hazard LLVM adds over the interpreters).**
LLVM programs may hold real pthreads (Track 10 workers) and the task substrate. `fork()` in a
multithreaded process clones only the calling thread; the child may only run async-signal-safe
code. Our child body is `dup2 ×3` + `execve` + `_exit(127)` — all async-signal-safe, and the argv
vector is fully built pre-fork. `pipe2`'s **atomic** `O_CLOEXEC` also matters here: a concurrent
`fork` on another thread can never inherit a half-flagged pipe end. Both properties are already the
oracle's discipline; this design makes them load-bearing requirements, not style.

---

## 3. The platform floor — `runtime/lv_plat.h` + `lv_plat_posix.c` + `lv_plat_win32.c`

### 3.1 `lv_plat.h` — four additive declarations (beside the socket block, ~`:98`)

```c
/* --- process floor (G-LANG-2, techdesign-spawn-llvm.md) --------------------
 * lv_plat_spawn: argv is NUL-terminated (argv[0] = path, no PATH search).
 *   On success returns pid > 0 and writes the parent pipe ends to fds[0..2]
 *   (stdin-write, stdout-read, stderr-read), all O_CLOEXEC + O_NONBLOCK.
 *   Returns -1 on pipe/fork failure (exec failure is the child's 127 instead).
 * lv_plat_pidfd_open: pollable fd, read-ready when pid exits; -1 unavailable.
 * lv_plat_reap: -1 still running / not ours; 0..255 exited; 128+sig signaled.
 *   Never blocks.
 * lv_plat_kill: 0/-1. Callers refuse pid <= 0 above this line; the floor
 *   refuses it again (broadcast forms never reach kill(2)).                  */
int lv_plat_spawn(const char* path, char* const argv[], int fds[3]);
int lv_plat_pidfd_open(int pid);
int lv_plat_reap(int pid);
int lv_plat_kill(int pid, int sig);
```

### 3.2 `lv_plat_posix.c` — the implementation (a transcription of the oracle)

```c
/* process floor — mirrors src/RuntimeNatives.cpp:1850-1933 syscall-for-syscall.
 * Child discipline (D7): argv is built by the caller BEFORE fork; between
 * fork and exec the child only dup2s and execs; a failed exec _exit(127)s.
 * Async-signal-safe only — LLVM programs are multithreaded (lv_thread.c). */
extern char **environ;

int lv_plat_spawn(const char* path, char* const argv[], int fds[3]) {
    static int sigpipe_ignored = 0;               /* disposition, not a handler */
    if (!sigpipe_ignored) { signal(SIGPIPE, SIG_IGN); sigpipe_ignored = 1; }
    int inP[2], outP[2], errP[2];
    if (pipe2(inP,  O_CLOEXEC) != 0) return -1;
    if (pipe2(outP, O_CLOEXEC) != 0) { close(inP[0]); close(inP[1]); return -1; }
    if (pipe2(errP, O_CLOEXEC) != 0) { close(inP[0]); close(inP[1]);
                                       close(outP[0]); close(outP[1]); return -1; }
    pid_t pid = fork();
    if (pid < 0) { /* close all six */ ... return -1; }
    if (pid == 0) {                                /* child: nothing allocates */
        dup2(inP[0], 0); dup2(outP[1], 1); dup2(errP[1], 2);
        execve(path, argv, environ);
        _exit(127);
    }
    close(inP[0]); close(outP[1]); close(errP[1]); /* parent drops child ends */
    fds[0] = inP[1]; fds[1] = outP[0]; fds[2] = errP[0];
    for (int i = 0; i < 3; i++) {
        int fl = fcntl(fds[i], F_GETFL, 0);
        fcntl(fds[i], F_SETFL, fl | O_NONBLOCK);
    }
    return (int)pid;
}

int lv_plat_pidfd_open(int pid) {
    if (pid <= 0) return -1;
    return (int)syscall(SYS_pidfd_open, (pid_t)pid, 0);   /* Linux >= 5.3; -1 else */
}

int lv_plat_reap(int pid) {
    if (pid <= 0) return -1;
    siginfo_t si; si.si_pid = 0;                   /* the WNOHANG zero-out idiom */
    if (waitid(P_PID, (id_t)pid, &si, WEXITED | WNOHANG) != 0 || si.si_pid == 0)
        return -1;
    return si.si_code == CLD_EXITED ? (si.si_status & 0xFF) : 128 + si.si_status;
}

int lv_plat_kill(int pid, int sig) {
    if (pid <= 0 || sig < 0) return -1;
    return kill((pid_t)pid, sig) == 0 ? 0 : -1;
}
```

Notes: SIGPIPE-once duplicates the TLS provider's ignore (`lv_tls_openssl.c:63`) — idempotent,
harmless. `sigpipe_ignored` is a plain static (not `LV_TLS`): first-spawn-wins is fine, `SIG_IGN`
is process-global anyway.

### 3.3 `lv_plat_win32.c` — always-fail stubs (the signal-floor precedent, `:227-231`)

```c
/* process floor: no Windows child-process story in v1 — the spawn/Channel
 * Windows-reject precedent. LlvmGen rejects sysSpawn* for a Windows target at
 * compile time; these stubs keep the archive linking and return the frozen
 * failure sentinels if ever reached. */
int lv_plat_spawn(const char* path, char* const argv[], int fds[3]) {
    (void)path; (void)argv; (void)fds; return -1;
}
int lv_plat_pidfd_open(int pid) { (void)pid; return -1; }
int lv_plat_reap(int pid)       { (void)pid; return -1; }
int lv_plat_kill(int pid, int sig) { (void)pid; (void)sig; return -1; }
```

---

## 4. The runtime natives — new `runtime/lv_proc.c` + `lv_abi.h` declarations

### 4.1 `lv_abi.h` — four additive declarations (beside the net/thread natives, `:648-743`)

Additive only — no existing shape, no `LV_OP_*` touch, so **no STOP** (`lv_abi.h:1-10` permits
adding functions). Contract comment follows the house style of the `:589-614` block:

```c
/* Process floor (techdesign-spawn-llvm.md; oracle RuntimeNatives.cpp:1841+).
 * lvrt_sysspawn: fresh Array<int> [pid, stdinFd, stdoutFd, stderrFd], or the
 *   empty array on SPAWN failure (pipe/fork; empty path). Exec failure is the
 *   child's _exit(127), collected via lvrt_sysreap. Returned array follows the
 *   lvrt_sysargs ownership convention (rc 0; codegen retains).
 * lvrt_syspidfdopen / lvrt_sysreap / lvrt_syskill: scalar LV_INT results,
 *   values per the frozen F7 contract (Resolver.cpp:1479-1482).              */
void lvrt_sysspawn(LvValue* out, const LvValue* path, const LvValue* args);
void lvrt_syspidfdopen(LvValue* out, const LvValue* pid);
void lvrt_sysreap(LvValue* out, const LvValue* pid);
void lvrt_syskill(LvValue* out, const LvValue* pid, const LvValue* sig);
```

### 4.2 `runtime/lv_proc.c` — LvValue marshaling over the plat floor

Marshaling facts used (all from `lv_abi.h`, verified in spawn.md §7.1/§7.3): string bytes are
NUL-terminated at `payload + 8` (so argv can point **directly** into the LvValue strings — no
copying); boxed-array length at `payload + 0`, element `i`'s payload at `payload + 8 + 16*i + 8`
(the `lvrt_systaskjoinall` read idiom, `lv_loop.c:189-200`); `lvrt_arr_new` + `lv_st_val` build the
result (the `lvrt_sysargs` pattern, `lv_runtime.c:2828-2837`).

```c
/* lv_proc.c — process natives (techdesign-spawn-llvm.md §4).
 * Thin marshaling over lv_plat_spawn/pidfd_open/reap/kill; ALL policy lives in
 * the plat floor + the prelude. Mirrors RuntimeNatives.cpp:1841-1933.        */
#include "lv_abi.h"
#include "lv_plat.h"
#include <stdlib.h>

void lvrt_sysspawn(LvValue* out, const LvValue* path, const LvValue* args) {
    /* empty result = spawn failure (frozen: [] not None) */
    const char* cpath = (const char*)(intptr_t)(path->payload + 8);
    int64_t plen = *(const int64_t*)(intptr_t)path->payload;
    if (plen <= 0) { lvrt_arr_new(out, 0); return; }

    /* argv built BEFORE fork (D7): [path, args..., NULL]; element bytes are
     * NUL-terminated in the string rep, so we alias, never copy. */
    int64_t n = (args->tag == LV_ARR) ? *(const int64_t*)(intptr_t)args->payload : 0;
    char** argv = (char**)malloc((size_t)(n + 2) * sizeof(char*));
    if (!argv) { lvrt_arr_new(out, 0); return; }
    argv[0] = (char*)cpath;
    for (int64_t i = 0; i < n; i++) {
        int64_t sp = *(const int64_t*)(intptr_t)(args->payload + 8 + 16 * i + 8);
        argv[i + 1] = (char*)(intptr_t)(sp + 8);
    }
    argv[n + 1] = NULL;

    int fds[3];
    int pid = lv_plat_spawn(cpath, argv, fds);
    free(argv);
    if (pid <= 0) { lvrt_arr_new(out, 0); return; }

    lvrt_arr_new(out, 4);                          /* rc 0; codegen retainDst()s (D1) */
    LvValue e; e.tag = LV_INT;
    e.payload = pid;    lv_st_val(out->payload, 8 + 16*0, &e);
    e.payload = fds[0]; lv_st_val(out->payload, 8 + 16*1, &e);
    e.payload = fds[1]; lv_st_val(out->payload, 8 + 16*2, &e);
    e.payload = fds[2]; lv_st_val(out->payload, 8 + 16*3, &e);
}

void lvrt_syspidfdopen(LvValue* out, const LvValue* pid) {
    out->tag = LV_INT; out->payload = lv_plat_pidfd_open((int)pid->payload);
}
void lvrt_sysreap(LvValue* out, const LvValue* pid) {
    out->tag = LV_INT; out->payload = lv_plat_reap((int)pid->payload);
}
void lvrt_syskill(LvValue* out, const LvValue* pid, const LvValue* sig) {
    out->tag = LV_INT; out->payload = lv_plat_kill((int)pid->payload, (int)sig->payload);
}
```

Implementation notes:
- `lv_st_val` is file-static in `lv_runtime.c` (`:48-51`); if not already exported, either expose it
  through the runtime-internal header the other TUs share, or write the two words inline (it is a
  16-byte store). Resolve at implementation time; do **not** duplicate a divergent copy.
- Oracle parity check on the empty-path guard: the oracle tests `path.empty()`; here `plen <= 0` on
  the length word — identical observable behavior.
- The argv `malloc` is parent-side, pre-fork, freed post-fork in the parent — the child never sees
  allocator state (D7). Spawn failure after allocation still frees.
- No pidfd/watch code here — reaping is entirely prelude-driven through the existing loop (D6).

---

## 5. Codegen — `src/LlvmGen.cpp` (four members, four decls, four rows)

**Members** (append to the `FunctionCallee` list near the other `rtSys*`, `:149-191`):
`rtSysSpawn, rtSysPidfdOpen, rtSysReap, rtSysKill`.

**Ctor decls** (in the §2.7 block, beside `rtSysTcpConnect` at `:345`; boundary rule — out-param
first, everything `ptrTy`):

```cpp
// Process floor (techdesign-spawn-llvm.md §5): POSIX-only, Windows-rejected below.
rtSysSpawn     = fn("lvrt_sysspawn",     voidTy, {ptrTy, ptrTy, ptrTy});
rtSysPidfdOpen = fn("lvrt_syspidfdopen", voidTy, {ptrTy, ptrTy});
rtSysReap      = fn("lvrt_sysreap",      voidTy, {ptrTy, ptrTy});
rtSysKill      = fn("lvrt_syskill",      voidTy, {ptrTy, ptrTy, ptrTy});
```

**`CallNativeFn` rows** (in the chain at `:2457-2684`, before the terminal
`fail("native floor function …")` at `:2680`; Windows guard shaped exactly like the threads row at
`:2564-2574`):

```cpp
} else if (n == "sysSpawn" || n == "sysPidfdOpen" || n == "sysReap" || n == "sysKill") {
    if (targetWindows) {
        fail("process spawn: unsupported on Windows (v1) — '" + n +
             "' has no Windows lowering (techdesign-spawn-llvm.md)");
    } else if (n == "sysSpawn") {
        b.CreateCall(rtSysSpawn, {regs[in.a], arg(0), arg(1)});
        retainDst();                       // fresh heap Array<int> -> +1 (D1, sysArgs parity)
    } else if (n == "sysPidfdOpen") {
        b.CreateCall(rtSysPidfdOpen, {regs[in.a], arg(0)});
    } else if (n == "sysReap") {
        b.CreateCall(rtSysReap, {regs[in.a], arg(0)});
    } else {  // sysKill
        b.CreateCall(rtSysKill, {regs[in.a], arg(0), arg(1)});
    }
}
```

Scalar-returning rows get **no** retain (int is immediate). The blanket `emitThrowCheck` after the
chain (`:2683`) covers these rows like every other; none of the four raises today (they return
sentinels), matching the oracle.

**Not applicable, stated to preempt confusion:** the `nativeMethodCovered`/`kCovered[]` guard
(`:1966-1979`) is the *instance-method* coverage path; `sys*` free functions are gated only by the
`CallNativeFn` chain. No `kCovered` change. No `Op` changes, no `Lower.cpp` changes, no
`Resolver`/`Checker`/`Eval`/`IrInterp` changes anywhere in this design — the front-end and
interpreters are already complete.

---

## 6. Build wiring + platform gating summary

### 6.1 Build lists (both, or the cross archives silently diverge — spawn.md §9.3)

- `CMakeLists.txt:1180-1186`: add `runtime/lv_proc.c` to `add_library(lvrt STATIC …)`.
- `runtime/build-triple.sh:111`: add `lv_proc.c` to `srcs=(…)`.
- Rebuild the prebuilt per-triple archives (`runtime/aarch64-linux-gnu/liblvrt.a`,
  `runtime/x86_64-pc-windows-gnu/liblvrt.a`) via `build-triple.sh` as the final landing step, same
  as every prior runtime change.
- `${LANG_LVRT_SOURCES}` (`CMakeLists.txt:819-829`) — the source list `run_native_llvm.sh` links
  test binaries against — must also gain `runtime/lv_proc.c`.

### 6.2 Gating matrix (who rejects what)

| Target | Gate | Behavior |
|---|---|---|
| LLVM / Linux | — | full support |
| LLVM / Windows triple | `targetWindows` guard (§5) | compile-time `fail(...)`, threads-precedent message |
| LLVM / Windows, guard bypassed | `lv_plat_win32.c` stubs (§3.3) | `[]` / `-1` sentinels; never crashes |
| emit-C++ (any) | `CGen.cpp:1053` terminal else | unchanged clean coverage-error (policy, §0) |
| comptime | `sys*` auto-deny (`reference.md:1515-1516`) | unchanged |
| oracle / IR | `RuntimeNatives.cpp` | unchanged (the reference) |

### 6.3 Non-Linux POSIX (recorded, not landed)

`pipe2` is Linux/BSD; `SYS_pidfd_open` is Linux ≥ 5.3. On a hypothetical macOS triple the file
would need `pipe`+`fcntl(FD_CLOEXEC)` and would rely on the prelude's poll-reap fallback. No such
triple is built today (`build-triple.sh` builds linux-gnu + windows-gnu only), so this stays a
documented seam, not code. If a macOS triple is ever added, that is its own small design.

---

## 7. Testing

### 7.1 Differential corpus — promote the spawn golden to LLVM

The existing golden (`tests/corpus/sys_natives/sys_spawn.lev` + `.expected` — `/bin/echo`
argv+onStdout, `/bin/cat` stdin-EOF round trip, bad-path→127) is already deterministic and
sequenced. Plan:

1. **Move** `sys_spawn.lev`/`sys_spawn.expected` to a new dir `tests/corpus/sys_spawn/`, and drop
   the "this dir is interpreter-only" header. `sys_natives/` keeps its interpreter-only lanes for
   whatever else remains deferred there.
2. **Add three lanes** in `CMakeLists.txt` beside the existing pattern (`:363-368` /
   `corpus_blocks_io_llvm` at `:1024` as the model): `corpus_sys_spawn_treewalk`,
   `corpus_sys_spawn_ir` (via `run_corpus.sh`), and `corpus_sys_spawn_llvm` (via
   `run_native_llvm.sh` + `${LANG_LVRT_SOURCES}`). All three diff the **same** `.expected` —
   byte-identity across oracle/IR/LLVM is the pass condition (the project doctrine).
3. **Keep `sys_spawn/` out of `corpus_llvm_full`'s exclusion list** (it is now LLVM-clean); confirm
   the full scan picks it up or the dedicated lane covers it — one of the two must compile it.

### 7.2 Flip the deferral assertions — `tests/run_sysnatives.sh:271-286`

The F5/`sysMonotonic` precedent ("flipped from 'defers' to 'covered'", `techdesign-08-…:377`):

- **LLVM leg:** replace the "clean spawn deferral" assertion with a covered assertion — compile the
  spawn program with `--native-obj`, link, run, diff expected output.
- **emit-C++ leg:** **keep** the deferral assertion exactly as is (policy, §0/§6.2) — it must keep
  failing cleanly with `native backend: native 'sysSpawn'`.

### 7.3 Behavior tests that must now pass on the LLVM binary

Extend the existing §8/§9 checks in `run_sysnatives.sh` to run against the LLVM-built binary too:

- `kill() → 143`: spawn a sleeper, `kill()`, `exitCode()` resolves `128+15`.
- **fd-churn ±0**: 8 spawn/reap rounds, then `/proc/self/fd` count unchanged — now also proves the
  compiled runtime leaks no pipe/pidfd.
- bad-path → `ok()` true-shaped spawn, `exitCode()` → `127` (exec-failure convention).
- stdin round trip through `/bin/cat` with `closeStdin()` → EOF → `onStdout` delivery →
  `pumpAll`-before-close ordering (already exercised by the golden).
- comptime hermeticity denial: unchanged, still asserted.

### 7.4 Runtime selftest + memcheck

`runtime/selftest.c` already forks children for its crash-report tests, and `runtime_selftest` runs
under CTest **valgrind** (`CMakeLists.txt:1219-1246`). Add one C-level case: `lv_plat_spawn` of
`/bin/echo`, drain stdout via `lv_plat_read` to EOF, `lv_plat_reap` until `>= 0`, assert exit 0 and
fd hygiene. This pins the plat floor independently of codegen, and the valgrind lane settles D1's
ARC question empirically (no leak, no double-free on the returned array — exercised via the corpus
lane as well).

### 7.5 Pidfd-fallback lane (best-effort)

The 20 ms poll-reap fallback path (`sysPidfdOpen` → `-1`) can't be forced on a pidfd-capable
kernel from language code. Cover it at the C level in selftest: call `lv_plat_reap` polling loop
directly after a spawn without opening a pidfd. The prelude fallback branch itself is already
exercised on any pre-5.3 kernel; we do not build a fake-negative hook for it (would touch the
frozen floor surface for a test-only concern).

---

## 8. Documentation updates (part of the landing, not optional)

1. `docs/reference.md:1515-1516` (§6.6.59): "Interpreters only (oracle + IR); compiled backends
   defer cleanly" → LLVM now covered; emit-C++ still defers; Windows targets reject at compile time.
2. `designs/complete/techdesign-08-system-natives.md` §13: append the landed log line (the
   sysMonotonic-style entry) recording F7's LLVM promotion and pointing here.
3. `designs/helm/techdesign-00-overview.md` §14: append — the G-LANG-2 **process half** is green on
   LLVM; Helm's golden lane can add LLVM for proc-bridge tests; the PTY half remains open.
4. `info.md` backend-coverage ladder (`:1970-1983`): add process spawn to the LLVM (A-M6) coverage
   list.
5. `spawn.md` (root): mark §13 items 1/2/4/5 as decided by this design (D1/D2/D4/§7), item 3
   recorded at §6.3 here.

---

## 9. Risks & mitigations

| # | Risk | Mitigation |
|---|------|-----------|
| R1 | fork() in a multithreaded LLVM process (workers/tasks live) misbehaves | child body is async-signal-safe only (dup2/execve/_exit), argv pre-built, atomic O_CLOEXEC (D7); selftest spawns from a threaded context |
| R2 | ARC mismatch on the returned array (leak or double-free) | sysArgs-parity convention (D1), settled empirically by the valgrind selftest + memcheck lane (§7.4) |
| R3 | Behavioral drift from the oracle breaks byte-identity | plat floor is a syscall-for-syscall transcription (§3.2); one shared `.expected` across all three lanes (§7.1) |
| R4 | Cross-triple archives silently missing `lv_proc.c` | both build lists updated in one commit + archive rebuild is an explicit landing step (§6.1) |
| R5 | Stale-fd races (pidfd/pipe number reuse) | unchanged prelude discipline: `sysUnwatch` before `sysClose`, `fd = -1` after; fd-churn ±0 test now runs on LLVM (§7.3) |
| R6 | Windows triple regression | double gate: compile-time reject + link-safe stubs (§6.2) |
| R7 | `lv_st_val` visibility across TUs | resolve via shared internal header or inline 16-byte store; no divergent copy (§4.2 note) |

**STOP conditions (unchanged from house rules):** any change to an existing `lv_abi.h` shape or
native signature; any new watch *kind* in the loop; any SIGCHLD handler; any X64Gen/ELF work; any
emit-C++ system-layer implementation. None are needed by this design; hitting one during
implementation means stop and escalate, not improvise.

---

## 10. Landing order & gates

| gate | contents | proves |
|------|----------|--------|
| **G-P0** | §3 plat floor + §4 `lv_proc.c` + §6.1 build wiring + §7.4 selftest case | the floor works, valgrind-clean, no codegen yet |
| **G-P1** | §5 LlvmGen rows + §7.1 corpus lanes green (oracle = IR = LLVM byte-identical) | end-to-end spawn on LLVM |
| **G-P2** | §7.2 assertion flips + §7.3 behavior tests on the LLVM binary + §6.1 triple-archive rebuild | no regression anywhere, deferral story updated |
| **G-P3** | §8 doc updates; Helm proc-bridge golden lane extended to LLVM (`examples/helm/tests`) | the consumer that forced the landing actually runs on it |

Estimated shape: G-P0/G-P1 are the work; G-P2/G-P3 are mechanical. Single track, disjoint file
ownership throughout (new files + additive rows only — no file this touches is owned by another
live track).

---

## 11. Implementation log (append-only)

- 2026-07-16 — design created from `spawn.md` (root research doc). Decisions D1–D7 recorded;
  open questions from spawn.md §13 resolved as: ARC = sysArgs parity (D1), placement =
  `lv_proc.c` (D2), portability = Linux-first with prelude fallback + §6.3 seam note (D5),
  Windows = reject precedent (D4), tests = dedicated `sys_spawn/` corpus dir + assertion flips
  (§7). No frozen contract touched; no STOP anticipated.
- 2026-07-16 — **LANDED IN FULL, G-P0–G-P3 same day.** As specified, no deviations: §3 plat
  floor (`lv_plat.h` decls, `lv_plat_posix.c` transcription, `lv_plat_win32.c` stubs — plus
  `_GNU_SOURCE` at the top of the posix TU, which `pipe2`/`SYS_pidfd_open` are gated on in C,
  and an `#ifdef SYS_pidfd_open` header guard degrading to the D5 poll-reap), §4 `lv_proc.c`
  (R7 resolved as the sanctioned inline 16-byte store — a local `lv_proc_st_val`), §5 LlvmGen
  rows + Windows reject, §6.1 build wiring (CMake `lvrt` + `LANG_LVRT_SOURCES` +
  `build-triple.sh`; both cross archives rebuilt), §7.1 corpus promotion (golden moved to
  `tests/corpus/sys_spawn/`, three lanes, byte-identical oracle=IR=LLVM on first run), §7.2
  flip (LLVM covered, emit-C++ deferral kept green), §7.3 LLVM behavior legs (kill→143;
  fd-churn ±0 probed via `sysOpen`'s lowest-available fd — `sysListDir` isn't on the LLVM
  floor, a per-backend probe substitution, not a semantic change), §7.4/§7.5 selftest
  (plat-floor spawn/drain/reap/kill + pidfd-less reap poll + the D1 marshaling
  retain/release + empty-path `[]`; fd hygiene as exact fd-triple reuse). Full
  `run_sysnatives.sh` green. §8 docs updated.
