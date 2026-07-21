#!/usr/bin/env bash
# ir_spec_complete (refactor_1 session 07): every enumerator of `enum class Op`
# in src/ir/Ir.hpp must appear as a heading (its EXACT name) in docs/ir-spec.md.
set -u
here="$(cd "$(dirname "$0")" && pwd)"
root="$here/../.."
hpp="$root/src/ir/Ir.hpp"
spec="$root/docs/ir-spec.md"

[ -f "$hpp" ]  || { echo "missing $hpp"; exit 1; }
[ -f "$spec" ] || { echo "missing $spec (docs/ir-spec.md not written)"; exit 1; }

# Extract enumerator names: lines between "enum class Op {" and the closing "};",
# taking the leading identifier of each "Name," / "Name" entry.
ops=$(awk '/enum class Op \{/{f=1;next} f&&/^\};/{exit} f' "$hpp" \
      | sed 's,//.*,,' \
      | grep -oE '^[[:space:]]*[A-Za-z_][A-Za-z0-9_]*[[:space:]]*,?' \
      | tr -d ' ,\t')

[ -n "$ops" ] || { echo "extracted no enumerators from $hpp — check the parser above"; exit 1; }

missing=0
total=0
for op in $ops; do
    total=$((total+1))
    # A heading line whose text is exactly the enumerator name.
    if ! grep -qE "^#+[[:space:]]+\`?$op\`?[[:space:]]*$" "$spec"; then
        echo "MISSING: no heading for opcode '$op' in docs/ir-spec.md"
        missing=$((missing+1))
    fi
done

if [ "$missing" != 0 ]; then
    echo "ir-spec incomplete: $missing of $total opcodes undocumented"
    exit 1
fi
echo "ir-spec complete: all $total opcodes have headings"
exit 0
