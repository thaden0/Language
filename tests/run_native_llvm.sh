#!/usr/bin/env bash
# LLVM-native corpus: emit an object file via the LLVM backend, link with the
# runtime v2 (runtime/lv_runtime.c + platform floor), run the binary, diff
# against .expected. Runtime C sources arrive as args 3..N and are compiled
# once (as C — cc, not g++) into objects shared by every program link.
bin="$1"; dir="$2"; shift 2; fail=0; n=0
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
rtobjs=()
for src in "$@"; do
  obj="$work/$(basename "${src%.c}").o"
  if ! cc -O2 -c -o "$obj" "$src" 2>"$work/rt.err"; then
    echo "FAIL runtime compile: $src"; head -5 "$work/rt.err"; exit 1
  fi
  rtobjs+=("$obj")
done
shopt -s nullglob
if [ -d "$dir" ]; then files=("$dir"/*.ext "$dir"/*.lev); else files=("$dir"); fi
for f in "${files[@]}"; do
  n=$((n+1))
  exp="${f%.*}.expected"
  name=$(basename "$f"); name="${name%.*}"
  if ! "$bin" --native-obj "$work/$name.o" "$f" 2>"$work/$name.err"; then
    echo "FAIL $f (codegen)"; cat "$work/$name.err"; fail=1; continue
  fi
  if ! cc -O2 -o "$work/$name" "$work/$name.o" "${rtobjs[@]}" -lm -lpthread 2>"$work/$name.cc"; then
    echo "FAIL $f (link)"; head -5 "$work/$name.cc"; fail=1; continue
  fi
  stdin="${f%.*}.stdin"
  if [ -f "$stdin" ]; then got=$("$work/$name" < "$stdin")
  else got=$("$work/$name"); fi
  want=$(cat "$exp")
  if [ "$got" != "$want" ]; then
    echo "FAIL $f (output)"; diff <(echo "$want") <(echo "$got") | head -6; fail=1
  fi
done
echo "$n program(s) compiled via LLVM and verified"
exit $fail
