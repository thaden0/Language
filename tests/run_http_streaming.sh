#!/usr/bin/env bash
# LA-HTTP-STREAM integration lane (designs/complete/techdesign-http_streaming_response-*.md §7).
# Runs the http_streaming corpus (head/framing, SSE return-before-end, keep-alive,
# writer-throw, pipelining, bodyless/version, flush barrier, sink abort, teardown
# churn, transport backpressure) on oracle / IR / LLVM — the three socket-capable
# engines — then sweeps the teardown churn under --mem-verify and asserts the
# reachable ROOT SET stays constant in N (the genuine no-leak proof; the LLVM
# escaping-tier byte count still carries the pre-existing base-HTTP loopback
# refcount cycle tracked as known_bugs #99, so it is reported, not gated).
#   usage: run_http_streaming.sh <leviathan-binary> <repo-root>
set -u
bin="$1"; root="$2"
cd "$root" || { echo "FAIL cannot cd $root"; exit 1; }
dir="$root/tests/corpus/http_streaming"
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
fail=0

strip_heap() { grep -v '^\[heap\]'; }

# --- 1. corpus on oracle + IR + LLVM ----------------------------------------
shopt -s nullglob
for f in "$dir"/*.lev; do
  name=$(basename "$f" .lev)
  exp="${f%.lev}.expected"
  [ -f "$exp" ] || continue
  want=$(cat "$exp")
  got=$(timeout 60 "$bin" --run "$f" 2>&1 | strip_heap)
  [ "$got" = "$want" ] && echo "ok   $name (oracle)" || { echo "FAIL $name (oracle)"; diff <(echo "$want") <(echo "$got") | head -8; fail=1; }
  got=$(timeout 90 "$bin" --ir "$f" 2>&1 | strip_heap)
  [ "$got" = "$want" ] && echo "ok   $name (ir)" || { echo "FAIL $name (ir)"; diff <(echo "$want") <(echo "$got") | head -8; fail=1; }
  if timeout 120 "$bin" --build-native "$work/$name" "$f" >"$work/$name.blog" 2>&1; then
    got=$(timeout 90 "$work/$name" 2>&1 | strip_heap)
    [ "$got" = "$want" ] && echo "ok   $name (llvm)" || { echo "FAIL $name (llvm)"; diff <(echo "$want") <(echo "$got") | head -8; fail=1; }
  else
    echo "FAIL $name (llvm codegen/link)"; head -5 "$work/$name.blog"; fail=1
  fi
done

# --- 2. teardown-churn ARC gate: reachable root set is CONSTANT in N ---------
# The oracle's reachability oracle is the ground truth for "no genuine leak":
# it must report the SAME reachable-at-exit root-set line count at small and
# large N (only program-lifetime roots survive; every connection/sink is
# reclaimed). A scaling root set would be a real teardown leak.
churn="$dir/churn.lev"
rootset_at() {
  local n="$1"
  sed "s/run(40);/run($n);/" "$churn" > "$work/churn_$n.lev"
  # The 60s budget doubles as the verifier-complexity regression floor
  # (known_bugs #106): sweep() is O(live set) per op, so N=400 finishes in a
  # couple of seconds; the old O(ops x totalAllocs) sweep took 60s+ here and
  # the timeout used to surface as a bogus "0 sites" count. A dead or
  # timed-out run must be reported as such, never counted.
  if ! timeout 60 "$bin" --run --mem-verify "$work/churn_$n.lev" \
       > "$work/churn_$n.mem" 2>&1; then
    echo "RUNFAIL"; return
  fi
  grep -c "reachable at exit" "$work/churn_$n.mem"
}
r_small=$(rootset_at 40)
r_large=$(rootset_at 400)
if [ "$r_small" = "RUNFAIL" ] || [ "$r_large" = "RUNFAIL" ]; then
  echo "FAIL churn --mem-verify run died or timed out (N=40: $r_small, N=400: $r_large)"; fail=1
elif [ "$r_small" = "$r_large" ] && [ "$r_small" -gt 0 ]; then
  echo "ok   churn root set constant ($r_small sites at N=40 and N=400)"
else
  echo "FAIL churn root set scales with N (N=40: $r_small sites, N=400: $r_large sites)"; fail=1
fi

if [ "$fail" = 0 ]; then echo "http_streaming: all lanes green"; fi
exit $fail
