#!/usr/bin/env bash
# Recon corpus test runner (DESIGN §13). Each test dir holds a trident.toml, a
# <name>.lev with Recon::main(), and a <name>.expected. Runs BOTH lanes
# (oracle via `trident run`, IR via `trident plan` + `leviathan --plan ... --ir`)
# and diffs stdout against the expected file. Exit 0 iff every test passes on
# both lanes byte-identically.
#
# Usage:  examples/recon/tests/run-tests.sh [test-dir ...]
# Run from the repo root (paths in trident.toml are cwd-relative through plans).

set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$ROOT"
TRIDENT="$ROOT/build/trident"
LEVIATHAN="$ROOT/build/leviathan"
TESTDIR="$ROOT/examples/recon/tests"

if [[ $# -gt 0 ]]; then
    DIRS=("$@")
else
    DIRS=()
    for d in "$TESTDIR"/*/; do
        [[ -f "$d/trident.toml" ]] && DIRS+=("$d")
    done
fi

pass=0; fail=0
for d in "${DIRS[@]}"; do
    d="${d%/}"
    name="$(basename "$d")"
    exp=$(ls "$d"/*.expected 2>/dev/null | head -1)
    if [[ -z "$exp" ]]; then
        echo "SKIP  $name (no .expected)"; continue
    fi
    # oracle
    got_oracle="$("$TRIDENT" run "$d" 2>/dev/null)"
    # ir: plan then interpret
    "$TRIDENT" plan "$d" >/dev/null 2>&1
    got_ir="$("$LEVIATHAN" --plan "$ROOT/build/plan.lvplan" --ir 2>/dev/null)"
    want="$(cat "$exp")"
    if [[ "$got_oracle" == "$want" && "$got_ir" == "$want" ]]; then
        echo "PASS  $name"; pass=$((pass+1))
    else
        echo "FAIL  $name"; fail=$((fail+1))
        if [[ "$got_oracle" != "$want" ]]; then
            echo "  --- oracle diff (want <  got >) ---"
            diff <(printf '%s' "$want") <(printf '%s' "$got_oracle") | head -20 | sed 's/^/  /'
        fi
        if [[ "$got_ir" != "$want" ]]; then
            echo "  --- ir diff (want <  got >) ---"
            diff <(printf '%s' "$want") <(printf '%s' "$got_ir") | head -20 | sed 's/^/  /'
        fi
    fi
done
echo "----"
echo "$pass passed, $fail failed"
[[ $fail -eq 0 ]]
