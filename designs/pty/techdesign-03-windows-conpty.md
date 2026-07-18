# Tech Design 03 — PTY Stage S3: Windows ConPTY Floor (gate G-PTY3)

**Status:** design, ready for implementation after G-PTY2. **Parent:** `techdesign-00-overview.md`.
**Scope:** the Windows lane — ConPTY allocation, the reader-thread↔socketpair bridge that makes
ConPTY's un-pollable pipes ride the WSAPoll loop, real win32 `lv_plat_reap`/`lv_plat_kill`
bodies over a pid→HANDLE registry, the **HARD** codegen gating split (sysReap/sysKill lose the
Windows reject; sysSpawn/sysPidfdOpen keep it), and the Windows behavioral test lane.
**Delivery standard:** the same as the existing win32 floor (`lv_plat_win32.c:1-9`):
**written-to-spec, compile-verified under the MinGW triple, execution-verified when a Windows
host is available** — the honest precedent, restated rather than overpromised.
**Version charter:** Windows 10 1809 (build 17763) / Server 2019 minimum, probed at runtime
(D-P8). No winpty (ratified, overview D-P8 / R open-q #7).

---

## 1. Windows rulings (D-W1 … D-W5)

**D-W1 — The bridge is the node-pty shape (R§B.3), one struct per pty.** ConPTY's anonymous
pipes cannot be WSAPoll'd. Each live pty owns a bridge: a loopback socketpair (the loop-side end
is registered in the existing socket fd table, `lv_plat_win32.c:44-77`, so it IS the `*master`
the language sees) plus two floor-internal threads — **conout pump** (blocking `ReadFile` on
ConPTY's output → `send` to the bridge socket) and **conin pump** (blocking `recv` on the bridge
socket → `WriteFile` to ConPTY's input). **No language code ever runs on a bridge thread**
(the signal self-pipe sanction, `lv_plat.h:82-88`); they move bytes and exit. Windows has no
`socketpair()`: the pair is built the classic way — loopback listener on port 0, connect,
accept, close listener — a private `lv_win_socketpair()` helper.

**D-W2 — Exit detection on Windows = bridge-socket EOF + poll-reap.** `lv_plat_pidfd_open`
**stays `-1` on Windows** — the `Pty` prelude then takes its existing 20 ms poll-reap fallback
branch (`Resolver.cpp` `exitCode()`, the D5 degrade that already ships) with **zero new prelude
code**. When the child exits, ConPTY breaks the output pipe, the conout pump's `ReadFile` fails,
the pump `shutdown()`s+closes its socket end, and the loop-side master reads 0 → the normal
close path. A wait-thread pidfd emulation is recorded as a possible refinement, not v1 — the
poll-reap fallback exists precisely for floors without a pollable exit fd.

**D-W3 — Reap/kill via a pid→state registry; never OpenProcess-by-pid (K7).**
`lv_plat_pty_spawn` records `{pid, hProcess, hThread, HPCON, bridge*}` in a floor-internal
registry. `lv_plat_reap(pid)`: registry hit → `WaitForSingleObject(hProcess, 0)`;
`WAIT_TIMEOUT` → `-1` (running); signaled → `GetExitCodeProcess` → `code & 0xFF`, then release
the registry entry (CloseHandles). The `STILL_ACTIVE`(259) ambiguity is dodged by ruling on the
wait, never on the code value. Registry miss → `-1` (not ours — same answer POSIX waitid gives
for a non-child). `lv_plat_kill(pid, sig)`: registry hit → `TerminateProcess(hProcess,
LV_PTY_KILLED)`; miss → `-1`. The signal number is accepted and ignored beyond
terminate-vs-nothing — Windows has no signals.

**D-W4 — The honest exit encoding (R§B.5, pitfall #10).** Normal exit → `code & 0xFF`.
Killed-by-us → the sentinel **`LV_PTY_KILLED = 254`**, a documented constant in the exited band —
**never a fake `128+sig`**. The `128+sig` band is POSIX-only; consumers that branch on 143 must
treat it as POSIX-lane behavior (goldens already do — the kill golden is a POSIX lane). 254 is
chosen clear of 127 (exec-fail) and 255 (commonly `exit(-1)`).

**D-W5 — Teardown discipline (the deadlock rules, R§B.4).** Ordering when the master fd is
closed (`lv_plat_close` routes a bias-range fd → registry lookup by bridge socket):
1. close the loop-side socket (conin pump's `recv` fails → it exits);
2. call `ClosePseudoConsole(hPC)` from the **calling** thread — never from the conout pump
   (the drainer), which keeps draining until the pipe breaks on its own (the pre-24H2
   deadlock rule; node-pty PR #415 / terminal PR #14160);
3. join both pumps with a bounded wait; CloseHandle the pipe ends;
4. the registry entry keeps `hProcess` until `lv_plat_reap` collects it (or is freed with a
   documented handle-release if the program never reaps — mirroring POSIX's honest "unreaped
   children are not reaped" stance).
`ClosePseudoConsole` terminates the attached tree — acceptable and documented: closing the
terminal hangs up the shell, the POSIX SIGHUP analogy (R§A.5). `CreatePseudoConsole` is always
called with **flags = 0** — `PSEUDOCONSOLE_INHERIT_CURSOR` is banned (its cursor-query
handshake deadlocks an unanswering host, pitfall #13).

---

## 2. `lv_plat_pty_spawn` on win32 — the creation flow (R§B.1, confirmed against MSDN)

```
probe:   CreatePseudoConsole via GetProcAddress(kernel32) once, cached.
         NULL (pre-1809) -> return -1 (D-P8 runtime degrade; language sees []).
pipes:   CreatePipe(&inR, &inW), CreatePipe(&outR, &outW)      // anonymous, inheritable=FALSE
conpty:  CreatePseudoConsole({cols, rows}, inR, outW, 0, &hPC) // flags = 0 ALWAYS (D-W5)
attr:    InitializeProcThreadAttributeList (size, then fill) +
         UpdateProcThreadAttribute(PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hPC)
proc:    CreateProcessW(NULL, cmdline, ..., EXTENDED_STARTUPINFO_PRESENT, ..., &siEx, &pi)
close:   CloseHandle(inR); CloseHandle(outW);   // the parent's copies — REQUIRED for EOF
         (pitfall analog of closing the POSIX slave, R§B.1 step 3)
bridge:  lv_win_socketpair(&loopSide, &pumpSide); spawn conout/conin pumps (D-W1)
register:pid -> {hProcess, hThread, hPC, bridge}; *master = socket-table fd of loopSide
return:  pi.dwProcessId
```

Deviations from POSIX the floor hides (and one it can't):
- **argv → command line.** Windows takes one command-line string: join `[path, args…]` with the
  MSVCRT quoting rules (quote-if-spaces, backslash-escape embedded quotes — the documented
  `CommandLineToArgvW` inverse). Path stays unsearched (`lpApplicationName = path`), preserving
  the no-PATH-search contract.
- **UTF-8 both directions** (R§B.2): the bytes on the bridge are UTF-8; no codepage translation
  anywhere in the floor.
- **termios/flags**: there is no termios; ConPTY owns the cooked behavior. `flags` bit0 is
  accepted and **ignored** — determinism on Windows comes from the behavioral test lane (§6),
  not byte goldens. Documented, not hidden.
- **What leaks through** (accepted in overview D-P1): the master bytes are ConPTY's re-rendered
  VT stream, not the child's raw output. Helm's VT parser consumes VT on both platforms —
  just *different* VT (R§E).
- **`0xc0000142` startup-race rule** (pitfall B.4): never tear down between `CreatePseudoConsole`
  and `CreateProcess` success without the full D-W5 sequence; validate hPC before CreateProcess.

`lv_plat_pty_resize` = registry lookup by master fd → `ResizePseudoConsole(hPC, {cols, rows})`.

## 3. The bridge, precisely (D-W1 mechanics)

```
conout pump:  for (;;) { ReadFile(outR, buf, 4096, &n, NULL)      // blocks
                         || break;                                 // pipe broke: child/ConPTY gone
                         send(pumpSide, buf, n) || break; }        // loop side closed
              shutdown(pumpSide, SD_SEND);                        // -> loop reads 0 = EOF (D-W2)
conin pump:   for (;;) { n = recv(pumpSide, buf, 4096, 0);
                         if (n <= 0) break;                        // loop side closed
                         WriteFile(inW, buf, n, &w, NULL) || break; }
```

- Pumps are `CreateThread`, small fixed stacks, no allocator use beyond their buffers, no
  callbacks — auditable in isolation.
- The conout pump **always drains while the child lives** (pitfall #4: a full ConPTY buffer
  blocks the child) — its loop has no pause path.
- Backpressure toward the language: if the loop stops reading the bridge socket, the socket
  buffer fills, `send` blocks the conout pump, ConPTY's buffer fills, the child blocks — the
  POSIX full-master analog, surfaced identically (D-P9: the language-side TcpStream keeps
  draining via its read-watch, so this only engages if language code stops pumping the loop).
- Bridge sockets ride the existing socket table → `lv_plat_poll`'s WSAPoll path and
  `lv_plat_send/recv` work on the master fd with **zero** changes.

## 4. Registry (D-W3 mechanics)

Static table, same growth idiom as the socket table (`lv_plat_win32.c:50-66`): slots of
`{int pid; HANDLE hProcess, hThread; HPCON hPC; int loopFd; HANDLE pumps[2]; HANDLE inW, outR;}`.
Single-threaded access from the loop thread (spawn/reap/kill/close/resize all originate there);
the pumps never touch the registry — they own only their handles/socket, so no lock is needed
beyond the teardown join. Entry lifetime: created at spawn; pipes+pumps+hPC released by D-W5
teardown (master close); `hProcess` released at reap-success; a leak-audit pass on process exit
is the existing floor's job, not new machinery.

## 5. win32 `lv_plat_recv` note

The master is a real socket on Windows (bridge), so the S2 EIO-collapse has no win32 twin to
write: EOF arrives as `recv == 0` from the pump's `shutdown` — already the closed signal.
Confirm the win32 recv path reports graceful-close as 0 (it does — the B-M5 contract,
`lv_plat_win32.c:11-14`).

## 6. Testing — the Windows behavioral lane (R§B.2/D.5; pitfall #9)

**Never byte goldens.** ConPTY re-renders; its VT stream is version-dependent. The lane asserts
behavior after VT stripping (a ~20-line ESC-sequence stripper in the test driver, not a parser):

1. spawn `cmd /c echo hi` → stripped output **contains** `hi`; exit 0.
2. write `ping\r\n` to `cmd /c more` → stripped output contains `ping`; EOF via ^Z+`\r\n`
   (the cmd convention) or master close; exit observed.
3. resize 30×100 → `ResizePseudoConsole` rc 0 (no output assert — `mode con` output is
   locale-dependent; the API rc + no-hang is the v1 assert).
4. kill → exit code `LV_PTY_KILLED` (254) exactly (D-W4 pinned).
5. teardown under load: spawn `cmd /c dir /s C:\Windows\System32`, close the master mid-stream —
   no hang (the D-W5 deadlock discipline, exercised hard on pre-24H2 = Server 2019 CI, R§D.4).
6. pre-1809 probe path: forced via a test hook that nulls the cached fn pointer → spawn returns
   `[]`, prelude resolves 127, nothing hangs.

Script: `tests/run_pty_win.sh`-style driver beside the existing win-lane scaffolding, gated on a
Windows host/wine-with-conpty availability; **compile of the MinGW triple is the CI-enforced
part** (the B-M5 standard, §0). Wine note: ConPTY emulation in wine is incomplete — the script
must probe and skip loudly, never hang (R§D.4's discipline).

## 7. **HARD** — Codegen gating split (`src/LlvmGen.cpp:2687-2705`)

The S2 arm for `sysPtySpawn|sysPtyResize` already lowers on all targets. S3 splits the process
arm — `sysReap`/`sysKill` gain Windows lowering (they now have win32 bodies), `sysSpawn`/
`sysPidfdOpen` keep the reject (D-P10):

```cpp
} else if (n == "sysSpawn" || n == "sysPidfdOpen") {
    // pipes-spawn stays POSIX-only (techdesign-spawn-llvm.md D4); the pty
    // path is the sanctioned Windows child story (designs/pty/ 03).
    if (targetWindows) {
        fail("process spawn: unsupported on Windows (v1) — '" + n +
             "' has no Windows lowering (techdesign-spawn-llvm.md)");
    } else if (n == "sysSpawn") {
        b.CreateCall(rtSysSpawn, {regs[in.a], arg(0), arg(1)});
        retainDst();
    } else {
        b.CreateCall(rtSysPidfdOpen, {regs[in.a], arg(0)});
    }
} else if (n == "sysReap" || n == "sysKill") {
    // Windows-clean since designs/pty/ 03 (registry-backed floor, D-W3).
    if (n == "sysReap") b.CreateCall(rtSysReap, {regs[in.a], arg(0)});
    else                b.CreateCall(rtSysKill, {regs[in.a], arg(0), arg(1)});
}
```

ABI note: the four callees/signatures are untouched — this is **gating** surgery only, no new
symbols, no ARC changes (`sysReap`/`sysKill` are scalar rows, no retain — unchanged). The
`run_sysnatives.sh` Windows-triple compile assertions flip accordingly: `sysReap`/`sysKill`
compile, `sysSpawn`/`sysPidfdOpen` still reject with the frozen message.

## 8. Landing checklist

1. `lv_plat_win32.c`: §2 spawn + §3 bridge + §4 registry + reap/kill/resize + D-W5 teardown;
   probe + `LV_PTY_KILLED` constant in `lv_plat.h` (additive, beside the pty decls).
2. **HARD** §7 LlvmGen split + assertion flips.
3. §6 behavioral script checked in; MinGW triple compiles; both archives rebuilt.
4. Docs: `reference.md` Windows column (pty: runtime-degrade pre-1809, ConPTY 1809+, exit
   encoding note incl. 254); overview §6 log; Helm §14 (H10's Windows story exists).

## 9. Risks (Windows-specific; overview K6/K7 restated concretely)

| # | risk | mitigation |
|---|---|---|
| W1 | ClosePseudoConsole deadlock on Server 2019 | D-W5 ordering; §6.5 stress test on the CI floor version |
| W2 | pid reuse between exit and reap | registry holds hProcess (the kernel keeps the object alive) — reap never goes by pid to the OS (D-W3) |
| W3 | bridge thread leaks on abnormal teardown | bounded joins in D-W5; leak audit in the behavioral lane |
| W4 | quoting bugs in argv→cmdline | the documented CommandLineToArgvW-inverse algorithm + a quoting unit table in the behavioral script |
| W5 | wine CI false-green/hang | probe-and-skip loudly; MinGW compile is the enforced gate (B-M5 standard) |

## 10. Implementation log (append-only)

- 2026-07-16 — doc created (S3 of designs/pty/). Awaiting G-PTY2.
