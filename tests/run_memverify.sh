#!/usr/bin/env bash
# Memory-safety harness (Phase A): run every corpus program under --mem-verify.
# The program output must still match .expected (the verifier is observation-only,
# so it cannot change behavior), and the reachability report must be produced.
bin="$1"; dir="$2"; fail=0; n=0
shopt -s nullglob
for f in "$dir"/*.ext "$dir"/*.lev; do
  n=$((n+1))
  exp="${f%.*}.expected"
  stdin="${f%.*}.stdin"
  if [ -f "$stdin" ]; then got=$("$bin" --mem-verify "$f" 2>/tmp/mem_err.txt < "$stdin"); rc=$?
  else got=$("$bin" --mem-verify "$f" 2>/tmp/mem_err.txt); rc=$?; fi
  want=$(cat "$exp")
  if [ "$got" != "$want" ] || [ $rc -ne 0 ]; then
    echo "FAIL $f (rc=$rc): output diverged under --mem-verify"; fail=1; continue
  fi
  if ! grep -q "^\[mem\]" /tmp/mem_err.txt; then
    echo "FAIL $f: no reachability report produced"; fail=1
  fi
done
echo "$n corpus file(s) mem-verified (Phase A: reachability oracle)"
exit $fail
