# Terminal window size (TIOCGWINSZ + `\x1b[6n` fallback) — Technical Design

**Status:** proposal (nothing implemented). **Date:** 2026-07-07.
**Motivating case:** kilo — it must know the terminal's rows×cols to lay out its
screen and status bar. **Related:** `terminal-raw-mode.md` (shares the `term`
namespace and the fallback needs raw mode); `signals.md` (SIGWINCH turns a
one-shot size query into a live-resize stream). Follows `argv.md`’s floor-native
+ top-level-namespace shape and comptime rules.

Verified against this worktree (`agent3`, Jul 7) at the cited `file:line`.

---

## 1. Current state (verified)

No window-size query exists. `grep TIOCGWINSZ|winsize|ioctl runtime/ src/` finds
only socket `ioctlsocket` matches on Win32; the floor interface
(`runtime/lv_plat.h:22–70`) has no `ioctl` and no terminal call. A program has
no way to discover the terminal geometry — so a TUI cannot size itself.

## 2. A point-in-time size is a floor value; resize is a stream

`term.size()` is a plain floor query, exactly like `sysStat` returns a file's
size at a moment (`argv.md §2`). The size *changing over time* is an event
stream — SIGWINCH — and that is `signals.md`’s job; this doc provides the query
that a SIGWINCH tick re-runs. So: one floor native for the value, no event-loop
contact here.

## 3. Native + language API

### 3.1 Floor + native (primary path: `TIOCGWINSZ`)

```c
/* runtime/lv_plat.h (+ lv_plat_posix.c): fill *rows,*cols from the tty.
 * Returns 0 on success, -1 on failure (not a tty, ioctl error, or a 0x0
 * report — some multiplexers answer 0, treated as failure so the caller
 * falls back). termios/ioctl stay inside the POSIX floor file. */
int lv_plat_term_size(int fd, int* rows, int* cols);   /* ioctl(fd, TIOCGWINSZ, &ws) */
```

Native, following the field-indexed `sysStat` shape (`RuntimeNatives.cpp:253`,
`lv_runtime.c:1251` — one interim int per call until `Block` lands):

```
namespace std {
    int sysWinSize(int fd, int field);   // field 0=rows, 1=cols; -1 on failure
}
```

The native calls `lv_plat_term_size` once and returns the requested field (the
runtime may cache the pair across a 0/1 pair of calls, as `sysStat` re-stats per
call today — an acceptable interim). `sys*`-prefixed ⇒ comptime-denied
automatically (`Eval.cpp:358`).

### 3.2 Language surface — `term.size()` with the escape-sequence fallback

The `\x1b[6n` fallback belongs in the **language**, not the floor: it is
write-then-read-then-parse (move the cursor to the far corner, ask where it
landed), which the language does cleanly with `sysWrite`/`sysRead` and its
string ops — no new OS primitive. A `WinSize` value struct + the `term` wrapper
(same top-level `namespace term` as raw mode):

```
class WinSize { int rows; int cols; }     // value struct: copies by value (§9)

namespace term {
    // Primary: TIOCGWINSZ. On failure, the cursor-report fallback (needs raw
    // mode active — terminal-raw-mode.md — to read the reply un-cooked). If
    // both fail, a conventional 24x80 so the UI still draws.
    WinSize size() {
        int r = std::sysWinSize(1, 0);
        int c = std::sysWinSize(1, 1);
        if (r > 0 && c > 0) return WinSize(r, c);
        WinSize f = sizeByCursorReport();          // \x1b[999C\x1b[999B then \x1b[6n
        if (f.rows > 0 && f.cols > 0) return f;
        return WinSize(24, 80);
    }
    int rows() => size().rows;
    int cols() => size().cols;
}
```

`sizeByCursorReport()` (prelude helper, in-language): `sysWrite(1,
"\x1b[999C\x1b[999B\x1b[6n")` to push the cursor to the bottom-right and request
its position, then `sysRead(0, …)` and parse the `\x1b[<rows>;<cols>R` reply with
`indexOf`/`subStr`/`toInt` (all Track-04 string natives that already exist). It
is guarded by `isRaw()` (`terminal-raw-mode.md`): un-raw, the reply would be
line-buffered and the read would hang, so a non-raw failure returns `WinSize(0,
0)` and lets `size()` fall to the 24×80 default.

