#!/usr/bin/env bash
# Helm test runner (H13). Each test dir holds a trident.toml + <name>_test.lev +
# <name>_test.expected golden. Helm's interpreter-bound features (subprocess
# bridge over `Process`) run on the oracle + IR engines only — the compiled
# backends defer the spawn floor (design §7.1, §14 / G-LANG-2) — so the golden
# lane is oracle + IR, compared byte-identically against ONE .expected.
#
# usage: run-tests.sh [test-dir ...]   (default: every dir under tests/ with a golden)
set -u
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../../.." && pwd)"
LEV="${LEVIATHAN:-$repo/build/leviathan}"
TRIDENT="${TRIDENT:-$repo/build/trident}"

fail=0; npass=0
if [ "$#" -gt 0 ]; then dirs=("$@"); else
  dirs=(); for d in "$here"/*/; do [ -f "$d/trident.toml" ] && dirs+=("$d"); done
fi

for dir in "${dirs[@]}"; do
  name="$(basename "$dir")"
  exp="$(ls "$dir"/*.expected 2>/dev/null | head -1)"
  [ -z "$exp" ] && continue
  want="$(cat "$exp")"
  if ! ( cd "$dir" && "$TRIDENT" plan . ) >/dev/null 2>&1; then
    echo "FAIL $name (plan)"; fail=1; continue
  fi
  ok=1
  for flag in --run --ir; do
    got="$( cd "$dir" && "$LEV" --plan build/plan.lvplan "$flag" 2>/dev/null )"
    if [ "$got" != "$want" ]; then
      echo "FAIL $name ($flag)"; diff <(echo "$want") <(echo "$got") | head -8; fail=1; ok=0
    fi
  done
  [ "$ok" = "1" ] && { echo "ok   $name (oracle+ir)"; npass=$((npass+1)); }
done
echo "---"; echo "$npass test(s) passed"
exit $fail
