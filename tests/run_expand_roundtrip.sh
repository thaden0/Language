#!/usr/bin/env bash
# Expand round-trip (metaprog Phase 4 §6 acceptance): for every meta program,
# `--expand` it to source, compile+run that source, and assert byte-identical
# run output to the original. This is the strong, mechanically-checkable proof
# that the source-shaped printer emits a faithful, recompilable artifact.
#
# Programs whose comptime code writes to STDOUT are skipped (marked
# `@no-roundtrip`): --expand interleaves that compile-time output with the
# source dump, so the captured file is not pure source — orthogonal to the
# printer's fidelity. Any enum-using program is also `@no-roundtrip` for an
# unrelated, pre-existing reason: known_bugs_2.md #69 (enum member globals'
# synthesized `$`-joined name re-lexes as a quasiquote hole on the printed
# round-trip source) — see LA-31 Stage 3
# (designs/expr-reification/techdesign-03-verification.md §5) for the row
# that first hit this while unrelated to expr:: reification itself.
# usage: run_expand_roundtrip.sh <leviathan-binary> <corpus-dir>
bin="$1"; dir="$2"; fail=0; n=0; skip=0
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
shopt -s nullglob
for f in "$dir"/*.ext "$dir"/*.lev; do
  if grep -q '@no-roundtrip' "$f"; then skip=$((skip+1)); continue; fi
  n=$((n+1))
  want=$("$bin" --run "$f" 2>&1)
  "$bin" --expand "$f" > "$tmp/rt.lev" 2>/dev/null
  got=$("$bin" --run "$tmp/rt.lev" 2>&1)
  if [ "$got" != "$want" ]; then
    echo "ROUND-TRIP FAIL $f"
    diff <(echo "$want") <(echo "$got") | head -12
    fail=1
  fi
done
echo "$n program(s) round-tripped ($skip skipped)"
exit $fail
