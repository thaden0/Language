#!/usr/bin/env bash
# Terminal floor completion differential (designs/techdesign-terminal-floor.md §7).
#  - winsize non-tty degradation -> 24x80 across --run/--ir/--build/--build-native
#  - winsize on a real pty with a set size (TIOCGWINSZ read-back)
#  - signal::on(USR1) self-kill delivery, two-subscriber fanout, WINCH coalescing
#  - SIGTERM under raw mode restores the terminal (safety handler), pty-verified
#  - emit-C++ compiles sysWinSize fully but rejects the signal stream (loop-bound)
#  - comptime hermeticity: sysWinSize / sysSignalOpen are sys*-denied
#
# usage: run_terminal_floor.sh <leviathan-binary> <corpus-dir>   (dir holds floor/*.lev)
set -u
bin="$1"; dir="$2"; d="$dir/floor"
here="$(cd "$(dirname "$0")" && pwd)"
fail=0; skip=0
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT

check() { if [ "$3" != "$2" ]; then echo "FAIL $1"; diff <(echo "$2") <(echo "$3")|head; fail=1; else echo "ok   $1"; fi; }

# --- 1. winsize non-tty degradation: no tty -> the 24x80 default ---------------
DEF="24x80"
check "winsize run  (non-tty)" "$DEF" "$(printf '' | "$bin" --run "$d/winsize.lev" 2>&1)"
check "winsize ir   (non-tty)" "$DEF" "$(printf '' | "$bin" --ir  "$d/winsize.lev" 2>&1)"
cxx=""; for c in clang++ g++ c++; do command -v "$c" >/dev/null 2>&1 && { cxx="$c"; break; }; done
if [ -n "$cxx" ] && "$bin" --build "$work/win_cpp" "$d/winsize.lev" >/dev/null 2>&1; then
  check "winsize cpp  (non-tty)" "$DEF" "$(printf '' | "$work/win_cpp" 2>&1)"
else echo "SKIP winsize cpp (no compiler / emit)"; skip=$((skip+1)); fi
rt="$(dirname "$bin")/liblvrt.a"; have_llvm=0
if [ -f "$rt" ] && [ -n "$cxx" ] && "$bin" --build-native "$work/win_llvm" "$d/winsize.lev" 2>/dev/null; then
  have_llvm=1
  check "winsize llvm (non-tty)" "$DEF" "$(printf '' | "$work/win_llvm" 2>/dev/null | grep -v '^\[heap\]')"
else echo "SKIP winsize llvm (no liblvrt.a/linker)"; skip=$((skip+1)); fi

# --- 2. emit-C++: sysWinSize is full; the signal stream is loop-bound ----------
if [ -n "$cxx" ]; then
  sigcpp=$("$bin" --build "$work/sig_cpp" "$d/usr1.lev" 2>&1); rc=$?
  if [ $rc -ne 0 ] && echo "$sigcpp" | grep -q "sysSignal"; then echo "ok   cpp signal rejected (loop-bound)"
  else echo "FAIL cpp signal (expected sysSignal rejection, rc=$rc): $sigcpp"; fail=1; fi
else echo "SKIP cpp signal rejection (no compiler)"; skip=$((skip+1)); fi

# --- 2b. SU-1 in-memory stream close: runs on ALL FOUR engines (no signals) ----
# The InStream.close() reachability fix (CGen by-name marking gated on
# instantiated classes) is what lets emit-C++ compile a stream program that
# closes an InStream — previously it dragged in TaskGroup::close -> sysTaskCancel.
IMEXP=$'count=2\nafter=2\npull:stream is closed\ndone'
check "unsub inmem run"  "$IMEXP" "$("$bin" --run "$d/unsub_inmem.lev" 2>&1)"
check "unsub inmem ir"   "$IMEXP" "$("$bin" --ir  "$d/unsub_inmem.lev" 2>&1)"
if [ -n "$cxx" ] && "$bin" --build "$work/inmem_cpp" "$d/unsub_inmem.lev" >/dev/null 2>&1; then
  check "unsub inmem cpp"  "$IMEXP" "$("$work/inmem_cpp" 2>&1)"
