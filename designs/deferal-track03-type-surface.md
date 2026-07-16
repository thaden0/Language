# Deferral — Track 03 type-surface items

**Status:** deferred (design-noted, not blockers). **Date:** 2026-07-08.
**Source of record:** `designs/complete/techdesign-03-core-types.md` (§1.1 char-range note,
§2.1 string-carrier note, §5 problem #1 call-arg note; §9 implementation log
2026-07-08). These are **surface refinements**, not gated on any contract — each
is a self-contained follow-up whenever it earns priority.

---

## 1. `char` ranges (`'a'..'z'`)

**Deferred.** `Range` is int-backed, so a char range would need either a
char-carrying `Range` or a parallel range type. v1 keeps ranges int: write
`'a'.code()..'z'.code()` and compare/convert via `code()`/`std::charFromCode`.
Comparisons on `char` (`< <= > >=`, landed) already make ordered-scalar logic
work without the literal range sugar. Lands if/when char ranges earn the range
machinery's generalization.

## 2. String-carrier enums (`enum E : string { ... }`)

**Deferred.** v1 enums carry `int` only (`enum E { ... }` auto 0..n-1, or
`enum E : int { ... }` with explicit carriers). A `string` carrier would change
`code()`'s return type and the `fromCode` signature per enum, and the desugar's
mangled-global initialization path is int-shaped today. `toString()` already
gives the member **name** as a string, which covers most of what a string carrier
is reached for; a distinct string *payload* is the deferred piece. Roadmap note
only — no blocker.

## 3. Char literals in call-argument position

**Deferred** (§5 problem #1). Char-literal target-typing (reference §1.4) is
expected-type-driven and fires at declaration/comparison/return/match-arm sites,
but **not** at a call argument: a bare `'x'` passed to a `char` parameter stays
`string` and fails to bind. A `char`-typed *value* binds to a `char` parameter
fine (verified — `up(mc)` in `tests/corpus/chars/chars.lev`); only the bare-literal
argument is unconverted. Deferred deliberately: extending target-typing into the
call path is exactly the "a second literal-typing mechanism starts growing" STOP
(§8 condition 2) risk, so it wants its own focused pass with the overload
back-compat rule (§5 problem #1: `string` wins when both `f(char)`/`f(string)`
exist) re-verified. Workaround today: bind through a `char` local, or call
`std::charFromCode(...)`.

---

None of these block Track 03 completion: M1–M3 are landed and green
(oracle + IR + emit-C++; enum full-coverage), and the native-lane / M4 work has
its own gate in `designs/deferal-char-block-abi.md`.
