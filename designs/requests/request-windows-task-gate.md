# Request: narrow the Windows task-native reject (and, at maximum, the win32 fiber leg)

**From:** the pty floor's Windows lane (`designs/complete/techdesign-03-pty-windows-conpty.md`,
S3 / gate G-PTY3), on behalf of Helm H10 (the integrated terminal, gate G-H6).
**Date:** 2026-07-19.
**Standing bug:** `known_bugs_2.md` **#97 [P1]** — same facts, defect side.
**Owner ruling needed:** which of the two contributors in §3 to narrow. Both are outside the
pty floor's scope (§7 of the pty doc scopes S3 to gating surgery on four process rows), which
is why this is a request rather than a fix.

## 1. What is being requested

A Windows-target build must be able to compile a program that uses **sockets, a child process,
or a pty** — capabilities that have real Windows floors today and that no design rejects —
without the **task** natives' Windows reject firing on their behalf.

Today it does fire, on a program with no task feature in it at all:

```
$ printf 'TcpListener l = TcpListener(9099);\n' > t.lev
$ leviathan --native-obj t.o --target x86_64-pc-windows-gnu t.lev
error: LLVM backend: tasks: unsupported on Windows (v1) — 'sysTaskCancel'
       has no Windows lowering
```

`TcpStream`, `Process`, and `Pty` fail identically.

**Explicitly NOT requested here:** overturning the ruling that `spawn`/`Channel`/`TaskGroup`
are unsupported on a Windows target. That ruling is sound and documented (LA-30 G5 — win32
needs the Fiber API, never hand-rolled asm, so tasks stay pump-pinned;
`runtime/lv_task.c:27-57` hardcodes `lv_tasks_enabled() == 0`). A program that genuinely uses
tasks on Windows should keep failing — loudly, and naming a construct the author wrote.

## 2. What I am working on that requires it

G-PTY3 landed the ConPTY floor: `lv_plat_pty_spawn`/`_resize`, the socketpair bridge, the
pid→HANDLE registry behind `lv_plat_reap`/`lv_plat_kill`, and the codegen split that lets
`sysReap`/`sysKill` lower on Windows. The **floor** is Windows-clean and exercised
(`tests/run_pty_win.sh`). What cannot be built for a Windows target is the **`Pty` prelude
class** — because it routes its master through `TcpStream`, which drags `sysTaskCancel` in.

Consequences already paid, in-tree, so the cost is concrete rather than hypothetical:

- `tests/pty_win_driver.lev` drives the raw floor natives instead of `Pty`. It proves the
  floor, but the class-level Windows lane has **no** coverage and cannot get any.
- Helm **H10**'s Windows story is therefore floor-only. H10 is `.lev` code and would have to
  hand-roll the entire `Pty` class (and its stream plumbing, since `TcpStream` is equally
  blocked) to host a terminal on Windows — re-implementing the prelude inside the IDE.
- Every future track wanting a socket, a subprocess, or a terminal on Windows re-discovers
  this and re-applies the same workaround. That is exactly `known_bugs_2.md` #97's P1.2
  justification.

## 3. The two contributors (narrowing **either** is sufficient)

1. **Prelude over-marking** — `src/Resolver.cpp:3232` already documents this mechanism
   breaking every program's `--build` once before (fixed then by fresh-per-injection binds).
   Marking is arity-blind and by-name, so `TcpStream`/`File`'s `close()` also marks
   `TaskGroup::close()`, whose body reaches `std::sysTaskCancel` (`src/Resolver.cpp:1414`).
2. **The reject is emission-gated, not reachability-gated** — `src/LlvmGen.cpp:2752-2765`
   fails the build the moment the row is emitted, reachable or not. The **wasm** gate
   immediately above it (`wasmGatedNative`, Track W hard-03) already has the answer shape:
   tier 1, reachable from user code → compile-time diagnostic; tier 2, prelude-only
   over-approximation → an `lvrt_unsupported` trap that never returns.

The second is the smaller, better-precedented change: the two-tier gate already exists in the
same else-chain and would be reused, not invented. The first is a deeper cleanup that would
also shrink Windows binaries and improve every diagnostic in the family.

