#!/usr/bin/env bash
# system-binds.md §6.4/§9 M3: a capability injection written directly inside
# a comptime-folded root must fail — no bind is in scope during comptime
# folding (it runs before the checker's bind-scope pass exists), so this is
# denied at the injection site itself, not the deeper sys* floor.
bin="$1"
src="$2"
out=$($bin --run "$src" 2>&1)
code=$?
if [ "$code" -eq 0 ]; then
  echo "FAIL: comptime capability injection compiled successfully"
  exit 1
fi
if [[ "$out" != *"no binding in scope for injected type 'IEnv'"* ]]; then
  echo "FAIL: comptime capability injection produced the wrong diagnostic"
  echo "$out" | head -6
  exit 1
fi
echo "comptime capability injection rejected at the injection site"
