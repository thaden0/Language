#!/usr/bin/env bash
# Native (AOT) corpus: emit C++, compile with the system compiler, run the
# BINARY, diff its output against .expected. Programs using constructs the
# native backend does not yet cover (sockets/timers/async/system I/O) are
# SKIPPED with a printed notice (no silent caps) rather than failing.
bin="$1"; dir="$2"; fail=0; n=0; skip=0
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
shopt -s nullglob
if [ -d "$dir" ]; then files=("$dir"/*.ext "$dir"/*.lev); else files=("$dir"); fi
for f in "${files[@]}"; do
  exp="${f%.*}.expected"
  name=$(basename "$f"); name="${name%.*}"
  if ! "$bin" --emit-cpp "$f" > "$work/$name.cpp" 2>"$work/$name.err"; then
    if grep -qE "native backend|not yet lowerable" "$work/$name.err"; then
      echo "SKIP (unsupported natively): $(basename "$f")"; skip=$((skip+1)); continue
    fi
    echo "FAIL $f (emit)"; cat "$work/$name.err"; fail=1; continue
  fi
  n=$((n+1))
  if ! g++ -O2 -o "$work/$name" "$work/$name.cpp" 2>"$work/$name.gcc"; then
    echo "FAIL $f (compile)"; head -5 "$work/$name.gcc"; fail=1; continue
  fi
  stdin="${f%.*}.stdin"
  if [ -f "$stdin" ]; then got=$("$work/$name" < "$stdin")
  else got=$("$work/$name"); fi
  want=$(cat "$exp")
  if [ "$got" != "$want" ]; then
    echo "FAIL $f (output)"; diff <(echo "$want") <(echo "$got") | head -6; fail=1
  fi
done
echo "$n program(s) compiled to native and verified, $skip skipped"
exit $fail
