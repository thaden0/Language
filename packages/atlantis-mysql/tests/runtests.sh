#!/usr/bin/env bash
# atlantis-mysql differential test runner (techdesign-05 §7, C8): every case is
# built to a plan through trident, then run on the tree-walk oracle, the IR
# interpreter, and the LLVM native binary; all three must match expected.txt
# byte-for-byte (the [heap] accounting line on stderr is ignored).
#
#   ./packages/atlantis-mysql/tests/runtests.sh [path/to/leviathan] [path/to/trident]
set -u
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
lev="${1:-$root/build/leviathan}"
tri="${2:-$root/build/trident}"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
fail=0

for dir in "$here"/*/; do
  name="$(basename "$dir")"
  [ -f "$dir/trident.toml" ] || continue
  [ -f "$dir/expected.txt" ] || { echo "SKIP $name (no expected.txt)"; continue; }
  plan="$work/$name.lvplan"
  if ! "$tri" plan "$dir" --plan "$plan" --leviathan "$lev" >/dev/null 2>"$work/$name.err"; then
    echo "FAIL $name (resolve)"; head -3 "$work/$name.err"; fail=1; continue
  fi
  want="$(cat "$dir/expected.txt")"
  # oracle
  got_o="$(timeout 60 "$lev" --run --plan "$plan" 2>/dev/null | grep -v '^\[heap\]')"
  # IR
  got_i="$(timeout 90 "$lev" --run --ir --plan "$plan" 2>/dev/null | grep -v '^\[heap\]')"
  # LLVM native
  bin="$work/$name.bin"
  "$lev" --build-native "$bin" --plan "$plan" >/dev/null 2>&1
  got_n="$(timeout 60 "$bin" 2>/dev/null | grep -v '^\[heap\]')"

  ok=1
  [ "$got_o" = "$want" ] || { ok=0; echo "FAIL $name (oracle)"; diff <(echo "$want") <(echo "$got_o") | head; }
  [ "$got_i" = "$want" ] || { ok=0; echo "FAIL $name (ir)"; diff <(echo "$want") <(echo "$got_i") | head; }
  [ "$got_n" = "$want" ] || { ok=0; echo "FAIL $name (llvm)"; diff <(echo "$want") <(echo "$got_n") | head; }
  [ "$ok" = 1 ] && echo "PASS $name (oracle+ir+llvm)" || fail=1
done

exit $fail
