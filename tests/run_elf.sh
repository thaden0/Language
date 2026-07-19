#!/usr/bin/env bash
# Pure native backend: emit a standalone ELF (own x86-64 + ELF, no g++/libc),
# run the BINARY, diff against .expected. Skips programs outside the backend's
# current coverage with a printed notice.
bin="$1"; dir="$2"; fail=0; n=0; skip=0
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
shopt -s nullglob
for f in "$dir"/*.ext "$dir"/*.lev; do
  name=$(basename "$f"); name="${name%.*}"
  if ! "$bin" --emit-elf "$work/$name" "$f" 2>"$work/$name.err"; then
    if grep -qE "native-elf backend|not yet lowerable" "$work/$name.err"; then
      echo "SKIP (beyond ELF coverage): $(basename "$f")"; skip=$((skip+1)); continue
    fi
    echo "FAIL $f (emit)"; cat "$work/$name.err"; fail=1; continue
  fi
  n=$((n+1)); chmod +x "$work/$name"
  stdin="${f%.*}.stdin"
  if [ -f "$stdin" ]; then got=$("$work/$name" < "$stdin"); else got=$("$work/$name"); fi
  want=$(cat "${f%.*}.expected")
  if [ "$got" != "$want" ]; then echo "FAIL $f"; diff <(echo "$want") <(echo "$got")|head -6; fail=1; fi
done
echo "$n program(s) compiled to a dependency-free ELF and verified, $skip skipped"
exit $fail
