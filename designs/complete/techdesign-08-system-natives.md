# Track 08 — System Natives: argv/env/exit, time, random, dirs, isatty, sockets+, spawn

**Status:** COMPLETE on the active engines (2026-07-10) — F1–F7 all landed:
F1(env)/F2/F3/F4/F6-phase1 + F7(spawn) on the oracle+IR interpreters, F5
(socket floor: nb-connect/write-watch/SO_ERROR/IPv6, `connectTimeout`,
`TcpStream.send` drain) on oracle+IR **and LLVM-native**. Every remaining item
is policy-deferred, not unfinished work: frozen-ELF halves (X64Gen freeze —
portable pivot), per-consumer LLVM coverage for the non-socket natives (the
`sysNow` precedent), F6 phase 2 (explicitly post-track roadmap). See §13. **Date:** 2026-07-05. **Depends on:** nothing (independent file
family; runs parallel to everything). `designs/complete/argv.md` is the **normative design
for F1** — this doc only schedules it and adds the siblings.
**Source:** suggested-features.md §11; curl-design §9 (the verdict table this
track exists to shrink); contracts C2 (consumer), C4 (consumer, Block overloads
later), C6 (owner).
**Owns:** `RuntimeNatives.cpp` sys region (~100–260), `RuntimeLoop.hpp/cpp` (the
event loop), kPrelude `namespace std` sys declarations (Resolver.cpp:179–260) +
thin wrappers (`env`, `Process`), `main.cpp` argv plumbing (flag loop at 57–92),
`X64Gen.cpp` syscall-emission region + `_start` entry, `CGen.cpp` sys stubs
policy (emit-C++ skips the system layer by design — keep its clean
coverage-errors).

**Sequencing rule inside the track:** every native lands interpreter-first
(oracle+IR share `RuntimeNatives.cpp` — one edit covers both), ELF second. That
is how sockets/timers landed; it keeps the semantic reference ahead of the
machine-code work.

---

## 1. F1 — argv, env, exit (the top-3 curl gaps)

Per `designs/complete/argv.md` (owner-ruled 2026-07-04): `std::env::args() -> Array<string>`
is a **function**, plus `std::env::get(string) -> string?`, and this track adds
`std::exit(int)` (argv.md leaves exit adjacent; spec here):

