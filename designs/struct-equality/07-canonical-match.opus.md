# Packet 07 — canonical `match` on float scalars (Model: **Opus**)

Milestone M3. Design §6: float patterns classify by the canonical relation;
`float::NaN` is an ordinary, reachable arm; duplicate/unreachable NaN-arm
diagnostics. Because canonical ≡ IEEE except NaN, **no existing float match
can change behavior** — the only new observable is the NaN arm.

## Mechanics — two sites, one relation

Match execution is split: the oracle walks arms directly; the other three
engines share ONE lowering (`Lower.cpp:2011-2052` desugars match to a
test-and-branch chain), so a lowering-level change covers IR/emit-C++/LLVM
simultaneously.

1. **Oracle** — `Evaluator::matchesValue` (`Eval.cpp:144-151`): when both
   subject and pattern value are `VKind::Float`, compare
   `lv_canon(subj.f) == lv_canon(pv.f)` instead of `combine(EqEq, ...)`.
   (Mixed int/float: match current promotion behavior — check what
   `combine`/`arithPrim` did for `1 => ` on a float subject and preserve it
   through the canon path: promote then canon.)
2. **Lowering** — the value-arm emission (`Lower.cpp:2034-2041`): when the
   scrutinee's static type is `float` and the arm is a value pattern, lower
   the test as a call to the `canonEq` native (packet 05) on the subject
   with the pattern as argument, instead of `Op::Arith EqEq`. Find how the
   lowerer emits a prelude-method call (it does it for other natives) and
   reuse that shape. Range arms are int-only today — untouched (§6: float
   ranges deferred).

   The design's lowering note (canon the scrutinee ONCE, integer compares
   per arm) is the optimized form. Implement the simple per-arm `canonEq`
   form FIRST — it is observably identical and uses the mandated integer
   canon inside the native. Only attempt the canon-once optimization if the
   simple form is green on all four lanes and the change stays small;
   otherwise leave a `// optimization deferred: canon-once (design §6)`
   comment.

`float::NaN` as a pattern needs no new code: it's a resolved-constant read
(packet 06) evaluating to NaN; `canonEq(NaN, NaN)` is true. Verify the
parser accepts `float::NaN =>` in arm position — `Identifier ::` leads a
value pattern (`Parser.cpp:458-459`), so `float :: NaN` works iff `float`
lexes as Identifier there (it's not a keyword, `docs/reference.md:61`).
If the arm parser trips on it, fix arm discrimination minimally.

## Diagnostics (Checker match case, `Checker.cpp:955-1027`)

No duplicate/unreachable arm diagnostics exist today for ANY type — do not
build a general framework. Narrow, per design §6:

- Duplicate `float::NaN` arms (two arms whose value pattern resolves to the
  NaN constant) → error `duplicate 'float::NaN' arm`.
- A `float::NaN` arm positioned after an `else` arm → error
  `unreachable 'float::NaN' arm after 'else'`. (The design says after `_`;
  this language's catch-all is `else` — `MatchArm::isElse`,
  `Ast.hpp:136`.)
- Float matches remain non-exhaustive without `else` (unchanged —
  exhaustiveness logic at `Checker.cpp:992-1025` doesn't know floats; leave
  it).

Detect "is the NaN constant" the same way packet 06 does in `typeOfBinary`
(resolved-decl pointer match) — share a small helper.

## Corpus (green, four lanes) + checker tests

- `match_float_nan_arm.lev` — match with `float::NaN => / 0.0 => / 1.5 => /
  else =>`; feed it NaN (via `fromBits`, different payloads — all hit the
  NaN arm), `0.0`, `-0.0` (hits the `0.0` arm — zeros collapse), `1.5`,
  `2.0`. Print arm markers.
- `match_float_zero_arm.lev` — `-0.0` scrutinee hits a `0.0` arm; and the
  seam pin: same value where `==` and match agree (non-NaN always agree).
- `tests/test_checker.cpp`: ERROR_HAS for the duplicate-NaN and
  NaN-after-else diagnostics; a no-error probe for a normal float match.

## Warnings

- The lowered form must go through the same canon symbol as everything else
  (hash-consistency law) — `canonEq` guarantees that; hand-rolled IR would
  not.
- Do NOT touch type-pattern arms, enum-member arms (`Checker.cpp:979-981`),
  or int/string/char value arms — float value arms only. Any diff in
  `corpus_meta_*` or enum match pins means you over-reached.
- Char scalar re-typing (`Checker.cpp:974-975`) happens before your float
  check — order your changes after it.

## Acceptance

Full `ctest` green; new pins byte-identical on four lanes; every
pre-existing match pin unmoved. Commit:
`Struct equality packet 07 (M3): canonical float match — reachable
float::NaN arm + narrow arm diagnostics`.
