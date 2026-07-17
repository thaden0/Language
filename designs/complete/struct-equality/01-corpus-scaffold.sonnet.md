# Packet 01 — corpus scaffold (Model: **Sonnet**)

Milestone M1 groundwork. Mechanical; no compiler code changes.

## Goal

Create the `equality` corpus cluster with all four engine lanes wired, and
land the M1 green pins that already pass today (they pin current
`keyEquals`-fallback behavior; later packets must keep them green while
swapping the mechanism underneath).

## Steps

1. **Wire the cluster.** In `CMakeLists.txt:664`:
   `set(LANG_COMPOSITION_CLUSTERS fnvalues aggregates names generics)` →
   append `equality`. This automatically adds the cluster to the four lanes
   (`composition_treewalk`/`_ir`/`_cpp` at lines 666-671, `composition_llvm`
   at line 973) and the red lane (line 975).
2. **Create dirs**: `tests/corpus/composition/equality/green/` and
   `.../equality/red/` (red stays empty for now; the red runner tolerates an
   empty dir via nullglob — verify by running the lane).
3. **Green pins** (each `.lev` + exact `.expected`, program ends with a
   top-level `main();` call — copy the shape of
   `tests/corpus/composition/aggregates/green/struct_default_eq.lev`):
   - `struct_eq_basic.lev` — int/bool/string/char fields; equal, unequal,
     `!=` derivation.
   - `struct_eq_nested.lev` — struct-in-struct recursion, mismatch in the
     inner struct.
   - `struct_eq_class_ref.lev` — a struct holding a class-reference field:
     same ref → true, field-identical distinct refs → false (reference
     identity per §5.2).
   - `struct_eq_optional_field.lev` — an `int?` field: `None == None` true,
     `None == 5` false, `5 == 5` true.
   - `struct_eq_explicit_override.lev` — a struct declaring its own `(==)`
     (e.g. compares only one of two fields); prove the author's relation is
     used and `!=` derives from it.
   - `struct_eq_enum_field.lev` — an enum field compares by carrier.
4. **Run all four lanes** and confirm every pin is green *before* commit:
   ```sh
   cmake --build build -j && ctest --test-dir build -R composition
   ```
   Generate each `.expected` from the ORACLE (`--run`), then verify `--ir`,
   `run_native.sh`, `run_native_llvm.sh` agree byte-for-byte.

## Warnings

- Do NOT put float fields in any of these pins — floats are packet 05's
  corpus. Do not print raw floats anywhere in this cluster.
- Do NOT move or edit the existing
  `composition/aggregates/green/struct_default_eq.*` pin — it stays green
  through the whole effort (packet 04 only refreshes its stale comment).
- If any pin is NOT already green on all four lanes today, that is a real
  pre-existing divergence: STOP and escalate with the diff — do not adjust
  the pin to match a divergent engine.
- `.expected` matches stdout+stderr combined (`run_corpus.sh` uses `2>&1`).

## Acceptance

`ctest --test-dir build -R composition` fully green; full `ctest` unchanged
otherwise. Commit: `Struct equality packet 01: equality corpus cluster (M1 pins)`.
