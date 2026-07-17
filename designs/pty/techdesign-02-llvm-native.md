# Tech Design 02 — PTY Stage S2: Platform Floor + LLVM Landing (gate G-PTY2)

**Status:** design, ready for implementation after G-PTY1. **Parent:** `techdesign-00-overview.md`.
**Scope:** the compiled lanes — `lv_plat_pty_*` in the platform floor, the `runtime/lv_pty.c`
natives TU, the `lv_plat_recv` EIO collapse, **HARD** `lv_abi.h`/`LlvmGen.cpp` wiring, build
lists, selftest, and the promotion of `tests/corpus/sys_pty/` to three-lane byte-identity.
Closing this gate makes the **G-LANG-2 terminal half GREEN on POSIX**.
**Template:** this doc deliberately mirrors `designs/techdesign-spawn-llvm.md` §3–§7 — same
conventions, same landing order, same gates-within-the-stage. Where that design said "spawn",
read "pty"; deviations are called out explicitly.

emit-C++ is untouched (deliberate system-layer deferral — the `CGen.cpp:1053` terminal else keeps
rejecting cleanly). X64Gen/ELF frozen, untouched.

---

## 1. Platform floor — `runtime/lv_plat.h` + `lv_plat_posix.c` + `lv_plat_win32.c`

### 1.1 `lv_plat.h` — two additive declarations (beside the process floor block, `:124-137`)

The exact block from overview §1.3, verbatim — additive only, no existing shape touched (no STOP).

### 1.2 `lv_plat_posix.c` — the implementation (beside the process floor, after `:515`)

A transcription of the oracle's §1.1/§1.2 blocks (doc 01), same discipline as `lv_plat_spawn`'s
transcription of F7 — syscall-for-syscall so the lanes cannot diverge. Full shape:

```c
/* --- pty floor (G-LANG-2 terminal half, designs/pty/ 02 §1.2) --------------
 * Mirrors RuntimeNatives.cpp sysPtySpawn/sysPtyResize call-for-call. D-P2:
 * ALL non-async-signal-safe work (openpt/grant/unlock/ptsname_r/open/
 * tcsetattr/TIOCSWINSZ) is parent-side, pre-fork; the child body is setsid/
 * TIOCSCTTY/dup2/close/execve/_exit only — LLVM programs are multithreaded.
 * The frozen termios profiles (D-P3) live HERE and in the oracle, nowhere
 * else; they must stay bit-identical (goldens enforce it). */

static void lv_pty_termios(struct termios* tio, int flags) {
    memset(tio, 0, sizeof *tio);
    tio->c_iflag = ICRNL | IXON;
    tio->c_oflag = OPOST | ONLCR;
    tio->c_cflag = CS8 | CREAD;
    tio->c_lflag = ISIG | ICANON | IEXTEN;
    if ((flags & 1) == 0) tio->c_lflag |= ECHO | ECHOE | ECHOK;  /* DEFAULT */
    /* DETERMINISTIC (bit0): whole echo family off — the one interleaving source */
    tio->c_cc[VMIN] = 1;  tio->c_cc[VTIME] = 0;
    tio->c_cc[VEOF] = 4;  tio->c_cc[VINTR] = 3;  tio->c_cc[VSUSP] = 26;
    tio->c_cc[VERASE] = 0x7f; tio->c_cc[VKILL] = 21; tio->c_cc[VQUIT] = 28;
    cfsetispeed(tio, B38400); cfsetospeed(tio, B38400);
}

int lv_plat_pty_spawn(const char* path, char* const argv[],
                      int rows, int cols, int flags, int* master) {
    if (!path || !path[0] || rows <= 0 || cols <= 0 || !master) return -1;
    static int sigpipe_ignored_pty = 0;      /* disposition, not a handler */
    if (!sigpipe_ignored_pty) { signal(SIGPIPE, SIG_IGN); sigpipe_ignored_pty = 1; }

    int mfd = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);   /* atomic (R§A.6) */
    if (mfd < 0) return -1;
    char sname[64];
    if (grantpt(mfd) != 0 || unlockpt(mfd) != 0 ||
        ptsname_r(mfd, sname, sizeof sname) != 0) { close(mfd); return -1; }
    int sfd = open(sname, O_RDWR | O_NOCTTY);  /* NOT cloexec: survives to the child's exec */
    if (sfd < 0) { close(mfd); return -1; }

    struct termios tio; lv_pty_termios(&tio, flags);
    struct winsize ws; memset(&ws, 0, sizeof ws);
    ws.ws_row = (unsigned short)rows; ws.ws_col = (unsigned short)cols;
    if (tcsetattr(sfd, TCSANOW, &tio) != 0 || ioctl(sfd, TIOCSWINSZ, &ws) != 0) {
        close(sfd); close(mfd); return -1;
    }

    pid_t pid = fork();
    if (pid < 0) { close(sfd); close(mfd); return -1; }
    if (pid == 0) {                            /* async-signal-safe ONLY (D-P2) */
        setsid();                              /* leader FIRST, or TIOCSCTTY no-ops */
        ioctl(sfd, TIOCSCTTY, 0);
        dup2(sfd, 0); dup2(sfd, 1); dup2(sfd, 2);
        if (sfd > 2) close(sfd);
        execve(path, argv, environ);
        _exit(127);
    }
    close(sfd);                                /* parent keeps ONLY the master */
    int fl = fcntl(mfd, F_GETFL, 0);
    fcntl(mfd, F_SETFL, fl | O_NONBLOCK);
    *master = mfd;
    return (int)pid;
}

int lv_plat_pty_resize(int master, int rows, int cols) {
    if (master < 0 || rows <= 0 || cols <= 0) return -1;
    struct winsize ws; memset(&ws, 0, sizeof ws);
    ws.ws_row = (unsigned short)rows; ws.ws_col = (unsigned short)cols;
    return ioctl(master, TIOCSWINSZ, &ws) == 0 ? 0 : -1;
}
```

