# Refactor 1 — Session 08: Clone Detection Ratchet (sonnet)

> Goal: keep the duplication from coming back. Check in the winnowing-style
> clone detector used to produce the refactor_1 evidence, generate a
> post-refactor baseline, and fail ctest when NEW cross-file clones appear.
> Date: 2026-07-26. Depends on: 06 (paths must be final before the baseline
> is captured).

## Files owned by this session

- `tools/clonedet.py` (new)
- `tools/clone_baseline.json` (new, generated)
- ctest wiring (CMake test addition only)

## The tool (fixed algorithm)

Token/line-window fingerprinting (the winnowing family, as used by CPD/MOSS):

1. Per source file: strip `//` comments, collapse whitespace; keep lines with
   ≥ 4 significant chars. Two modes: **exact** and **identifier-blind**
   (every identifier replaced by `$`).
2. Slide a window of 8 significant lines; hash each window (md5 of the
   joined normalized lines).
3. A hash appearing in ≥ 2 files (or ≥ 2 disjoint sites in one file) is a
   clone window. Aggregate per file-pair.
4. Scan set: `src/**/*.{cpp,hpp,c,h}` **excluding** any path matching `X64`
   (the frozen backend is never read by tooling) and excluding
   `build/generated/`.

Modes of operation:
- `clonedet.py scan` — print the per-pair table (human use).
- `clonedet.py baseline > tools/clone_baseline.json` — emit
  `{ "pair": count }` for identifier-blind mode.
- `clonedet.py check` — exit nonzero if any pair's identifier-blind count
  exceeds its baseline entry (missing entry = baseline 0). Decreases are
  allowed and should be re-baselined opportunistically (a decrease prints a
  reminder to regenerate the baseline; it does not fail).

## Steps

1. Add `tools/clonedet.py` implementing the above (a working reference
   implementation from the refactor_1 evidence run exists; behavior above is
   the specification).
2. Generate `tools/clone_baseline.json` from current master (post-06).
3. Register ctest test `clone-ratchet` running `clonedet.py check`.
4. Add a one-paragraph "Duplication policy" note to `docs/archectecture.md`
   (documentation edit, permitted): new shared logic goes in `RuntimeCore` /
   the owning layer, and the ratchet enforces it.

## Validation

- `ctest -j4` green including `clone-ratchet` (flake policy per overview).
- Manual check: duplicate any 10-line function into a second file locally →
  `clonedet.py check` must fail; revert → green.
- `clonedet.py scan` post-refactor shows `Eval.cpp` ↔ `IrInterp.cpp` < 8
  windows (session 02's ending state, now enforced).

## Ending state (fixed)

Detector + baseline checked in; ctest fails on new cross-file duplication;
X64 never scanned. Future duplication is a visible, failing signal instead
of an archaeology exercise.

## STOP-and-escalate

Escalate on: baseline counts wildly different from the evidence table in
techdesign-00 (suggests a scan-set mistake); any need to weaken the window
size or thresholds to get green; any temptation to add an exclusion beyond
the two listed.
