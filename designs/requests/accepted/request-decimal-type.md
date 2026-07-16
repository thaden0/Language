# Summary: `decimal` — a precise, non-binary-float numeric type

There is no fixed/arbitrary-precision decimal type. Money, invoicing, and any calculation
where `float`'s binary rounding is unacceptable have no honest home in the type system
today; the only options are `float` (wrong for money) or `int`-as-cents (a convention, not
a type). First raised in `designs/suggested-features.md` §2.5, deliberately deferred there
("genuinely useful for money, genuinely large to do right — defer past the framework
milestone"); this ticket exists so the deferral is a tracked decision, not a silent gap,
now that the source document is being retired.

## Request Details

A `decimal` matters specifically because `float` (IEEE double, info.md §2.4) cannot
represent most base-10 fractions exactly — `0.1 + 0.2 != 0.3` in binary floating point is
the canonical footgun, and it lands directly on money, billing, and any Atlantis-style web
framework consumer that touches prices or ledgers. The language's own "honesty over hidden
magic" stance (info.md §1) argues against pretending `float` is fine for that domain.

This is a genuinely large feature, not a small primitive addition: unlike `char`/`byte`, a
useful `decimal` needs a defined precision/scale model (fixed-point vs. arbitrary-precision
vs. IEEE 754-2008 decimal), arithmetic semantics (rounding modes, overflow behavior), and a
storage representation that plays well with the value-size-for-unions story (info.md §9 —
"union member size is value size, not content size"). None of that is designed yet.

## Requested Specific Feature

Not prescribing an implementation — this is the open question. Considerations for whoever
designs it:

- **Representation:** a fixed-point integer-plus-scale pair (simple, bounded size, fits the
  union/value-size story cleanly) versus arbitrary-precision (correct for any input, but
  variable-size storage breaks the "union member size is value size" invariant every other
  value type relies on).
- **Object mask:** should follow the same primitive-as-value-type-class rule as `int`/
  `float`/`char` (info.md §9) if it is added as a true primitive; a `struct`-based library
  type (info.md §9, "value types: `struct`") is the lighter-weight alternative if the
  language would rather not grow its primitive set.
- **Arithmetic and literals:** whether decimal literals get their own syntax (`1.50d`?
  target-typed from context, like `char`'s single-quote literal, info.md §9?) and how mixed
  `decimal`/`int`/`float` arithmetic is disallowed or explicitly converted.

## Known Warnings

- This is explicitly the kind of feature that should not be rushed to fit a deadline —
  `suggested-features.md` §2.5 already flagged it as "genuinely large to do right."
- Interacts with the union value-size invariant (info.md §9) if a variable-size
  representation is chosen — needs explicit resolution, not a silent exception carved out.

## Acceptance Criteria

1. A design decision on representation (fixed-point vs. arbitrary-precision) recorded
   before any implementation starts.
2. If implemented: basic arithmetic (`+ - * /`), comparison, and `toString()`/`parse()`
   round-tripping, correct to the chosen precision, on every active engine.
3. `info.md` §2 gains a `decimal` entry (or an explicit note that it remains deferred, with
   the reason restated) so the decision doesn't quietly disappear along with this ticket's
   source document.

## Interim Fallback

Money/precise-decimal values are represented as `int`-scaled-by-a-fixed-factor (e.g. cents)
by convention, or `float` where the imprecision is acceptable. No code is currently blocked
waiting on this.
