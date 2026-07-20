#!/usr/bin/env bash
# designs/pty/ 03 §6 — the Windows ConPTY lane.
#
# NEVER byte goldens (pitfall #9): ConPTY re-renders, and its VT stream is
# version-dependent. Every assertion here is post-VT-strip content, an API
# result code, or "this finished at all" (a deadlock is a hang, not a wrong
# answer). The lane has three tiers, in decreasing order of what CI can pin:
#
#   Tier 1 (ALWAYS, the B-M5 standard / doc 03 §0 delivery standard):
#     the win32 floor compiles under the MinGW triple and archives cleanly.
#   Tier 2 (needs a runner — wine or a Windows host):
#     the argv->cmdline quoting table (risk W4) and the pre-1809 probe path
#     (§6.6) — neither needs a working ConPTY.
#   Tier 3 (needs a runner WITH a live ConPTY bridge):
#     §6.1-§6.5. Wine's ConPTY emulation is incomplete — it spawns and reaps,
#     but the child keeps writing to the inherited console instead of the
#     pseudoconsole pipes — so the bridge is PROBED and the content cases skip
#     LOUDLY there (risk W5: never a false green, never a hang). On a native
#     Windows host a dead bridge is a FAILURE, not a skip.
#
# usage: run_pty_win.sh <leviathan-binary> [triple] [runner]
#   triple defaults to x86_64-pc-windows-gnu; runner defaults to `wine` when
#   the host is not Windows, and to running the .exe directly when it is.
set -uo pipefail

bin="$1"
triple="${2:-x86_64-pc-windows-gnu}"
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

native_windows=0
case "${OSTYPE:-}" in msys*|cygwin*|win32*) native_windows=1 ;; esac
if [ "$#" -ge 3 ]; then
  runner="$3"
elif [ "$native_windows" = 1 ]; then
  runner=""
else
  runner="$(command -v wine || true)"
fi

fail=0
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

run_exe() {   # run_exe <exe> <args...>  — through the runner, if there is one
  if [ -n "$runner" ]; then WINEDEBUG=-all "$runner" "$@"; else "$@"; fi
}

# --- Tier 1: the MinGW compile + archive (the CI-enforced part) -------------
cc_prefix=x86_64-w64-mingw32
if ! command -v "$cc_prefix-gcc" >/dev/null 2>&1 && ! command -v clang >/dev/null 2>&1; then
  echo "SKIP pty-win: no cross C compiler for $triple (need $cc_prefix-gcc or clang)"
  exit 0
fi
if LVRT_OUT_DIR="$repo/runtime/$triple" bash "$repo/runtime/build-triple.sh" "$triple" \
     > "$work/archive.log" 2>&1; then
  echo "ok   floor compiles + archives for $triple"
else
  echo "FAIL floor build for $triple"; cat "$work/archive.log"; exit 1
fi

if [ -z "$runner" ] && [ "$native_windows" = 0 ]; then
  echo "SKIP pty-win tiers 2-3: no wine and not a Windows host (compile gate passed)"
  exit 0
fi

# --- Tier 2a: the argv -> cmdline quoting table (risk W4) ------------------
qcc="$cc_prefix-gcc"
command -v "$qcc" >/dev/null 2>&1 || qcc="clang --target=$triple"
if $qcc -std=gnu17 -O2 -Wall -Wextra -I"$repo/runtime" \
        "$repo/tests/win_pty_quote.c" -o "$work/quote.exe" -lws2_32 -lbcrypt \
        > "$work/quote_build.log" 2>&1; then
  if qout=$(run_exe "$work/quote.exe" 2>&1); then
    echo "$qout" | sed 's/^/     /'
    echo "ok   quoting table (W4)"
  else
    echo "FAIL quoting table (W4)"; echo "$qout"; fail=1
  fi
else
  echo "FAIL quoting-table build"; cat "$work/quote_build.log"; fail=1
fi

# --- build the behavioral driver -------------------------------------------
# It drives the FLOOR natives, not the `Pty` prelude class: `Pty` routes its
# master through TcpStream, whose IDisposable over-marking drags
# sysTaskCancel — still under the LA-30 tasks Windows reject — into the build
# (doc 03 §10). The floor is what this doc lands, and it is what this asserts.
if ! "$bin" --build-native "$work/drv" --target "$triple" \
        "$repo/tests/pty_win_driver.lev" > "$work/drv_build.log" 2>&1; then
  echo "FAIL behavioral driver build"; cat "$work/drv_build.log"; exit 1
fi
drv="$work/drv.exe"
echo "ok   behavioral driver builds for $triple"

# Only the driver's own result lines are the answer. An incomplete emulator
# lets the child write to the INHERITED console — i.e. onto this very stdout,
# mid-line, with no trailing newline — so the result is CUT OUT by its case
# prefix (grep -o) rather than matched as a whole line.
drv_lines='(probe reap|probe|echo|more|resize|kill|load): .*'
drv_run() {   # drv_run <case> [timeout-seconds]
  timeout "${2:-120}" env WINEDEBUG=-all LV_PTY_NO_CONPTY= \
    ${runner:+"$runner"} "$drv" "$1" 2>/dev/null | grep -oE "$drv_lines"
}
expect_line() {   # expect_line <label> <want> <got>
  if [ "$3" = "$2" ]; then echo "ok   $1"; else
    echo "FAIL $1: want [$2] got [$3]"; fail=1; fi
}

# --- Tier 2b: §6.6 the pre-1809 probe path ---------------------------------
# The floor's cached CreatePseudoConsole pointer is forced NULL: spawn must
# return [] (the language's ordinary spawn failure — a RUNTIME degrade, D-P8)
# and nothing may hang.
got=$(timeout 120 env WINEDEBUG=-all LV_PTY_NO_CONPTY=1 ${runner:+"$runner"} "$drv" probe \
        2>/dev/null | grep -oE "$drv_lines")
expect_line "§6.6 pre-1809 degrade" "probe: 0" "$got"

# --- Tier 3 gate: is there a live ConPTY, and does the bridge carry bytes? --
probe=$(drv_run probe)
if ! echo "$probe" | grep -q '^probe: 2'; then
  echo "SKIP §6.1-§6.5: no ConPTY on this host (got: $probe) — pre-1809 or an" \
       "emulator without CreatePseudoConsole"
  exit $fail
fi
# §6.4 and §6.5 need only spawn/kill/reap/teardown, which even an incomplete
# emulator gets right — assert them before the bridge gate.
expect_line "§6.4 kill -> LV_PTY_KILLED" "kill: 0 254" "$(drv_run kill)"
load=$(drv_run load 300)
expect_line "§6.5 teardown under load (no hang)" "load: ok true" "$load"

echo_out=$(drv_run echo)
if ! echo "$echo_out" | grep -q '^echo: true'; then
  if [ "$native_windows" = 1 ]; then
    echo "FAIL §6.1 echo (bridge carried no output on a native Windows host): $echo_out"
    fail=1
  else
    echo "SKIP §6.1-§6.3 content asserts: this runner's ConPTY is incomplete" \
         "(spawn+reap work, the child's output never reaches the pseudoconsole" \
         "pipes — the documented wine limitation, risk W5). got: $echo_out"
    exit $fail
  fi
else
  expect_line "§6.1 echo (stripped contains hi)" "echo: true 0" "$echo_out"
  expect_line "§6.2 more (write + ^Z EOF)"       "more: true true" "$(drv_run more)"
  expect_line "§6.3 resize rc"                   "resize: 0 254" "$(drv_run resize)"
fi

exit $fail
