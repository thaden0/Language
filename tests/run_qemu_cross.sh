#!/usr/bin/env bash
# Track B — B-M4 item 4 (doc-2 §6): cross corpus lane under qemu-user.
# Cross-compiles the runtime for <triple>, builds each corpus program with
# `leviathan --build-native --target <triple>`, runs it under qemu-user, and diffs
# against .expected. Enabled by CMake ONLY when qemu + a cross compiler are
# present (feature-detected), so it never runs on a host without a cross
# environment. Env LVRT_SYSROOT is threaded to both the runtime cross-build
# and qemu's -L library path.
#
# usage: run_qemu_cross.sh <leviathan-binary> <qemu> <triple> <corpus-dir>
set -uo pipefail

bin="$1"; qemu="$2"; triple="$3"; dir="$4"
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
sysroot="${LVRT_SYSROOT:-}"

# 1. Build the per-triple runtime archive the driver will resolve by triple.
if ! LVRT_OUT_DIR="$repo/runtime/$triple" bash "$repo/runtime/build-triple.sh" "$triple"; then
  echo "run_qemu_cross: could not build runtime archive for $triple" >&2
  exit 1
fi

qemu_args=("$qemu")
[ -n "$sysroot" ] && qemu_args+=(-L "$sysroot")

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

fail=0; n=0
for f in "$dir"/*.ext; do
  [ -e "$f" ] || continue
  n=$((n+1))
  exp="${f%.ext}.expected"
  stdin="${f%.ext}.stdin"
  binout="$work/$(basename "${f%.ext}")"
  if ! "$bin" --build-native "$binout" --target "$triple" "$f" >"$work/build.log" 2>&1; then
    echo "FAIL $f (cross build)"; cat "$work/build.log"; fail=1; continue
  fi
  # stdout only, matching run_native_llvm.sh's convention: the §2.5
  # escaping-tier meter (LlvmGen.cpp's lv_main exit report) goes to fd 2 by
  # design ("stdout is untouched, so the corpus differentials are
  # unaffected") and must not be merged into the .expected diff.
  if [ -f "$stdin" ]; then
    got=$("${qemu_args[@]}" "$binout" < "$stdin")
  else
    got=$("${qemu_args[@]}" "$binout")
  fi
  want=$(cat "$exp")
  if [ "$got" != "$want" ]; then
    echo "FAIL $f (qemu-$triple)"
    diff <(echo "$want") <(echo "$got") | head -10
    fail=1
  fi
done
echo "$n corpus file(s) checked (qemu-$triple)"
exit $fail
