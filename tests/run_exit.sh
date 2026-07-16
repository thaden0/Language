#!/usr/bin/env bash
# Process exit-code differential (designs/exit-codes.md §7). One `$?` contract
# checked across the four non-frozen engines (--run, --ir, --build, --build-native):
# - env.exit(3) / env.setExitCode(7)-then-finish / fall-off-the-end -> 3 / 7 / 0
# - an uncaught throw exits 1 (gap b) AND still prints the traceback
# - 8-bit truncation: exit(257) -> 1, exit(-1) -> 255 (masked so all engines agree)
# - exit-now abandons the event loop (a 10s timer never fires; wall-clock < 1s)
# plus the two negative cases the design calls for: the frozen ELF backend must
# DEFER cleanly (sysExit/sysSetExitCode), and `comptime env::exit(1)` must fail
# via the sys* hermeticity gate. A pty group (needs python3) verifies the
# terminal is restored on exit-now (the shared §3.4 epilogue).
#
# usage: run_exit.sh <leviathan-binary> <corpus-dir>   (dir holds exit/*.lev)
set -u
bin="$1"; dir="$2"; d="$dir/exit"
fail=0; skip=0
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT

# Toolchain probes (each engine group self-skips when unavailable, exactly like
# run_argv.sh / run_term.sh — the test stays meaningful on a minimal box).
cxx=""; for c in clang++ g++ c++; do command -v "$c" >/dev/null 2>&1 && { cxx="$c"; break; }; done
rt="$(dirname "$bin")/liblvrt.a"
have_cpp=0; have_llvm=0
[ -n "$cxx" ] && have_cpp=1
{ [ -f "$rt" ] && [ -n "$cxx" ]; } && have_llvm=1

# expect <label> <want-stdout> <want-rc> <actual-stdout> <actual-rc>
expect() {
  local label="$1" wo="$2" wc="$3" ao="$4" ac="$5"
  if [ "$ao" = "$wo" ] && [ "$ac" = "$wc" ]; then
    echo "ok   $label"
  else
    echo "FAIL $label: got rc=$ac want=$wc"; diff <(printf '%s' "$wo") <(printf '%s' "$ao") | head -8; fail=1
  fi
}

# run_all <prog.lev> <want-stdout> <want-rc>: assert (stdout,$?) identical on every
# non-frozen engine. Compiled binaries print a heap meter to stderr, so only
# stdout is compared (2>/dev/null); the interpreters capture stdout in the sink.
run_all() {
  local prog="$d/$1" wo="$2" wc="$3" tag="$1"
  local o
  o=$("$bin" --run "$prog" 2>/dev/null); expect "run  $tag" "$wo" "$wc" "$o" "$?"
  o=$("$bin" --ir  "$prog" 2>/dev/null); expect "ir   $tag" "$wo" "$wc" "$o" "$?"
  if [ $have_cpp -eq 1 ] && "$bin" --build "$work/b_cpp" "$prog" >/dev/null 2>&1; then
    o=$("$work/b_cpp" 2>/dev/null); expect "cpp  $tag" "$wo" "$wc" "$o" "$?"
  else echo "SKIP cpp  $tag (no compiler / build failed)"; skip=$((skip+1)); fi
  if [ $have_llvm -eq 1 ] && "$bin" --build-native "$work/b_llvm" "$prog" 2>/dev/null; then
    o=$("$work/b_llvm" 2>/dev/null); expect "llvm $tag" "$wo" "$wc" "$o" "$?"
  else echo "SKIP llvm $tag (no liblvrt.a / linker)"; skip=$((skip+1)); fi
}

# --- 1. the core $? contract: 3 / 7 / 0 --------------------------------------
run_all exit3.lev  $'before'        3   # env.exit(3): "after" unreachable
run_all set7.lev   $'before\nafter' 7   # setExitCode(7): both lines, then 7
run_all fall.lev   $'done'          0   # fall off the end: 0

# --- 2. uncaught throw exits 1 (gap b) and still prints the traceback --------
run_all throw.lev  $'before\nUncaught RuntimeException: boom' 1

# --- 3. 8-bit truncation: exit(257) -> 1, exit(-1) -> 255 --------------------
run_all mask257.lev $''  1
run_all maskneg.lev $''  255

