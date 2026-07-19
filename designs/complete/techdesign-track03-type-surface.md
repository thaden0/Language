# Track 03 type-surface items

**Status:** item 3 (char literals in call-argument position) **IMPLEMENTED** —
see §9 implementation log. Items 1 and 2 stay **Deferred** roadmap notes, not
gated on any contract but not scheduled — each is a self-contained follow-up
whenever it earns priority (item 1 needs the Range machinery's char-carrying
generalization; item 2 needs a `code()`/`fromCode` signature change and a
non-int-shaped mangled-global init path — both genuinely new design work, not
line-item follow-ups). Originally promoted out of deferral 2026-07-17 (was
`deferal-track03-type-surface.md`); `char` and `enum` are landed in full on all
four active engines, so nothing below is substrate-blocked. Priority note added
2026-07-17: item 3 had recurred as a real friction cost twice — Sonar T04 filed
bug.md **#50** (char literal not retyping in call-arg position) and T06 filed
**#58** (char literals don't retype at comptime, forcing `.code()` comparisons
throughout the template engine) — so item 3 was scheduled first, with the
§5-problem-#1 overload back-compat rule (`string` wins when both `f(char)`/`f(string)`
exist) re-verified as its acceptance gate. **Date:** 2026-07-08 (design); 2026-07-17
(promoted); 2026-07-19 (item 3 implemented).
**Source of record:** `designs/complete/techdesign-03-core-types.md` (§1.1 char-range note,
§2.1 string-carrier note, §5 problem #1 call-arg note; §9 implementation log
2026-07-08).

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

**Implemented (2026-07-19 — see §9).** Char-literal target-typing (reference
§1.4) is expected-type-driven and fires at declaration/comparison/return/
match-arm sites; it now also fires at a call argument: a bare `'x'` passed to a
`char` parameter binds as `char`. A `char`-typed *value* already bound to a
`char` parameter fine (verified — `up(mc)` in `tests/corpus/chars/chars.lev`);
the bare-literal argument case (`up('m')`) is the piece this item adds. The
overload back-compat rule (§5 problem #1: `string` wins when both `f(char)`/
`f(string)` exist) holds regardless of declaration order — re-verified as the
acceptance gate (§9). Workarounds (`char` local, `std::charFromCode(...)`)
remain valid but are no longer required.

---

None of these block Track 03 completion: M1–M3 are landed and green
(oracle + IR + emit-C++; enum full-coverage), and the native-lane / M4 work has
its own gate in `designs/deferal-char-block-abi.md`.

## 9. Implementation log

- **2026-07-19 — item 3 (char literals in call-argument position) IMPLEMENTED.**
  On inspection, the call-argument target-typing mechanism itself (`isCharLiteral`/
  `markCharLiteral`/`expectsChar` applied to call-argument scoring and
  finalization, `src/Checker.cpp` around the overload-resolution scoring loop
  and the winning-candidate argument finalization) was already present in the
  tree — `up('m')` (bare literal, no intermediate `char` local) already bound
  correctly. What was **not** yet correct was the §5-problem-#1 acceptance gate:
  a bare char-literal argument scored an exact tie (2) against both an `f(char)`
  and an `f(string)` overload, so the winner fell back to declaration order —
  `f(char)` would win if declared first, silently reversing the intended
  back-compat rule. Fix: the char-literal-to-`char`-parameter special case now
  scores 1 (below the exact-type-match tier's 2), so an `f(string)` candidate's
  ordinary exact match always outscores it, independent of declaration order.
  A real `char`-typed value (not a bare literal) is unaffected — it still hits
  the exact-match tier against an `f(char)` parameter.
  Verified across all three landed char lanes (oracle/IR/emit-C++, byte-identical):
  `up('m')` binds and evaluates; `f('m')` picks the `string` overload whether
  `f(string)`/`f(char)` or `f(char)`/`f(string)` is declared first. Added to
  `tests/corpus/chars/chars.lev` (+ `.expected`): bare-literal call-arg line,
  and a same-argument two-overload-order pair (`g1`/`g2`). Added checker-level
  coverage in `tests/test_checker.cpp` pinning the overload choice by return
  type in both declaration orders. Full `ctest` (excluding the frozen ELF lanes)
  green; `checkertests` 382/382.
