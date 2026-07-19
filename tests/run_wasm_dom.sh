#!/usr/bin/env bash
# Track W W-M3 (techdesign-05-dom-bridge.md §8): the JS/DOM bridge lane.
# usage: run_wasm_dom.sh <leviathan-binary> <dir-or-file> [more ...]
#
# The DOM bridge has no --ir counterpart (DOM/JS is a wasm-GAINED capability;
# the native sysHost* natives raise), so unlike tests/run_wasm.sh this lane does
# NOT diff against --ir — it builds each program for wasm32, runs it headlessly
# against the node DOM-stub host (tests/wasm_node_run.mjs's nodeDom(), the
# stand-in for a real browser document, doc 05 §8), and diffs STDOUT against a
# committed <file>.expected golden. An optional <file>.expected-rc pins a
# nonzero exit (the throwing-handler pin); default expected exit is 0.
#
# Kept OUT of default CTest (doc 05 §8: "one script tests/run_wasm_dom.sh"),
# runnable on demand. Skips (not fails) when node / wasm-ld / the runtime
# archive are absent — those belong to the archive owner, not this lane.
#
# JSPI: DOM handlers await (doc 05 §4), so this needs the JSPI host of record —
# Node >= 24 with --experimental-wasm-jspi (auto-detected below), or Chrome for
# the manual lv_host_page.html path.
set -u
bin="$1"; shift
triple=wasm32-unknown-unknown
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
runner="$here/wasm_node_run.mjs"
node_flags=""
if node --experimental-wasm-jspi -e "" >/dev/null 2>&1; then
  node_flags="--experimental-wasm-jspi"
fi

if ! command -v node >/dev/null; then
  echo "run_wasm_dom.sh: node not found — DOM lane skipped"; exit 0
fi

shopt -s nullglob
files=()
for arg in "$@"; do
  if [ -d "$arg" ]; then files+=("$arg"/dom_*.lev "$arg"/dom_*.ext); else files+=("$arg"); fi
done

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
fail=0; n=0; skipped=0
for f in "${files[@]}"; do
  n=$((n+1))
  base=$(basename "$f")
  out="$work/${base%.*}.wasm"
  if ! "$bin" --build-native "$out" --target "$triple" "$f" 2>"$work/err"; then
    if grep -q "cannot locate the $triple runtime archive\|no wasm linker" "$work/err"; then
      skipped=$((skipped+1)); n=$((n-1)); continue
    fi
    echo "FAIL $f (wasm build)"; head -5 "$work/err"; fail=1; continue
  fi
  exp="${f%.*}.expected"
  exprc_file="${f%.*}.expected-rc"
  want_rc=0
  [ -f "$exprc_file" ] && want_rc=$(cat "$exprc_file")
  got=$(node $node_flags "$runner" "$out" 2>/dev/null); rc=$?
  want=$(cat "$exp" 2>/dev/null)
  if [ "$got" != "$want" ]; then
    echo "FAIL $f (stdout != $exp)"
    diff <(echo "$want") <(echo "$got") | head -20
    fail=1
    continue
  fi
  if [ "$rc" -ne "$want_rc" ]; then
    echo "FAIL $f (exit $rc, want $want_rc)"; fail=1; continue
  fi
  echo "ok   $base"
done
echo "$n wasm-dom file(s) checked, $skipped skipped"
exit $fail
