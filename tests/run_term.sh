#!/usr/bin/env bash
# Terminal raw-mode differential (designs/terminal-raw-mode.md §7).
# - non-tty degradation across --run/--ir/--build-native/--build (enableRaw==false)
# - the frozen ELF clean deferral, and comptime hermeticity denial
# - pty end-to-end (needs a tty + python3): a program enters raw mode, reads ONE
#   keystroke with NO Enter and NO echo, restores — and the terminal is verified
#   restored even after an UNCAUGHT throw (the runtime exit epilogue, §3.4).
#
# usage: run_term.sh <leviathan-binary> <corpus-dir>   (dir holds term/*.lev)
set -u
bin="$1"; dir="$2"; d="$dir/term"
fail=0; skip=0
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT

check() { if [ "$3" != "$2" ]; then echo "FAIL $1"; diff <(echo "$2") <(echo "$3")|head; fail=1; else echo "ok   $1"; fi; }

# --- 1. non-tty degradation: enableRaw() is false when stdin isn't a terminal --
NOTTY=$'raw=off\ndone'
check "run  (non-tty)" "$NOTTY" "$(printf '' | "$bin" --run "$d/toggle.lev" 2>&1)"
check "ir   (non-tty)" "$NOTTY" "$(printf '' | "$bin" --ir  "$d/toggle.lev" 2>&1)"
cxx=""; for c in clang++ g++ c++; do command -v "$c" >/dev/null 2>&1 && { cxx="$c"; break; }; done
if [ -n "$cxx" ] && "$bin" --build "$work/toggle_cpp" "$d/toggle.lev" >/dev/null 2>&1; then
  check "cpp  (non-tty)" "$NOTTY" "$(printf '' | "$work/toggle_cpp" 2>&1)"
else echo "SKIP cpp (no compiler / emit)"; skip=$((skip+1)); fi
rt="$(dirname "$bin")/liblvrt.a"; have_llvm=0
if [ -f "$rt" ] && [ -n "$cxx" ] && "$bin" --build-native "$work/toggle_llvm" "$d/toggle.lev" 2>/dev/null; then
  have_llvm=1
  check "llvm (non-tty)" "$NOTTY" "$(printf '' | "$work/toggle_llvm" 2>/dev/null)"
else echo "SKIP llvm (no liblvrt.a/linker)"; skip=$((skip+1)); fi

# --- 2. frozen ELF: clean deferral, no miscompile -----------------------------
elf=$("$bin" --emit-elf "$work/elf" "$d/toggle.lev" 2>&1); rc=$?
if [ $rc -ne 0 ] && echo "$elf" | grep -q "sysTermRaw"; then echo "ok   elf  (deferral)"
else echo "FAIL elf (expected sysTermRaw deferral, rc=$rc): $elf"; fail=1; fi

# --- 3. comptime hermeticity --------------------------------------------------
printf 'comptime int x = std::sysTermRaw(0);\nconsole.writeln(x);\n' > "$work/ct.lev"
ct=$("$bin" --run "$work/ct.lev" 2>&1); rc=$?
if [ $rc -ne 0 ] && echo "$ct" | grep -q "may not perform I/O"; then echo "ok   comptime hermeticity"
else echo "FAIL comptime (expected denial, rc=$rc): $ct"; fail=1; fi

# --- 4. pty end-to-end (raw read + restore, incl. abnormal exit) --------------
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
os.write(m,b"Z")                                   # single keystroke ('Z' appears
dl=time.time()+3                                   # nowhere else in the output)
while b"KEY=" not in buf and time.time()<dl: buf+=rd(0.2)
buf+=rd(0.3); os.waitpid(pid,0)
lf=termios.tcgetattr(s)[3]
restored=bool(lf&termios.ICANON) and bool(lf&termios.ECHO)
txt=buf.decode(errors='replace')
# no-echo: the sent 'Z' must appear ONLY inside the program's "KEY=Z" line; a
# stray 'Z' would be the terminal echoing the keystroke (i.e. ECHO not cleared).
echoed = txt.replace("KEY=Z","").count("Z") > 0
ok = ("KEY=Z" in txt) and restored and (not echoed)
print(f"key_no_enter={'KEY=Z' in txt} restored={restored} no_echo={not echoed}")
sys.exit(0 if ok else 1)
PY
  pty_one() {  # <label> <cmd...>
    local label="$1"; shift
    if out=$(python3 "$work/pty.py" "$@" 2>&1); then echo "ok   $label ($out)"
    else echo "FAIL $label ($out)"; fail=1; fi
  }
  # normal exit (restore called)
  [ $have_llvm -eq 1 ] && { "$bin" --build-native "$work/echo_llvm" "$d/raw_echo.lev" 2>/dev/null && pty_one "pty echo  llvm" "$work/echo_llvm"; }
  pty_one "pty echo  run"  "$bin" --run "$d/raw_echo.lev"
  pty_one "pty echo  ir"   "$bin" --ir  "$d/raw_echo.lev"
  # abnormal exit (uncaught throw, no restore call — epilogue must restore)
  [ $have_llvm -eq 1 ] && { "$bin" --build-native "$work/crash_llvm" "$d/raw_crash.lev" 2>/dev/null && pty_one "pty crash llvm" "$work/crash_llvm"; }
  pty_one "pty crash run"  "$bin" --run "$d/raw_crash.lev"
  pty_one "pty crash ir"   "$bin" --ir  "$d/raw_crash.lev"
fi

echo "terminal raw-mode differential done ($skip group(s) skipped)"
exit $fail
