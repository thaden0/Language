# Process exit codes — Technical Design

**Status:** implemented. **Date:** 2026-07-07. **Completed:** 2026-07-09.
**Motivating case:** any CLI tool, kilo included — `kilo: no such file` must exit
non-zero so a shell/`make`/CI can tell success from failure. **Related:**
`argv.md §12` explicitly deferred exit to “its own short design” — this is it.
`terminal-raw-mode.md §3.4` and `signals.md §3.4` share the exit epilogue this
doc defines.

> **Supersedes** the exit sketch in `designs/complete/techdesign-08-system-natives.md §1`
> (F1), which is pre-pivot: it specifies “ELF: `syscall(60, code)`” and capturing
> exit through “X64Gen’s `_start` emission,” i.e. **new `X64Gen` work** — which
> the portable pivot forbids (`X64Gen` is frozen). This design re-grounds exit in
> the *current* runtime (`lv_rt_exit_code`, the runtime-owned `lv_entry.c` entry)
> and defers ELF instead of extending it. The `env.exit` shape and the “exit
> means exit” semantics are carried forward.

Verified against this worktree (`agent3`, Jul 7). Implemented in the runtime,
tree-walk/IR interpreters, LLVM backend, emit-C++ backend, prelude surface,
reference docs, and `tests/run_exit.sh` differential coverage. The pure ELF
backend remains frozen and reports the designed deferral for `sysExit` /
`sysSetExitCode`.

---

## 1. Current state (verified)

Exit codes are **hardcoded 0.**

```c
// runtime/lv_runtime.c:1393
int lv_rt_exit_code(void) {
    /* X64Gen's generated _start unconditionally exits 0 regardless of an
     * uncaught throw (src/X64Gen.cpp:3813), and Eval::run's oracle return
     * doesn't distinguish either — matched here rather than invented. */
    return 0;
}
```

- `runtime/lv_abi.h:507` declares it `/* always 0 for now */`.
- The runtime-owned entry already **returns it**: `runtime/lv_entry.c:39`,
  `return lv_rt_exit_code();`. So the seam to make it settable is one function.
- **Two real gaps, both cited in that comment:** (a) a program cannot set its own
  exit code, and (b) an **uncaught exception still exits 0** — a program that
  crashed reports success. Both are fixed here.
- The floor primitive already exists: `void lv_plat_exit(int code)`
  (`lv_plat.h:28`, `lv_plat_posix.c:43` `_exit(code)`). No new floor call is
  needed for exit — only for *routing a code into it*.

## 2. Two capabilities, one small surface

- **Set-and-complete** (the common case): a program sets its exit code and lets
  execution finish normally (loop drains, cleanups run). `env.setExitCode(n)`.
- **Exit-now** (curl/kilo error paths): terminate immediately with a code,
  abandoning pending loop work. `env.exit(n)` — “exit means exit”
  (techdesign-08’s phrase, kept).

Plus one correctness fix that needs no surface: **an uncaught exception exits 1.**

## 3. Native + language API

### 3.1 Runtime seam (no new floor primitive)

```c
/* runtime/lv_runtime.c: a settable global backing lv_rt_exit_code. */
static int g_exit_code = 0;
void lv_rt_set_exit_code(int code) { g_exit_code = code & 0xFF; }   /* Unix 8-bit */
int  lv_rt_exit_code(void) { return g_exit_code; }                 /* was: return 0 */
```

`lv_entry.c:39` already returns `lv_rt_exit_code()`, so **set-and-complete works
for compiled programs the moment the global is threaded** — no entry change.
The uncaught reporter (`lvrt_uncaught`, the path the old comment cites) calls
`lv_rt_set_exit_code(1)` before printing the traceback — closing gap (b).

`env.exit(n)` is immediate and routes through the shared epilogue:

```c
/* runtime: restore terminal (raw-mode §3.4), then hard-exit. No loop drain. */
void lvrt_sysexit(const LvValue* code) {
    lvrt_term_shutdown();               /* restore cooked mode if raw (§3.4) */
    lv_plat_exit((int)(code->payload & 0xFF));
}
```

### 3.2 Native surface

```
namespace std {
    void sysExit(int code);          // native: immediate termination (never returns)
    void sysSetExitCode(int code);   // native: set the code for normal completion
}
```

`sysExit` is typed `void` but never returns (the language has no `Never`;
documented, as techdesign-08 did). Both are `sys*` ⇒ comptime-denied
automatically (`Eval.cpp:358`) — a build cannot terminate the compiler or set a
build-time exit code.

### 3.3 Language surface — `env` (argv’s namespace, process facts)

Exit is a process fact, so it joins argv under the landed top-level
`namespace env` (`argv.md §4`) rather than a new namespace:

```
namespace env {
    // ...args(), name() (argv.md)...
    void exit(int code)        => std::sysExit(code);        // terminate now
    void setExitCode(int code) => std::sysSetExitCode(code); // finish, then this code
}
```

kilo’s error path (and the general CLI shape):

```
Array<string> a = env::args();
if (a.length() != 2) {
    console.writeln("usage: " + a[0] + " <file>");
    env::exit(64);                 // EX_USAGE — no loop work to drain, exit now
}
```

(Considered `std::exit` per techdesign-08: rejected only for namespace
consistency — argv already put process facts under `env::`, and one home beats
two. Mechanically identical.)

## 4. Per-backend coverage

