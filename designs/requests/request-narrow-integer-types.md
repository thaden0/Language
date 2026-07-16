# Summary: Sized/narrow integer types (`byte`, `int8`/`int16`/`int32`, `uint`)

The scalar type system today has exactly one integer width (`int`, 64-bit). There is no
`byte` primitive, no `int8`/`int16`/`int32`, and no unsigned variant. This request tracks
whether that stays a deliberate, permanent decision or gets a narrow exception — the
recommendation that first raised it (`designs/suggested-features.md` §2.4, now retired)
already leaned toward "defer, with widths living at the `Block`/struct-layout boundary,"
and that is in fact what got built: `Block.int32At`/`int64At` exist, but no standalone
narrow scalar type does. This ticket exists so that outcome is a **recorded decision**,
not an absence nobody chose.

## Request Details

Two related asks, both narrow-integer-shaped, are worth deciding together since a `byte`
is, in every other language that has one, just `uint8`:

1. **`byte` as a primitive** — a single unsigned 8-bit scalar following the object-mask
   rule (like `char`, info.md §9): unboxed, methods dispatch through the normal shape
   machinery, `this` is the raw value. Today the closest equivalent is `Block.byteAt(i)`
   / `string.byteAt(i)`, both of which return a plain `int` clamped to `0..255` at the
   API boundary rather than carrying a distinct type — so a byte value has no type-level
   guarantee it's in range once it's sitting in a variable, and `Array<int>` is the only
   way to hold a buffer of them outside a `Block`.
2. **`int8`/`int16`/`int32`/`uint`** — narrower/unsigned siblings of `int`, for binary
   protocol work, columnar layouts, and FFI/ABI boundaries where a specific width is a
   correctness requirement, not a style choice.

Whether either should exist depends on the same question the language already answered
once for `int` itself (info.md §9): **"either all primitives are full words or all are
abbreviated... one honest `int` is a feature, not a gap."** Adding `byte`/`int8`/`int16`
reopens that stance specifically for the low end of the width range. The case *for* is
narrow but real: protocol/columnar/FFI code currently has no way to say "this is exactly
one unsigned byte" or "this is exactly a 16-bit signed field" except by convention and a
comment, and `Block`'s typed accessors already imply the runtime has no objection to
sub-64-bit reads — they just don't surface as first-class types a variable can hold.

## Requested Specific Feature

Not prescribing a shape — this is the open question, not a design. Two shapes to choose
between if the answer is "yes, add something":

- **(a) `Block`-boundary only (status quo, ratified explicitly):** no new scalar types;
  `Block.byteAt`/`int32At`/`int64At` (and their `set*` counterparts) remain the one place
  widths are expressed, and this ticket closes by recording that as the deliberate
  answer rather than an oversight.
- **(b) Add `byte` only:** the one width that keeps showing up at API boundaries
  (`byteAt`, buffer contents) gets promoted to a real object-masked primitive, the same
  way `char` was; `int16`/`int32`/`uint` stay deferred (rarer asks, easily expressed via
  masking/`Block` today).
- **(c) Add the full narrow set:** `byte`/`int8`/`int16`/`int32`/`uint`, each object-masked
  like `int`/`char`, with defined promotion/conversion rules to and from `int` (implicit
  widening, explicit narrowing — the same shape as most C-family languages, adapted to
  this language's "no truthiness, no silent conversions" stance).

## Known Warnings

- Whatever is chosen must not reopen integer-promotion special-casing — the exact trap
  info.md §9 cites as the reason `int` stayed singular. A narrow type that participates
  in arithmetic needs explicit promotion rules stated up front, not discovered per bug.
- `char` (Track 03) deliberately carries **no arithmetic** to sidestep this same class of
  problem (info.md §9: "it never drags in C's integer-promotion special cases"). If
  `byte` is added, decide up front whether it follows `char`'s no-arithmetic stance or
  `int`'s full-arithmetic one — they are different precedents already in the language.
- Touches the same unions/value-size story as every primitive (info.md §9's union table):
  a new primitive changes nothing about union mechanics, but should be cross-checked.

## Acceptance Criteria

1. An explicit ruling recorded in `info.md` (§9, primitives table) stating which of (a),
   (b), or (c) above is the answer — even if the answer is "no new types, `Block` is the
   permanent boundary."
2. If (b) or (c): the chosen type(s) follow the object-mask rule, are documented in
   `docs/reference.md` alongside `char`, and specify their arithmetic/promotion/conversion
   rules explicitly.
3. If (b) or (c): corpus coverage on every active engine (oracle/IR/emit-C++/LLVM — no
   ELF lane, X64Gen frozen).

## Interim Fallback

Byte-shaped data continues through `Block`/`Array<int>` with values manually kept in
`0..255` (or the relevant range) by convention; no code is blocked waiting on this.