## 4. Minimum implementation

- `sysTaskRun`/`sysTaskCancel`/`sysTaskShield`/`sysTaskJoinAll`/`sysAwaitAny2` on a Windows
  target take the **two-tier** treatment: reachable from user code → today's compile-time
  `tasks: unsupported on Windows (v1)` diagnostic, unchanged; prelude-only → a trap, so the
  build succeeds and the program only fails if it actually reaches one.
- `TcpStream`, `TcpListener`, and `Pty` build for `--target *windows*` — the three whose
  Windows floors exist (Winsock since B-M5; ConPTY since G-PTY3).
- **Open question for the design agent to settle by experiment:** whether the same
  over-approximation also drags `sysSpawn`/`sysPidfdOpen` into these builds. Today the tasks
  reject fires first, so it masks the answer; it can be measured by temporarily disabling the
  tasks arm and recompiling the repro. If it does, the spawn arm needs the same tier-2
  treatment (its win32 floor already returns the failure sentinel, so a trap or the sentinel
  is the honest tier-2 there) — otherwise narrowing the task gate alone will not unblock the
  three classes above, and this request's minimum is not met.
- The diagnostic for a genuine task use still names the user's construct, not a native the
  program never mentioned.
- Regression coverage: the three classes compile for the Windows triple (an assertion beside
  the existing `tests/run_sysnatives.sh` §12 win-gate block), and a program that really uses
  `spawn`/`Channel`/`TaskGroup` still rejects with the frozen message.
- No behavior change on any POSIX lane, and no change to the LA-30 ruling.

Not required by the minimum: that `Pty`/`TcpStream` *run* correctly under wine — wine's
ConPTY is partial (G-PTY3 §10, risk W5). Executing on a real Windows host is the honest bar,
same as the rest of the win32 floor.

## 5. Maximum implementation

Everything above, plus the **win32 task leg** so tasks are genuinely supported rather than
gated: the Fiber API (`ConvertThreadToFiber`/`CreateFiber`/`SwitchToFiber`) behind the
`lv_task.h` seam via `lv_plat_win32.c`, per LA-30 G5 —
`designs/complete/techdesign-01-task-substrate.md:200`. That deferral was filed as instructed
(`designs/suspension/techdesign-00-overview.md:166` — M4 "iff the wine lane executes async
corpus"; the audit said it does not, and `tests/run_wine_cross.sh:26` carries the resulting
explicit `LANG_PUMP=1` pin). At maximum:

- `lv_tasks_enabled()` returns 1 on win32; `spawn`/`Channel`/`TaskGroup`/`await` work there.
- The wine (or Windows) lane executes async corpus, and the `LANG_PUMP=1` pin plus its
  port-milestone comment are deleted **together** with the stub — the doc-5 §5 rule that the
  lane must never diverge silently.
- The Windows reject disappears from `src/LlvmGen.cpp` entirely rather than being narrowed.

## 6. What this unblocks more widely

- **Helm H10 / gate G-H6 on Windows** — an embedded terminal built on the `Pty` class rather
  than a hand-rolled floor wrapper. The ConPTY floor it needs already exists.
- **Sockets on Windows for compiled programs** — `TcpStream`/`TcpListener` (and everything
  layered on them: HTTP, Trident's fetch paths, Atlantis) currently cannot be built for a
  Windows target at all. The Winsock floor under them has been complete and wine-verified
  since B-M5; only this gate stands in front of it.
- **A wider wine corpus lane** — `tests/run_wine_cross.sh` is scoped to core + llvm_objects
  today. Net corpus is excluded for port-locking reasons, but it also would not compile; the
  minimum implementation removes the second obstacle.
- **`Process` on Windows stays out regardless**, and this request does not ask for it:
  pipes-spawn has no win32 floor, and `techdesign-spawn-llvm.md` D4 keeps
  `sysSpawn`/`sysPidfdOpen` rejected (narrowed, not lifted, by G-PTY3). A program that really
  constructs a `Process` should keep failing on a Windows target — with a diagnostic naming
  spawn, which is the honest one. The pty path is the sanctioned Windows child story.
