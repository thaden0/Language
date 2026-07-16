# Tech design — struct equality synthesis & the canonical float relation

**Status: DESIGN ONLY.** Implementation deferred by owner, 2026-07-15.
Nothing below is implemented; no engine work has started. When implemented, this
closes `known_bugs_2.md` #77 (ruled P1.1) and sweeps its `docs/footguns.md` row.

**Owner ruling (2026-07-15).** Struct `==` is **field-wise by default**
(Option A), synthesized under a comparability gate; float fields compare via
the **canonical relation** defined here; a struct with a non-comparable field
gets a **loud compile error**, never a silent `false`. The status-quo behavior
— bare `==` on an `(==)`-less struct silently returning `false` — dies in M1
under every option and must not survive in any form.

**Decision log** (ratified in design review, 2026-07-15):

1. The canonical NaN constant is a *language* constant, identical on every
   target — no per-architecture build configuration exists or is permitted.
2. One relation per construct *kind*, stated once (§4) — no Python-style
   identity shortcuts or per-callsite fallbacks anywhere in the language.
3. Engines emit the **integer/branchless** `canon` form only (§3.2) —
   `-ffast-math`-immune by construction; never prelude `.lev`.
4. `match` on float scalars compares **canonically**; `float::NaN` is an
   ordinary, reachable pattern (§6).
5. A hand-written `(==)` always overrides synthesis and keeps IEEE operator
   semantics by its author's own hand (documented divergence, §5.4).
6. `canon_hash`/`canon_cmp` are **specified now** (§7) and implemented only
   when float keys / sorted containers land (M4, dormant).

---

## §1 The problem being fixed

