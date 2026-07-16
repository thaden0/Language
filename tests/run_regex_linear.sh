#!/usr/bin/env bash
# Timed pathological-family gate. Permit scheduler noise, reject superlinear blow-up.
bin="$1"
src="$2"
out=$($bin --run "$src") || exit 1
if [[ "$out" == *":true"* ]]; then
  echo "FAIL: pathological non-match reported a match"
  exit 1
fi
t200=$(echo "$out" | sed -n '2s/^[^:]*:\([^:]*\):.*$/\1/p')
t400=$(echo "$out" | sed -n '3s/^[^:]*:\([^:]*\):.*$/\1/p')
if [ -z "$t200" ] || [ -z "$t400" ] || [ "$t400" -gt $((t200 * 3 + 50)) ]; then
  echo "FAIL: pathological regex scaling was not linear-ish"
  echo "$out"
  exit 1
fi
echo "$out"