| engine | primacy | set-and-complete | exit-now |
|---|---|---|---|
| **LLVM** (`--build-native`) | **primary** | `lvrt_syssetexitcode` → `g_exit_code`; `lv_entry.c` returns it | `lvrt_sysexit` → epilogue → `lv_plat_exit(code)` |
| tree-walk / IR (`--run`/`--ir`) | interp | code threaded out of `Eval::run` to `main.cpp`’s process exit | clean unwind (§5), not a raw `_exit` |
| emit-C++ (`--build`) | port | `g_exit_code`; generated `main` returns it (was `return 0`, `CGen.cpp:935`) | `rt_sysexit` → epilogue → `exit(code)` |
| pure ELF (`--emit-elf`) | **frozen** | **deferral diagnostic** | **deferral diagnostic** |

**Interpreters must not raw-`_exit`.** `--run`/`--ir` run *inside* the
`leviathan` process and capture stdout in a sink flushed at the end
(`Eval::run`’s `out_`). A `_exit(code)` mid-run would discard that buffer and
skip the terminal restore. So `sysExit` on the interpreters sets an
exit-requested flag + code and performs a **clean unwind** back to `Eval::run`
(the same return path an uncaught throw uses, but non-error); `Eval::run` returns
the captured output **and** the code; `main.cpp` prints, then `std::exit(code)`.
This is the “tear down cleanly enough not to corrupt” techdesign-08 asked for,
made concrete (and it flushes — a plain `_exit` would not).

**Frozen ELF:** `lv_plat_exit` exists, but wiring `sysExit`/`sysSetExitCode` into
`X64Gen`’s `CallNativeFn` is new codegen — forbidden. Both natives hit the native
fallthrough → clean `native floor function 'sysExit'` deferral (`argv.md §5.4`;
`X64Gen.cpp:3854`). An ELF program keeps today’s always-0 behavior; portable CLIs
target LLVM. **Do not edit `X64Gen.cpp`.** (This is the specific correction to
techdesign-08 §1, which wanted `syscall(60)` emitted in `X64Gen`.)

## 5. Edge cases

- **8-bit truncation**: Unix exit status is `code & 0xFF`. `env.exit(256)` → 0,
  `env.exit(257)` → 1, `env.exit(-1)` → 255. Masked in `lv_rt_set_exit_code` and
  `lvrt_sysexit` so every engine agrees; documented (a program wanting a portable
  code stays in 0–125, the conventional safe range).
- **Uncaught exception ⇒ exit 1** (gap b). An unhandled throw at top level now
  exits 1 on every non-frozen engine — matching every other language and letting
  `set -e`/CI catch a crash. Explicitly a behavior *change* from today’s silent 0.
- **`setExitCode` then normal end vs `exit` now**: `setExitCode` lets the event
  loop drain (pending timers/promises finish); `exit` abandons them. Both
  documented; the drain-vs-abandon distinction is the whole reason for two calls.
- **Multiple sets**: last `setExitCode` wins; first `exit` wins (it doesn’t
  return).
- **`exit` with raw mode on**: the epilogue restores the terminal first (§3.1),
  so a TUI that `env.exit`s from an error path doesn’t orphan the terminal —
  the same guarantee as `terminal-raw-mode.md §3.4`, reached through this path.
- **exit vs pending stdout**: compiled `lv_plat_write` is unbuffered, so there is
  nothing to flush; the interpreter path flushes its sink via the clean unwind
  (§4). No lost output on either.

## 6. Phased plan

- **P0 — runtime seam:** `g_exit_code` + `lv_rt_set_exit_code`; make
  `lv_rt_exit_code` return it; `lvrt_uncaught` sets 1. Compiled set-and-complete
  + uncaught-nonzero work immediately (`lv_entry.c` already returns it). A
  `selftest.c` assertion.
- **P1 — natives + `env` surface:** `sysExit`/`sysSetExitCode` in the runtime and
  `nativeFreeCall`; `env.exit`/`env.setExitCode` in the prelude. Interpreters get
  the clean-unwind exit (§5) and thread the code out of `Eval::run` to `main.cpp`.
- **P2 — LLVM primary:** `lvrt_sysexit`/`lvrt_syssetexitcode` + `LlvmGen` wiring.
- **P3 — emit-C++:** `rt_sysexit`/`g_exit_code`; generated `main` returns
  `g_exit_code` (was `return 0`), uncaught sets 1.
- **(frozen) ELF:** deferral only.

## 7. Testing — `$?` across engines

- **Shell differential** (`tests/run_exit.sh`): programs that `env.exit(3)`,
  `env.setExitCode(7)` then finish, and fall off the end; run each on `--run`,
  `--ir`, `--build-native`, `--build` and assert `$?` is `3`, `7`, `0`
  respectively — identical across the four non-frozen engines.
- **Uncaught ⇒ 1**: a program that throws uncaught asserts `$? == 1` on all four
  engines (the gap-(b) fix), plus the traceback still prints.
- **8-bit mask**: `env.exit(257)` asserts `$? == 1`; `env.exit(-1)` asserts
  `$? == 255` — same on every engine.
- **exit-now abandons the loop**: a program that schedules a 10s timer then
  `env.exit(0)` returns immediately (asserts wall-clock < 1s), proving no drain.
- **Terminal restore on exit-now** (pty): a raw-mode program that `env.exit`s
  leaves the pty cooked (shared §3.4 guarantee).
- **comptime**: `comptime env.exit(1)` fails via the `sys*` hermeticity deny.
- **ELF**: `--emit-elf` asserts the clean `sysExit` deferral; the produced binary
  (of a program not using exit) still exits 0 as today.
