#!/usr/bin/env bash
# Ship-as-files prelude (techdesign-prelude-ship-as-files-opus.md §6.1): the
# per-target selection + directory-resolution seam. None of these cases need
# the wasm linker toolchain — they exercise only the front-end (resolve/parse).
#
# usage: run_prelude_select.sh <leviathan-binary> <prelude-src-dir>
set -u
bin="$1"; preludeSrc="$2"
fail=0; n=0
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

pass() { echo "  ok: $1"; }
bad()  { echo "  FAIL: $1"; fail=1; }

# A program that references the wasm-only Dom surface from an UNCALLED function
# (so only resolution, never execution, depends on it) plus a top-level print.
dom="$tmp/domref.lev"
printf 'void touch() { DomNode n = Dom.byId("x"); }\nconsole.writeln("ok");\n' > "$dom"

# --- Case 1: native build excludes wasm.lev, so Dom does not resolve. ---------
n=$((n+1))
if out="$("$bin" --ir "$dom" 2>&1)"; then
    bad "1 native: expected non-zero exit, Dom should be absent"
elif echo "$out" | grep -qi 'dom'; then
    pass "1 native excludes Dom (resolve error names Dom)"
else
    bad "1 native: non-zero but error did not name Dom: $out"
fi

# --- Case 2: wasm32* triple includes wasm.lev, so Dom resolves; prints ok. ----
n=$((n+1))
if out="$("$bin" --ir --target wasm32-unknown-unknown "$dom" 2>&1)" \
   && echo "$out" | grep -q '^ok$'; then
    pass "2 wasm includes Dom (compiles, prints ok)"
else
    bad "2 wasm: expected exit 0 and 'ok', got: $out"
fi

# --- Case 3: an explicit --prelude override is never silently ignored. --------
n=$((n+1))
if out="$("$bin" --prelude /nonexistent/dir --ir "$dom" 2>&1)"; then
    bad "3 override: expected non-zero for a missing --prelude dir"
elif echo "$out" | grep -q 'prelude directory' \
     && echo "$out" | grep -q 'does not exist'; then
    pass "3 explicit --prelude missing dir is fatal"
else
    bad "3 override: wrong diagnostic: $out"
fi

# --- Case 4: incomplete LV_PRELUDE_DIR is fatal; a complete one compiles. -----
hello="$tmp/hello.lev"
printf 'console.writeln("hi");\n' > "$hello"
copy="$tmp/prelude-copy"
cp -r "$preludeSrc" "$copy"

n=$((n+1))
if LV_PRELUDE_DIR="$copy" "$bin" --run "$hello" 2>&1 | grep -q '^hi$'; then
    pass "4a complete LV_PRELUDE_DIR compiles"
else
    bad "4a complete LV_PRELUDE_DIR should compile a hello program"
fi

n=$((n+1))
rm -f "$copy/std.lev"
if out="$(LV_PRELUDE_DIR="$copy" "$bin" --run "$hello" 2>&1)"; then
    bad "4b incomplete: expected non-zero when std.lev is missing"
elif echo "$out" | grep -q 'std.lev'; then
    pass "4b incomplete LV_PRELUDE_DIR (missing std.lev) is fatal"
else
    bad "4b incomplete: diagnostic did not name std.lev: $out"
fi

echo "prelude_select: ran $n cases"
exit $fail
