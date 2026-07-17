#!/usr/bin/env bash
# Track W (techdesign-02-backend-column.md §7): the wasm corpus lane.
# usage: run_wasm.sh <leviathan-binary> <file-or-dir>
#
# Two modes per file:
#  - gated_*.lev — NEGATIVE (hard-03 tier-1 pin): the wasm compile must FAIL
#    and its stderr must contain the pinned diagnostic in <file>.expected.
#  - everything else — end-to-end: --build-native out.wasm --target
#    wasm32-unknown-unknown, run under wasmtime (--invoke main), diff stdout
#    against the --ir lane. SKIPs (not fails) while the §6 runtime archive,
#    wasm-ld, or wasmtime are missing — those are the archive owner's, not
#    the HARD packets'.
bin="$1"; dir="$2"; fail=0; n=0; skipped=0
triple=wasm32-unknown-unknown
shopt -s nullglob
if [ -d "$dir" ]; then files=("$dir"/*.ext "$dir"/*.lev); else files=("$dir"); fi
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
for f in "${files[@]}"; do
  n=$((n+1))
  base=$(basename "$f")
  if [[ "$base" == gated_* ]]; then
    exp="${f%.*}.expected"
    got=$("$bin" --native-obj "$work/gate.o" --target "$triple" "$f" 2>&1)
    rc=$?
    if [ $rc -eq 0 ]; then
      echo "FAIL $f (gated program compiled for wasm)"; fail=1; continue
    fi
    if ! echo "$got" | grep -qF "$(cat "$exp")"; then
      echo "FAIL $f (diagnostic mismatch)"
      echo "want substring: $(cat "$exp")"
      echo "got: $got" | head -5
      fail=1
    fi
    continue
  fi
  if ! command -v wasmtime >/dev/null; then
    skipped=$((skipped+1)); continue
  fi
  out="$work/${base%.*}.wasm"
  if ! "$bin" --build-native "$out" --target "$triple" "$f" 2>"$work/err"; then
    if grep -q "cannot locate the $triple runtime archive\|no wasm linker" "$work/err"; then
      skipped=$((skipped+1)); continue     # §6 archive / wasm-ld not built yet
    fi
    echo "FAIL $f (wasm build)"; head -5 "$work/err"; fail=1; continue
  fi
  want=$("$bin" --ir "$f" 2>&1)
  got=$(wasmtime run --invoke main "$out" 2>&1)
  if [ "$got" != "$want" ]; then
    echo "FAIL $f (wasm output != --ir)"
    diff <(echo "$want") <(echo "$got") | head -10
    fail=1
  fi
done
echo "$n wasm corpus file(s) checked, $skipped skipped"
exit $fail
