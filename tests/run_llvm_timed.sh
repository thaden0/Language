#!/usr/bin/env bash
# Timed single-program LLVM lane (§10.2): compile+link normally, but the RUN
# must finish within the given wall-clock budget — a lost COW in-place path
# turns the indexed-store loop O(n^2) and blows the budget loudly.
bin="$1"; prog="$2"; budget="$3"; shift 3
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
rtobjs=()
for src in "$@"; do
  obj="$work/$(basename "${src%.c}").o"
  cc -O2 -c -o "$obj" "$src" || exit 1
  rtobjs+=("$obj")
done
name=$(basename "$prog" .ext)
"$bin" --native-obj "$work/$name.o" "$prog" || { echo "FAIL codegen"; exit 1; }
cc -O2 -o "$work/$name" "$work/$name.o" "${rtobjs[@]}" -lm || { echo "FAIL link"; exit 1; }
got=$(timeout "$budget" "$work/$name") || { echo "FAIL: run exceeded ${budget}s (COW regression?)"; exit 1; }
want=$(cat "${prog%.ext}.expected")
if [ "$got" != "$want" ]; then
  echo "FAIL output"; diff <(echo "$want") <(echo "$got") | head -6; exit 1
fi
echo "timed smoke OK (< ${budget}s)"
