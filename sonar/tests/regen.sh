#!/usr/bin/env bash
# Regenerate golden .expected files from the ORACLE engine's stdout (Track 10 §4:
# "snap output IS the expected file's content"). Goldens are byte-identical
# across engines, so the oracle is the canonical generator. Review the diffs
# like any code change before committing.
#
# usage: regen.sh [test-dir ...]   (default: every golden dir under tests/ + examples/)
set -u
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
repo="$(cd "$root/.." && pwd)"
LEV="${LEVIATHAN:-$repo/build/leviathan}"
TRIDENT="${TRIDENT:-$repo/build/trident}"
export SONAR_SCRIPT=1

regen_one() {
  local dir="$1"
  [ -f "$dir/trident.toml" ] || return 0
  [ -f "$dir/expected.error" ] && return 0
  local exp; exp="$(ls "$dir"/*.expected 2>/dev/null | head -1)"
  [ -z "$exp" ] && return 0
  local abs; abs="$(cd "$dir" && pwd)"
  ( cd "$abs" && "$TRIDENT" plan . ) >/dev/null 2>&1 || { echo "SKIP $(basename "$dir") (plan failed)"; return 0; }
  ( cd "$abs" && "$LEV" --plan build/plan.lvplan --run ) > "$exp" 2>/dev/null
  echo "regen $(basename "$dir") -> $(basename "$exp")"
}

if [ "$#" -gt 0 ]; then dirs=("$@"); else
  dirs=()
  for d in "$here"/*/ "$root"/examples/*/; do dirs+=("$d"); done
fi
for d in "${dirs[@]}"; do regen_one "$d"; done
