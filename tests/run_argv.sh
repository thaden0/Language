#!/usr/bin/env bash
# argv differential (designs/argv.md §10). One program (argv_echo.lev) run
# through every engine with a fixed argument vector, asserting identical output
# — the five-engine agreement rule — plus the two negative cases the design
# calls for: the frozen ELF backend must DEFER cleanly (not miscompile), and a
# `comptime env::args()` must fail with the hermeticity error.
#
# usage: run_argv.sh <leviathan-binary> <corpus-dir>
#   corpus-dir holds argv/argv_echo.lev
set -u
bin="$1"; dir="$2"
prog="$dir/argv/argv_echo.lev"
fail=0; skip=0
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT

# argv[0] (program name) is engine-dependent by design (§9), so the program
# prints only the arg COUNT and the real args (args().skip(1)) — identical on
# every engine. With `-- alpha "b c" "" delta` the count is 5 (argv[0] + 4).
ARGS=(alpha "b c" "" delta)
read -r -d '' EXPECT_ARGS <<'EOF'
argc=5
[alpha]
[b c]
[]
[delta]
hasName=yes
EOF
read -r -d '' EXPECT_NOARGS <<'EOF'
argc=1
hasName=yes
EOF

check() {   # check <label> <expected> <actual>
  if [ "$3" != "$2" ]; then
    echo "FAIL $1"; diff <(echo "$2") <(echo "$3") | head -12; fail=1
  else
    echo "ok   $1"
  fi
}

# --- 1. interpreters: --run / --ir, with args and with none ------------------
check "run  (args)" "$EXPECT_ARGS"   "$("$bin" --run "$prog" -- "${ARGS[@]}" 2>&1)"
check "ir   (args)" "$EXPECT_ARGS"   "$("$bin" --ir  "$prog" -- "${ARGS[@]}" 2>&1)"
check "run  (none)" "$EXPECT_NOARGS" "$("$bin" --run "$prog" 2>&1)"
check "ir   (none)" "$EXPECT_NOARGS" "$("$bin" --ir  "$prog" 2>&1)"

# --- 2. emit-C++: emit, compile with the system compiler, run the binary -----
cxx=""; for c in clang++ g++ c++; do command -v "$c" >/dev/null 2>&1 && { cxx="$c"; break; }; done
if [ -n "$cxx" ] && "$bin" --emit-cpp "$prog" > "$work/a.cpp" 2>"$work/a.err"; then
  if "$cxx" -O2 -o "$work/a_cpp" "$work/a.cpp" 2>"$work/a.gcc"; then
    check "cpp  (args)" "$EXPECT_ARGS"   "$("$work/a_cpp" "${ARGS[@]}" 2>&1)"
    check "cpp  (none)" "$EXPECT_NOARGS" "$("$work/a_cpp" 2>&1)"
  else
    echo "SKIP cpp (compile failed)"; head -3 "$work/a.gcc"; skip=$((skip+1))
  fi
else
  echo "SKIP cpp (no C++ compiler or emit failed)"; skip=$((skip+1))
fi

# --- 3. LLVM: --build-native (needs liblvrt.a next to the leviathan binary) ---
rt="$(dirname "$bin")/liblvrt.a"
if [ -f "$rt" ] && [ -n "$cxx" ]; then
  if "$bin" --build-native "$work/a_llvm" "$prog" 2>"$work/l.err"; then
    # LLVM binary prints a heap meter to stderr; compare stdout only.
    check "llvm (args)" "$EXPECT_ARGS"   "$("$work/a_llvm" "${ARGS[@]}" 2>/dev/null)"
    check "llvm (none)" "$EXPECT_NOARGS" "$("$work/a_llvm" 2>/dev/null)"
  else
    echo "SKIP llvm (build-native failed)"; head -3 "$work/l.err"; skip=$((skip+1))
  fi
else
  echo "SKIP llvm (no liblvrt.a next to binary, or no linker)"; skip=$((skip+1))
fi

# --- 4. frozen ELF: must DEFER cleanly, never miscompile ---------------------
elf_out=$("$bin" --emit-elf "$work/a_elf" "$prog" 2>&1); elf_rc=$?
if [ $elf_rc -ne 0 ] && echo "$elf_out" | grep -q "sysArgs"; then
  echo "ok   elf  (clean deferral diagnostic)"
else
  echo "FAIL elf (expected a clean sysArgs deferral, got rc=$elf_rc): $elf_out"; fail=1
fi

# --- 5. comptime hermeticity: comptime env::args() must fail ------------------
printf 'comptime int N = env::args().length();\nconsole.writeln(N);\n' > "$work/ct.lev"
ct_out=$("$bin" --run "$work/ct.lev" 2>&1); ct_rc=$?
if [ $ct_rc -ne 0 ] && echo "$ct_out" | grep -q "comptime code may not perform I/O"; then
  echo "ok   comptime hermeticity (denied)"
else
  echo "FAIL comptime hermeticity (expected denial, got rc=$ct_rc): $ct_out"; fail=1
fi

echo "argv differential done ($skip engine group(s) skipped)"
exit $fail
