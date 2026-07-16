# Track 06 — Stdlib: Math & Numeric Masks

**Status:** ready. **Date:** 2026-07-05. **Depends on:** Track 01 (shifts/hex —
only for `toHex`; start anytime, land `toHex` after 01).
**Source:** suggested-features.md §8.
**Owns (regions):** kPrelude `class int` (Resolver.cpp:15–20) + `class float`
(21–23) + new `namespace std` math block; `RuntimeNatives.cpp` new `float`/math
branches; CGen mirrors; `X64Gen.cpp` SSE math emission; `LlvmGen.cpp` intrinsics
(best-effort).

This is the smallest track — a good first track for a new implementer, and
independent of everything except the `toHex` tail.

---

## 1. Surface

### 1.1 `int` mask additions

| method | impl | notes |
|---|---|---|
| `int pow(int e)` | in-language | square-and-multiply loop; `e < 0` → 0 (integer semantics, documented); overflow wraps (int64 two's complement — documented, no trap v1) |
| `int clamp(int lo, int hi)` | in-language | `lo > hi` → RuntimeException (loud) |
| `int sign()` | in-language | -1/0/1 |
| `float toFloat()` | **native** | exact for \|x\| < 2^53; documented |
| `string toHex()` | in-language | div/mod-16 over a hex-digit table string, or shifts post-Track-01; no `0x` prefix, lowercase, `-` sign for negatives (defined here) |
| `string toString(int radix)` | in-language | radix 2..36 else throw |

### 1.2 `float` mask additions

| method | impl | notes |
|---|---|---|
| `float abs()` | in-language | `this < 0.0 ? 0.0 - this : this` (NaN passes through — fine) |
| `float floor()/ceil()/round()/trunc()` | **native** | round = half-away-from-zero (C `round`); documented |
| `int toInt()` | **native** | truncation; NaN/±inf/out-of-int64-range → RuntimeException (loud, not UB) |
| `float sqrt()` | **native** | negative → NaN (IEEE; not a throw — math convention, documented) |
| `float pow(float e)` | **native** | |
| `bool isNaN()` | in-language | `this != this` (IEEE — free and cute; verify float `!=` is IEEE-honest on all engines: P2) |
| `bool isInfinite()` | in-language | `this == 1.0/0.0 \|\| this == (0.0-1.0)/0.0` — **P2 must confirm float division by zero yields inf (not a throw)** on all engines; if it throws anywhere, make isInfinite native and skip the cuteness |

### 1.3 `std::math` namespace (free functions + consts)

```
namespace std {
    namespace math {
        const float pi = 3.141592653589793;
        const float e  = 2.718281828459045;
        float log(float x);   float log2(float x);   float exp(float x);
        float sin(float x);   float cos(float x);    float tan(float x);
        float atan2(float y, float x);
        int   min(int a, int b) => a < b ? a : b;     // in-language
        int   max(int a, int b) => a > b ? a : b;
        float min(float a, float b) => a < b ? a : b;
        float max(float a, float b) => a > b ? a : b;
    }
}
```
Nested namespace + `uses std::math;` / `math::pi` access (nested-namespace
imports are the bug.md #1 zone — see problem #3). Consts use the just-landed
const machinery (prelude-global precedent: `std::read`, Resolver const marking).

## 2. Engine coverage plan (the ELF split)

| group | oracle/IR (RuntimeNatives) | CGen | ELF (X64Gen) | LLVM |
|---|---|---|---|---|
| floor/ceil/round/trunc/sqrt | libm calls | libm in emitted C++ | **emit SSE**: `roundsd` imm 0b01/0b10/—/0b11 + `sqrtsd` (round: no direct half-away mode — see problem #1) | intrinsics if trivial else uncovered-report |
| pow/log/log2/exp/trig/atan2 | libm | libm | **deferred with diagnostic** (`ELF: math native 'sin' not yet emitted`) — the documented zero-dep boundary; polynomial emission is a later project | uncovered-report |
| toFloat/toInt | casts | casts | `cvtsi2sd` / `cvttsd2si` + range/NaN check branch to throw helper | trivial |

The ELF diagnostic follows the existing coverage-error pattern (reference §7.3:
"out-of-coverage constructs fail with a diagnostic"). This split is **the design**
— an implementer must not silently link libm into the pure backend (zero-dep is
the whole point of that engine) nor silently skip the diagnostic.

## 3. P-probes

- **P1:** float arithmetic + comparison baseline on all five engines (`0.1+0.2`,
  prints, `==` behavior) — establishes float is actually IEEE double end-to-end
  (X64 stores 16-byte tagged values; confirm the payload is a real double on
  ELF: grep float handling in X64Gen — if float math is int-faked anywhere, STOP
  immediately; the whole track assumes IEEE doubles).
- **P2:** `1.0/0.0` and `0.0/0.0` per engine (inf/NaN vs throw) → decides §1.2's
  in-language forms.
- **P3:** nested-namespace access `std::math::pi` + `uses std::math` on `--ir`
  (bug.md #1 adjacency — file-level is fixed; confirm the *qualified* path).
- **P4:** float formatting today (`(3.14).toString()` output shape per engine) —
  toString consistency matters for corpus expected-files; pin the current shape,
  do not change it in this track.

## 4. Foreseeable problems & solution strategies

| # | problem | strategy |
|---|---|---|
| 1 | **`roundsd` has no half-away-from-zero mode** (SSE4.1 rounds half-to-even under those immediates). | Emit round as `trunc(x + copysign(0.5, x))` composed from `roundsd`(trunc) + sign fiddling via `andpd/orpd` with a sign-mask constant — 4 instructions, matches C `round`. Pin with corpus values (2.5, -2.5, 0.5). |
| 2 | **libm availability in RuntimeNatives** — trivial (`<cmath>`), but CGen's emitted translation unit must add `#include <cmath>` and possibly `-lm`; check CGen's compile driver flags. | One-line preamble edit; run_native.sh is the check. |
| 3 | **`std::math` nested-namespace on IR** hits bug.md #1's *block-level* cousin or a new wrinkle. | P3 first. If qualified access breaks on --ir: fall back to flat names in `std` (`std::mathPi`? NO — ugly) — correct fallback is `namespace math` at TOP level (not nested in std), which the resolver handles like any namespace; implicit `uses std` doesn't cover it but `uses math;`/`math::pi` works. Log + reference-doc whichever ships; revisit when bug.md #1 fixed. |
| 4 | **Const float globals in the prelude** — const machinery landed for the OpenMode ints; float consts unproven. | Probe in M1's first commit; if float const init misbehaves, plain `var` globals + doc note (consts are the intent; bug-file the gap). |
| 5 | **`min`/`max` overload set in one namespace** (int + float pairs) — overload-by-arg-type across a namespace is core machinery, but two same-name functions in one namespace must actually register as an overload set. | The resolver supports overloads for functions (§9); quick probe. If namespace-function overloads collide, split names (`fmin`/`fmax`) with a log note — small wart, revisit. |
| 6 | **NaN in corpus expected-files** (text comparison of `nan` spellings differs per engine's formatter). | Corpus programs avoid printing raw NaN/inf; they print `isNaN()` booleans instead. Formatting consistency is P4-pinned, not expanded. |

## 5. Milestones & acceptance

| M | deliverable | accept |
|---|---|---|
| M1 | P1–P4 logged; int mask (§1.1 minus toHex); float in-language items | math.ext corpus part 1, all engines |
| M2 | float natives on oracle/IR/CGen; ELF SSE group + deferral diagnostics for the transcendental group | math.ext part 2; run_elf shows sqrt/floor/ceil/round/trunc/toInt live + clean diagnostics for sin-class |
| M3 | `std::math` namespace; `toHex`/`toString(radix)` (post-01) | math.ext part 3; reference §6.1 + new §6.1b (math) |

Target: **Jul 8 – Jul 14** (M1 2d, M2 3d, M3 1d).

## 6. Reference-doc duty

reference §6.1 (int/float method tables), new math-namespace section, ELF
coverage note in §7.3. info.md: none (no design change — pure surface).

## 7. STOP conditions

- P1 reveals non-IEEE float anywhere (design premise void).
- ELF SSE emission requires touching the value-tag layout.
- Any pressure to link libm into the ELF backend.

## 8. Implementation log

**2026-07-06 — M1/M2/M3 all landed in one pass (int mask, float mask, `math`
namespace). ctest 47/47 green (44 pre-existing + 3 new math_transcendental
lanes); `tests/corpus/math.ext` byte-identical across all five engines.**

**Probes (against `build/leviathan` of this tree, 2026-07-06):**
- **P1 (float IEEE baseline):** confirmed on all five engines —
  `0.1+0.2 == 0.3` is `false`, `> 0.29` is `true`, identical output
  everywhere including ELF (real SSE2 doubles, not int-faked; no STOP).
- **P2 (`1.0/0.0`, `0.0/0.0`):** oracle/IR/emit-C++/ELF give real IEEE
  `inf`/`nan` (ELF renders the NaN sign differently, `nan` vs `-nan` —
  pre-existing, unrelated bug.md #12 cosmetic gap, not touched). **LLVM
  diverges**: bug.md #12 (already filed, awaiting a cross-engine ruling,
  explicitly not to be fixed unilaterally by a portable-backend agent) means
  `0.0/0.0` silently yields `0.0` on LLVM, not NaN. Consequence for this
  design: never confirmed to throw anywhere, so per §1.2's own contingency
  the in-language forms stay — but the literal `this == 1.0/0.0` formula for
  `isInfinite()` would be silently wrong (always false) on LLVM specifically,
  since it depends on that exact division producing a real infinity. Fixed
  by reformulating (see below), not by touching bug.md #12.
- **P3 (`std::math::pi` qualified nested-namespace access):** confirmed
  broken — silently resolves wrong on `--run`, hard-errors ("IR: not yet
  lowerable: name 'std'") on `--ir`/`--emit-cpp`. A **new** wrinkle, not
  covered by the already-fixed bug.md #1/#2 (those were block-scoped `uses`
  imports, not qualified static paths into a nested namespace). Filed
  nowhere separately since the design's own problem #3 pre-authorizes the
  fix: a **top-level** `namespace math` (not nested in `std`), verified
  working on all five engines including the int/float `min`/`max` overload
  set (problem #5, confirmed non-issue) and `uses math;` unqualified access.
  Landed that way; reference.md §6.1b documents the `std::math` → `math`
  substitution and why.
- **P4 (float `toString` shape):** unchanged fixed 6-decimal format
  (`std::to_string`/`%f`-equivalent) on all five engines for ordinary
  magnitudes — pinned, not touched, per the design's own instruction.

**Deviations from the literal design text (all self-contained, none needed
escalation):**
- **`isInfinite()` reformulated**, not `this == 1.0/0.0 || this ==
  (0.0-1.0)/0.0` as literally written. Uses `this.abs() > <DBL_MAX exact
  literal>` instead — sidesteps bug.md #12's LLVM division divergence
  entirely (verified honest for +inf/-inf/finite/NaN on all five engines via
  a division-independent NaN/inf source: `big*big` overflow and `inf-inf`).
  Still in-language, still "free and cute" per the design's own goal for
  this method — just a different bug-independent formula. The design's own
  text pre-authorizes exactly this kind of swap ("if it throws anywhere,
  make isInfinite native and skip the cuteness"); this applies the same
  reasoning to "silently wrong on one engine" instead of "throws," and finds
  a fix that avoids going native at all.
- **`toHex()`/`toString(int radix)` INT64_MIN handling**, not spelled out in
  the design. Negating `this` to get a magnitude overflows for INT64_MIN (no
  positive counterpart in two's complement) — `toHex()` special-cases it
  (`this == 1 << 63 → "-8000000000000000"`, since shift-based digit
  extraction never terminates on a negative number under arithmetic
  right-shift); `toString(radix)` avoids the problem structurally instead,
  never negating `this` and extracting each digit's sign from the (possibly
  negative) remainder — division-based digit extraction terminates for
  every int64 value including INT64_MIN, so no special case is needed there.
- **Nested `std::math` → top-level `math`** — see P3 above.

**New bugs found and filed (not fixed, per this track's owned region):**
- **bug.md #18**: `X64Gen.cpp`'s `genCallNative`/`LlvmGen.cpp`'s
  `emitNativeRows` (the shared native-METHOD dynamic-dispatch tables) fall
  through to a silent void result for an unrecognized (tag, name) pair,
  unlike the free-function dispatch (`CallNativeFn`), which already fails
  loud per-callsite. Pre-existing and general (`string.byteAt`/`toFloat`/
  `Array.concatAll` have the same hole on ELF/LLVM today) — found because
  `float.pow` needed a clean diagnostic and the obvious place to add it
  (inside the shared dispatch tables) turned out to be wrong (they're
  generated once, unconditionally, per program — a `fail()` there would
  break every program, not just ones calling `pow()`). Worked around with a
  **per-callsite** compile-time check at each backend's `Op::CallDyn`
  handling, scoped to exactly (and only) `pow`; the pre-existing holes for
  the other three natives are untouched, left for an owner ruling per the
  bug.md entry.

**X64Gen/portable-backend-pivot conflict (found mid-implementation, escalated
and resolved by the owner):** this design's §2 engine-coverage table and its
"Owns (regions)" line both call for real `X64Gen.cpp`/`X64.hpp` SSE emission
(`roundsd`/`sqrtsd`, the `floor`/`ceil`/`round`/`trunc`/`sqrt`/`toInt`/
`toFloat` native rows) — written and verified working (byte-identical to
the other four engines) before discovering `designs/complete/techdesign-portable-
backend.md` §0.4's explicit, owner-approved freeze: "`src/X64.hpp`,
`src/X64.cpp`, `src/X64Gen.hpp`, `src/X64Gen.cpp` — read-only reference
material... never edit them." This design has no awareness of that pivot
(no currency-check note, unlike techdesign-03/04's siblings); techdesign-04
hit the identical tension for its own new string natives and resolved it by
never touching X64Gen at all, excluding ELF from that work's corpus
coverage instead. Escalated to the owner rather than deciding unilaterally
(this is exactly the model-escalation case the portable-backend doc's own
§0.3 STOP protocol describes). **Owner ruling (2026-07-06): leave the
already-built, complete, verified-working X64Gen/X64.hpp changes in place —
"if your changes dont hurt thats fine, don't waste time removing it" — but
do no further X64Gen work going forward; LLVM is the real backend now.**
Consequence for this track: the ELF column of §2's coverage table is
implemented as designed (not reverted), but should be read as a one-time
exception, not a precedent — the transcendental group (`pow`, `log`, `log2`,
`exp`, `sin`, `cos`, `tan`, `atan2`) stays deferred on ELF exactly as
originally planned, and no *new* X64Gen work should be picked up from this
design's text by a future implementer.

**Corpus:** `tests/corpus/math.ext` (the fully-five-engine-covered surface —
int mask, float mask minus `pow`, `math` consts/`min`/`max`) in the shared
corpus dir, verified byte-identical `--run`/`--ir`/`--emit-cpp`/`--emit-elf`/
`--native-obj`. `tests/corpus/math_transcendental/trig.ext` (the deferred
group: `float.pow`, `math::log/log2/exp/sin/cos/tan/atan2`) in its own dir
with three new ctest lanes (treewalk/ir/emit-cpp only — mirrors
`strings_native`'s pattern, but excludes LLVM too since this group is
uncovered on both native backends, not just ELF).

**Reference-doc duty:** §6.1's int/float tables rewritten; new §6.1b
(`namespace math`, incl. the `std::math` → `math` substitution and why);
§7.3 gained a concrete out-of-coverage example. info.md: no change (pure
surface addition, per the design's own call).

No STOP conditions from §7 were triggered (P1 stayed IEEE-honest; ELF SSE
emission never touched the value-tag layout; libm was never linked into the
ELF or LLVM backends for the deferred group).
