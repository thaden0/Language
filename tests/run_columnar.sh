#!/usr/bin/env bash
# techdesign-columnar plan §3.1: every columnar corpus program must produce
# byte-identical output across oracle / IR / emit-C++ / LLVM AND with --columnar
# ON and OFF. The oracle (tree-walk) is layout-independent, so it is the single
# reference and runs once; the other three engines run in BOTH modes (7 configs).
# usage: run_columnar.sh <leviathan> <dir> <runtime-c-sources...>
bin="$1"; dir="$2"; shift 2
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
rtobjs=()
for src in "$@"; do
  o="$work/$(basename "${src%.*}").o"
  cc -O2 -c -o "$o" "$src" 2>"$work/rt.err" || { echo "FAIL runtime compile: $src"; head -5 "$work/rt.err"; exit 1; }
  rtobjs+=("$o")
done
fail=0; n=0
shopt -s nullglob
for f in "$dir"/*.lev; do
  n=$((n+1)); name=$(basename "${f%.lev}"); want=$(cat "${f%.lev}.expected")
  check() { # <label> <captured-output>
    if [ "$2" != "$want" ]; then echo "FAIL $name [$1]"; diff <(echo "$want") <(echo "$2") | head -6; fail=1; fi
  }
  # oracle reference (columnar-independent — the tree-walk never went dense)
  check "oracle" "$("$bin" --run "$f" 2>/dev/null)"
  # explicit --no-columnar (row-major escape hatch) AND --columnar, robust to the
  # compile-time DEFAULT (now columnar-on after the C-M5 flip).
  for mode in "--no-columnar" "--columnar"; do
    tag=${mode/--no-columnar/off}; tag=${tag/--columnar/on}
    # IR interpreter
    check "ir/$tag" "$("$bin" $mode --ir "$f" 2>/dev/null)"
    # emit-C++
    if "$bin" $mode --emit-cpp "$f" > "$work/$name.cpp" 2>"$work/$name.e"; then
      if g++ -O2 -o "$work/$name.cpp.bin" "$work/$name.cpp" 2>"$work/$name.ge"; then
        check "cpp/$tag" "$("$work/$name.cpp.bin" 2>/dev/null)"
      else echo "FAIL $name [cpp/$tag compile]"; head -5 "$work/$name.ge"; fail=1; fi
    else echo "FAIL $name [cpp/$tag emit]"; head -5 "$work/$name.e"; fail=1; fi
    # LLVM native
    if "$bin" $mode --native-obj "$work/$name.o" "$f" 2>"$work/$name.le"; then
      if cc -O2 -o "$work/$name.llvm.bin" "$work/$name.o" "${rtobjs[@]}" -lm -lpthread 2>"$work/$name.lke"; then
        check "llvm/$tag" "$("$work/$name.llvm.bin" 2>/dev/null)"
      else echo "FAIL $name [llvm/$tag link]"; head -5 "$work/$name.lke"; fail=1; fi
    else echo "FAIL $name [llvm/$tag codegen]"; head -5 "$work/$name.le"; fail=1; fi
  done
done
echo "$n columnar program(s) verified across oracle + IR/cpp/LLVM x {on,off}"
exit $fail
