#!/usr/bin/env bash
# Ownership verification: run every corpus program under --ir-verify. The
# program output must match .expected AND the ownership soundness check must
# pass (exit 0: no scope-owned allocation survived its frame).
bin="$1"; dir="$2"; fail=0; n=0
for f in "$dir"/*.ext; do
  n=$((n+1))
  exp="${f%.ext}.expected"
  stdin="${f%.ext}.stdin"
  if [ -f "$stdin" ]; then got=$("$bin" --ir-verify "$f" 2>/tmp/verify_err.txt < "$stdin"); rc=$?
  else got=$("$bin" --ir-verify "$f" 2>/tmp/verify_err.txt); rc=$?; fi
  want=$(cat "$exp")
  if [ "$got" != "$want" ] || [ $rc -ne 0 ]; then
    echo "FAIL $f (rc=$rc)"
    cat /tmp/verify_err.txt
    fail=1
  fi
done
echo "$n corpus file(s) ownership-verified"
exit $fail
