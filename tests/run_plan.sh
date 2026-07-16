#!/usr/bin/env bash
# Build-plan contract conformance (techdesign-toolchain.md §3.3): a small,
# hand-written plan.lvplan per fixture (no trident involved) must drive
# `leviathan --plan` to the fixture's own expected.txt/expected.error, for a
# File-entry, a Function-entry, and a phantom-dep case. This is independent,
# permanent regression insurance for the frozen contract itself — trident's
# writer and leviathan's reader must keep agreeing on the wire format even as
# each evolves on its own side of the split.
#
# `@DIR@` in each plan.lvplan.tmpl stands in for the fixture's own absolute
# directory — the plan format takes only absolute, already-resolved paths
# (contract rule 1).
bin="$1"; root="$2"; fail=0
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT

check_case() {
  name="$1"; dir="$root/$name"; tmpl="$dir/plan.lvplan.tmpl"
  if [ ! -f "$tmpl" ]; then echo "FAIL $name (no plan.lvplan.tmpl)"; fail=1; return; fi
  plan="$work/$name.lvplan"
  sed "s#@DIR@#$dir#g" "$tmpl" > "$plan"

  if [ -f "$dir/expected.error" ]; then
    esub=$(cat "$dir/expected.error")
    err=$("$bin" --run --plan "$plan" 2>&1 >/dev/null)
    if "$bin" --run --plan "$plan" >/dev/null 2>&1; then
      echo "FAIL $name (expected an error, but it compiled)"; fail=1
    elif ! grep -qF "$esub" <<<"$err"; then
      echo "FAIL $name (error did not match '$esub')"; echo "$err" | head -3; fail=1
    fi
    return
  fi

  want=$(cat "$dir/expected.txt")
  got=$("$bin" --run --plan "$plan" 2>/dev/null)
  if [ "$got" != "$want" ]; then
    echo "FAIL $name (--plan --run output did not match expected.txt)"
    diff <(echo "$want") <(echo "$got") | head -8; fail=1
  fi
}

check_case entry_file
check_case entry_fn
check_case phantom_dep

if [ $fail -eq 0 ]; then echo "3 build-plan conformance case(s) checked"; fi
exit $fail
