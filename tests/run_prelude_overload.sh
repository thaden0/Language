#!/usr/bin/env bash
# Regression floor for the arity-blind prelude-overload bug (fixed alongside the
# DOM @extern bindgen, techdesign-bindgen-metaprog-scope.md §3.1): a call to one
# of several DISTINCT-ARITY overloads of a namespaced prelude helper, made from
# an UNCHECKED prelude body, must resolve to the overload whose parameter count
# matches the call — not merely the first same-named function.
#
# Prelude bodies carry no checker-resolved call target (the prelude is trusted /
# unchecked), so the IR lowering fallback (src/ir/Lower.cpp) and the oracle's
# resolveFunction (src/runtime/Eval.cpp) resolve such calls by name. Before the
# fix they stopped at the FIRST same-named function, so every `Marshal::pack(...)`
# collapsed onto the arity-2 overload: the oracle miscounted args, and the IR /
# LLVM backends emitted a call with the wrong argument count (LLVM verifier:
# "Incorrect number of arguments passed to called function"). The DOM marshal
# family (`__act`/`__str`/`__new`) is the first prelude code to hit this — it
# calls one helper name at several arities from generated method bodies.
#
# This is the general, native-free pin (the DOM surface itself can only run on
# the wasm lane, tests/run_wasm_dom.sh, since sysHost* raise elsewhere): pure
# Leviathan overloads, so all three engines (--run oracle, --ir bytecode, LLVM
# native) execute and must AGREE. It copies the real prelude and appends a
# self-contained spike to one target-independent segment (core.lev); the shipped
# prelude/*.lev files are never touched (same discipline as run_prelude_rules.sh).
#
# usage: run_prelude_overload.sh <leviathan-binary> <prelude-src-dir>
set -u
bin="$1"; preludeSrc="$2"
fail=0; n=0
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

pass() { echo "  ok: $1"; }
bad()  { echo "  FAIL: $1"; fail=1; }

copy="$tmp/prelude-copy"
cp -r "$preludeSrc" "$copy"

# An overloaded helper family (three distinct arities) plus an unchecked-prelude
# driver that calls it at ALL THREE arities via the `::` qualifier — exactly the
# DOM marshal family's shape, but pure Leviathan so every engine can run it.
cat >> "$copy/core.lev" <<'EOF'

namespace OverloadAritySpike {
    string pack(int h, string op)                     => op + "|a1|" + h;
    string pack(int h, string op, string a)           => op + "|a2|" + h + "|" + a;
    string pack(int h, string op, string a, string b) => op + "|a3|" + h + "|" + a + "|" + b;
    string drive() {
        return OverloadAritySpike::pack(5, "one")
             + " ; " + OverloadAritySpike::pack(5, "two", "A")
             + " ; " + OverloadAritySpike::pack(5, "three", "A", "B");
    }
}
EOF

caller="$tmp/caller.ext"
printf 'console.writeln(OverloadAritySpike::drive());\n' > "$caller"
want="one|a1|5 ; two|a2|5|A ; three|a3|5|A|B"

# --run (oracle) and --ir (bytecode): both must resolve each call to the arity
# that matches, so the marshaled tuple is correct on both.
for modeflag in --run --ir; do
    n=$((n+1))
    if out="$(LV_PRELUDE_DIR="$copy" "$bin" "$modeflag" "$caller" 2>&1)" \
       && [ "$out" = "$want" ]; then
        pass "arity-correct overload ($modeflag): $out"
    else
        bad "wrong overload ($modeflag): expected [$want], got: $out"
    fi
done

# LLVM native: the pre-fix bug surfaced here as a verifier abort (wrong arg
# count). Build + run and diff stdout.
n=$((n+1))
host="$tmp/spike.host"
if LV_PRELUDE_DIR="$copy" "$bin" --build-native "$host" "$caller" >"$tmp/blderr" 2>&1 \
   && out="$("$host" 2>/dev/null | head -1)" && [ "$out" = "$want" ]; then
    pass "arity-correct overload (LLVM native): $out"
else
    bad "wrong overload / build failed (LLVM native): expected [$want], got: ${out:-<build failed>}"
    head -3 "$tmp/blderr"
fi

echo "prelude_overload: ran $n cases"
exit $fail
