#!/usr/bin/env bash
# Regression floor for bug #98 (fixed): a rule/attribute declared INSIDE the
# shipped prelude must participate in the rule stage — collect, match, and
# rewrite — exactly like a rule in an ordinary project file. Before the fix the
# rule engine only ever walked the user's own parsed tree; the prelude's AST was
# handed in solely to seed comptime globals, so a prelude-authored rule silently
# never fired and the annotated symbol ran its untouched placeholder body.
#
# This test is prelude-agnostic: it copies the real prelude to a temp dir and
# appends a self-contained `generates body of` spike to one segment, then points
# leviathan at that copy with `--prelude`. The real prelude/*.lev files are never
# touched (the spike is not a shipped feature — same rationale the bug entry gave
# for reverting its own repro edit).
#
# usage: run_prelude_rules.sh <leviathan-binary> <prelude-src-dir>
set -u
bin="$1"; preludeSrc="$2"
fail=0; n=0
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

pass() { echo "  ok: $1"; }
bad()  { echo "  FAIL: $1"; fail=1; }

copy="$tmp/prelude-copy"
cp -r "$preludeSrc" "$copy"

# A prelude-authored rule + attribute + annotated method, co-located in one
# namespace (co-location needs no `uses`). `core.lev` is target-independent, so
# this fires on every triple, not just wasm.
cat >> "$copy/core.lev" <<'EOF'

namespace PreludeRuleSpike {
    attribute GenSpike { }
    rule genSpikeRule generates body of m {
        match @GenSpike on method m
        replace `return 12345;`
    }
    @GenSpike
    int __spikeProbe() => 0;
}
EOF

# A plain caller with NO metaprogramming of its own — the rule must still fire,
# because it lives in the prelude the caller pulls in.
caller="$tmp/caller.ext"
printf 'console.writeln(PreludeRuleSpike::__spikeProbe());\n' > "$caller"

# The oracle and the bytecode IR must both see the rewritten body (12345),
# not the placeholder (0) — the §17 two-variants contract, extended to a
# prelude-declared rule.
for modeflag in --run --ir; do
    n=$((n+1))
    if out="$(LV_PRELUDE_DIR="$copy" "$bin" "$modeflag" "$caller" 2>&1)" \
       && [ "$out" = "12345" ]; then
        pass "prelude rule fires ($modeflag): $out"
    else
        bad "prelude rule did not fire ($modeflag): expected 12345, got: $out"
    fi
done

# Control: the SAME caller against the UNMODIFIED prelude must fail to resolve
# PreludeRuleSpike at all (proving the copy — not the shipped prelude — carries
# the spike, and that the test is actually exercising the prelude path).
n=$((n+1))
if out="$("$bin" --ir "$caller" 2>&1)"; then
    bad "control: unmodified prelude unexpectedly resolved PreludeRuleSpike"
else
    pass "control: spike is absent from the shipped prelude"
fi

echo "prelude_rules: ran $n cases"
exit $fail
