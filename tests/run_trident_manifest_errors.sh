#!/usr/bin/env bash
# Trident's own manifest-format diagnostics (techdesign-toolchain.md H-2):
# a malformed trident.toml must fail with a clear, substring-matchable error
# from trident itself — these are format errors, not the compiler's semantic
# ones (phantom-dep/entry-rule/use-scoping stay covered by
# tests/run_project.sh through leviathan, via the plan).
trident="$1"; root="$2"; fail=0; n=0
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT

for dir in "$root"/*/; do
  name=$(basename "$dir")
  exp="$dir/expected.error"
  [ -f "$exp" ] || continue
  n=$((n+1))
  esub=$(cat "$exp")
  plan="$work/$name.lvplan"
  err=$("$trident" plan "$dir" --plan "$plan" 2>&1 >/dev/null)
  if "$trident" plan "$dir" --plan "$plan" >/dev/null 2>&1; then
    echo "FAIL $name (expected trident to reject the manifest, but it resolved)"; fail=1
  elif ! grep -qF "$esub" <<<"$err"; then
    echo "FAIL $name (error did not match '$esub')"; echo "$err" | head -3; fail=1
  fi
done

if [ $fail -eq 0 ]; then echo "$n trident manifest-format error case(s) checked"; fi
exit $fail
