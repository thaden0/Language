#!/usr/bin/env bash
# Moby Track 10 §4 — the differential engine-matrix corpus runner.
#
# Every test is a trident project directory holding a <name>.expected golden and
# a trident.toml (dep on ../.. as Moby). Each test runs on the tree-walk oracle,
# the IR interpreter, and the LLVM native binary against ONE shared .expected
# (byte-identical across engines — the ecosystem's standing rule). emit-C++ gets
# a COMPILE-ONLY lane: the package minus run()/pump paths must compile; a test
# that legitimately uses the async run loop is SKIPPED there (no silent caps —
# every skip is printed). LLVM's stdout is compared alone; its `[heap]` meter
# goes to stderr and is expected.
#
# usage: runtests.sh [test-dir ...]   (default: every dir under tests/ with a golden)
set -u
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
repo="$(cd "$root/.." && pwd)"
LEV="${LEVIATHAN:-$repo/build/leviathan}"
TRIDENT="${TRIDENT:-$repo/build/trident}"
LVRT="${LVRT:-$repo/build/liblvrt.a}"
CC="${CC:-cc}"
CXX="${CXX:-g++}"

fail=0; npass=0; nskip=0
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT

# Examples ship a scripted-mode flag: MOBY_SCRIPT=1 binds ScriptedInput +
# TestRenderer and prints snapshots instead of calling App.run() (§6). Exporting
# it here makes every example ALSO a differential test; the golden tests under
# tests/ do not read it, so it is harmless there.
export MOBY_SCRIPT=1

# The golden for a test dir: <dir>/<name>.expected (name = a single .lev entry's
# basename, or the dir name).
golden_for() {
  local dir="$1" g
  g=$(ls "$dir"/*.expected 2>/dev/null | head -1)
  echo "$g"
}

run_one() {
  local dir="$1"
  local name; name="$(basename "$dir")"
  [ -f "$dir/trident.toml" ] || return 0                 # only trident projects
  [ -f "$dir/expected.error" ] && return 0               # negative tests: run elsewhere
  local exp; exp="$(golden_for "$dir")"
  [ -z "$exp" ] && return 0     # not a golden test dir
  local w="$work/$name"; mkdir -p "$w"
  local absdir; absdir="$(cd "$dir" && pwd)"

  # 1. resolve the trident plan. Plans hold paths relative to the test dir, so
  #    every leviathan invocation below runs with cwd = the test dir.
  if ! ( cd "$absdir" && "$TRIDENT" plan . ) >"$w/plan.log" 2>&1; then
    echo "FAIL $name (plan)"; sed 's/^/    /' "$w/plan.log" | head -8; fail=1; return
  fi
  local plan="build/plan.lvplan"
  if [ ! -f "$absdir/$plan" ]; then echo "FAIL $name (no plan)"; fail=1; return; fi
  local want; want="$(cat "$exp")"

  # 2. oracle
  local got; got="$( cd "$absdir" && "$LEV" --plan "$plan" --run 2>"$w/o.err" )"
  if [ "$got" != "$want" ]; then echo "FAIL $name (oracle)"; diff <(echo "$want") <(echo "$got") | head -8; fail=1; return; fi

  # 3. IR
  got="$( cd "$absdir" && "$LEV" --plan "$plan" --ir 2>"$w/i.err" )"
  if [ "$got" != "$want" ]; then echo "FAIL $name (ir)"; diff <(echo "$want") <(echo "$got") | head -8; fail=1; return; fi

  # 4. LLVM native (compare stdout only; [heap] meter is on stderr)
  if ( cd "$absdir" && "$LEV" --plan "$plan" --native-obj "$w/a.o" ) 2>"$w/ll.err"; then
    if "$CC" -O2 -o "$w/a" "$w/a.o" "$LVRT" -lm -lpthread -lssl -lcrypto 2>"$w/lk.err"; then
      got="$("$w/a" 2>/dev/null)"
      if [ "$got" != "$want" ]; then echo "FAIL $name (llvm)"; diff <(echo "$want") <(echo "$got") | head -8; fail=1; return; fi
    else echo "FAIL $name (llvm link)"; head -6 "$w/lk.err"; fail=1; return; fi
  else echo "FAIL $name (llvm codegen)"; head -6 "$w/ll.err"; fail=1; return; fi

  # 5. emit-C++ compile-only lane (skip on the documented async/native gap)
  if ( cd "$absdir" && "$LEV" --plan "$plan" --emit-cpp ) >"$w/c.cpp" 2>"$w/c.err"; then
    if ! "$CXX" -O2 -c -o "$w/c.o" "$w/c.cpp" 2>"$w/cc.err"; then
      echo "FAIL $name (emit-c++ compile)"; head -6 "$w/cc.err"; fail=1; return
    fi
    echo "ok   $name (oracle+ir+llvm+cpp)"
  elif grep -qE "native backend|not yet lowerable" "$w/c.err"; then
    echo "ok   $name (oracle+ir+llvm; cpp SKIP: async/native gap)"; nskip=$((nskip+1))
  else
    echo "FAIL $name (emit-c++ emit)"; head -6 "$w/c.err"; fail=1; return
  fi
  npass=$((npass+1))
}

if [ "$#" -gt 0 ]; then dirs=("$@"); else
  dirs=()
  for d in "$here"/*/ "$root"/examples/*/; do
    [ -f "$d/trident.toml" ] && dirs+=("$d")
  done
fi

for d in "${dirs[@]}"; do run_one "$d"; done
echo "---"
echo "$npass test(s) passed, $nskip emit-C++ skip(s)"
exit $fail
