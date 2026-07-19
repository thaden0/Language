#!/usr/bin/env bash
# LA-31 Stage 3 §2 (techdesign-03-verification.md): hand-written-twin
# byte-equivalence. A twin file hand-authors the exact expr::Expr(...)
# construction a reified sibling file produces; --expand of both files must
# print that construction statement byte-identically (the reifier's output
# is, by construction, exactly what a human would have written by hand).
# usage: run_expr_reify_twin.sh <leviathan-binary> <original.lev> <twin.lev>
bin="$1"; orig="$2"; twin="$3"
o=$("$bin" --expand "$orig" 2>&1 | grep -o 'expr::Expr(.*')
t=$("$bin" --expand "$twin" 2>&1 | grep -o 'expr::Expr(.*')
if [ -z "$o" ]; then
  echo "FAIL: no expr::Expr(...) construction found in --expand of $orig"
  exit 1
fi
if [ -z "$t" ]; then
  echo "FAIL: no expr::Expr(...) construction found in --expand of $twin"
  exit 1
fi
if [ "$o" != "$t" ]; then
  echo "FAIL: $orig and $twin do not print byte-identical constructions"
  diff <(echo "$o") <(echo "$t")
  exit 1
fi
echo "OK: $orig and $twin print byte-identical constructions"