else echo "FAIL unsub inmem cpp (expected emit-C++ to compile InStream.close now)"; fail=1; fi
if [ $have_llvm -eq 1 ] && "$bin" --build-native "$work/inmem_llvm" "$d/unsub_inmem.lev" 2>/dev/null; then
  check "unsub inmem llvm" "$IMEXP" "$("$work/inmem_llvm" 2>/dev/null | grep -v '^\[heap\]')"
else echo "SKIP unsub inmem llvm (no liblvrt.a/linker)"; skip=$((skip+1)); fi

# --- 2c. D-B/DM-3: InStream : IIterable<T> (stream iteration) -----------------
# Timer-fed for..in delivers every tick then exits at close(); iterator/
# subscribe/pull exclusivity; producer-EOF drains buffered items before
# closing; asSeq() joins the lazy Seq pipeline. Deterministic (no PTY): the
# waiter-promise blocking pull is real suspension (LA-30), not a pump-spin.
# emit-C++: Timer is unsupported there regardless (native 'sysTimerStart',
# pre-existing, independent of this design) — checked for the SAME honest
# per-native rejection the signal lane gets above, not a generic construct
# error (that generic error is exactly what the CGen by-name-exclusion fix
# this design adds prevents: a stream-touching program must still compile on
# emit-C++ when it never actually iterates, §5#6/§9 coordination note).
SIEXP=$'tick 1\ntick 2\ntick 3\ntick 4\ntick 5\nticks done count=5\niter-then-pull: consumer end is claimed by an iterator\nsub-then-iter: consumer end is claimed by a subscriber\ndrained=3\n10\n20\n30\nseq-take=3\n1\n2\n3'
check "stream iter run"  "$SIEXP" "$("$bin" --run "$d/stream_iter.lev" 2>&1)"
check "stream iter ir"   "$SIEXP" "$("$bin" --ir  "$d/stream_iter.lev" 2>&1)"
if [ -n "$cxx" ]; then
  sicpp=$("$bin" --build "$work/si_cpp" "$d/stream_iter.lev" 2>&1); rc=$?
  if [ $rc -ne 0 ] && echo "$sicpp" | grep -q "sysTimerStart"; then echo "ok   stream iter cpp rejected (Timer unsupported, honest diagnostic)"
  else echo "FAIL stream iter cpp (expected sysTimerStart rejection, rc=$rc): $sicpp"; fail=1; fi
else echo "SKIP stream iter cpp (no compiler)"; skip=$((skip+1)); fi
if [ $have_llvm -eq 1 ] && "$bin" --build-native "$work/si_llvm" "$d/stream_iter.lev" 2>/dev/null; then
  check "stream iter llvm" "$SIEXP" "$("$work/si_llvm" 2>/dev/null | grep -v '^\[heap\]')"
else echo "SKIP stream iter llvm (no liblvrt.a/linker)"; skip=$((skip+1)); fi

# --- 3. comptime hermeticity: sys*-prefixed floor calls are denied -------------
printf 'comptime int x = std::sysWinSize(1, 0);\nconsole.writeln(x);\n' > "$work/ctw.lev"
ct=$("$bin" --run "$work/ctw.lev" 2>&1); rc=$?
if [ $rc -ne 0 ] && echo "$ct" | grep -q "may not perform I/O"; then echo "ok   comptime hermeticity (sysWinSize)"
else echo "FAIL comptime sysWinSize (expected denial, rc=$rc): $ct"; fail=1; fi

# --- 4. pty end-to-end (winsize / signals / raw kill) --------------------------
if ! command -v python3 >/dev/null 2>&1; then
  echo "SKIP pty (no python3)"; skip=$((skip+1))
