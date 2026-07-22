#!/usr/bin/env bash
# Conformance runner (refactor_1 session 07 — techdesign-07).
#
# usage: run.sh <leg> <path/to/prog.lev>
#   legs:
#     interp  leviathan --run prog                (IrInterp-family reference)
#     cgen    leviathan --build  <tmp> prog && <tmp>   (emit-C++, g++)
#     llvm    leviathan --build-native <tmp> prog && <tmp>
#   There is NO x64/ELF leg — ever (X64Gen is frozen).
#
# Contract per program (see techdesign-07 Part B):
#   prog.expected  exact stdout
#   prog.exit      expected exit code (optional; default 0)
#   skip.<leg>.txt (next to this script) lines: "<area>/<prog>.lev  <reason>"
#                  a listed (leg, program) exits 77 (ctest SKIP_RETURN_CODE),
#                  printing the reason — skips are visible, never silent. A
#                  skip line with an EMPTY reason is itself an error.
#
# Every program is executed from a fresh private tmpdir (natives-fs programs
# use relative paths, so their writes are tmpdir-confined and parallel-safe).
set -u

here="$(cd "$(dirname "$0")" && pwd)"
leg="${1:?usage: run.sh <interp|cgen|llvm> <prog.lev>}"
prog="${2:?usage: run.sh <interp|cgen|llvm> <prog.lev>}"
prog="$(cd "$(dirname "$prog")" && pwd)/$(basename "$prog")"
bin="${LEVIATHAN_BIN:-$here/../../build/leviathan}"

case "$leg" in interp|cgen|llvm) ;; *) echo "unknown leg '$leg' (interp|cgen|llvm)"; exit 2;; esac
[ -x "$bin" ] || { echo "leviathan binary not found: $bin (set LEVIATHAN_BIN)"; exit 2; }

# --- skip handling: visible, reason REQUIRED --------------------------------
rel="${prog#"$here"/}"
skipfile="$here/skip.$leg.txt"
if [ -f "$skipfile" ]; then
    while IFS= read -r line; do
        case "$line" in ''|'#'*) continue;; esac
        entry="${line%%[[:space:]]*}"
        reason="${line#"$entry"}"
        reason="$(echo "$reason" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')"
        if [ "$entry" = "$rel" ]; then
            if [ -z "$reason" ]; then
                echo "skip.$leg.txt lists $rel with an EMPTY reason — a skip must be justified"
                exit 1
            fi
            echo "SKIP ($leg) $rel: $reason"
            exit 77
        fi
    done < "$skipfile"
fi

expected="${prog%.lev}.expected"
exitfile="${prog%.lev}.exit"
[ -f "$expected" ] || { echo "missing $expected"; exit 1; }
wantrc=0
[ -f "$exitfile" ] && wantrc="$(tr -d '[:space:]' < "$exitfile")"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# --- build (compiled legs) / choose the run command -------------------------
case "$leg" in
    interp)
        runcmd=("$bin" --run "$prog")
        ;;
    cgen)
        if ! "$bin" --build "$work/prog.bin" "$prog" >"$work/build.log" 2>&1; then
            echo "FAIL ($leg) $rel: build failed"; cat "$work/build.log"; exit 1
        fi
        runcmd=("$work/prog.bin")
        ;;
    llvm)
        if ! "$bin" --build-native "$work/prog.bin" "$prog" >"$work/build.log" 2>&1; then
            echo "FAIL ($leg) $rel: build failed"; cat "$work/build.log"; exit 1
        fi
        runcmd=("$work/prog.bin")
        ;;
esac

# --- run from the private tmpdir, compare stdout + exit code ----------------
mkdir "$work/cwd"
(cd "$work/cwd" && "${runcmd[@]}" >"$work/out" 2>"$work/err")
rc=$?
ok=1
if ! diff -u "$expected" "$work/out" >"$work/diff"; then ok=0; fi
if [ "$rc" != "$wantrc" ]; then ok=0; fi
if [ "$ok" = 1 ]; then
    exit 0
fi
echo "FAIL ($leg) $rel"
[ "$rc" != "$wantrc" ] && echo "  exit code: got $rc, want $wantrc"
if [ -s "$work/diff" ]; then
    echo "  stdout diff (expected vs got):"
    head -40 "$work/diff" | sed 's/^/  /'
fi
if [ -s "$work/err" ]; then
    echo "  stderr:"
    head -10 "$work/err" | sed 's/^/  /'
fi
exit 1
