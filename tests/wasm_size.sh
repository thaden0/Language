#!/usr/bin/env bash
# Track W — W-M4 §3 size & performance visibility
# (designs/wasm-frontend/techdesign-06-bindgen-and-ship.md §3).
#
# Records each program's .wasm size at --opt-level 0 and 2, raw and gzip -9,
# so the size CURVE is visible from W-M1 on. NO budget is enforced in v1 (doc
# 06 §3): this measures, it never fails on size. When `wasm-opt` is on PATH it
# also records a `wasm-opt -O2` post-pass size — a measurement only; whether to
# ADOPT that pass is a separate call ("only if free — no behaviour diff on the
# corpus", doc 06 §3), which is run_wasm.sh's differential, not this script's.
#
# usage: wasm_size.sh <leviathan-binary> <file-or-dir> [more ...]
#
# SKIPs cleanly (exit 0, like tests/run_wasm.sh) when the wasm32 runtime
# archive or the wasm linker is absent — those belong to the archive owner,
# not this lane. Build the archive with runtime/build-triple.sh wasm32-wasi.
set -u
bin="${1:?usage: wasm_size.sh <leviathan-binary> <file-or-dir> ...}"; shift
triple=wasm32-unknown-unknown

have_wasmopt=0
command -v wasm-opt >/dev/null 2>&1 && have_wasmopt=1

shopt -s nullglob
files=()
for arg in "$@"; do
  if [ -d "$arg" ]; then files+=("$arg"/*.lev "$arg"/*.ext); else files+=("$arg"); fi
done

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

gz() { gzip -9 -c "$1" | wc -c; }        # gzipped byte count
sz() { wc -c < "$1"; }

# header
if [ "$have_wasmopt" -eq 1 ]; then
  printf '%-40s %10s %10s %10s %10s %10s %10s\n' \
    file O0 O0.gz O2 O2.gz wopt wopt.gz
else
  printf '%-40s %10s %10s %10s %10s\n' file O0 O0.gz O2 O2.gz
  echo "(wasm-opt not on PATH — post-pass column omitted)"
fi

n=0; skipped=0
for f in "${files[@]}"; do
  base=$(basename "$f")
  o0="$work/${base%.*}.O0.wasm"
  o2="$work/${base%.*}.O2.wasm"
  if ! "$bin" --build-native "$o0" --opt-level 0 --target "$triple" "$f" 2>"$work/err"; then
    if grep -q "cannot locate the $triple runtime archive\|no wasm linker" "$work/err"; then
      skipped=$((skipped+1)); continue
    fi
    echo "FAIL $f (wasm build, O0)"; head -5 "$work/err"; continue
  fi
  if ! "$bin" --build-native "$o2" --opt-level 2 --target "$triple" "$f" 2>"$work/err"; then
    echo "FAIL $f (wasm build, O2)"; head -5 "$work/err"; continue
  fi
  n=$((n+1))
  if [ "$have_wasmopt" -eq 1 ]; then
    wo="$work/${base%.*}.wopt.wasm"
    wasm-opt -O2 "$o2" -o "$wo" 2>/dev/null || cp "$o2" "$wo"
    printf '%-40s %10s %10s %10s %10s %10s %10s\n' \
      "$base" "$(sz "$o0")" "$(gz "$o0")" "$(sz "$o2")" "$(gz "$o2")" "$(sz "$wo")" "$(gz "$wo")"
  else
    printf '%-40s %10s %10s %10s %10s\n' \
      "$base" "$(sz "$o0")" "$(gz "$o0")" "$(sz "$o2")" "$(gz "$o2")"
  fi
done

echo "$n file(s) measured, $skipped skipped (archive/linker absent)"
exit 0
