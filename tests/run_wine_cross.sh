#!/usr/bin/env bash
# Track B — B-M5 Windows wine test lane (doc-2 §7): cross corpus lane under
# wine, the last of the two driver wiring items the 2026-07-05 B-M5 pass
# deferred ("a feature-detected wine test lane"). Cross-compiles the runtime
# for a MinGW triple, builds each corpus program with
# `leviathan --build-native --target <triple>`, runs the resulting .exe under
# wine, and diffs stdout against .expected.
#
# Scope (doc-2 §7's acceptance is "core + objects corpus subsets"): pass one
# or more corpus directories. Net-corpus programs (timers/sockets/http/async)
# deliberately stay OUT of this lane — they bind fixed loopback ports and
# need the RESOURCE_LOCK corpus_net_ports group (CMakeLists.txt), which this
# lane does not carry; add it explicitly before pointing this script at them.
#
# usage: run_wine_cross.sh <leviathan-binary> <wine> <triple> <corpus-dir>...
set -uo pipefail

# LA-30 M5 flip (doc 5 §5): this lane is EXPLICITLY pinned to the pump. The
# win32 task leg does not exist yet — it is the Fiber-API port (G5: TIB
# StackBase/Limit, SEH, __chkstk; never hand-rolled asm), gated behind its own
# milestone (doc 1 §5's M4 audit ruled it out of scope because this lane
# executes no async corpus: core + llvm_objects only). lv_task.c's _WIN32
# stub already reports tasks-disabled at the runtime level; this env pin is
# the S5-required *explicit* marker so the divergence is never silent. Remove
# both together when the win32 fiber leg lands.
export LANG_PUMP=1

bin="$1"; wine="$2"; triple="$3"; shift 3
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# 1. Build the per-triple runtime archive the driver will resolve by triple.
if ! LVRT_OUT_DIR="$repo/runtime/$triple" bash "$repo/runtime/build-triple.sh" "$triple"; then
  echo "run_wine_cross: could not build runtime archive for $triple" >&2
  exit 1
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

fail=0; n=0
for dir in "$@"; do
  for f in "$dir"/*.ext; do
    [ -e "$f" ] || continue
    n=$((n+1))
    exp="${f%.ext}.expected"
    stdin="${f%.ext}.stdin"
    binout="$work/$(basename "${f%.ext}")"
    if ! "$bin" --build-native "$binout" --target "$triple" "$f" >"$work/build.log" 2>&1; then
      echo "FAIL $f (cross build)"; cat "$work/build.log"; fail=1; continue
    fi
    # --build-native appends .exe for a Windows triple (src/main.cpp).
    exe="$binout.exe"
    # stdout only (2>/dev/null): matches run_qemu_cross.sh's convention — the
    # §2.5 escaping-tier meter goes to fd 2 by design and is not part of the
    # .expected diff.
    if [ -f "$stdin" ]; then
      got=$("$wine" "$exe" 2>/dev/null < "$stdin")
    else
      got=$("$wine" "$exe" 2>/dev/null)
    fi
    want=$(cat "$exp")
    if [ "$got" != "$want" ]; then
      echo "FAIL $f (wine-$triple)"
      diff <(echo "$want") <(echo "$got") | head -10
      fail=1
    fi
  done
done
echo "$n corpus file(s) checked (wine-$triple)"
exit $fail
