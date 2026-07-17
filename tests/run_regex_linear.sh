#!/usr/bin/env bash
# Pathological-family linearity gate. techdesign-regex-linear-gate.md: assert on
# the engine's DETERMINISTIC step count (programPikeProbe), not wall-clock, so a
# correct engine passes regardless of host speed / parallel load / valgrind. The
# CTest TIMEOUT stays as a pure hang-guard, not a throughput judge.
#
# Because the step count is engine-independent (identical on --run/--ir/LLVM),
# the gate is free to pick the fastest no-build engine — the IR interpreter,
# ~2.8x the tree-walk oracle — for the same assertion on the same numbers.
#
# Input lines are `n:steps:hit` for n = 50/100/200. Pass iff:
#   - no probe reports a match  (hit must be false — pathological non-match)
#   - steps200 <= 2*steps100 + steps50   (doubling input at most doubles work;
#     the smallest probe is the self-scaling O(1)/startup slack term)
bin="$1"
src="$2"
out=$($bin --ir "$src") || exit 1
if [[ "$out" == *":true"* ]]; then
  echo "FAIL: pathological non-match reported a match"
  echo "$out"
  exit 1
fi
s50=$(echo "$out"  | sed -n '1s/^[^:]*:\([^:]*\):.*$/\1/p')
s100=$(echo "$out" | sed -n '2s/^[^:]*:\([^:]*\):.*$/\1/p')
s200=$(echo "$out" | sed -n '3s/^[^:]*:\([^:]*\):.*$/\1/p')
if [ -z "$s50" ] || [ -z "$s100" ] || [ -z "$s200" ]; then
  echo "FAIL: could not parse step counts"
  echo "$out"
  exit 1
fi
if [ "$s200" -gt $((2 * s100 + s50)) ]; then
  echo "FAIL: pathological regex scaling was not linear-ish"
  echo "$out"
  exit 1
fi
echo "$out"
