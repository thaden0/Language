# Packet 06 — `float::NaN` + the always-false compare error (Model: **Opus**)

Milestone M2 tail. Design §6 (the constant), §4 loudness rule (the
diagnostic). Requires packet 05 (`fromBits`).

## 1. `float::NaN` — a language constant, one definition

Follow the enum-member-constant mechanism (the only `Name::Member` constant
precedent — `Checker.cpp:1125-1139` resolves `Enum::Member` to a const
global stamped on `e->resolved`; `Eval.cpp:1362-1364` and the backends read
that global):

- Synthesize (in the Resolver, alongside the packet 02 pass) one const
  global through the synth channel:
  `float float$NaN = float::fromBits(9221120237041090560);`
  (that decimal = 0x7FF8000000000000; use the hex literal if the lexer
  takes hex here — check `docs/reference.md` line ~77, hex ints exist).
- Checker: in `typeOfMember`'s TypeValue/static branch
  (`Checker.cpp:1152-1162`), resolve `float::NaN` → that global, type
  `float`, stamp `e->resolved` — mirroring the enum path so ALL engines
  read the same global with zero per-engine work.
- Global init ordering: the initializer calls a native at program start —
  confirm globals with call initializers already work on all four engines
  (enum globals are ctor calls, so yes). Printing it prints `nan` as today
  (design: don't change formatting).
- Decision 1 (ratified): the bit pattern is a LANGUAGE constant — no
  per-target configuration, no `#ifdef`. It's one literal in one place.

## 2. The compile error: `x == float::NaN` / `x != float::NaN`

Design §4: an OPERATOR compare against the NaN constant is statically
always-false/true under IEEE → compile error with fixit. In
`Checker::typeOfBinary`, before the primitive-comparison branch returns
`bool` (`Checker.cpp:2036-2039`):

- Detect: op is `EqEq`/`BangEq` AND either operand is an
  `ExprKind::Member` with `colon` whose resolution is the `float$NaN`
  global (match on the resolved decl pointer, not on spelling — an aliased
  read through a variable is NOT the constant and stays legal).
- Error text:
  `comparing against float::NaN with '==' is always false (IEEE) — use x.isNaN() or a float::NaN match arm`
  (mirror shape for `!=` / "always true").
- `x != x` stays legal (no pattern-match on that — it's just not this
  node shape). Orderings (`<` etc.) against NaN: design names only
  `==`/`!=`; leave orderings alone.
- Match arms do NOT pass through `typeOfBinary` (value arms are typed via
  `typeOf(arm.value)`, `Checker.cpp:976`) so `float::NaN =>` arms are
  unaffected — verify with a test once packet 07 lands.

## Tests

- `tests/test_checker.cpp` (append-only; unrelated in-flight edits in this
  file — commit only your hunks):
  `ERROR_HAS("... x == float::NaN ...", "use x.isNaN()")`, the `!=` variant,
  and a NO-error probe: `float n = float::NaN; bool b = x == n;` stays legal
  (documented escape hatch — it's honestly IEEE-false at runtime).
- Green pin `float_nan_constant.lev`: `float::NaN.isNaN()` → true;
  `float::NaN.bits() == 9221120237041090560` → true; a struct field set to
  `float::NaN` equal to another via canonical `(==)`.

## Warnings

- The constant global's mangled name (`float$NaN`) must go through
  `program.synthNames` for string_view stability.
- Don't special-case `float::NaN` in engines — if you find yourself editing
  Eval/IrInterp/CGen/LlvmGen for the CONSTANT, you've left the design's
  one-definition path; stop and re-read §6/§8.
- `float::fromBits` in a const-global initializer executes at startup on
  every engine; if the in-flight const-system work (uncommitted in this
  tree) lands rules about const initializers, coordinate — the global here
  follows whatever the enum member globals do (`isConst = true`,
  `Resolver.cpp:6028`).

## Acceptance

Full `ctest` + `checkertests` green; pin green on four lanes. Commit:
`Struct equality packet 06 (M2): float::NaN constant + always-false NaN
compare diagnostic`. M2 COMPLETE — full-suite pause before M3.