Portability notes (the §6.3 seam style): `posix_openpt(O_CLOEXEC)` is honored on Linux/glibc and
musl; if a future non-Linux triple rejects the flag, the documented fallback is
`fcntl(F_SETFD, FD_CLOEXEC)` immediately after open — recorded, not coded (no such triple builds
today). `ptsname_r` is glibc/musl; macOS spells it differently — the same future-triple seam.
The TU needs `<termios.h>`, `<sys/ioctl.h>`, `<pty.h>`-free (we never use openpty).

### 1.3 `lv_plat_recv` — the EIO collapse (D-P4), the floor half

`lv_plat_posix.c:446-451` gains the same arm as the oracle (doc 01 §1.3):

```c
int64_t lv_plat_recv(int fd, void* buf, int64_t n) {
    int64_t r = (int64_t)recv(fd, buf, (size_t)n, 0);
    if (r < 0 && errno == ENOTSOCK) {
        r = (int64_t)read(fd, buf, (size_t)n);
        /* pty master, child gone: Linux -1/EIO == macOS 0 == orderly close
         * (designs/pty/ D-P4; CPython bpo-26228). Scoped to the read fallback:
         * socket semantics untouched. */
        if (r < 0 && errno == EIO) r = 0;
    }
    return r;
}
```

The sys-native layer above (`lv_loop.c`'s recv marshaling) already maps 0 → None/closed
(`lv_plat.h:44-46` contract) — no change there.

### 1.4 `lv_plat_win32.c` — always-fail stubs until S3

```c
/* pty floor: ConPTY lands in designs/pty/ 03. Until then the stubs keep the
 * archive linking; sysPtySpawn surfaces [] (runtime degrade, D-P8 — NOT a
 * compile-time reject; the same binary must run on pre- and post-S3 floors). */
int lv_plat_pty_spawn(const char* path, char* const argv[],
                      int rows, int cols, int flags, int* master) {
    (void)path; (void)argv; (void)rows; (void)cols; (void)flags; (void)master;
    return -1;
}
int lv_plat_pty_resize(int master, int rows, int cols) {
    (void)master; (void)rows; (void)cols; return -1;
}
```

Win32 `lv_plat_recv` has its own body; give its file/pipe read path the equivalent
closed-collapse **in S3** (doc 03 §5 — on Windows the "master" is a bridge socket, so the S2
win32 recv needs nothing).

---

## 2. **HARD** — Runtime natives: new `runtime/lv_pty.c` + `lv_abi.h` declarations

Everything in this section links into native binaries and carries ABI/ARC obligations. Follow
`lv_proc.c` (`runtime/lv_proc.c:1-67`) *exactly*; it is 67 lines and every convention below is
visible in it.

### 2.1 `lv_abi.h` — two additive declarations (beside the process natives)

```c
/* Pty floor (designs/pty/ 02; oracle RuntimeNatives.cpp sysPtySpawn).
 * lvrt_sysptyspawn: fresh Array<int> [pid, masterFd], or the empty array on
 *   failure (bad args / allocation / fork). Exec failure is the child's
 *   _exit(127) via lvrt_sysreap. Return follows the lvrt_sysargs ownership
 *   convention (rc 0; codegen retains) — the lvrt_sysspawn parity.
 * lvrt_sysptyresize: scalar LV_INT 0/-1.                                     */
void lvrt_sysptyspawn(LvValue* out, const LvValue* path, const LvValue* args,
                      const LvValue* rows, const LvValue* cols, const LvValue* flags);
void lvrt_sysptyresize(LvValue* out, const LvValue* master,
                       const LvValue* rows, const LvValue* cols);
```

Additive only — no existing shape, no `LV_OP_*` touch, **no STOP**.

### 2.2 `runtime/lv_pty.c`

```c
/* lv_pty.c — pty natives (G-LANG-2 terminal half, designs/pty/ 02 §2).
 * Thin LvValue marshaling over lv_plat_pty_spawn/resize; ALL policy lives in
 * the plat floor + the prelude (Pty/TcpStream in Resolver.cpp). Mirrors the
 * sysPtySpawn oracle. Ownership pattern: lv_loop.c sockets, lv_thread.c
 * threads, lv_proc.c processes, THIS TU ptys (D-P7). */
#include "lv_abi.h"
#include "lv_plat.h"
#include <stdlib.h>

/* the sanctioned inline 16-byte element store (lv_proc.c precedent, R7) */
static void lv_pty_st_val(int64_t payload, int64_t off, const LvValue* v) {
    int64_t* p = (int64_t*)((uint8_t*)(intptr_t)payload + off);
    p[0] = v->tag; p[1] = v->payload;
}

void lvrt_sysptyspawn(LvValue* out, const LvValue* path, const LvValue* args,
                      const LvValue* rows, const LvValue* cols, const LvValue* flags) {
    /* empty result = spawn failure (frozen: [] not None) */
    const char* cpath = (const char*)(intptr_t)(path->payload + 8);
    int64_t plen = *(const int64_t*)(intptr_t)path->payload;
    if (plen <= 0 || rows->payload <= 0 || cols->payload <= 0) {
        lvrt_arr_new(out, 0); return;
    }
    /* argv built BEFORE fork (D-P2): [path, args..., NULL]; string bytes are
     * NUL-terminated in the rep — alias, never copy (lv_proc.c idiom). */
    int64_t n = 0;
    if (args->tag == LV_ARR) {
        n = *(const int64_t*)(intptr_t)args->payload;
        if (n < 0) n = 0;      /* dense/columnar marker — Array<string> is boxed */
    }
    char** argv = (char**)malloc((size_t)(n + 2) * sizeof(char*));
    if (!argv) { lvrt_arr_new(out, 0); return; }
    argv[0] = (char*)cpath;
    for (int64_t i = 0; i < n; i++) {
        int64_t sp = *(const int64_t*)(intptr_t)(args->payload + 8 + 16 * i + 8);
        argv[i + 1] = (char*)(intptr_t)(sp + 8);
    }
    argv[n + 1] = NULL;

    int master = -1;
    int pid = lv_plat_pty_spawn(cpath, argv, (int)rows->payload,
                                (int)cols->payload, (int)flags->payload, &master);
    free(argv);                /* parent-side, post-fork */
    if (pid <= 0) { lvrt_arr_new(out, 0); return; }

    lvrt_arr_new(out, 2);      /* rc 0; codegen retainDst()s (sysSpawn parity) */
    LvValue e; e.tag = LV_INT;
    e.payload = pid;    lv_pty_st_val(out->payload, 8 + 16 * 0, &e);
    e.payload = master; lv_pty_st_val(out->payload, 8 + 16 * 1, &e);
}

void lvrt_sysptyresize(LvValue* out, const LvValue* master,
                       const LvValue* rows, const LvValue* cols) {
    out->tag = LV_INT;
    out->payload = lv_plat_pty_resize((int)master->payload,
                                      (int)rows->payload, (int)cols->payload);
}
```

ARC facts restated (K5): elements are immediate `LV_INT`s — no per-element retain; the array
returns at rc 0 and the codegen row retains (+1 transfer), the `sysArgs`/`sysSpawn` convention.
Do **not** stamp `HDR(payload)[0] = 1` (that is `lvrt_arr_fill`'s transfer path — mixing them on
a retainDst row leaks; spawn design D1). Settled empirically by the valgrind lane (§5).

---

## 3. **HARD** — Codegen: `src/LlvmGen.cpp`

### 3.1 Members + ctor decls

Append to the `FunctionCallee` list: `rtSysPtySpawn, rtSysPtyResize`. Decls beside the process
floor block (`:353-356`); boundary rule — out-param first, everything `ptrTy`:

```cpp
// Pty floor (designs/pty/ 02 §3): lowers on ALL targets incl. Windows —
// pre-S3 win32 stubs return the failure sentinels (D-P8 runtime degrade).
rtSysPtySpawn  = fn("lvrt_sysptyspawn",  voidTy, {ptrTy, ptrTy, ptrTy, ptrTy, ptrTy, ptrTy});
rtSysPtyResize = fn("lvrt_sysptyresize", voidTy, {ptrTy, ptrTy, ptrTy, ptrTy});
```

### 3.2 `CallNativeFn` rows (the chain at `:2687-2705`, before the terminal fail)

A **separate** `else if` arm from the sysSpawn family — the Windows gating differs (D-P10:
sysPty* must NOT sit under the `targetWindows` fail):

```cpp
} else if (n == "sysPtySpawn" || n == "sysPtyResize") {
    // Pty floor (designs/pty/): no Windows reject — D-P8's runtime degrade.
    // ConPTY lands in designs/pty/ 03; until then the win32 stubs return
    // the frozen failure sentinels and the language sees [].
    if (n == "sysPtySpawn") {
        b.CreateCall(rtSysPtySpawn,
                     {regs[in.a], arg(0), arg(1), arg(2), arg(3), arg(4)});
        retainDst();           // fresh heap Array<int> -> +1 (sysSpawn parity)
    } else {  // sysPtyResize
        b.CreateCall(rtSysPtyResize, {regs[in.a], arg(0), arg(1), arg(2)});
    }
}
```

Scalar row: no retain (int is immediate). The blanket `emitThrowCheck` after the chain covers
these rows; neither raises (sentinel returns), matching the oracle. The existing
`sysSpawn|sysPidfdOpen|sysReap|sysKill` arm at `:2687` is **untouched in S2** (its Windows fail
still guards all four; S3 splits it — doc 03 §7, also HARD).

Not applicable (stated to preempt confusion, the spawn design's note): no `kCovered[]` touch
(instance-method path), no `Op`/`Lower.cpp`/front-end changes — S1 already proved the front-end
complete via the interpreter lanes.

### 3.3 Argument-count check

`sysPtySpawn` is the first 5-arg `sys*` native through this chain; verify `arg(i)` marshaling
handles indices 0–4 in this block's idiom (it is positional — no structural limit expected). If
any arity assumption bites, that is an implementation finding for the log, not a design change.

---

## 4. Build wiring (both lists + archives, one commit — K9)

- `CMakeLists.txt` `add_library(lvrt STATIC …)` (`:1180-1186` region): add `runtime/lv_pty.c`.
- `${LANG_LVRT_SOURCES}` (`:819-829`): add `runtime/lv_pty.c` (the list `run_native_llvm.sh`
  links test binaries against).
- `runtime/build-triple.sh` `srcs=(…)` (`:111`): add `lv_pty.c`.
- Rebuild both prebuilt per-triple archives (`runtime/aarch64-linux-gnu/liblvrt.a`,
  `runtime/x86_64-pc-windows-gnu/liblvrt.a`) as the final landing step.

---

## 5. Selftest + memcheck (`runtime/selftest.c`)

One C-level case beside the spawn-floor case (the §7.4 precedent), valgrind-covered by the
existing `runtime_selftest` CTest lane:

1. `lv_plat_pty_spawn("/bin/echo", {"/bin/echo","hi",NULL}, 24, 80, /*flags*/1, &master)` →
   pid > 0, master ≥ 0;
2. drain via `lv_plat_recv` until 0 (**this asserts the EIO collapse at the C level** — on Linux
   the underlying read errors EIO and the wrapper must still say 0);
3. accumulated bytes == `"hi\r\n"` (the frozen DETERMINISTIC profile's ONLCR);
4. `lv_plat_reap` poll-loop → 0; `lv_plat_pty_resize` on the **closed** master → -1;
5. fd hygiene: exact fd-number reuse across two rounds (the spawn selftest's probe idiom);
6. refusals: empty path / rows 0 → -1, no fd leaked (open-fd count unchanged).

Spawn the case from a threaded context (K1 — the spawn selftest already establishes the pattern).

---

## 6. Corpus promotion + assertion flips

1. `tests/corpus/sys_pty/` gains `corpus_sys_pty_llvm` (via `run_native_llvm.sh` +
   `${LANG_LVRT_SOURCES}`), diffing the **same** `.expected` as treewalk/IR — three-lane
   byte-identity is the pass condition (R§D.5: same kernel + frozen termios ⇒ achievable, so
   required).
2. Remove `sys_pty/` from `corpus_llvm_full`'s exclusion list (added in S1); drop the S1 header
   note in the dir.
3. `tests/run_sysnatives.sh`: add the pty behavior legs against the LLVM binary — kill→143,
   winsize seed, VEOF round trip, fd-churn ±0 with pty rounds (doc 01 §4's list, compiled lane).
4. emit-C++ leg: add the deferral assertion for `sysPtySpawn` (must fail cleanly with the
   `native backend: native 'sysPtySpawn'` coverage-error shape) — the policy assert, mirroring
   the spawn one that stays.

---

## 7. Documentation updates (part of the landing)

1. `docs/reference.md`: `Pty` class + `sysPtySpawn`/`sysPtyResize` rows in §6.6; coverage note
   "oracle + IR + LLVM; emit-C++ defers; Windows: runtime degrade until designs/pty/ 03".
2. `designs/helm/techdesign-00-overview.md` §14: append — **G-LANG-2 terminal half green on
   POSIX**; H10 can start against `Pty` on the oracle+IR+LLVM lanes.
3. `techdesign-00-overview.md` (this dir) §6: the landing entry, including any implementation
   findings (the append-only log discipline).
4. `info.md` backend-coverage ladder: add the pty floor to the LLVM coverage list.

---

## 8. Landing order within S2

| step | contents | proves |
|---|---|---|
| S2-a | §1 plat floor + §4 build wiring + §5 selftest | floor works, valgrind-clean, no codegen yet |
| S2-b | **HARD** §2 `lv_pty.c` + `lv_abi.h`, §3 LlvmGen rows | end-to-end pty on LLVM |
| S2-c | §6 corpus promotion + flips + archive rebuild | oracle = IR = LLVM byte-identical |
| S2-d | §7 docs | paper trail; G-PTY2 closed |

---

## 9. Implementation log (append-only)

- 2026-07-16 — doc created (S2 of designs/pty/). Awaiting G-PTY1.
