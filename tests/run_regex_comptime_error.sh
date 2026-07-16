#!/usr/bin/env bash
# M5: malformed constant patterns fail at the source line during comptime.
bin="$1"
src="$2"
out=$($bin --run "$src" 2>&1)
code=$?
if [ "$code" -eq 0 ]; then
  echo "FAIL: malformed comptime regex compiled successfully"
  exit 1
fi
if [[ "$out" != *"comptime evaluation failed: regex: unterminated class at offset 1"* ]]; then
  echo "FAIL: malformed comptime regex produced the wrong diagnostic"
  echo "$out" | head -6
  exit 1
fi
echo "malformed comptime regex rejected at source"