`known_bugs_2.md` #77: two field-identical structs compare unequal —
`Point(1,2) == Point(1,2)` is `false` on every engine, silently, exit 0. This
contradicts `info.md` §9's own axiom ("No identity. A struct is its fields...
Equality is field-wise / by a defined `(==)`") and produced the Harpoon
`assertEqual(struct, struct)` footgun. The silent `false` also violates the
loudness principle (§1: an unresolvable read "is an error rather than a
guess") — it is neither field-wise nor an error.

The float complication: naive field-wise `==` through IEEE 754 makes derived
equality **non-reflexive** (a struct holding NaN is not equal to itself —
`assert_eq!(s, s)` fails, the exact Rust wart the `ordered-float` crate
exists to paper over). You cannot simultaneously have IEEE scalars, reflexive
aggregate equality, and one relation. This design keeps IEEE scalars, makes
aggregates reflexive, and pays with a second relation whose divergence from
the first is **exactly one case** (NaN), documented in one sentence.

---

## §2 Architecture: one type, two relations, one seam

- **Operator relation** — IEEE 754 `compareQuietEqual`, bound to
  `==`/`!=`/`<`/`<=`/`>`/`>=` on float **scalar** operands. Compiles to the
  native FPU compare. Partial equivalence: symmetric, transitive, **not
  reflexive** (`NaN != NaN`), identifies the zeros (`-0.0 == +0.0`).
  `x != x` remains the legal, blessed NaN idiom for arithmetic code.
- **Canonical relation** — a true equivalence (reflexive, symmetric,
  transitive) used by every *value/key-shaped* construct (§4). Defined by a
  normalization function (§3), not by new comparison logic.

**The seam, as a complete case table.** For non-NaN `a, b`: the relations
always agree, including on zeros. For NaN operands: `==` is always false;
`canon_eq` is always true. There are no other cases.

> Reference sentence (goes in `docs/reference.md` at M2): *float scalars
> compare IEEE; derived equality, hashing, ordering, and match — like all
> value contexts — compare canonically; the two differ only on NaN.*

---

## §3 `canon` — the normalization function

### §3.1 Definition (float = 64-bit)

```
canon(x: float) -> u64:
    if isNaN(x)   -> 0x7FF8_0000_0000_0000    // canonical qNaN
    if x is ±0.0  -> 0x0000_0000_0000_0000    // +0.0
    else          -> bits(x)

canon_eq(a, b)  := canon(a) == canon(b)       // u64 compare
canon_hash(x)   := hash_u64(canon(x))         // §7, dormant
```

Equivalence classes: all 2⁵³−2 NaN bit patterns (both signs, quiet and
signaling, every payload) collapse into one class; `-0.0`/`+0.0` collapse
into one; every other pattern is a singleton. `canon_eq(a,b)` ⟺ IEEE
`a == b` ∨ (both NaN). If a 32-bit float type ever lands, the analogues are
`0x7FC0_0000` and `0x0000_0000`.

**Constant rationale** (doc-honest version): `0x7FF8…0` is the positive
quiet NaN with zero payload — the same value Java's `doubleToLongBits`
canonicalizes to (interop), and the default NaN ARM and RISC-V manufacture.
x86 manufactures `0xFFF8…0` (sign bit set, the "indefinite" QNaN) — this is
**irrelevant at runtime** because `canon` collapses every pattern; it matters
only to the constant's justification, and the *positive* sign is load-bearing
for §7's order (NaN sorts above `+∞`). Do not claim "most hardware produces
it."

### §3.2 Implementation mandate: integer form only

Two ways to write `canon`: FPU compares (`x != x` for NaN) or pure integer
ops on the raw bits. **Engines must emit/implement the integer form:**

```
canon(x):
    b       = bits(x)                                  // reinterpret, 1 op
    is_nan  = (b & 0x7FF0_0000_0000_0000) == 0x7FF0_0000_0000_0000
              && (b & 0x000F_FFFF_FFFF_FFFF) != 0
    is_zero = (b << 1) == 0
    select(is_nan, 0x7FF8_0000_0000_0000, select(is_zero, 0, b))
```

3–5 instructions, no memory traffic, branchless. The FPU form is destroyed by
`-ffast-math`-family flags (the compiler folds `x != x` → `false`), and the
emit-C++ backend hands generated source to *user* toolchains whose flags we
do not control. The integer form is immune by construction. Our own lanes use
plain `-O2` (`sonar/tests/runtests.sh`, corpus runners) — the mandate protects
downstream users, not us.

### §3.3 Normalize at compare/hash time, never at storage

Structs store whatever bits arithmetic produced. `canon` runs inside the
generated `(==)`/hash bodies on values as read. Consequences: NaN payloads and
sign bits survive in memory — `copysign`, payload-based error propagation,
bit-exact serialization round-trips, and FFI all see original bits. Two
structs differing only in NaN payload are canonically equal but not
bit-identical; both facts hold simultaneously. Raw at-rest bits of
hardware-manufactured NaNs may differ per architecture (x86 vs ARM); a
program wanting cross-platform bit-identical serialized floats serializes
`canon(x)` — an application choice, not a language one.

**Hash-consistency law:** `eq` and `hash` must normalize through the *same*
`canon`. Each engine exposes exactly one canon helper and both consumers call
it — the classic corruption bug (normalize in eq, hash raw bits → zeros equal
but differently hashed) is thereby unwritable, not merely discouraged.

---

## §4 One relation per construct kind (the anti-Python rule)

| Construct | Relation |
|---|---|
| Infix `==`/`!=`/ordering on float **scalars** | IEEE (operator) |
| Derived struct `(==)` — float fields | canonical |
| `Map` keys, `Set` membership | canonical |
| `Array.contains` / `indexOf` / dedup / distinct | canonical |
| Hashing | canonical (`canon_hash`, §7) |
| Sorting / sorted containers | canonical (`canon_cmp`, §7) |
| `match` on float scalars | canonical (§6) |
| Hand-written `(==)` bodies | whatever the author wrote (IEEE ops, §5.4) |

No construct consults identity, address, or a fallback chain. Python's
incoherence (`nan in [nan]` true via identity shortcut while `nan == nan` is
false) is structurally impossible here: membership is `canon_eq`, period.

**Loudness rule (new diagnostic, M2):** `x == float::NaN` and
`x != float::NaN` — an operator compare against the NaN constant — is
*statically always-false/true* under IEEE and therefore a **compile error**,
with fixit: *use `x.isNaN()` or a `float::NaN` match arm*. (`x != x` stays
legal; numeric code legitimately writes it.)

---

## §5 Derived struct `(==)` synthesis (the #77 fix proper)

### §5.1 The gate

Synthesize field-wise `(==)` for a struct **iff every field is comparable**
per §5.2. Otherwise `s1 == s2` on that struct is a compile error naming the
first non-comparable field:

```
error: struct 'Job' has no '(==)': field 'onDone' (a function value) is not
comparable — define an explicit 'bool (==)(Job other)' to opt in
```

Either way, the silent-`false` path is removed entirely.

### §5.2 Field comparability ladder

| Field type | Compares by |
|---|---|
| `int`, `bool`, `char` | value (existing scalar compare) |
| `enum` | carrier (existing; confirmed working in #77 entry) |
| `string` | content (existing) |
| `float` | **canonical** (`canon_eq`, §3) |
| `struct` | that struct's `(==)` — synthesized recursively or explicit; non-comparability propagates. Value structs cannot cycle by value (infinite size, already rejected), so recursion terminates. |
| class reference | reference identity (matches today's class behavior in `assertEqual`) |
| `T?` / unions | tag first; payloads by this ladder; `None == None` is true |
| `Array`, `Map`, `Block`, function values | **not comparable in v1** → gate fires. Container equality is its own future design (§10); function equality is undecidable. |

### §5.3 Semantics of the synthesized body

Fields compare in declaration order, first mismatch short-circuits false. No
user code runs (all ladder legs are pure), so order is unobservable except in
cost. `!=` derives as the negation. Ordering operators are **not**
synthesized (out of scope).

### §5.4 Explicit `(==)` overrides

A struct that declares `(==)` gets no synthesis — the author's body is the
relation, and `x == other.x` inside it uses IEEE operators (their own hand;
non-reflexivity is then their documented choice). The reference notes:
*derived = canonical; hand-written = what you wrote.*

### §5.5 Implementation strategy (all four engines, one definition)

Materialize the synthesized `(==)` as a **real generated method decl on the
struct** at resolve/check time, through the existing generated-decl channel
(the machinery attribute rules / `enumDesugars` / Atlantis `FromJson`
generation already use). All four engines then execute an *ordinary method* —
no per-engine field-walk logic, one point of truth. The only per-engine work
is the `canon` primitive itself (§8). This also keeps the oracle (`Eval.cpp`
tree-walk) and the checker in agreement for free, and the generated body is
visible to `--expand` for debugging.

---

## §6 `match` on float scalars

Patterns compare **canonically** — a pattern is a value-classification
question ("is this value in this arm's key set"), the same family §4 assigns
to canonical. Consequences:

```
match (x) {
    float::NaN => handleNan();   // reachable; matches every payload, both signs
    0.0        => zeroish();     // catches -0.0 too (both relations agree)
    1.5        => exact();
    _          => rest();
}
```

- `float::NaN` is a new language constant (its value: the canonical qNaN;
  printing it prints `nan` as today). `x.isNaN()` ships alongside as the
  blessed operator-world test.
- Because canonical ≡ IEEE except NaN, a float match **never disagrees with
  `==`** on any value `==` can distinguish; the divergence is confined to the
  one arm `==` cannot express. No third relation, no special-cased pattern.
- Diagnostics: duplicate `float::NaN` arms → duplicate-arm error; a NaN arm
  after `_` → unreachable error. Float matches are never exhaustive without
  `_` (unchanged).
- Lowering note: the *pattern literal's* canon folds at compile time; the
  scrutinee is canon'd once before the arm chain — one `canon` + integer
  compares per match, not one per arm.
- Float **range** patterns (if ever added) use `canon_cmp`; NaN falls in no
  range. Deferred until float ranges exist.

Rejected alternatives: forbidding float patterns (Rust's road — loses the NaN
arm the owner explicitly wants); a magic NaN-only pattern inside an
otherwise-IEEE match (two relations in one construct — the Python move).

---

## §7 `canon_hash` and `canon_cmp` — specified now, dormant until M4

```
canon_hash(x) := hash_u64(canon(x))

canon_cmp(a, b) := totalOrder(canon(a), canon(b))
    where totalOrder compares the bits as i64 after the sign-magnitude flip:
    if (b < 0) b = INT64_MIN - b;   // flip non-sign bits for negatives
    then compare as signed integers.
```

Resulting order: `−∞ < negative reals < ±0.0 (one position) < positive reals
< +∞ < NaN (one position, top — the canonical NaN is positive)`. It is total,
and `canon_cmp(a,b) == Equal ⟺ canon_eq(a,b)` **by construction** — hash
maps, tree maps, and sorts agree on the key population. (Contrast Rust's
`f64::total_cmp`, which distinguishes `-0.0 < +0.0` and orders NaNs by
payload — misaligned with any canonical equality; that incoherence is
impossible here.)

Activation trigger: float-keyed `Map`/`Set`, or a sort over floats/structs
containing floats. Until then these are spec-only; no code path consumes
them. A NaN key inserted into a future map is retrievable, occupies one slot
regardless of payload, and deduplicates against every other NaN.

---

## §8 Engine legs & the runtime seam

- **One canon per engine, single-sourced:**
  - oracle (`Eval.cpp`) + IR (`IrInterp`): one shared C++ inline helper.
  - emit-C++ (`CGen`): emit one `static inline` function; generated `(==)`
    bodies call it. Integer form only (§3.2).
  - LLVM (`LlvmGen`): inline the 5-op IR sequence at canon sites (or one
    function, implementer's call — but one definition, both eq and future
    hash must route through it).
  - **Never** prelude `.lev` — the prelude is unchecked and the known prelude
    footguns apply; canon needs bit reinterpretation anyway.
- **New natives (tiny, independently useful):** `float.bits() -> int` and
  `float::fromBits(int) -> float` — one instruction each; serialization wants
  them regardless.
- **Self-hosting impact: positive.** Given `bits()`/`fromBits()`, `canon` is
  expressible in pure Leviathan integer math — the self-hosted compiler
  carries zero FPU-quirk dependence, strictly *less* platform surface to
  bootstrap on a new architecture. Add both natives to the self-host
  prerequisite list. No build flags, no per-target constants (Decision 1).

---

## §9 Milestones

House rules apply: engine-differential at every step (oracle/IR/emit-C++/
LLVM byte-identical), corpus pins land with the code, STOP-and-escalate on
any cross-engine divergence. Each milestone is one focused session.

- **M1 — gate + non-float synthesis.** Comparability ladder minus floats
  (int/bool/char/enum/string/recursive struct/class-ref/`T?`); generated-decl
  channel (§5.5); loud non-comparable diagnostic; silent-`false` removed.
  Corpus (`tests/corpus/composition/aggregates/green/` or a new `equality/`
  cluster): `struct_eq_basic`, `struct_eq_nested`, `struct_eq_class_ref`,
  `struct_eq_optional_field`, `struct_eq_explicit_override`, plus
  `struct_eq_noncomparable.expected-error`. Harpoon `assertEqual` doc-comment
  simplified in the same commit.
- **M2 — canon + the float leg.** Engine canon helpers (§8); float fields in
  synthesis; `float.bits()`/`float::fromBits()` natives; `float::NaN`
  constant + `isNaN()`; the `== float::NaN` compile error (§4). Corpus:
  `float_field_nan_reflexive`, `float_field_zero_signs`,
  `float_field_payload_collapse`, `nan_literal_compare.expected-error`.
  `docs/reference.md` + `info.md` §9 sentences land here.
- **M3 — canonical match.** Float patterns via canon (scrutinee canon'd
  once); NaN-arm reachability; duplicate/unreachable diagnostics. Corpus:
  `match_float_nan_arm`, `match_float_zero_arm`,
  `match_nan_dup.expected-error`.
- **M4 — dormant until containers.** `canon_hash`/`canon_cmp` wiring when
  float keys/sorted containers/sort land; churn-lane coverage for NaN-keyed
  maps. No work before the trigger.

**Acceptance (whole design):** all four engines byte-identical on every green
pin; no existing golden moves (no `.expected` today encodes struct-`==`
output — verified while re-tiering #77); `known_bugs_2.md` #77 entry deleted
and `docs/footguns.md` row swept in the landing commit, per the standing
definition-of-done.

---

## §10 Deliberately deferred

- `Array`/`Map`/`Block` element-wise equality (own design; when it lands,
  those field kinds flow through §5.2's gate automatically).
- Float range patterns (§6).
- Struct hashing / structs as map keys (rides on M4 + container-key design;
  `canon_hash` is ready for it).
- Any change to operator semantics on float scalars: **none, ever** — IEEE
  stays IEEE.
