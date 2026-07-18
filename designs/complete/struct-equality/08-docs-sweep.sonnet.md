# Packet 08 — docs, trackers, and close-out (Model: **Sonnet**)

The design's acceptance bookkeeping. Text-only; no compiler changes.

## Steps

1. **`docs/reference.md`** — add the §2 reference sentence verbatim (design
   requires it at M2):
   > float scalars compare IEEE; derived equality, hashing, ordering, and
   > match — like all value contexts — compare canonically; the two differ
   > only on NaN.
   Place it in the equality/float section (grep "Equality derivation",
   info.md §566 mentions one; reference.md has the float method table at
   ~line 921). Also add to the float method table: `bits() -> int`,
   `float::fromBits(int) -> float`, `canonEq(float) -> bool` (present it as
   the canonical-relation primitive), and the `float::NaN` constant, and the
   derived-vs-handwritten note (§5.4): *derived = canonical; hand-written =
   what you wrote.*
2. **`info.md` §9** (the struct axioms, ~line 826): extend the equality
   bullet — field-wise by default **via a synthesized `(==)` method**
   (visible to `--expand`), float fields canonical, non-comparable field =
   compile error (never silent false). Keep it to 2-3 sentences matching the
   file's voice.
3. **Trackers — verify, don't re-do**: `known_bugs_2.md` #77 entry and the
   `docs/footguns.md` row were already swept in commit d68f1e8. Confirm no
   stale #77 text remains (`grep -rn "#77" known_bugs_2.md docs/footguns.md`)
   and that `harpoon/src/assert.lev`'s assertEqual doc-comment carries no
   stale struct caveat.
4. **Design doc close-out**: edit
   `designs/struct-equality/techdesign-struct-equality.md` — flip the
   header: `**Status: IMPLEMENTED (M1-M3) — M4 dormant, <date>.**`, note the
   packet commits; then `git mv` the WHOLE `designs/struct-equality/`
   directory to `designs/complete/struct-equality/` (repo convention:
   completed designs live under `designs/complete/`).
5. **Diagnostics doc**: if the repo documents compiler diagnostics anywhere
   (grep for the non-comparable message's style precedents), add the two new
   diagnostics (comparability gate, NaN compare). If no such doc exists,
   skip — do not create one.

## Warnings

- Convert no relative dates; use absolute dates (today's).
- Do not restate design content into reference.md beyond the sentences
  above — reference.md documents the LANGUAGE, the design doc documents the
  decision history.
- The tree has unrelated in-flight const-design edits under `designs/` —
  do not touch `designs/complete/const.md` or
  `designs/techdesign-const-system-extensions.md`.

## Acceptance

`ctest` full suite green (docs changes can't move it — run anyway, it's the
final gate); `git log --oneline` shows packets 01-08 as eight commits.
Commit: `Struct equality packet 08: reference/info sentences, tracker
verification, design moved to complete/`.
