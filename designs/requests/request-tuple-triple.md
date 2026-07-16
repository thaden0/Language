# Summary: `Triple<A, B, C>` (and the anonymous-tuple question it stands in for)

`Pair<A, B>` exists and is used throughout the stdlib (`Array.zip`, `Array.withIndex`,
`Array.join`, `Map.entries`, all landed per `designs/complete/techdesign-05-stdlib-collections.md`).
There is no three-element counterpart. First raised in `designs/suggested-features.md` §2.5
("`Triple<A,B,C>` is cheap and occasionally wanted, but anonymous tuple *syntax* is a bigger
decision; defer syntax, add `Triple`") — that source document is being retired, so this
ticket carries the recommendation forward as a tracked, standalone ask.

## Request Details

`Triple` is a small, mechanical addition on top of an already-landed pattern — it is not a
new design, it's `Pair` with one more type parameter and one more field. The value is
narrow but real: any three-way pairing (a coordinate + a label, a key + two related values,
a `groupJoin`-adjacent three-way zip) currently has to fall back to a purpose-built `struct`
or nested `Pair<A, Pair<B, C>>`, both worse than a dedicated type for something this common
in data-shaped code (the "bulk data" thrust info.md and `suggested-features.md` §13 both
lean on).

The bigger question this ticket deliberately does **not** resolve — flagged for the owner,
not decided here — is **anonymous tuple syntax** (`(int, string, bool)` as a type, or
literal tuple construction without naming `Pair`/`Triple`/etc.). `suggested-features.md`
explicitly separated these: add the named type now, decide tuple syntax later as its own
question. If `Triple` is approved, whoever implements it should confirm this separation
still holds rather than accidentally deciding the syntax question by default.

## Requested Specific Feature

```
struct Triple<A, B, C> {
    A first;
    B second;
    C third;
}
```

Mirroring `Pair`'s existing shape (`struct`, value type, per info.md §9's "value types:
`struct`" rule) — copied not aliased, field-wise equality, no identity. Candidate stdlib
uses once it exists: a `zip3`-shaped `Array` combinator (not requested here, but the
natural next ask), or ad hoc construction (`Triple(a, b, c)`) wherever code currently
builds a purpose-built 3-field struct just to move three values together.

## Known Warnings

- Keep this scoped to the named type. Anonymous tuple syntax is a separate, larger design
  question (per `suggested-features.md`'s own note) and should not ride along by default.
- If a 4-element case turns out to be wanted later, decide then whether the pattern
  continues (`Quad`) or whether that's the trigger to finally design anonymous tuples —
  don't pre-decide that here.

## Acceptance Criteria

1. `Triple<A, B, C>` exists in the prelude with the same construction/equality/printing
   conventions as `Pair<A, B>`.
2. Corpus coverage on every active engine (oracle/IR/emit-C++/LLVM).
3. `docs/reference.md` documents it alongside `Pair`.

## Interim Fallback

Purpose-built small `struct`s, or nested `Pair<A, Pair<B, C>>`, wherever three values need
to travel together today. No code is blocked waiting on this.
