#!/usr/bin/env bash
# refactor_1 session 06: enforce the physical layering of src/ (see
# docs/archectecture.md and designs/refactor_1/techdesign-06-folder-reorg-sonnet.md).
#
# Keyed on directory-qualified includes: every local project include among the
# layered files is written `#include "<layer>/Header.hpp"`, so a forbidden
# dependency edge is detectable by grep. The frozen X64 files live at src/
# root (not under any layer directory) and are therefore naturally exempt.
#
# Forbidden edges (layer -> layers it may NOT include from):
#   core     -> frontend sema meta ir backend runtime driver
#   frontend -> sema meta ir backend runtime driver
#   ir       -> sema meta backend driver
#   backend  -> sema meta frontend driver
#   runtime  -> sema frontend backend driver meta
# (sema, meta and driver are unconstrained.)
set -u

SRC="$(cd "$(dirname "$0")/.." && pwd)/src"
fail=0

check_layer() {
    layer="$1"; shift
    forbidden="$*"
    for f in "$SRC/$layer"/*.cpp "$SRC/$layer"/*.hpp "$SRC/$layer"/*.h; do
        [ -e "$f" ] || continue
        for dep in $forbidden; do
            hits=$(grep -n "#include \"$dep/" "$f" || true)
            if [ -n "$hits" ]; then
                echo "LAYERING VIOLATION: $layer -> $dep in $f"
                echo "$hits"
                fail=1
            fi
        done
    done
}

check_layer core     frontend sema meta ir backend runtime driver
check_layer frontend sema meta ir backend runtime driver
check_layer ir       sema meta backend driver
check_layer backend  sema meta frontend driver
check_layer runtime  sema frontend backend driver meta

if [ "$fail" -ne 0 ]; then
    exit 1
fi
echo "layering OK"
exit 0
