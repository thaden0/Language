#!/usr/bin/env bash
# LA-31 Stage 3 §3 (techdesign-03-verification.md): a reification reject is a
# hard compile error carrying a named diagnostic, at the lambda site naming
# the construct. Runs `--run <src>`, asserts a non-zero exit and that the
# expected substring appears in the output.
# usage: run_expr_reify_error.sh <leviathan> <src> <expected-substring>
bin="$1"; src="$2"; want="$3"
out=$("$bin" --run "$src" 2>&1)
code=$?
if [ "$code" -eq 0 ]; then
  echo "FAIL: $src compiled successfully (expected a reification reject)"
  echo "$out" | head -6
  exit 1
fi
if [[ "$out" != *"$want"* ]]; then
  echo "FAIL: $src did not produce the expected diagnostic"
  echo "  wanted substring: $want"
  echo "$out" | head -8
  exit 1
fi
echo "OK: $src rejected with '$want'"
