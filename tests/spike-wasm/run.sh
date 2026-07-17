#!/usr/bin/env bash
# Track W — W-M0 de-risking spike (designs/wasm-frontend/techdesign-01-spike.md §3.4).
#
# THROWAWAY differential harness. For each pure-compute corpus file it:
#   1. emits a wasm32 object via the UNMODIFIED compiler (--native-obj --target),
#   2. hand-links it against a wasm build of the real runtime (lv_runtime.c +
#      lv_loop.c + lv_entry.c) plus the spike floor (lv_plat_spike.c) and
#      wasi-libc,
#   3. runs it under wasmtime,
#   4. asserts byte-identical stdout against the IR interpreter (--ir).
#
# The point (proposal §9): prove the uniform tagged 16-byte LvValue compiles and
# runs on wasm32 with NO compiler edits and NO monomorphization. This wires no
# CTest and commits to nothing (spike §3, §4). doc-02/03 build the real lane.
#
# usage:  run.sh <leviathan-binary> [corpus-file ...]
#   with no corpus files, runs the curated pure-compute subset below.
#
# Toolchain (env overrides; all must resolve or the run SKIPs, like run_wasm.sh):
#   CLANG          clang that can target wasm32-wasi          (default: clang)
#   WASM_LD        the wasm linker                            (default: wasm-ld)
#   WASMTIME       the runner                                 (default: wasmtime)
#   WASI_SYSROOT   wasi-libc sysroot (has lib/wasm32-wasi/)   (required)
#   WASI_BUILTINS  libclang_rt.builtins-wasm32.a              (required)
# See designs/requests/proposal-wasm-frontend.md §14 for where these came from
# on the spike host.
set -u

bin="${1:?usage: run.sh <leviathan-binary> [corpus-file ...]}"; shift || true
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
rt="$repo/runtime"
triple=wasm32-wasi

CLANG="${CLANG:-clang}"
WASM_LD="${WASM_LD:-wasm-ld}"
WASMTIME="${WASMTIME:-wasmtime}"
SR="${WASI_SYSROOT:-}"
BUILTINS="${WASI_BUILTINS:-}"

skip() { echo "SKIP spike-wasm: $1"; exit 0; }
command -v "$CLANG"    >/dev/null 2>&1 || skip "no clang (set CLANG=) that targets wasm32-wasi"
command -v "$WASM_LD"  >/dev/null 2>&1 || skip "no wasm-ld (set WASM_LD=; install lld)"
command -v "$WASMTIME" >/dev/null 2>&1 || skip "no wasmtime (set WASMTIME=)"
[ -n "$SR" ] && [ -e "$SR/lib/wasm32-wasi/libc.a" ] || skip "WASI_SYSROOT not set / no libc.a (apt wasi-libc, then point at the sysroot)"
[ -n "$BUILTINS" ] && [ -e "$BUILTINS" ] || skip "WASI_BUILTINS not set (libclang_rt.builtins-wasm32.a from a wasi-sdk release)"

L="$SR/lib/wasm32-wasi"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

CC=("$CLANG" --target="$triple" --sysroot="$SR" -std=gnu17 -O2 -I "$rt")

echo "== building the wasm runtime (once) =="
# lv_loop.c uses memcpy without <string.h> (glibc pulls it in transitively; wasi
# does not) — supply it as a build flag, no source edit (spike = zero edits).
"${CC[@]}" -c "$rt/lv_runtime.c"       -o "$work/lv_runtime.o"   || skip "lv_runtime.c failed to compile for $triple"
"${CC[@]}" -include string.h -c "$rt/lv_loop.c" -o "$work/lv_loop.o" || skip "lv_loop.c failed to compile for $triple"
"${CC[@]}" -c "$rt/lv_entry.c"         -o "$work/lv_entry.o"     || skip "lv_entry.c failed to compile for $triple"
"${CC[@]}" -c "$here/lv_plat_spike.c"  -o "$work/lv_plat_spike.o"|| skip "lv_plat_spike.c failed to compile for $triple"
RT_OBJS=("$work/lv_runtime.o" "$work/lv_loop.o" "$work/lv_entry.o" "$work/lv_plat_spike.o")

# curated pure-compute subset (objects, generics, structs, arrays, maps,
# closures, strings, arithmetic, control flow, exceptions — print only).
default_corpus=(
  math structs structs_array generics collections loops match match_nested
  classes oo floats literals const sieve strcmp bitops optional exceptions
  cow maps_set namespaces qualified use seq iterator readonly
  class_dispatch method_refs named_defaults
)
# NB: using.ext (File) and generic_iface.lev (Channel) are intentionally NOT
# here — the compiler's capability gate (hard-03) rejects them at emit time
# with a clean "not available on this target" diagnostic. That is the gate
# working, not a spike failure; run_wasm.sh's gated_ lane owns those pins.
files=()
if [ "$#" -gt 0 ]; then
  files=("$@")
else
  for b in "${default_corpus[@]}"; do
    for ext in ext lev; do
      [ -f "$repo/tests/corpus/$b.$ext" ] && { files+=("$repo/tests/corpus/$b.$ext"); break; }
    done
  done
fi

pass=0; fail=0
for f in "${files[@]}"; do
  base="$(basename "$f")"; base="${base%.*}"
  obj="$work/$base.o"; mod="$work/$base.wasm"
  if ! "$bin" --native-obj "$obj" --target "$triple" "$f" 2>"$work/emit.err"; then
    echo "FAIL $base (emit)"; head -3 "$work/emit.err"; fail=$((fail+1)); continue
  fi
  if ! "$WASM_LD" "$L/crt1.o" "$obj" "${RT_OBJS[@]}" "$L/libc.a" "$L/libm.a" "$BUILTINS" \
        -o "$mod" 2>"$work/link.err"; then
    echo "FAIL $base (link)"; grep -i error "$work/link.err" | head -3; fail=$((fail+1)); continue
  fi
  got="$("$WASMTIME" run "$mod" 2>/dev/null)"
  want="$("$bin" --ir "$f" 2>/dev/null)"
  if [ "$got" = "$want" ]; then
    echo "ok   $base"; pass=$((pass+1))
  else
    echo "FAIL $base (wasm stdout != --ir)"; diff <(echo "$want") <(echo "$got") | head -12; fail=$((fail+1))
  fi
done

echo "== spike-wasm: $pass matched, $fail mismatched (of $((pass+fail))) =="
[ "$fail" -eq 0 ]