else
  drv="$here/floor_pty.py"
  pty_one() {  # <label> <mode-args...> -- <cmd...>
    local label="$1"; shift
    if out=$(python3 "$drv" "$@" 2>&1); then echo "ok   $label ($out)"
    else echo "FAIL $label ($out)"; fail=1; fi
  }
  # winsize read-back on a real pty
  pty_one "winsize pty run"  winsize 31 101 -- "$bin" --run "$d/winsize.lev"
  pty_one "winsize pty ir"   winsize 31 101 -- "$bin" --ir  "$d/winsize.lev"
  [ $have_llvm -eq 1 ] && { "$bin" --build-native "$work/win2_llvm" "$d/winsize.lev" 2>/dev/null && pty_one "winsize pty llvm" winsize 31 101 -- "$work/win2_llvm"; }
  # cursor-report (CPR) fallback on a 0x0 pty: TIOCGWINSZ fails, raw mode is on,
  # so term::size() runs the \x1b[999C\x1b[999B\x1b[6n probe and parses the reply.
  # The ONLY automated exercise of that path (known-bugs #96 was found by hand
  # because nothing covered it); LLVM is the lane that regressed.
  pty_one "cpr pty run"  cpr 31 101 -- "$bin" --run "$d/winsize_cpr.lev"
  pty_one "cpr pty ir"   cpr 31 101 -- "$bin" --ir  "$d/winsize_cpr.lev"
  [ $have_llvm -eq 1 ] && { "$bin" --build-native "$work/cpr_llvm" "$d/winsize_cpr.lev" 2>/dev/null && pty_one "cpr pty llvm" cpr 31 101 -- "$work/cpr_llvm"; }
  # signal delivery (plain pipe; SIGUSR1 == 10)
  pty_one "signal usr1 run"  signal 10 got=10 -- "$bin" --run "$d/usr1.lev"
  pty_one "signal usr1 ir"   signal 10 got=10 -- "$bin" --ir  "$d/usr1.lev"
  # two-subscriber fanout
  pty_one "fanout run"       signal 10 b=10   -- "$bin" --run "$d/fanout.lev"
  pty_one "fanout ir"        signal 10 b=10   -- "$bin" --ir  "$d/fanout.lev"
  # WINCH coalescing (storm 5 resizes -> >=1 tick, final 40x120)
  pty_one "winch run"        winch -- "$bin" --run "$d/winch.lev"
  pty_one "winch ir"         winch -- "$bin" --ir  "$d/winch.lev"
  # SIGTERM under raw mode restores the terminal
  pty_one "rawkill run"      rawkill -- "$bin" --run "$d/raw_term_kill.lev"
  pty_one "rawkill ir"       rawkill -- "$bin" --ir  "$d/raw_term_kill.lev"
  # SU-1 unsubscribe: close-stops-delivery + broadcast isolation + self-close
  # (send USR1 twice; closed sub A must not see the 2nd, sibling B still does)
  pty_one "unsub isolation run" signal2 10 "a=1,b=1,b=2,done" "a=2" -- "$bin" --run "$d/unsub_isolation.lev"
  pty_one "unsub isolation ir"  signal2 10 "a=1,b=1,b=2,done" "a=2" -- "$bin" --ir  "$d/unsub_isolation.lev"
  # SU-1 loop-drain exit: `using` last-out teardown releases the watch so a
  # one-shot program EXITS with no signal (a live watch would pin the loop)
  pty_one "unsub drain run"     drain "subscribed,after-scope" -- "$bin" --run "$d/unsub_drain.lev"
  pty_one "unsub drain ir"      drain "subscribed,after-scope" -- "$bin" --ir  "$d/unsub_drain.lev"
  if [ $have_llvm -eq 1 ]; then
    "$bin" --build-native "$work/usr1_llvm"   "$d/usr1.lev"   2>/dev/null && pty_one "signal usr1 llvm" signal 10 got=10 -- "$work/usr1_llvm"
    "$bin" --build-native "$work/fan_llvm"    "$d/fanout.lev" 2>/dev/null && pty_one "fanout llvm"      signal 10 b=10   -- "$work/fan_llvm"
    "$bin" --build-native "$work/winch_llvm"  "$d/winch.lev"  2>/dev/null && pty_one "winch llvm"       winch -- "$work/winch_llvm"
    "$bin" --build-native "$work/rawk_llvm"   "$d/raw_term_kill.lev" 2>/dev/null && pty_one "rawkill llvm" rawkill -- "$work/rawk_llvm"
    "$bin" --build-native "$work/iso_llvm"    "$d/unsub_isolation.lev" 2>/dev/null && pty_one "unsub isolation llvm" signal2 10 "a=1,b=1,b=2,done" "a=2" -- "$work/iso_llvm"
    "$bin" --build-native "$work/drain_llvm"  "$d/unsub_drain.lev" 2>/dev/null && pty_one "unsub drain llvm" drain "subscribed,after-scope" -- "$work/drain_llvm"
  fi
fi

echo "terminal floor differential done ($skip group(s) skipped)"
exit $fail