## 4. Per-backend coverage

| engine | primacy | size source |
|---|---|---|
| **LLVM** (`--build-native`) | **primary** | `lvrt_syswinsize` → `lv_plat_term_size` (`ioctl TIOCGWINSZ`); fallback runs in-language over `sysWrite`/`sysRead` |
| tree-walk / IR (`--run`/`--ir`) | interp | `nativeFreeCall "sysWinSize"` → `ioctl` directly; fallback identical (shared prelude) |
| emit-C++ (`--build`) | port | `rt_syswinsize` (`<sys/ioctl.h>`); fallback identical |
| pure ELF (`--emit-elf`) | **frozen** | **deferral diagnostic** (needs the `TIOCGWINSZ` ioctl syscall) — not extended |

**Frozen ELF:** `sysWinSize` hits the native fallthrough → clean `native floor
function 'sysWinSize'` deferral (`argv.md §5.4`; `X64Gen.cpp:3854`). Even though
the cursor-report fallback is pure writes/reads that ELF *could* run, the
primary native defers and the fallback needs raw mode (also ELF-deferred), so
`term.size()` is an LLVM/interpreter feature. **Do not edit `X64Gen.cpp`.**
**Win32:** `lv_plat_term_size` → `GetConsoleScreenBufferInfo` (`srWindow`), same
signature.

## 5. Edge cases

- **Not a tty / piped**: `ioctl` fails → fallback (which also fails un-raw) →
  `WinSize(24, 80)`. Never crashes; a redirected run still lays out.
- **0×0 report** (tmux/screen mid-attach): treated as failure by the floor (§3.1)
  so the fallback/default takes over, rather than dividing by a zero width.
- **Fallback requires raw mode**: documented; `size()` guards on `isRaw()` and
  degrades to the default otherwise (the reply can’t be read in cooked mode).
- **Resize between query and use**: inherent; the fix is to re-query on the
  SIGWINCH stream (`signals.md`) rather than cache-and-hope — cross-referenced,
  not solved here.
- **Huge/garbage reply** to `\x1b[6n`: the parser validates `\x1b[…R` shape and
  `toInt` returns `None` on non-digits → treated as fallback failure → default.
- **argv[0]/program identity**: irrelevant here (unlike `argv.md`), listed only
  to note winsize has no naming ambiguity.

## 6. Phased plan

- **P0 — floor:** `lv_plat_term_size` + POSIX `ioctl`; a `selftest.c` pty test
  (`TIOCSWINSZ` to set, read back).
- **P1 — interpreters:** `sysWinSize` in `nativeFreeCall`; `WinSize` + `term.size`
  (with fallback) in the prelude. `--run`/`--ir` report geometry.
- **P2 — LLVM primary:** `lvrt_syswinsize` + `LlvmGen` wiring.
- **P3 — emit-C++:** `rt_syswinsize`.
- **(frozen) ELF:** deferral only.

Depends on nothing except (for the fallback to be *useful*)
`terminal-raw-mode.md`; the primary `TIOCGWINSZ` path stands alone.

## 7. Testing — pty with a known geometry

- **Floor unit** (`selftest.c`): `openpty`, `ioctl(TIOCSWINSZ, {rows:40,cols:120})`,
  assert `lv_plat_term_size` returns `40,120`. Engine-independent.
- **Native/interpreter differential** (`tests/run_term.sh`, pty): set a known
  size, run a program printing `term.rows()`/`term.cols()`, assert it matches on
  `--run`, `--ir`, `--build-native`, `--build` (the five-engine rule minus ELF).
- **Fallback path**: run under a pty whose driver answers `TIOCGWINSZ` with 0×0
  but replies to `\x1b[6n` with a synthetic `\x1b[30;100R`; assert `size()`
  returns `30,100` — proves the parser and the raw-mode-guarded read.
- **Non-tty default**: piped stdin asserts `WinSize(24,80)` and no hang.
- **comptime**: `comptime term.size()` fails via the `sys*` hermeticity deny.
- **ELF**: `--emit-elf` asserts the clean `sysWinSize` deferral.
