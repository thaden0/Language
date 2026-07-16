#!/usr/bin/env bash
# Red-lane corpus runner (designs/complete/techdesign-composition-corpus.md).
# Each *.lev/*.ext in <dir> is a minimal repro for a currently-open bug; its
# .expected pins the CORRECT (post-fix) behavior, and its sibling .engines
# file is a comma-separated subset of {run,ir,cpp,llvm} listing which
# engines the repro currently reproduces on (a bug that only hits LLVM, say,
# should not be checked against the oracle, which is already correct).
#
# Inverted exit semantics from run_corpus.sh: this script exits 0 while
# every listed engine for every file STILL disagrees with .expected (the
# normal, currently-red state) and exits 1 the moment any listed engine
# starts AGREEING with .expected — a fixed bug demands promotion to green/
# in the same commit (delete the red file + its .engines, add the green
# pair, per the fix definition-of-done in the design doc's §2).
#
# usage: run_red_corpus.sh <leviathan-binary> <dir> [runtime-c-source...]
# The optional trailing runtime C sources (runtime/lv_runtime.c and friends)
# are only compiled if some file's .engines lists 'llvm'.
bin="$1"; dir="$2"; shift 2
rtsrcs=("$@")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
shopt -s nullglob

rtobjs=()
rtobjs_built=0
build_rtobjs() {
  [ "$rtobjs_built" = 1 ] && return
  rtobjs_built=1
  for src in "${rtsrcs[@]}"; do
    obj="$work/$(basename "${src%.c}").o"
    if ! cc -O2 -c -o "$obj" "$src" 2>"$work/rt.err"; then
      echo "FAIL runtime compile: $src"; cat "$work/rt.err"; exit 1
    fi
    rtobjs+=("$obj")
  done
}

# run_engine <engine> <lev-file> <name> <stdin> <out-file>
# Writes captured stdout to <out-file>; returns 0 if the whole pipeline
# (codegen/compile/link/run) succeeded, 1 otherwise. Does NOT use command
# substitution internally so exit status survives (a `$(...)` capture runs
# in a subshell and loses ordinary variable/return-code propagation).
run_engine() {
  local eng="$1" f="$2" name="$3" stdin="$4" out="$5" rc
  : > "$out"
  case "$eng" in
    run|ir)
      if [ -n "$stdin" ] && [ -f "$stdin" ]; then
        "$bin" "--$eng" "$f" > "$out" 2>"$work/$name.$eng.err" < "$stdin"; rc=$?
      else
        "$bin" "--$eng" "$f" > "$out" 2>"$work/$name.$eng.err"; rc=$?
      fi
      return "$rc"
      ;;
    cpp)
      if "$bin" --emit-cpp "$f" > "$work/$name.cpp" 2>"$work/$name.cpp.err" \
         && g++ -O2 -o "$work/$name.cppbin" "$work/$name.cpp" 2>"$work/$name.gcc.err"; then
        if [ -n "$stdin" ] && [ -f "$stdin" ]; then
          "$work/$name.cppbin" > "$out" 2>"$work/$name.cpp.run.err" < "$stdin"; rc=$?
        else
          "$work/$name.cppbin" > "$out" 2>"$work/$name.cpp.run.err"; rc=$?
        fi
        return "$rc"
      fi
      return 1
      ;;
    llvm)
      build_rtobjs
      if "$bin" --native-obj "$work/$name.o" "$f" 2>"$work/$name.llvm.err" \
         && cc -O2 -o "$work/$name.llvmbin" "$work/$name.o" "${rtobjs[@]}" -lm -lpthread 2>"$work/$name.link.err"; then
        if [ -n "$stdin" ] && [ -f "$stdin" ]; then
          "$work/$name.llvmbin" > "$out" 2>"$work/$name.llvm.run.err" < "$stdin"; rc=$?
        else
          "$work/$name.llvmbin" > "$out" 2>"$work/$name.llvm.run.err"; rc=$?
        fi
        return "$rc"
      fi
      return 1
      ;;
    *)
      echo "unknown engine '$eng' (want run/ir/cpp/llvm)"; exit 1
      ;;
  esac
}

files=("$dir"/*.ext "$dir"/*.lev)
n=0; unexpected_pass=0
for f in "${files[@]}"; do
  n=$((n+1))
  exp="${f%.*}.expected"
  eng="${f%.*}.engines"
  stdin="${f%.*}.stdin"
  name=$(basename "$f"); name="${name%.*}"
  if [ ! -f "$eng" ]; then
    echo "FAIL $f: missing .engines file (required for every red-lane repro)"
    unexpected_pass=1
    continue
  fi
  want=$(cat "$exp")
  IFS=',' read -ra engines <<< "$(cat "$eng" | tr -d '[:space:]')"
  for e in "${engines[@]}"; do
    [ -z "$e" ] && continue
    outfile="$work/$name.$e.out"
    if run_engine "$e" "$f" "$name" "$stdin" "$outfile"; then eng_rc=0; else eng_rc=1; fi
    got=$(cat "$outfile")
    if [ "$eng_rc" = 0 ] && [ "$got" = "$want" ]; then
      echo "UNEXPECTED PASS: $f ($e) now matches .expected — promote to green/ in this commit"
      unexpected_pass=1
    else
      echo "still red: $f ($e)"
    fi
  done
done
echo "$n red repro(s) checked; $([ "$unexpected_pass" = 1 ] && echo 'one or more unexpectedly passed' || echo 'all still red as expected')"
exit $unexpected_pass