- **`std::sysArgs() -> Array<string>`** floor native; prelude `namespace env`
  wrapper `args()` returns the *program's* arguments (not the compiler's).
  `main.cpp:57-92`: everything after the source file / `--project manifest`
  argument belongs to the program (`lang --run app.ext -- -v http://...` — adopt
  the `--` separator argv.md specifies; without `--`, unrecognized trailing args
  after the source are the program's). ELF: argc/argv are on the stack at
  `_start` per the SysV ABI — capture in the entry prologue (X64Gen's `_start`
  emission), materialize lazily into the tagged-array form on first sysArgs call
  (or eagerly at startup — simpler; a dozen strings, negligible).
- **`std::sysEnv(string) -> string?`** — `getenv` on interpreters; ELF: envp
  follows argv on the entry stack; linear scan compare. Optional-return follows
  the C2 recipe (Track 04's log).
- **`std::exit(int code)`** — immediate termination: flushes nothing (sysWrite is
  unbuffered), runs no cleanups, live event-loop work does not block it
  (documented: exit means exit — curl semantics). Interpreters: `std::exit` after
  tearing down cleanly enough to not corrupt (plain `_exit(code)` is honest);
  ELF: `syscall(60, code)`. **The ELF heap-stats line** (X64Gen.cpp:3383 emits at
  exit): exit(code) must route through the same epilogue so the meter still
  prints — OR skip it; decision: **skip on explicit exit** (the meter measures
  normal completion; curl-design §2.7#2 wants it suppressible anyway — while in
  this region, add the `LANG_NO_HEAP_METER=1`-style env check or a compile flag,
  which resolves that curl finding as a rider).

## 2. F2 — time & random (contract C6)

- `std::sysNow() -> int` — wall clock, epoch **milliseconds**
  (`clock_gettime(CLOCK_REALTIME)`; ELF syscall 228).
- `std::sysMonotonic() -> int` — `CLOCK_MONOTONIC` ms; for durations/deadlines.
- `std::sysRandom(int n) -> string` — n cryptographically-random bytes
  (`getrandom(2)`, ELF syscall 318; interim string-carried per the sys-floor
  convention; a `Block` overload follows Track 03 M4 — noted, not blocking).
  Loop on short reads; n ≤ 0 → `""`; n > 1MB → RuntimeException (arbitrary
  sanity bound, documented).

## 3. F3 — dirs & fs metadata

- `std::sysListDir(string path) -> Array<string>?` — entry names, no `.`/`..`;
  `None` = not a directory / can't open (distinguishable from empty). ELF:
  `getdents64` (syscall 217) — parse the dirent buffer in emitted code
  (fixed-layout struct walk; tedious, mechanical).
- `std::sysMkdir(string) -> int` (0/-1), `std::sysRemove(string) -> int`
  (unlink, then rmdir on EISDIR), `std::sysRename(string, string) -> int`.
- Prelude wrappers can wait (a `Dir` class is framework-era sugar); the floor is
  what unblocks tooling.

## 4. F4 — isatty

`std::sysIsTty(int fd) -> bool` — interpreters `isatty(3)`; ELF: `ioctl(fd,
TCGETS, buf)` success test (syscall 16).

## 5. F5 — socket-floor upgrades (connect-timeout enablers + IPv6)

The blocking `sysTcpConnect` freezes the single-threaded runtime (curl-design
§2.9) and no write-watch exists (§2.1) — two halves of one fix:

1. **`std::sysTcpConnectNb(string host, int port) -> int`** — non-blocking
   socket, `connect` returns immediately (fd, even on EINPROGRESS; -1 only on
   immediate failure like bad literal).
2. **`std::sysWatchWrite(fd, (int) => void cb) -> int`** — write-readiness watch
   (the loop today watches read only; `RuntimeLoop` gains a second fd set —
   anchor: wherever `sysWatch` registers, mirror it).
3. **`std::sysConnectResult(fd) -> int`** — post-writability
   `getsockopt(SO_ERROR)` (0 = connected, else errno-shaped code).
4. Prelude: `std::connectTimeout(host, port, ms, (int) => void cb)` composing
   1+2+3 + a Timer — **in-language**, the streams-philosophy payoff; lcurl's
   `--connect-timeout` verdict flips.
5. **IPv6:** extend `sysTcpConnect`/`sysTcpConnectNb`/`sysTcpListen`: if the host
   literal contains `:`, use `AF_INET6`/`sockaddr_in6` (+ `[::1]:port` bracket
   parsing stays in-language in URL code — the floor takes a bare address
   string). Listen gains dual-stack via V6ONLY=0 where asked (v1: listen stays
   v4 unless the bind address is a v6 literal).
6. `sendAll`-grade write-watch usage in prelude `TcpStream.send` is a rider
   (curl-design §2.1 notes prelude send ignores short writes): with
   sysWatchWrite available, upgrade `TcpStream.send` to queue-and-drain. Small,
   high-value, in-language.

ELF: the emitted event loop must add the write-fd set (X64Gen's loop region —
find the poll/select emission; this is the one genuinely fiddly ELF item in the
track; budget it — see problem #2).

## 6. F6 — DNS (`sysResolve`) — two-phase

- **Phase 1 (this track):** `std::sysResolve(string host) -> string?` —
  `getaddrinfo` (A record, first result, dotted-quad string) on
  oracle/IR/emit-C++. **ELF: clean diagnostic** ("resolve: not on the pure
  backend yet") — the zero-dep backend has no libc resolver, and faking it badly
  would betray the engine's whole premise.
- **Phase 2 (roadmap, post-track):** `sysUdpSend/sysUdpRecv` floor natives + an
  **in-language DNS client** (parse /etc/resolv.conf via File, build the query
  packet via Block, one UDP round-trip) — which then runs on ELF *because it is
  just language code over the floor* (the streams philosophy closing its own
  gap). Depends on Track 03 Block. Recorded on the overview roadmap.

## 7. F7 — `sysSpawn` (the agentic primitive)

```
std::sysSpawn(string path, Array<string> args) -> Array<int>
// [pid, stdinFd, stdoutFd, stderrFd]  — or [] on spawn failure (v1 shape;
// a Process class wraps this in the prelude)
class Process {                      // prelude, in-language over the floor
    int pid; int inFd; int outFd; int errFd;
    OutStream-ish stdin  (sysSend-style writes -> write(2) on the pipe)
    onStdout((string) => void)       // rides sysWatch — pipes are fds; the
    onStderr((string) => void)       //   existing read-watch loop just works
    Promise<int> exitCode()          // see reaping, below
    void kill()                      // sysKill(pid, SIGTERM)
}
```

- Floor: `pipe2` ×3, `fork`, child: `dup2` + `execve(path, argv, envp)`, parent:
  close child ends. **No PATH search in v1** (explicit path; documented — the
  honest boundary; a PATH-walking `findExecutable` helper can be in-language
  later over sysEnv+sysStat).
- **Reaping without blocking the loop:** `pidfd_open(pid)` (syscall 434) returns
  a pollable fd → register with the existing read-watch → on readiness,
  `waitid(WNOHANG)` collects the code and resolves the Promise. No SIGCHLD
  handler (signals + the loop is a can of worms deliberately avoided), no
  blocking waitpid. `sysKill(int pid, int sig) -> int` rounds it out.
- ELF phase 2 (after interpreter phase proves the surface): fork/execve/pipe2/
  dup2/pidfd_open/waitid syscalls emitted; envp already captured for F1.

## 8. P-probes

- **P1:** `main.cpp` arg-consumption reality (argv.md documented it; re-verify
  on current tree — flags moved since).
- **P2:** RuntimeLoop structure read-through: where fds/watches/timers live, one
  paragraph in the log (it is the substrate for F5/F7; the ELF loop likewise in
  X64Gen — find by grepping the sysWatch fixup name).
- **P3:** optional-return recipe status from Track 04 (C2); if 08 starts first,
  run 04's P1 probe here for `sysEnv`.
- **P4:** `getrandom`/`pidfd_open` availability on the dev kernel (6.17 — both
  fine; probe is a formality for CI parity).

## 9. Foreseeable problems & solution strategies

| # | problem | strategy |
|---|---|---|
| 1 | **`--` separator ambiguity** with `--project`/mode flags (`lang --run app.ext -- -v x` vs `lang --project m.mf -- args`). | argv.md owns the grammar; implement exactly its ruling (everything after `--` is the program's; before it, unrecognized-arg-is-source-path behavior is unchanged). Add main.cpp usage-string cases + a checker-free CLI test script (`tests/run_project.sh` pattern). |
| 2 | **ELF event-loop write-watch surgery** — the emitted loop is machine code; adding an fd set risks destabilizing timers/sockets corpus. | Land interpreter-first (the rule); before touching X64Gen, snapshot `run_elf.sh` green; make the loop change behind a "no write-watches registered → identical code path" guarantee so existing programs are provably unaffected; sockets/timers corpus + lcurl on ELF is the regression net. If the emitted loop's structure can't take a second set without rework, STOP (loop rework is portable-pivot territory — LLVM backend interactions). |
| 3 | **fork() in a process with live interpreter state** (oracle/IR run inside the compiler binary — fork copies everything; exec makes it fine, but a failed exec must `_exit(127)` immediately, never return into two interpreters). | Child path: exec-or-_exit(127), nothing else (no allocation between fork and exec — async-signal-safety discipline). Parent maps 127-exit + pipe-EOF to a spawn-failure signal on the Promise. |
| 4 | **Zombie/fd leaks across error paths** (spawn fails mid-pipes; process killed; program exits with children live). | The Process wrapper owns all 4 fds + the pidfd; close-all on exitCode-resolution and on kill; churn-style corpus (spawn-cancel loop, +0 fd growth — assert via /proc/self/fd count in the test script, not in-language). Program exit with live children: document (children are not reaped — standard Unix; no magic). |
| 5 | **`exit()` vs ARC/meter invariants** — skipping cleanups on exit may read as leaks in tooling. | §1's decision: meter skipped on explicit exit; `--mem-verify` runs never call exit in corpus; document in reference §6.6.5x that exit forgoes the exit-report. |
| 6 | **Env access vs comptime hermeticity** (comptime denies sys floor — new natives must be in the deny-set). | Find the comptime deny list (grep the sys-denial in the comptime evaluator) and add every new native there in the same commit — one forgotten name breaks build hermeticity silently. Checklist item per native in every milestone. |
| 7 | **IPv6 literal parsing** drift between floor (bare address) and URL code (brackets). | The floor takes bare addresses only; bracket handling stays in lcurl/url code. One corpus probe per form. |

## 10. Milestones & acceptance

| M | deliverable | accept |
|---|---|---|
| M1 | F1 argv/env/exit on oracle+IR+CGen-policy; main.cpp `--`; comptime deny-list | args_env.ext corpus (via run-script passing args); lcurl reads argv when present (stdin fallback retained — cli.ext gains ~5 lines); exit-code checked in a shell test |
| M2 | F1 on ELF (_start argv/envp, exit syscall, meter-skip + suppress flag) | run_elf on args_env; `echo $?` test; curl-design §2.7#2 rider closed |
| M3 | F2 time/random + F4 isatty (all engines incl. ELF) | time monotonicity + random-length corpus (no golden values — property checks) |
| M4 | F3 dirs (interpreters; ELF getdents64 stretch) | dir round-trip test script (mkdir/write/list/remove in a temp dir) |
| M5 | F5 socket upgrades + prelude connectTimeout + TcpStream.send drain | connect-timeout corpus vs a blackholed port (test server refuses accept); sockets corpus unregressed; ELF write-watch per problem #2 gate |
| M6 | F7 spawn on oracle+IR (+ Process prelude class) | spawn corpus: run `/bin/echo`, capture stdout via onStdout, exitCode==0; failure path (bad path → 127-mapped) |
| M7 | F6 DNS phase 1 | resolve probe (localhost); ELF diagnostic pinned |

Target: **Jul 20 – Aug 7** (M1–M2 4d, M3 1d, M4 2d, M5 4d, M6 3d, M7 1d).

## 11. Reference-doc duty

reference §6.6.5 (sys floor table — every new native), new §6.6.58 (env/args/
exit), §6.6.59 (Process), §7.3 ELF coverage notes (DNS deferral, meter
suppression); curl-design §9 verdict-table rows get "landed" annotations (edit
the copy in examples/curl only if asked — the design doc is historical record;
instead note closures in bug.md-adjacent docs? NO: record closures in THIS
design's log + overview roadmap).

## 12. STOP conditions

- Problem #2's loop rework trigger.
- argv grammar needs to deviate from argv.md's ruling (that doc is owner-ruled;
  deviations escalate, not improvise).
- Any signal-handler temptation (SIGCHLD et al.) — the design's pidfd route
  exists precisely to avoid it.

## 13. Implementation log

### 2026-07-10 — F1(env)/F2/F3/F4/F6-phase1 on the interpreters

**Landed** (oracle + IR, via the shared `RuntimeNatives.cpp::nativeFreeCall` —
one edit covers both engines, the track's interpreter-first sequencing rule),
with `namespace std` prelude declarations and comptime hermeticity that falls
out of the `sys*`-prefix gate automatically (problem #6 — verified: a
`comptime std::sysMonotonic()` and a `comptime env::get(...)` both hit
"comptime code may not perform I/O"):

| feature | native(s) | notes |
|---|---|---|
| **F1** (env) | `std::sysEnv(string) -> string?` + `env::get` wrapper | argv/exit already landed (designs/complete/argv.md, exit-codes.md); `sysEnv` was the one genuine F1 gap. `None` = unset, distinct from set-but-empty. |
| **F2** (time) | `std::sysMonotonic() -> int` | `CLOCK_MONOTONIC` ms; sibling of the existing `sysNow`. |
| **F2** (random) | `std::sysRandom(int n) -> string` | `getrandom(2)`, loops on short reads/EINTR; `n<=0`→`""`, `n>1MB`→RuntimeException. |
| **F3** (dirs) | `sysMkdir` / `sysRemove` / `sysRename` / `sysListDir` | scalar 0/-1 for the first three (`sysRemove` unlinks, falls back to `rmdir` on EISDIR/EPERM); `sysListDir -> Array<string>?` (no `.`/`..`; `None` = not a dir, distinct from `[]`). |
| **F4** (isatty) | `std::sysIsTty(int fd) -> bool` | `isatty(3)`. |
| **F6** (DNS, phase 1) | `std::sysResolve(string) -> string?` | `getaddrinfo`, first A record as a dotted-quad; `None` on failure. localhost→127.0.0.1 verified. |

**Files:** `src/RuntimeNatives.cpp` (the nine native cores + three includes:
`<dirent.h>`, `<netdb.h>`, `<sys/random.h>`), `src/Resolver.cpp` (`namespace std`
declarations + the `env::get` wrapper), `src/Parser.cpp` (the fix below).

**Parser fix (prerequisite for `env::get`).** `get`/`set` are keyword tokens
(`KwGet`/`KwSet`, for accessors). The **class-member** parser already accepts
them as ordinary member names via `isContextualName` (`void get(...)` on
`HttpClient` works), but the **top-level / namespace** function parser
(`parseTopLevelItemInner`) required the function/var *name* to be a plain
`Identifier` — so a prelude `string? get(string) => ...` mis-parsed, and because
prelude parse errors are silently swallowed (`DiagnosticSink dummy`, "the prelude
is trusted"), the function simply vanished and `env::get` was unresolvable at
runtime. Fix: the top-level parser now accepts a contextual-name keyword as the
name too, exactly as the member parser does (two lines: the `declOrFunc`
lookahead and the name-consume). This is the same rule applied uniformly, not a
new special case — it also un-hid any namespace function named `get`/`set`/`is`/
`in`/… Validated: user-level `namespace Foo { string get(string k) => ... }` now
resolves; the full unit-test suite (parser/resolver/checker/eval/meta) and the
whole corpus stay green on both interpreters.

**Tests:** `tests/corpus/sys_natives.lev` (+`.expected`) — property checks only
(monotonic ordering, random length, isatty self-consistency, env-unset→None,
listDir non-dir→None), environment-neutral so it lives in the golden corpus
(picked up by `corpus_treewalk`/`corpus_ir`). `tests/run_sysnatives.sh` (CTest
`sys_natives`) — the environment-dependent halves: env PATH, DNS localhost, a
filesystem round-trip (each identical on oracle+IR), comptime hermeticity, and
the compiled-backend clean-deferral assertion.

### Deferred (documented, not attempted) — why this track is NOT complete

This session landed the interpreter-first slice of F1(env)/F2/F3/F4/F6-phase1.
The remaining milestones are genuinely large and/or blocked by standing
guardrails, so per the design's own STOP guidance they are recorded here rather
than half-built:

- **Compiled backends (emit-C++, LLVM) for the new natives.** They currently
  emit clean coverage-errors naming the native (verified) — the design's stated
  policy for emit-C++ ("skips the system layer by design — keep its clean
  coverage-errors"), and the "clean diagnostic" deferral pattern for the rest.
  A full LLVM landing is a deeper multi-layer job (`runtime/lv_plat_posix.c` +
  `runtime/lv_plat_win32.c` parity → `runtime/lv_runtime.c` `lvrt_*` wrapper →
  `LlvmGen.cpp` declaration + dispatch, then a `liblvrt.a` rebuild), to be done
  per-native when a consumer needs it (the `sysNow` precedent). No consumer
  forces it today.
- **ELF / X64Gen (M2 and all ELF halves of M3–M7).** The X64Gen backend is
  **frozen** (portable pivot: LLVM is the real machine-code backend); no new work
  goes on `X64Gen.cpp`/`X64.hpp`. The design's ELF instructions predate the
  pivot. ELF programs using these natives get the existing clean deferral.
- **F5 — socket-floor upgrades (`sysTcpConnectNb`/`sysWatchWrite`/
  `sysConnectResult`, IPv6, `connectTimeout`).** Deliberately not started: it
  requires adding a second (write) fd-set to the event loop, which the design
  itself gates behind a **STOP condition** (problem #2 / §12) and which is
  portable-pivot territory (LLVM loop interactions). Needs its own focused pass.
- **F7 — `sysSpawn` + `Process`.** Not started: fork/execve/pipe2/dup2 plus
  non-blocking reaping via `pidfd_open`+`waitid` wired into the event loop, and a
  prelude `Process` class riding `sysWatch`. Larger than the rest of the track
  combined and touches the loop substrate.

Because M5–M7 and the machine-code backends remain, **this design is not moved to
`designs/complete/`** — it stays at `designs/` top with this log recording the
landed subset and the precise remaining work.

### 2026-07-10 (second pass) — F5 + F7 landed; track complete on the active engines

**F5 — socket-floor upgrades (M5).** Interpreter-first per the sequencing rule,
then the LLVM half in the same pass (forced by a real consumer — see below):

- **P2 probe (the promised loop read-through):** `RuntimeLoop` (interpreters)
  keeps two flat vectors — `timers_` (due-time ordered at batch time) and
  `watches_` (fd + callback); `nextBatch()` polls all watch fds up to the
  earliest timer deadline, snapshots ready ids before firing (callbacks may
  mutate the registries), and auto-retires `POLLNVAL` watches. The compiled
  runtime's loop (`runtime/lv_loop.c`) is a faithful C port of the same
  structure driving `lv_plat_poll`. Both took a write-kind flag on the watch
  record with the "no write watches ⇒ bit-identical poll set" guarantee —
  problem #2's surgery was **not** needed (that STOP concerned the frozen
  X64Gen-emitted loop, which the pivot removed from scope).
- **Floor:** `sysTcpConnectNb` (O_NONBLOCK *before* connect; fd back even on
  EINPROGRESS), `sysWatchWrite` (same registry/id space/cancel as read
  watches; ERR/HUP wake it so a refused connect delivers its verdict),
  `sysConnectResult` (SO_ERROR). **IPv6** (F5.5): a bare `:`-bearing literal
  selects AF_INET6 on `sysTcpConnect`/`sysTcpConnectNb`; listen stays v4
  (it takes no bind address — nothing to rule on).
- **`sysSend` contract sharpened** (F5.6 prerequisite): bytes written; **-1
  retryable** (would-block/EINTR, nothing written), **-2 fatal** (peer gone).
  The drain needs the split — a write-watch keeps firing on a dead fd, so
  "retry" and "give up" must be distinguishable at the floor. Existing
  consumers only test `< 0` (lcurl's sendAll stall-retries either way), so
  the refinement is compatible. `sysSend`/`sysRecv` also gained the
  `ENOTSOCK → write(2)/read(2)` fallback: pipes are fds (F7 rides this).
- **Prelude:** `TcpStream.send` is now queue-and-drain (curl-design §2.1
  closed: no silent short writes — 512KB through one `send()` verified
  byte-exact on oracle, IR, and an LLVM-native binary); `(<<)` routes through
  it. `std::connectTimeout(host, port, ms, cb)` composes nb-connect +
  write-watch + SO_ERROR + a one-shot Timer as a one-shot in-language state
  machine (`ConnectAttempt`) — the streams-philosophy payoff the design
  promised; lcurl's `--connect-timeout` verdict flips when examples adopt it.
- **LLVM half (not deferred — a consumer forced it):** the drain lives in
  `TcpStream.send`, which every socket program reaches, so the previously-
  compiling LLVM corpus (sockets/http/async) would have regressed to
  coverage-errors. Landed: `Watch.write` in `lv_loop.c` + per-slot readiness,
  `lvrt_syswatchwrite`/`lvrt_systcpconnectnb`/`lvrt_sysconnectresult`,
  `lv_plat_tcp_connect_nb`/`lv_plat_connect_result` + the -1/-2 send contract
  + IPv6 on BOTH plat floors (posix + win32; win32's async-connect blind spot
  (B-H8, pre-2004 WSAPoll) degrades to the timer path — documented, never a
  hang), ERR/HUP→LV_POLLOUT in both poll mappings, LlvmGen declarations +
  dispatch. `lvrt_sysmonotonic` rode along (5 lines over the existing
  `lv_plat_now_ns`; deadline flows measure elapsed with it).

**F7 — sysSpawn + Process (M6).** Interpreters only (no compiled consumer;
clean deferral asserted):

- **Floor:** `sysSpawn(path, args) -> [pid, stdinFd, stdoutFd, stderrFd]`
  (`[]` = spawn failure = pipes/fork only). `pipe2(O_CLOEXEC)` ×3; argv built
  BEFORE fork; child does dup2 ×3 + `execve(path, argv, environ)` +
  `_exit(127)` — nothing else, no allocation (problem #3 discipline). Parent
  ends CLOEXEC + non-blocking. SIGPIPE → SIG_IGN once at first spawn (a
  disposition, not a handler — §12's rule is about SIGCHLD reentrancy; EPIPE
  now surfaces as -2 from sysSend instead of killing the process).
  `sysPidfdOpen` (pollable exit signal), `sysReap` (waitid WNOHANG: -1
  running; exit code; 128+sig if signaled — shell convention), `sysKill`
  (**pid ≤ 0 refused at the floor** — kill(2)'s group/broadcast forms are
  never exposed to programs).
- **Prelude `Process`:** the three pipes are internal `TcpStream`s (pipes are
  fds — stdin gets queue-and-drain, stdout/stderr ride the ordinary
  read-watch loop; zero new stream machinery). `exitCode()` lazily opens the
  pidfd and watches it like any fd; on readiness `sysReap` resolves the
  `Promise<int>` — **after pumping any output still buffered in the pipes**
  (the child's last write can race the pidfd readiness; `TcpStream.pumpAll`
  drains before the fds close), then closes all owned fds (problem #4: the
  8-round spawn-churn test holds the fd table at ±0 via /proc/self/fd).
  Spawn failure resolves 127 (problem #3's mapping). No `exitCode()` call ⇒
  no reap machinery ⇒ the program may exit with live children (documented,
  standard Unix). `kill()` = SIGTERM + stdin close; with a pending
  `exitCode()` the promise resolves 143. pidfd-less kernels fall back to a
  20ms poll-reap timer — still no signals, still non-blocking.

**Hermeticity (problem #6):** all seven new natives are `sys*`-prefixed —
comptime denial verified for `sysSpawn` (and `sysMonotonic` before it) in the
run script.

**Tests.** Golden corpus `corpus/sys_natives/sys_spawn.lev` (sequenced
echo-argv / cat-stdin-EOF / badpath-127 — deterministic, both interpreters).
`run_sysnatives.sh` grew: connectTimeout success/refused/blackhole (TEST-NET-3;
the assertion is route-independent), 512KB drain byte-exact on oracle+IR+LLVM-
native, kill→143, 8-round spawn fd-churn ±0, comptime `sysSpawn` denial,
emit-C++/LLVM clean spawn deferral, and §5's LLVM sysMonotonic check flipped
from "defers" to "covered". Full suite: **106/106 green** (including
corpus_llvm/_full, corpus_win_wine, corpus_core_aarch64_qemu).

**Milestone ledger:** M1 ✅ (F1: argv/exit prior tracks, env this one) · M2 —
ELF, frozen-superseded · M3 ✅ (F2+F4; ELF half superseded) · M4 ✅ (F3;
getdents64 stretch superseded) · M5 ✅ (F5 on oracle/IR/LLVM; ELF half
superseded) · M6 ✅ (F7 on oracle/IR) · M7 ✅ (F6 phase 1).

**Ruling: the track is complete as scoped under the portable pivot** — every
feature family runs on the semantic-reference engines (F5 also on LLVM), and
what remains is standing policy (X64Gen frozen; per-consumer LLVM coverage for
env/random/dirs/tty/DNS/spawn; F6 phase 2 on the roadmap), not unstarted
design work. Moved to `designs/complete/` per the exit-codes precedent, with
this log as the record.
