#!/usr/bin/env bash
# atlantis corpus runner: every corpus case is built to a plan through trident,
# then run on the tree-walk oracle, the IR interpreter, and the LLVM native
# binary; all three must match the case's *.expected byte-for-byte (the [heap]
# accounting line is ignored). Mirrors packages/atlantis-mysql/tests/runtests.sh.
#
#   ./packages/atlantis/tests/runtests.sh [path/to/leviathan] [path/to/trident]
set -u
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
lev="${1:-$root/build/leviathan}"
tri="${2:-$root/build/trident}"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
fail=0

for dir in "$here"/corpus/*/; do
  name="$(basename "$dir")"
  [ -f "$dir/trident.toml" ] || continue
  exp="$(ls "$dir"/*.expected 2>/dev/null | head -1)"
  [ -n "$exp" ] || { echo "SKIP $name (no .expected)"; continue; }
  plan="$work/$name.lvplan"
  if ! "$tri" plan "$dir" --plan "$plan" --leviathan "$lev" >/dev/null 2>"$work/$name.err"; then
    echo "FAIL $name (resolve)"; head -3 "$work/$name.err"; fail=1; continue
  fi
  want="$(cat "$exp")"
  # negative cases (compile-error expectations) emit on stderr and error
  # identically on every engine — compare the oracle's combined output once,
  # planned from inside the case dir so diagnostic path prefixes match the
  # recorded expectations.
  if grep -q "error:" "$exp"; then
    ( cd "$dir" && "$tri" plan . --plan "$plan" --leviathan "$lev" >/dev/null 2>&1 )
    got="$( cd "$dir" && timeout 90 "$lev" --run --plan "$plan" 2>&1 | grep -v '^\[heap\]')"
    if [ "$got" = "$want" ]; then echo "PASS $name (negative)"; else
      echo "FAIL $name (negative)"; diff <(echo "$want") <(echo "$got") | head; fail=1; fi
    continue
  fi
  got_o="$(timeout 90 "$lev" --run --plan "$plan" 2>/dev/null | grep -v '^\[heap\]')"
  got_i="$(timeout 120 "$lev" --run --ir --plan "$plan" 2>/dev/null | grep -v '^\[heap\]')"
  bin="$work/$name.bin"
  "$lev" --build-native "$bin" --plan "$plan" >/dev/null 2>&1
  got_n="$(timeout 90 "$bin" 2>/dev/null | grep -v '^\[heap\]')"

  ok=1
  [ "$got_o" = "$want" ] || { ok=0; echo "FAIL $name (oracle)"; diff <(echo "$want") <(echo "$got_o") | head; }
  [ "$got_i" = "$want" ] || { ok=0; echo "FAIL $name (ir)"; diff <(echo "$want") <(echo "$got_i") | head; }
  [ "$got_n" = "$want" ] || { ok=0; echo "FAIL $name (llvm)"; diff <(echo "$want") <(echo "$got_n") | head; }
  [ "$ok" = 1 ] && echo "PASS $name (oracle+ir+llvm)" || fail=1
done

exit $fail
