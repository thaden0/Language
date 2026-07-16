#!/usr/bin/env bash
# doc 2 §6: an un-narrowed find() result is a compile error in user code.
bin="$1"
src="$2"
out=$($bin --run "$src" 2>&1)
code=$?
if [ "$code" -eq 0 ]; then
  echo "FAIL: un-narrowed Match? member access compiled successfully"
  exit 1
fi
if [[ "$out" != *"narrow the union before member access"* ]]; then
  echo "FAIL: un-narrowed Match? access produced the wrong diagnostic"
  echo "$out" | head -6
  exit 1
fi
echo "un-narrowed find() result rejected at compile time"
