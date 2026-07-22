#!/usr/bin/env bash
# Sonar test matrix. Each suite is a Trident project ([[dep]] sonar) with a
# <name>.expected golden. Every suite runs on the tree-walk oracle, the IR
# interpreter, and the LLVM native binary against ONE shared .expected
# (byte-identical across engines — the ecosystem's standing rule). The
# discovery suite is additionally diffed against the hand-seeded registry_runner
# twin (the rule_orm twin discipline: rule-discovered == hand-seeded), and the
# inline/sibling examples are diffed against each other (location-agnostic
# discovery). emit-C++ has no async surface, so the concurrency suite is not run
# there; every other suite compiles.
#
# usage: runtests.sh            (run everything)
set -u
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
repo="$(cd "$root/.." && pwd)"
LEV="${LEVIATHAN:-$repo/build/leviathan}"
TRIDENT="${TRIDENT:-$repo/build/trident}"
LVRT="${LVRT:-$repo/build/liblvrt.a}"
CC="${CC:-cc}"
fail=0; npass=0
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT

# Resolve a Trident project and run it on oracle/IR/LLVM; echo the oracle output
# on success, and fail loudly on any engine divergence. $1 = project dir.
run_project() {
  local dir="$1" name; name="$(basename "$dir")"
  ( cd "$dir" && rm -rf build && "$TRIDENT" plan . ) >"$work/$name.plan" 2>&1 || {
    echo "FAIL $name (plan)"; sed 's/^/    /' "$work/$name.plan" | head -8; fail=1; return 1; }
  local plan="$dir/build/plan.lvplan"
  local o i
  o="$( cd "$dir" && "$LEV" --plan build/plan.lvplan --run 2>/dev/null )"
  i="$( cd "$dir" && "$LEV" --plan build/plan.lvplan --ir  2>/dev/null )"
  if [ "$o" != "$i" ]; then echo "FAIL $name (oracle != IR)"; diff <(echo "$o") <(echo "$i") | head -8; fail=1; return 1; fi
  if ( cd "$dir" && "$LEV" --plan build/plan.lvplan --native-obj "$work/$name.o" ) 2>"$work/$name.ll"; then
    if "$CC" -O2 -o "$work/$name.bin" "$work/$name.o" "$LVRT" -lm -lpthread -lssl -lcrypto 2>"$work/$name.lk"; then
      local l; l="$("$work/$name.bin" 2>/dev/null)"
      if [ "$o" != "$l" ]; then echo "FAIL $name (oracle != LLVM)"; diff <(echo "$o") <(echo "$l") | head -8; fail=1; return 1; fi
    else echo "FAIL $name (llvm link)"; head -6 "$work/$name.lk"; fail=1; return 1; fi
  else echo "FAIL $name (llvm codegen)"; head -6 "$work/$name.ll"; fail=1; return 1; fi
  ( cd "$dir" && rm -rf build )
  echo "$o" > "$work/$name.out"
  return 0
}

check_golden() {
  local dir="$1" name; name="$(basename "$dir")"
  run_project "$dir" || return
  if diff -q "$dir/main.expected" "$work/$name.out" >/dev/null; then
    echo "ok   $name (oracle==IR==LLVM==golden)"; npass=$((npass+1))
  else
    echo "FAIL $name (golden)"; diff "$dir/main.expected" "$work/$name.out" | head -8; fail=1
  fi
}

for suite in assertions registry_runner discovery concurrency; do
  check_golden "$here/$suite"
done

# Twin: rule-discovered == hand-seeded.
if diff -q "$work/discovery.out" "$work/registry_runner.out" >/dev/null 2>&1; then
  echo "ok   twin (discovery == registry_runner, byte-identical)"; npass=$((npass+1))
else
  echo "FAIL twin (discovery != registry_runner)"; fail=1
fi

# Location-agnostic: inline example == sibling example.
run_project "$root/examples/inline"        >/dev/null && mv "$work/inline.out"        "$work/ex_inline.out"        2>/dev/null
run_project "$root/examples/sibling/tests" >/dev/null && mv "$work/tests.out"         "$work/ex_sibling.out"       2>/dev/null
if diff -q "$work/ex_inline.out" "$work/ex_sibling.out" >/dev/null 2>&1; then
  echo "ok   examples (inline == sibling, location-agnostic discovery)"; npass=$((npass+1))
else
  echo "FAIL examples (inline != sibling)"; fail=1
fi

echo "---"
[ "$fail" = 0 ] && echo "$npass check(s) passed" || echo "FAILURES above"
exit $fail
