# Packet 05 — canon + the float leg (Model: **Opus**)

Milestone M2 core. Design §3 (the `canon` normalization), §3.2 (integer
form MANDATORY), §3.3 (normalize at compare time, never storage), §8
(engine legs). This packet has the most per-engine surface — go slowly,
one engine at a time, differential after each.

## 1. The canon helper — one per engine, single-sourced (§8)

Reference semantics (§3.1): NaN (any payload/sign) → `0x7FF8000000000000`;
±0.0 → `0`; else raw bits. **Integer/branchless form only** (§3.2) — never
`x != x`, never FPU compares; the emit-C++ backend's output is compiled by
user toolchains with unknown `-ffast-math` flags:

```c
static inline uint64_t lv_canon(double x) {
    uint64_t b;  memcpy(&b, &x, 8);                       // bit reinterpret
    uint64_t is_nan  = ((b & 0x7FF0000000000000ull) == 0x7FF0000000000000ull)
                     & ((b & 0x000FFFFFFFFFFFFFull) != 0);
    uint64_t is_zero = (b << 1) == 0;
    return is_nan ? 0x7FF8000000000000ull : (is_zero ? 0 : b);
}
```
(ternaries on integer predicates compile branchless enough; the mandate is
about no-FPU-compares, not literally cmov — don't over-engineer.)

- **Oracle + IR interp**: ONE inline helper in `RuntimeValue.hpp` (both
  include it). Use it in `keyEquals`' float leg (`RuntimeValue.hpp:249`):
  `case VKind::Float: return lv_canon(a.f) == lv_canon(b.f);` — Map keys
  are canonical per §4.
- **emit-C++**: emit one `static inline` canon function into the generated
  preamble (near the emitted `keyEq`, `CGen.cpp:231` region); emitted
  `keyEq` float leg (`CGen.cpp:239`) calls it.
- **LLVM**: add `lvrt_canon(double)->u64` (or take/return via LvValue —
  match local convention) to `runtime/lv_runtime.c` + `runtime/lv_abi.h`;
  give `lvrt_keyeq` an explicit `LV_FLOAT` case (`lv_runtime.c:2090`
  currently bit-compares payloads — note: that was ALREADY divergent from
  the interpreters on -0.0/+0.0 map keys; canon fixes it, and if you can
  demonstrate the old divergence with a two-line map program, record it in
  the commit message as a bug swept by this packet).
- The **hash-consistency law** (§3.3): there is exactly one canon symbol
  per engine; future hash work must call the same one. Put that sentence in
  a comment at each definition.

## 2. `float.bits()` / `float::fromBits(int)` natives (§8)

- Prelude declarations in `class float` (`Resolver.cpp:143-168`):
  `int bits();` (instance, native). For `fromBits`: check whether the
  prelude/dispatch machinery supports a static (`::`) method on the float
  mask class the way `Enum$fromCode` works. If a static-on-primitive isn't
  supported, mirror the enum mechanism (a free function + checker
  `float::fromBits` routing) — and if THAT exceeds a focused change, STOP
  and propose `math::floatFromBits` as the fallback spelling (needs owner
  sign-off; the design names `float::fromBits`).
- Implementations: `RuntimeNatives.cpp` float section (~line 889):
  `bits` = bit_cast double→int64; `fromBits` = int64→double. CGen native
  table (`CGen.cpp:571-585`) and LlvmGen native-method list
  (`LlvmGen.cpp:1969` + emission nearby): one instruction each.
  X64/ELF: do NOT add (frozen; keep tests out of ELF lanes — they already
  are, composition has no ELF lane).
- These are load-bearing for tests: **NaN construction in corpus files must
  use `float::fromBits`, never `0.0/0.0`** (bug #12: LLVM's 0.0/0.0 yields
  0.0 — a known open divergence; do not step on it).

## 3. `canonEq` — the seam generated code compares floats through

The synthesized `(==)` body is Leviathan source; it needs a callable canon
compare. Add a prelude-declared native `bool canonEq(float other)` on
`class float`, implemented in each engine by calling that engine's ONE
canon helper (law above). Then flip packet 02's float leg: generated
bodies emit `this.f.canonEq(other.f)` for float fields (remove the
`// packet 05 flips this` comment).

Do NOT document `canonEq` as public API in reference.md yet — packet 08
decides its documentation posture (it is de-facto public once in the
prelude; the doc should present it as the canonical-relation primitive).

## 4. Scalar operators: UNCHANGED

`==`/`!=`/orderings on float scalars stay IEEE everywhere — `arithPrim`
(`RuntimeValue.hpp:405-421`), emitted `ar` (CGen), `lvrt_arith`
(`lv_runtime.c:2440-2461`), LLVM FCmp fast path (`LlvmGen.cpp:2207-2240`).
Touch none of them. `isNaN() => this != this` (`Resolver.cpp:157`) stays.

## Corpus (green, four lanes — design M2 list)

- `float_field_nan_reflexive.lev` — struct with a NaN field (via
  `float::fromBits(0x7FF8000000000000)`); `s == s` true; also
  `x != x` true for the bare scalar in the same program (the seam, both
  relations in one pin). Print only bools.
- `float_field_zero_signs.lev` — `-0.0` vs `+0.0` fields equal; scalar
  `-0.0 == 0.0` also true (relations agree on zeros).
- `float_field_payload_collapse.lev` — two different NaN payloads via
  `fromBits(0x7FF8000000000001)` vs `0x7FF8000000000cafe`-style; fields
  equal (collapse), `bits()` round-trip shows storage untouched (§3.3):
  print `s.f.bits() == t.f.bits()` → false, `s == t` → true.
- `float_map_key_nan.lev` — Map with a float key: NaN key retrievable,
  -0.0/+0.0 same key. (Map keys are canonical NOW — §4 table row — this
  pin locks the lvrt_keyeq fix.)
- Do not print raw NaN/float text unless you first verify all four engines
  format it identically; prefer bools.

## Warnings

- Byte-order/`memcpy` only for bit reinterpretation in C/C++ (no unions in
  C++, no `reinterpret_cast` UB).
- `bits()` returns int64 with the high bit possibly set (negative floats,
  0xFFF8… NaNs) — Leviathan `int` is signed 64; hex literals like
  `0x7FF8000000000000` fit, `0xFFF8000000000000` would be negative. Tests
  should stick to positive-sign patterns.
- LLVM lane: decide call-vs-inline for canon (§8 allows either) — a runtime
  call (`lvrt_canoneq`) is simpler and safer than hand-built IR; prefer it
  unless the map-key path already forces inline IR.

## Acceptance

Full `ctest` green; new pins byte-identical on four lanes; scalar float
corpus (math_transcendental etc.) unmoved. Commit:
`Struct equality packet 05 (M2): canon relation — engine helpers,
bits/fromBits/canonEq natives, float fields + Map keys canonical`.
