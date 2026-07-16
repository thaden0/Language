#!/usr/bin/env bash
# Corpus runner: run every .ext/.lev under a directory and diff against .expected.
# usage: run_corpus.sh <leviathan-binary> <mode-flag> <corpus-dir>
bin="$1"; mode="$2"; dir="$3"; fail=0; n=0
shopt -s nullglob
for f in "$dir"/*.ext "$dir"/*.lev; do
  n=$((n+1))
  exp="${f%.*}.expected"
  stdin="${f%.*}.stdin"
  if [ -f "$stdin" ]; then got=$("$bin" "$mode" "$f" 2>&1 < "$stdin")
  else got=$("$bin" "$mode" "$f" 2>&1); fi
  want=$(cat "$exp")
  if [ "$got" != "$want" ]; then
    echo "FAIL $f ($mode)"
    diff <(echo "$want") <(echo "$got") | head -10
    fail=1
  fi
done
echo "$n corpus file(s) checked ($mode)"
exit $fail
