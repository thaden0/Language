#!/usr/bin/env bash
# Track W (techdesign-02-backend-column.md §7): the wasm corpus lane.
# usage: run_wasm.sh <leviathan-binary> <file-or-dir> [file-or-dir ...]
#
# Three modes per file:
#  - gated_*.lev — NEGATIVE (hard-03 tier-1 pin): the wasm compile must FAIL
#    and its stderr must contain the pinned diagnostic in <file>.expected.
#  - trap_*.lev — RUNTIME TRAP (hard-03 tier-2 pin, doc 03 §5): the wasm
#    compile must SUCCEED (tier 2 keeps @ginit-only gated natives buildable),
#    the run must exit 134 (lvrt_unsupported's lv_plat_exit code), and its
#    stderr must contain the pinned message in <file>.expected.
#  - everything else — end-to-end: --build-native out.wasm --target
#    wasm32-unknown-unknown, run headlessly (wasm_node_run.mjs — see below),
#    diff its output against the --ir lane. SKIPs (not fails) while the §6
#    runtime archive or wasm-ld are missing — those are the archive owner's,
#    not the HARD packets'.
#
# Why Node instead of plain `wasmtime run --invoke main`: wasmtime's CLI only
# auto-supplies WASI imports, but lv_plat_wasm.c's real output path is the
# "lv" import module (techdesign-03-floor-wasm.md §1), which nothing but our
# own host code can satisfy. wasm_node_run.mjs is that host, built on Node's
# native WebAssembly support and runtime/lv_host.js's shared import object —
# doc 03 §3's "node/wasmtime shim", concretely.
#
# W-M2 (techdesign-04-async-jspi.md §5) adds two modes and the JSPI floor:
#  - probe_*.lev — like end-to-end, but run with LV_PROBE_FETCH=1: the driver
#    additionally drives one lv_probe_fetch activation through a GENUINE
#    Suspending fetch (data: URI) + a yield while the program's own
#    activations park on timers, and fails (exit 70) unless it returns 42.
#  - the async corpus needs JSPI. Headless host of record: Node >= 24 with
#    --experimental-wasm-jspi (V8 ships JSPI behind that flag in Node 24;
#    Chrome >= 137 has it on by default — the browser check's floor).
#    Detected below; without it, pure-compute files still pass (the host
#    degrades to plain imports) and async files fail LOUD with lv_host.js's
#    message naming this floor — never a silent skip.
bin="$1"; shift
fail=0; n=0; skipped=0
triple=wasm32-unknown-unknown
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
node_runner="$here/wasm_node_run.mjs"
node_flags=""
if node --experimental-wasm-jspi -e "" >/dev/null 2>&1; then
  node_flags="--experimental-wasm-jspi"
fi
shopt -s nullglob
files=()
for dir in "$@"; do
  if [ -d "$dir" ]; then files+=("$dir"/*.ext "$dir"/*.lev); else files+=("$dir"); fi
done
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
for f in "${files[@]}"; do
  n=$((n+1))
  base=$(basename "$f")
  if [[ "$base" == gated_* ]]; then
    exp="${f%.*}.expected"
    got=$("$bin" --native-obj "$work/gate.o" --target "$triple" "$f" 2>&1)
    rc=$?
    if [ $rc -eq 0 ]; then
      echo "FAIL $f (gated program compiled for wasm)"; fail=1; continue
    fi
    if ! echo "$got" | grep -qF "$(cat "$exp")"; then
      echo "FAIL $f (diagnostic mismatch)"
      echo "want substring: $(cat "$exp")"
      echo "got: $got" | head -5
      fail=1
    fi
    continue
  fi
  if ! command -v node >/dev/null; then
    skipped=$((skipped+1)); continue
  fi
  out="$work/${base%.*}.wasm"
  if ! "$bin" --build-native "$out" --target "$triple" "$f" 2>"$work/err"; then
    if grep -q "cannot locate the $triple runtime archive\|no wasm linker" "$work/err"; then
      skipped=$((skipped+1)); continue     # §6 archive / wasm-ld not built yet
    fi
    echo "FAIL $f (wasm build)"; head -5 "$work/err"; fail=1; continue
  fi
  if [[ "$base" == trap_* ]]; then
    exp="${f%.*}.expected"
    node $node_flags "$node_runner" "$out" >/dev/null 2>"$work/trap_err"; rc=$?
    if [ $rc -ne 134 ]; then
      echo "FAIL $f (want exit 134, got $rc)"; fail=1; continue
    fi
    if ! grep -qF "$(cat "$exp")" "$work/trap_err"; then
      echo "FAIL $f (trap message mismatch)"
      echo "want substring: $(cat "$exp")"
      echo "got: $(head -3 "$work/trap_err")"
      fail=1
    fi
    continue
  fi
  if [[ "$base" == probe_* ]]; then
    want=$("$bin" --ir "$f" 2>/dev/null)
    got=$(LV_PROBE_FETCH=1 node $node_flags "$node_runner" "$out" 2>"$work/probe_err"); rc=$?
    if [ $rc -ne 0 ]; then
      echo "FAIL $f (probe run rc=$rc)"; head -3 "$work/probe_err"; fail=1; continue
    fi
    if [ "$got" != "$want" ]; then
      echo "FAIL $f (wasm output != --ir under probe)"
      diff <(echo "$want") <(echo "$got") | head -10
      fail=1
    fi
    continue
  fi
  # stdout only (matches tests/run_qemu_cross.sh's convention): the LLVM
  # backend's escaping-tier meter (LlvmGen.cpp "[heap] escaping-tier...")
  # prints unconditionally to fd 2 on every native/cross/wasm build and has
  # no --ir counterpart — comparing it would FAIL every file, not just wasm.
  # Exit codes are diffed too since W-M2: the async corpus pins C2/C3's
  # exit-1 contract (a crashed program must not report success).
  want=$("$bin" --ir "$f" 2>/dev/null); want_rc=$?
  got=$(node $node_flags "$node_runner" "$out" 2>/dev/null); got_rc=$?
  if [ "$got" != "$want" ] || [ $got_rc -ne $want_rc ]; then
    echo "FAIL $f (wasm output/exit != --ir: rc $got_rc vs $want_rc)"
    diff <(echo "$want") <(echo "$got") | head -10
    fail=1
  fi
done
echo "$n wasm corpus file(s) checked, $skipped skipped"
exit $fail