# --- 4. exit-now abandons the event loop (10s timer never fires; < 1s) -------
loop_timed() {  # <label> <cmd...>
  local label="$1"; shift
  local t0 t1
  t0=$(date +%s%N); local o; o=$("$@" 2>/dev/null); local rc=$?; t1=$(date +%s%N)
  local ms=$(( (t1 - t0) / 1000000 ))
  if [ "$o" = "scheduling then exiting" ] && [ "$rc" = "0" ] && [ "$ms" -lt 1000 ]; then
    echo "ok   $label (${ms}ms, no drain)"
  else
    echo "FAIL $label: rc=$rc ms=$ms out=[$o]"; fail=1
  fi
}
loop_timed "loop run "  "$bin" --run "$d/loop.lev"
loop_timed "loop ir  "  "$bin" --ir  "$d/loop.lev"
[ $have_llvm -eq 1 ] && "$bin" --build-native "$work/loop_llvm" "$d/loop.lev" 2>/dev/null \
  && loop_timed "loop llvm" "$work/loop_llvm"

# --- 5. frozen ELF: clean deferral for BOTH natives, no miscompile -----------
elf=$("$bin" --emit-elf "$work/e1" "$d/exit3.lev" 2>&1); rc=$?
if [ $rc -ne 0 ] && echo "$elf" | grep -q "sysExit"; then echo "ok   elf  (sysExit deferral)"
else echo "FAIL elf (expected sysExit deferral, rc=$rc): $elf"; fail=1; fi
elf=$("$bin" --emit-elf "$work/e2" "$d/set7.lev" 2>&1); rc=$?
if [ $rc -ne 0 ] && echo "$elf" | grep -q "sysSetExitCode"; then echo "ok   elf  (sysSetExitCode deferral)"
else echo "FAIL elf (expected sysSetExitCode deferral, rc=$rc): $elf"; fail=1; fi

# --- 6. comptime hermeticity: comptime env::exit(1) must be denied -----------
printf 'comptime env::exit(1);\nconsole.writeln("x");\n' > "$work/ct.lev"
ct=$("$bin" --run "$work/ct.lev" 2>&1); rc=$?
if [ $rc -ne 0 ] && echo "$ct" | grep -q "may not perform I/O"; then echo "ok   comptime hermeticity"
else echo "FAIL comptime (expected denial, rc=$rc): $ct"; fail=1; fi

# --- 7. pty: terminal restored on exit-now (shared §3.4 epilogue) ------------
if ! command -v python3 >/dev/null 2>&1; then
  echo "SKIP pty (no python3)"; skip=$((skip+1))
else
cat > "$work/pty.py" <<'PY'
import os,pty,sys,termios,select,time
prog=sys.argv[1:]
m,s=os.openpty()
a=termios.tcgetattr(s); a[3]|=(termios.ICANON|termios.ECHO); termios.tcsetattr(s,termios.TCSANOW,a)
pid=os.fork()
if pid==0:
    os.close(m); os.setsid(); os.dup2(s,0); os.dup2(s,1)
    os.dup2(os.open('/dev/null',os.O_WRONLY),2); os.close(s)
    os.execvp(prog[0],prog); os._exit(127)
buf=b""
def rd(t):
    r,_,_=select.select([m],[],[],t)
    try: return os.read(m,4096) if r else b""
    except OSError: return b""
dl=time.time()+1.0
while time.time()<dl:
    buf+=rd(0.1)
    if b"READY" in buf: break
os.write(m,b"Z")
dl=time.time()+3
while b"KEY=" not in buf and time.time()<dl: buf+=rd(0.2)
buf+=rd(0.3)
_,status=os.waitpid(pid,0)
code = os.WEXITSTATUS(status) if os.WIFEXITED(status) else -1
lf=termios.tcgetattr(s)[3]
restored=bool(lf&termios.ICANON) and bool(lf&termios.ECHO)
txt=buf.decode(errors='replace')
ok = ("KEY=Z" in txt) and restored and (code==0)
print(f"key={'KEY=Z' in txt} restored={restored} exit0={code==0}")
sys.exit(0 if ok else 1)
PY
  pty_one() {  # <label> <cmd...>
    local label="$1"; shift
    if out=$(python3 "$work/pty.py" "$@" 2>&1); then echo "ok   $label ($out)"
    else echo "FAIL $label ($out)"; fail=1; fi
  }
  pty_one "pty exit run"  "$bin" --run "$d/raw_exit.lev"
  pty_one "pty exit ir "  "$bin" --ir  "$d/raw_exit.lev"
  [ $have_llvm -eq 1 ] && "$bin" --build-native "$work/raw_llvm" "$d/raw_exit.lev" 2>/dev/null \
    && pty_one "pty exit llvm" "$work/raw_llvm"
fi

echo "exit-code differential done ($skip group(s) skipped)"
exit $fail
