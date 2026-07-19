#!/usr/bin/env bash
# 005 R4: a `match` value pattern that types as a bare type (a class-rooted
# static-side read like `C::f`) can never equal the subject — the R2 resolver
# leaves it a value pattern and the Checker backstop makes it a compile error
# instead of silently taking `else`.
bin="$1"
src="$2"
out=$($bin --run "$src" 2>&1)
code=$?
if [ "$code" -eq 0 ]; then
  echo "FAIL: type-as-value match pattern compiled successfully"
  exit 1
fi
if [[ "$out" != *"used as a value — this arm can never match"* ]]; then
  echo "FAIL: type-as-value match pattern produced the wrong diagnostic"
  echo "$out" | head -6
  exit 1
fi
echo "type-as-value match pattern rejected at compile time"
