# `decimal` — a precise, non-binary-float numeric type — technical research dossier

**Status:** pre-design research. **Not** a tech design. This is the complete technical
substrate a tech design for `designs/requests/accepted/request-decimal-type.md` must build
on: what the numeric surface is today, exactly how values are represented on every engine,
which layers a new numeric type touches, the representation decision that dominates the whole
design, and the questions the design has to answer. Every implementation claim below is
anchored to code (`file:line`) verified against the current tree (`docs/reference.md` /
`info.md` are cited only for *intent*, never as evidence that something is built).

Audience: whoever writes the tech design (a Fable-class author per
`[[feedback_techdesign-conventions]]`), and the implementers after them. This is intended to
be the **only** document needed to write the design — it is deliberately long and repeats
context so the designer never has to go spelunking.

---

## 0. One-paragraph orientation

Leviathan has exactly one non-integer numeric type, `float` — an IEEE-754 binary double.
Binary doubles cannot represent most base-10 fractions exactly (`0.1 + 0.2 != 0.3`), which is
wrong for money, invoicing, ledgers, and any Atlantis web-framework consumer that touches
prices. Today the only honest options are `float` (wrong for money) or `int`-as-scaled-units
(a convention, not a type). A `decimal` type would give base-10 fractions an exact home. The
feature was **deliberately deferred** three times (suggested-features §2.5 → the deferral note
→ this ticket) precisely because it is "genuinely large to do right": it is not a small
primitive addition like `char`, because a useful decimal forces four coupled decisions —
**representation** (fixed-point vs. C#-128-bit vs. IEEE-754 decimal vs. arbitrary-precision),
**arithmetic semantics** (rounding modes, overflow, division), **literals** (how `9.99`
becomes a decimal *without* passing through a lossy `double*`), and **engine/ABI reach** (the
16-byte tagged-value ABI has room for exactly one 64-bit payload word, which is the single
fact that dominates the representation choice). The good news the request does not mention:
the `enum` precedent proved you can land a **full-coverage-including-LLVM** new type with
**zero** ABI/runtime work by desugaring to a value `struct` + operator methods written in the
language — so a low-risk "decimal as a prelude `struct`" path exists that sidesteps most of
the "genuinely large" cost. The central design tension is exactly that: *primitive-grade
(new ABI tag, fast, C-implemented arithmetic) vs. library-grade (`struct`, zero ABI, slower,
in-language arithmetic)*, crossed with *fixed-size vs. arbitrary-precision*.

---

## 1. The request, restated, and its lineage

**The ask** (`request-decimal-type.md`): a numeric type where base-10 rounding is exact, for
money/billing/ledgers. The request explicitly **does not prescribe** a representation — that is
the open question it hands the designer. Its acceptance criteria:

1. A recorded decision on representation (fixed-point vs. arbitrary-precision) **before** any
   implementation.
2. If implemented: `+ - * /`, comparison, and `toString()`/`parse()` round-tripping, correct
   to the chosen precision, **on every active engine**.
3. `info.md` §2/§9 gains a `decimal` entry (or an explicit "remains deferred, here is why"
   note) so the decision cannot silently vanish with the ticket.

**Lineage** (why this keeps getting deferred, recovered from git `85a3e99^`):

- The retired `designs/suggested-features.md` §2.5 listed it as: *"decimal — genuinely useful
  for money, genuinely large to do right; defer past the framework milestone, note it for the
  data thrust."*
- §2.4 of the same doc ruled that **integer widths stay singular** ("one honest `int` is a
  feature, not a gap") and that width concerns live at the **`Block`/struct-layout boundary**,
  not in the scalar type system. This is the same stance `request-narrow-integer-types.md`
  now tracks, and it matters for `decimal` because *decimal is the one numeric-precision ask
  §2.4 did **not** wave off* — it was explicitly carved out as a real future type.
- The interim fallback (still in force, nothing is blocked on this): money is `int`-scaled by a
  fixed factor (cents), or `float` where imprecision is tolerable.

**Sibling requests to keep coherent with** (all in `designs/requests/`): `request-narrow-integer-types.md`
(byte/int8/…/uint — the other "should the numeric primitive set grow?" ticket; decide the two
with a consistent philosophy), and `request-columnar-dense-array-struct.md` (accepted; the
data/columnar thrust that a fixed-size decimal would want to be dense-storable inside).

---

## 2. The governing philosophy the design must satisfy

These are `info.md` §1 rules that a `decimal` design cannot violate without an explicit ruling.
They are the acceptance gate the design will be judged against by the owner.

| rule (info.md) | what it forces on `decimal` |
|---|---|
| **Honesty over hidden magic** (§1) | A `decimal` literal must **not** silently detour through `float`. `decimal d = 0.1;` that parses `0.1` as a `double` first has already lost the exactness the type exists to provide. The literal must parse from the **source text** to the decimal representation directly. This is the single sharpest correctness constraint in the whole feature. |
| **Resolution is by type, everywhere** (§1, §9) | Mixed `decimal`/`int`/`float` arithmetic resolves by operator overloading on operand type (§5, `(op)` methods), not by a promotion lattice. There is **no implicit numeric promotion** in the language (see §3 below) — decimal must not introduce one. |
| **Object mask** (§9) | If `decimal` is a primitive, `this` inside its methods is the raw value, methods dispatch through the same shape machinery as `int`/`char`, storage is unboxed. If it is a `struct`, `this` is the value record. Either way it wears methods like every other numeric type — `d.toString()`, `d.abs()`, etc. |
| **The gate pattern** (§16) | Not obviously invoked — decimal is a *safe* type, not a bazooka. But the *lossy* conversions around it (decimal→float, decimal→int truncation, float→decimal) are the footgun surface and should be **explicit methods**, never implicit coercions. |
| **One rule over many special cases** (§1) | Prefer reusing existing machinery (value `struct`, operators-as-methods, labeled ctors, target-typed literals) over inventing decimal-specific mechanisms. The `enum` desugar and the `DateTime`/`Duration` structs are the models. |
| **Loud runtime failures** (§12.6, reference §3.7) | Overflow, divide-by-zero, and un-representable conversions should throw catchable `RuntimeException`s (the language's "no silent-and-distant failure" rule), unless the design deliberately picks IEEE-style quiet NaN/inf — which would be a conscious departure worth a ruling. |

---

## 3. The current numeric surface (what `decimal` sits beside)

### 3.1 `int` — signed 64-bit

- One integer width, 64-bit signed, deliberately singular (info.md §9, §19 #1).
- Immediate value: ABI tag `LV_INT = 1`, payload = the value itself (`runtime/lv_abi.h:39,59`).
- Methods (reference §6.1, `docs/reference.md:808`): `abs()`, `max/min(int)`, `toString()`,
  `pow(int)` (overflow **wraps**, two's-complement), `clamp(int,int)`, `sign()`, `toHex()`,
  `toString(int radix)` (2..36), `toFloat()`.
- Integer operators `<< >> ^ ~` are **int-only** (reference §3.5b, `docs/reference.md:400`);
  a shift count outside `0..63` **throws** rather than masking — the loudness precedent for
  what decimal overflow/precision violations should do.
- Div/mod by zero: interpreters yield `0` for int (`runtime/lv_abi.h:322`, a documented
  interim), a wart the design should **not** copy for decimal (money divide-by-zero must be
  loud).

### 3.2 `float` — IEEE-754 binary double

- ABI tag `LV_FLOAT = 2`; payload holds the **double bit pattern** via `memcpy`, never a cast
  through a long (`runtime/lv_abi.h:58-59`). This is the exact "reinterpret 64 bits" trick a
  64-bit decimal encoding (IEEE decimal64, or a packed coefficient) would reuse.
- Methods (reference §6.1, `docs/reference.md:809`): `toString()`, `abs()`, `floor()`,
  `ceil()`, `round()` (half-away-from-zero), `trunc()`, `sqrt()`, `pow(float)`, `toInt()`
  (truncates; NaN/±inf/out-of-range → **RuntimeException**, loud), `isNaN()`, `isInfinite()`.
- **The float-rendering wart (a positive argument for decimal).** `float.toString()` renders
  the C `%f`-style `42.000000` form; JSON number rendering inherits it and is pinned as a
  known-ugly interim (reference §6.11, `docs/reference.md:2005`; info.md §19 #16). Whatever
  global float-formatting cleanup eventually happens, **`decimal.toString()` must be clean and
  exact from day one** (`"42"`, `"9.99"`, `"0.10"`), because exact human-readable rendering is
  half the reason the type exists. The design should treat decimal formatting as independent
  of (and a model for) the deferred float-formatting work, not blocked on it.

### 3.3 No implicit promotion — the load-bearing fact

Leviathan has **no C-style numeric promotion lattice.** `int` and `float` are distinct
primitive classes; mixing them in arithmetic goes through operator resolution by operand type,
and `~`/unary-`-`/etc. are type-gated with loud errors on the wrong operand
(`src/Checker.cpp:781-796`). `char` was designed to carry **no arithmetic at all** specifically
to "never drag in C's integer-promotion special cases" (info.md §9; reference §2.2). The
narrow-int request's Known-Warnings section hammers the same point: *"Whatever is chosen must
not reopen integer-promotion special-casing."* **A `decimal` that participates in arithmetic
must state its mixed-type rules up front and explicitly** — this is a stated acceptance gate,
not a detail.

---

## 4. THE central constraint — the value/ABI model

Everything about the representation decision is downstream of one fact: **a runtime value is
16 bytes with one 64-bit payload word.**

### 4.1 The tagged-value layout

`runtime/lv_abi.h:36`:
```c
typedef struct LvValue { int64_t tag; int64_t payload; } LvValue;   /* 16 bytes */
```
The closed tag set (`runtime/lv_abi.h:39-49`):
```
LV_VOID=0 LV_INT=1 LV_FLOAT=2 LV_BOOL=3 LV_STR=4 LV_OBJ=5 LV_ARR=6 LV_MAP=7 (8) LV_CLO=9
LV_CHAR=10 LV_BLOCK=11
```
- **Immediates** — `LV_INT`, `LV_FLOAT`, `LV_BOOL`, `LV_CHAR` — store the value **directly in
  the single `payload` word**, no heap, no ARC (`runtime/lv_abi.h:58-60`).
- **Heap types** — `LV_STR`, `LV_OBJ`, `LV_ARR`, `LV_MAP`, `LV_CLO`, `LV_BLOCK` — store a
  **pointer** in `payload`, to a record preceded by a 16-byte `LvHeader {rc, meta}` at
  `payload-16` (`runtime/lv_abi.h:68-89`).
- Value `struct`s ride as **tag-5 (`LV_OBJ`)** with a value flag; a tag-5 payload may point
  **inline into a dense array buffer** rather than owning a header (`runtime/lv_abi.h:61-63`).

**The hard consequence for decimal:** an *immediate* decimal has exactly **64 bits** of room.
That is enough for (a) an `int64` coefficient with a *fixed* implied scale, or (b) an
IEEE-754 **decimal64** encoding — and **not** enough for a C#-style 96-bit coefficient, a
128-bit fixed-point, or arbitrary precision. Anything wider than 64 bits **must be a heap type**
(a pointer to a record), exactly like `Block`, or a **value `struct`** (whose payload is a
pointer to a multi-slot record). There is no third option: widening `LvValue` past 16 bytes is
a whole-ABI rewrite touching every engine, every op, every allocator prefix — categorically
out of scope and never on the table.

### 4.2 The union value-size invariant — and the request's misconception

info.md §9 states: *"Union member size is value size, not content size — every heap-backed
type (string, array) has a small value size regardless of content, so unions over them are
cheap."* Every `LvValue` is 16 bytes; heap types carry a pointer. A union slot is sized to the
largest **value** (16 bytes), never the largest **content**.

**This corrects a premise in the request.** The request warns that *"arbitrary-precision …
variable-size storage breaks the 'union member size is value size' invariant."* At the ABI
level this is **not true**: an arbitrary-precision decimal implemented as a **heap-backed
handle** (like `string`, like `Block`) has a fixed 16-byte value size and slots into unions
exactly as cheaply as `string` does. What arbitrary precision actually forfeits is *unboxed /
immediate storage* and *trivial bit-copy value semantics* — not union compatibility. The
design should state this clearly: **the real representation axis is "immediate (≤64-bit,
unboxed, no ARC)" vs. "heap handle (any width, ARC-managed, pointer-copied)," and both are
union-safe.** The only thing that would genuinely break the invariant is trying to make decimal
an *inline multi-word immediate*, which the 16-byte `LvValue` forbids anyway.

### 4.3 What "add a numeric value kind" costs on each engine

The five consumers of the one bytecode IR (reference §17, `docs/reference.md:1718`):

| # | engine | file(s) | what a new value kind costs |
|---|---|---|---|
| 1 | tree-walk oracle | `src/Eval.cpp`, `src/RuntimeValue.hpp` | a `VKind` (unless struct-desugared) + native bodies |
| 2 | IR interpreter | `src/IrInterp.cpp` (shares `RuntimeValue.hpp`) | same `VKind` + IR-op plumbing |
| 3 | emit-C++ | `src/CGen.cpp` | mirror the value model in the embedded C++ runtime |
| 4 | **LLVM (primary AOT)** | `src/LlvmGen.cpp` + `runtime/lv_runtime.c` + `runtime/lv_abi.h` | a **new ABI tag** (STOP-gated contract change) + ARC rows + native rows |
| 5 | pure x86-64/ELF | `src/X64Gen.cpp` | **FROZEN — no lane. Never gate the design on it.** (`[[feedback_x64gen-frozen]]`, info.md §0/§17) |

**Coverage bar for this feature:** all four active engines (oracle, IR, emit-C++, LLVM); **no
ELF lane** and **the design is never gated on an ELF finding** (`[[feedback_x64gen-frozen]]` —
HARD, owner-enforced). `.lev` extension only, never `.ext` (`[[feedback_ext-vs-lev]]`).

---

## 5. The three "how to add a type" precedents (and which decimal can follow)

Leviathan has landed three new types recently; each is a different point on the cost curve, and
decimal can follow any of them. This is the most important section for scoping the work.

### 5.1 Precedent A — `enum`: zero ABI, full coverage, desugar to struct+globals

`enum` (Track 03 §2, reference §4.2c, `designs/complete/techdesign-03-core-types.md` §2)
**desugars in the resolver** to a value `struct` (one `int $code` field) + compiler-mangled
per-member const globals + a free function, with two narrow gated checker rules. Because it
lowers to *structs + ints + globals — machinery that already runs on every engine* — it needs
**no new `VKind`, no ABI tag, and is full-coverage including LLVM** at zero runtime cost
(`techdesign-03-core-types.md:228-232`). This is the existence proof that a numerically-shaped
type can reach every engine with pure front-end work.

### 5.2 Precedent B — `char`: a new **immediate** tag

`char` (Track 03 §1) is `LV_CHAR = 10`, a **pure immediate** (payload = the Unicode scalar, no
heap, no ARC) (`abi-addendum-lv-block.md` §2; `runtime/lv_abi.h:49,59`). It carries no
arithmetic, comparisons only, target-typed literals. The layer tour it required is the exact
template for an *immediate* decimal: `VKind::Char` in `RuntimeValue.hpp`, native `code()`/
`toString()`, checker literal target-typing, and (deferred until an ABI addendum landed) the
`LV_CHAR` tag + `LlvmGen` retag-inline construction. **A 64-bit fixed-point or decimal64
`decimal` would be a `char`-shaped effort** (immediate tag) — but *with* arithmetic, which
`char` deliberately avoided, so it is strictly harder than `char`.

### 5.3 Precedent C — `Block`: a new **heap** tag with ARC

`Block` (Track 03 §3) is `LV_BLOCK = 11`, a **heap** type: payload → a 32-byte body
(`{parentPtr, off, len, dataPtr}`) behind the standard 16-byte header, counted by ARC, with a
`lv_recursive_free` branch (`abi-addendum-lv-block.md` §3-4; `runtime/lv_abi.h:147`). Adding a
tag to the **closed** set is an automatic **STOP-gated contract change**
(`abi-addendum-lv-block.md` §1 ②; `techdesign-portable-backend-2.md` §0.2) — the enum extension
and its ARC/native implementation must land **together, atomically**, never "declared but
unimplemented." **A C#-128-bit or arbitrary-precision heap `decimal` would be a `Block`-shaped
effort** (new heap tag + ARC + recursive-free + STOP-gated co-landed pass).

### 5.4 Precedent D — `DateTime`/`Duration`: value `struct`s with labeled factories

`DateTime`/`Duration` (Track 09 F2, reference §6.12, `docs/reference.md:2010`) are **value
`struct`s** with **labeled "static" constructors** — `DateTime::now()`, `DateTime::ofEpochMs(n)`,
`Duration::ofHours(1).plus(Duration::ofMinutes(2))` — and instance methods
(`.plus`/`.minus`/`.toString()`). They are full-coverage on all four active engines with **no
ABI tag** (they are just structs). This is the precedent for **decimal as a `struct` library
type**: `Decimal::fromString("9.99")`, `Decimal::ofScaled(999, 2)`, `d.plus(e)`, `d.toString()`
— all in-language, zero ABI work, immediately full-coverage. The language has **no `static`
keyword** (reference §6.6; the enum/char/DateTime designs all note this), so factories are
**labeled constructors** (`Type::Label(args)`, reference §3.3) or `std::`/namespace free
functions — never `static` methods.

### 5.5 The decision matrix this sets up

| representation | fits immediate (≤64b)? | ABI cost | arithmetic | precedent | full-coverage day 1? |
|---|---|---|---|---|---|
| `int64`-coefficient, **fixed** scale | yes | new immediate tag **or** struct | C or in-language | char / DateTime | tag: after addendum · struct: yes |
| **C#-style** 96-bit coeff + scale (128b) | no → heap/struct | heap tag **or** 2-slot struct | C or in-language | Block / DateTime | tag: no (STOP pass) · struct: yes |
| IEEE-754 **decimal64** | yes | new immediate tag | needs a decimal-fp lib or in-language | char | tag: after addendum |
| IEEE-754 **decimal128** | no → heap | heap tag | needs a decimal-fp lib | Block | no |
| **arbitrary-precision** BigDecimal | no → heap handle | heap tag **or** struct-over-`Block`/`Array<int>` | in-language | Block / DateTime | struct-over-existing: yes |

**Reading of the matrix (for the designer, not a decision):** the two *lowest-risk* corners are
(1) **`int64` fixed-scale as a struct** (DateTime-shaped, immediate-ish, cheap, but scale must
be pinned — see §6) and (2) **C#-style 128-bit as a value `struct`** carrying two `int64`
slots (DateTime-shaped, no ABI work, full coverage, the "money" sweet spot most languages ship).
The *highest-fidelity* corner is arbitrary-precision, which is also the largest. The
*most-primitive-feeling but highest-ABI-risk* corner is a dedicated immediate/heap tag. The
`enum`/`DateTime` precedents strongly suggest **starting with a `struct` representation** to get
correctness and full coverage first, with a tag-based fast path as an optional later
optimization (the same "correct now, fast later" staging the `Dictionary`/join work used,
info.md §11).

---

## 6. Representation options — deep analysis

The request calls representation "the open question." Here is each candidate with its real
consequences.

### 6.1 Fixed-point: `int64` coefficient + scale

`value = coefficient × 10^(−scale)`. E.g. `9.99` = coefficient `999`, scale `2`.

- **If scale is a fixed global constant** (e.g. always 4 places): the whole value is a single
  `int64`, fits an **immediate** tag or a one-slot struct. Cheapest. But it is essentially
  "typed cents" — range is ±9.2×10^18 / 10^scale (≈ ±9.2×10^14 at scale 4), and it cannot
  represent a value needing more places. Multiplication/division must rescale and round.
- **If scale is per-value** (runtime field): needs **coefficient + scale = 2 words**, so it is
  a **2-slot value `struct`** or a heap record — no longer immediate. This is the flexible,
  "any decimal you type keeps its own scale" model. Addition/subtraction must align scales;
  `*` adds scales; `/` picks a result scale + rounding mode.
- **Scale-as-a-type-parameter** (`decimal<2>`, like a C++ template): would be
  generics-on-a-primitive. The language *has* class-level generics, but a *primitive* carrying
  a type parameter is unprecedented and collides with the "singleton/primitive has no T-typed
  state" rules (info.md §6.6). **Likely rejected** — call it out and dismiss it, or the designer
  will re-derive it.

**Verdict:** per-value-scale fixed-point in a 2-slot struct is the natural "fixed-point" answer
and maps onto the DateTime-struct precedent cleanly.

### 6.2 C#-style 128-bit (`System.Decimal` shape)

96-bit integer coefficient + a scale byte (0–28) + a sign bit, packed into 128 bits. This is
what "money people" expect from a `decimal` keyword; it is fixed-size, exact for 28–29
significant digits, and has well-specified arithmetic and rounding (banker's or away-from-zero).

- Does **not** fit a 64-bit immediate → **heap tag** (Block-shaped, STOP-gated) **or** a
  **2-slot value `struct`** (`{ int64 hi; int64 lo; }` or `{ int64 coeff; int64 scaleSignFlags; }`,
  DateTime-shaped, zero ABI).
- Arithmetic is non-trivial but bounded and fully specified by the .NET reference — a strong
  advantage: the designer can lift the exact algorithm and rounding rules rather than invent
  them.

**Verdict:** the strongest "obvious money decimal." As a value `struct` it needs **no ABI work**
and is full-coverage immediately — the single most attractive risk/reward point.

### 6.3 IEEE-754-2008 decimal (`decimal64` / `decimal128`)

The standard the request names first. `decimal64` fits a 64-bit immediate (payload = the
BID/DPD bit pattern, reusing the `LV_FLOAT` memcpy trick); `decimal128` needs heap.

- **Pro:** it is *the* standard; interop and correctness are well-defined.
- **Con:** essentially no hardware does decimal FP; you need a software library
  (Intel BID, IBM decNumber/libdecfloat) or a hand-rolled implementation. Bringing in a C
  dependency for the LLVM/emit-C++ lanes contradicts the language's dependency-minimalism
  (info.md §17; the math namespace already accepts libm, and TLS accepts OpenSSL behind a seam
  — so *a sanctioned, feature-detected dependency behind a seam* is a landed pattern, LA-2 —
  but it is a deliberate, owner-level decision each time). A hand-rolled decimal-FP arithmetic
  is substantial.

**Verdict:** highest standards-fidelity, highest implementation cost, and it forces the
"do we take another C dependency?" question. Probably not v1 unless the owner wants IEEE
conformance specifically.

### 6.4 Arbitrary-precision (BigDecimal)

Unbounded coefficient (a digit/limb vector) + scale. Correct for any input.

- **Storage:** a **heap handle** (like `string`) — value size stays 16 bytes, **union-safe**
  (see §4.2; this refutes the request's stated worry). Could be a new heap tag, or — the
  cheap path — a value `struct` wrapping an existing `Array<int>`/`Block` of limbs + an
  `int scale`. The latter needs **zero ABI work** (structs + arrays already run everywhere).
- **Arithmetic:** `+ - *` are schoolbook big-integer ops; `/` **requires** a precision context
  + rounding mode (division can be non-terminating: `1/3`). This is the feature that most needs
  a "MathContext"-style precision/rounding parameter, and the one place the design must be most
  careful.
- **Cost:** the largest of the five, and the one the deferral notes were most wary of.

**Verdict:** the "correct for everything" option; realistic only as a `struct`-over-`Array<int>`
if chosen, and the design must nail the division/rounding-context story.

### 6.5 The `struct` library type (orthogonal to 6.1–6.4 — it is a *packaging*, not a rep)

Independently of *which* numeric model above, decimal can be packaged as a prelude **value
`struct`** with all arithmetic **written in the language** via operator methods (§7). This is the
`enum`/`DateTime` path and it is worth stating as a first-class option because it changes the
whole risk profile:

- **Zero ABI/runtime work.** No `VKind`, no tag, no `lv_runtime.c` edit, no STOP-gated pass.
  Full-coverage on all four active engines the day it lands, by construction (it is just a
  struct + methods, and structs already run everywhere).
- **Operators as methods** already work on classes/structs — `OpenMode (|)(OpenMode)` is a live
  prelude example (`src/Resolver.cpp:2384`), and the enum desugar synthesizes
  `bool (==)(...)` (`src/Resolver.cpp:5774`). So `Decimal (+)(Decimal)`, `bool (==)(Decimal)`,
  etc. are ordinary members (info.md §5; `(!=)` derives from `(==)` for free).
- **Cost:** performance — arithmetic dispatches through methods rather than an intrinsic, and a
  struct value is a tag-5 heap-ish record outside dense arrays (§4.1). For money workloads this
  is almost always fine; for bulk analytics it is the thing a later tag-based fast path would
  optimize.
- **The one genuinely hard part it does *not* eliminate:** literals (§7). A prelude `struct`
  getting **target-typed numeric literals** (`decimal d = 9.99;`) is checker work regardless of
  packaging — the struct path does not make the literal problem go away, it just isolates it.

**This is the recommended spine of the design to evaluate first:** a value `struct` holding the
chosen numeric model (6.1 fixed-scale or 6.2 C#-128), arithmetic in-language, literals via the
char-precedent target-typing mechanism, with a dedicated ABI tag explicitly deferred as a
future optimization. It satisfies acceptance criterion #2 (all engines) at the lowest risk, and
matches how `enum`/`DateTime` actually shipped.

---

## 7. Literals & target-typing — the sharpest correctness problem

This is where "honesty over hidden magic" bites hardest and where most of the real compiler work
lives regardless of representation.

### 7.1 How numeric literals are lexed today

`src/Lexer.cpp:63` `lexNumber()`: a run of digits, optionally `.` + digits, yields
`TokenKind::IntLiteral` or `TokenKind::FloatLiteral` (`src/Lexer.cpp:84-90`). Hex/binary are
int-only. **There is no suffix mechanism** — `9.99d` would lex `9.99` as a `FloatLiteral` and a
separate `d` identifier. The token records its **source span**, so the *original decimal text*
is always recoverable — this is what makes exact parsing possible.

The AST/Expr node (`src/Ast.hpp`) already carries literal-typing flags set by the checker —
`singleQuoted` (`:173`) and `charLit` (`:179`) are the char precedent: *the token stays a
`StringLiteral`; the checker flips a flag when the expected type is `char`, and the engines then
produce a `char`.*

### 7.2 The char precedent — and why decimal must go further

The checker's `isCharLiteral`/`markCharLiteral`/`expectsChar` (`src/Checker.cpp:604-635`) do
exactly the target-typing decimal needs: at a site with a `char`-expected type (declared type,
comparison, param, return), a qualifying literal re-types to `char`. `expectsChar`
(`src/Checker.cpp:628`) matches a bare `char` type or a union containing it (`char?`). The float
literal typing is trivially `FloatLit → primType("float")` (`src/Checker.cpp:683`).

**Decimal's mechanism mirrors this**: at a `decimal`-expected site, re-type a numeric literal to
`decimal`. **But with one non-negotiable extra rule the char case does not have:** the decimal
value must be built by **parsing the literal's source text directly into the decimal
representation** — *never* by reading the already-lexed `double`. `decimal d = 0.1;` that took
the `FloatLiteral`'s parsed double would store the binary approximation of 0.1 and defeat the
type. So the checker/lowerer must, for a decimal-typed numeric literal, re-scan the source span
(the digits and the `.` position give coefficient + scale exactly) rather than reuse
`parseFloatLiteral` (`src/Token.hpp:132`). This is the "honesty" acceptance gate made concrete.

### 7.3 Suffix vs. target-typing — the design choice

- **Target-typing** (`decimal d = 9.99;`, char-style): consistent with resolution-by-type, no
  lexer change, matches the language's grain. **Downside:** a *bare* decimal literal with no
  type context (`var x = 9.99;`) stays `float` — exactly as `var s = 'a';` stays `string`
  (reference §2.2). If you want a context-free way to *write* a decimal, you need a suffix.
- **Suffix** (`9.99d` / `9.99m`): explicit, context-free, but adds a lexer token form and a
  new literal syntax (the language is wary of "unprincipled syntactic sugar," info.md §1). C#
  uses `m`; a `d` suffix collides visually with "double."
- **Both** (target-typed by default, suffix for context-free): the fullest surface; more
  spec/lexer work.

**Known deferral precedent:** char-literal target-typing was **not** extended to call-argument
position in v1 (`designs/deferal-track03-type-surface.md`; `src/Checker.cpp` char path notes
"call-arg position deferred"). Decimal should decide deliberately whether decimal literals
target-type in call-arg position or only at declared-type/return/comparison sites, and log the
choice — the same seam char left open.

### 7.4 The overload back-compat hazard

char's design problem #1 (`techdesign-03-core-types.md:317`): where both `f(char)` and
`f(string)` exist, a bare single-quoted literal selects **`string`** (back-compat wins).
Decimal has the analogous hazard: where both `f(decimal)` and `f(float)` (or `f(int)`) exist, a
bare numeric literal argument must have a **stated, tested precedence**. The likely rule
(matching char): the *pre-existing* numeric type (`float`/`int`) wins for a bare literal, and
reaching the `decimal` overload needs a `decimal`-typed expression — but this is a design ruling
to make explicitly, not assume.

---

## 8. Arithmetic, operators, rounding, mixed-type policy

### 8.1 Operators are members (info.md §5, reference §3.5/§4.4)

`+ - * /` on a decimal are `Decimal (+)(Decimal)` etc. — members with symbolic selectors,
returning the decimal's own type (the value that lands in the LHS). Comparisons return `bool`;
`(==)` must return `bool` and `(!=)` **derives automatically** as `!(==)` (reference §3.5,
`docs/reference.md:396`). Ordering (`< <= > >=`) is likewise operator methods (the string type
already defines lexicographic `<` etc.; enum defines `<` by carrier). Live prelude precedents:
`src/Resolver.cpp:2384` (`OpenMode (|)`), `src/Resolver.cpp:5774` (enum `(==)`). So decimal's
whole operator surface is ordinary member declarations — **no new operator machinery**.

Resolution is by right-operand type (reference §3.5), so `decimal + decimal`, `decimal + int`,
and `decimal + float` are **distinct overloads** the type author chooses to provide or omit.

### 8.2 Mixed-type policy — must be explicit (acceptance gate)

The request asks "how mixed `decimal`/`int`/`float` arithmetic is disallowed or explicitly
converted." Options the design must pick between and state:

- **Disallow entirely** (no `decimal (+)(float)` overload → `d + f` is a compile error;
  user must convert): safest, most honest, matches "no implicit promotion." `decimal + int` is
  the one mixed form worth allowing (int is exact and widens losslessly into a decimal
  coefficient).
- **Allow `decimal ⊕ int`** (int → decimal is lossless), **disallow `decimal ⊕ float`**
  (float → decimal is a lossy binary→decimal conversion and would smuggle imprecision back in —
  the exact thing decimal exists to prevent). This is the principled middle and the recommended
  default to evaluate.
- Explicit conversions both ways as **methods**, never coercions: `d.toFloat()`,
  `d.toInt()` (truncate/round — loud on overflow, like `float.toInt()`), `Decimal.ofInt(n)`,
  and a deliberately-named lossy `Decimal.ofFloat(f)` (or refuse float→decimal entirely and
  force `Decimal::fromString`). Mirror `float.toInt()`'s loud out-of-range throw
  (`docs/reference.md:809`).

### 8.3 Rounding, overflow, division (the semantics the request flags as "genuinely large")

- **Rounding mode:** at minimum a default (banker's / half-even is the money standard, or
  half-away-from-zero to match `float.round()`), ideally selectable for `/` and for scale
  reduction. A `RoundingMode` **enum** is the obvious fit (enum is landed, full-coverage).
- **Overflow:** loud (`RuntimeException`) is the language default (reference §3.7); int's
  silent two's-complement wrap (`int.pow`) is the *exception*, not the model to copy for money.
- **Division:** the hard case. Fixed-point/`C#`-128 division needs a result scale + rounding;
  arbitrary-precision division needs a precision context or it can be non-terminating
  (`1/3`). Whatever representation is chosen, **`/` is where the rounding-mode/precision
  parameter becomes mandatory** — the design cannot hand-wave it.
- **Divide-by-zero:** must be **loud** for decimal (unlike int's interim silent-0,
  `runtime/lv_abi.h:322`) — money divide-by-zero is a bug, not a value.

---

## 9. The object-mask method surface & factories

Following `int`/`float`/`DateTime`, a `decimal` (primitive or struct) should carry:

- **Rendering:** `toString()` — exact, clean, no trailing-zero/`%f` ugliness (the anti-pattern
  §3.2 flags). Consider `toString(int places)` for fixed display scale.
- **Parsing:** `Decimal::fromString(string) -> decimal?` (optional-returning, total — data
  errors are values, not throws, matching `string.toInt()/toFloat()` at
  `docs/reference.md:840` and the JSON/encoding "decoders are total" rule §6.13). This is the
  `parse()` half of acceptance criterion #2's round-trip.
- **Factories** (no `static` keyword exists): labeled constructors `Decimal::ofInt(int)`,
  `Decimal::ofScaled(int coeff, int scale)`, and possibly `Decimal::zero`/`Decimal::one`
  const globals — the `DateTime::now()`/`Duration::ofHours()` precedent (reference §6.12).
- **Queries/ops in-language over a minimal core:** `abs()`, `sign()`, `floor()`/`ceil()`/
  `round(RoundingMode)`, `scale()`, `min`/`max` — the pattern of `int`/`float` (a small native
  core, the rest in-language, info.md §9/§11).
- **Default value** (bare declaration auto-constructs, info.md §3): `decimal d;` must yield
  **zero** (coefficient 0). For a struct this is field-default; for an immediate tag it is
  payload 0.
- **String interpolation** `"${d}"` calls `.toString()` (reference §3.2, `docs/reference.md:262`)
  — free once `toString()` exists.

---

## 10. Cross-feature interactions (each is a checklist item for the design)

- **Unions / `None` / narrowing** (info.md §9, reference §2.3): `decimal?` must work —
  `decimal | None`, narrowed by `!= None`/`is`, `??`, `?.`. For a *struct* decimal this is
  automatic (structs already union/narrow — char's problem #8, `techdesign-03-core-types.md:324`,
  is the checklist). For a *new tag*, add union-tag/narrow/`match`-type-pattern cases explicitly.
- **`match`** (reference §3.15): value patterns on decimal (`match (d) { 0 => …; else => … }`)
  need the literal-in-pattern to target-type to decimal the same way arm heads do for enum/int.
- **`Map`/`Set` keys** (info.md §11 "Key equality C3", reference §6.4.5/§6.4.7): a decimal key
  compares **by value**. For a struct, key equality is field-wise recursive (a struct IS its
  fields) — so `{coeff, scale}` structs with *different scales but equal value* (`1.0` vs `1.00`)
  would compare **unequal** field-wise unless the design **normalizes** or defines `(==)` to
  compare by numeric value. This is a real trap: decide whether `1.0 == 1.00` (yes, numerically)
  and make key-equality consistent with `(==)`.
- **`const`/`readonly` fields** (reference §4.3b/§4.3c, `designs/complete/const.md`): a
  `const decimal` **field** initializer must be a *compile-time constant* (info.md §1 slot
  axis). A decimal literal is a natural compile-time constant, but the const-folding path
  (comptime oracle, info.md §16.5) must be able to build a decimal value at fold time — verify
  the comptime evaluator can hold one (for a struct it can; for a tag it needs the fold path).
- **Columnar / dense arrays** (`request-columnar-dense-array-struct.md`, accepted; info.md §9):
  a *fixed-size* decimal `struct` is dense-array-eligible (flat inline records, tag-5 payload
  pointing into the buffer — `runtime/lv_abi.h:116-120`), which is the whole point of the data
  thrust. An *arbitrary-precision* decimal is **not** dense-storable (variable content → heap
  handle per element). This is a concrete reason the data direction favors a fixed-size rep.
- **JSON** (reference §6.11): `json` numbers are IEEE doubles today; a decimal-aware JSON path
  (parse a number as decimal, render a decimal exactly) is a natural but **separate** follow-on,
  not in scope — but the design should note that decimal's clean `toString()` is exactly what
  the deferred JSON/float number-rendering cleanup (info.md §19 #16) will want.
- **Threads / copy boundaries** (info.md §14, reference §6.6.66): every value crossing a thread
  boundary is deep-copied. A struct/immediate decimal copies trivially; a heap-tag decimal needs
  its flatten/rebuild case in the copy engine (like Block/string). No issue for the struct path.
- **`await`/streams/`Promise<decimal>`**: no special interaction — decimal is an ordinary value
  type; a promise of one just works.
- **Atlantis / money use case** (the motivating consumer, info.md §"Atlantis"): prices, ledgers,
  invoice lines. Confirms the *fixed-size, exact, clean-rendering* profile (C#-128 or
  fixed-scale) over arbitrary precision for the primary consumer.

---

## 11. Engine coverage, differential testing, corpus

- **Bar:** oracle + IR + emit-C++ + LLVM, byte-identical via differential testing against the
  shared corpus (info.md §17, reference §7.1). **No ELF lane** (`[[feedback_x64gen-frozen]]`).
- **Corpus:** a new `tests/corpus/decimal/decimal.lev` (+`.expected`) is the deliverable, run on
  all four active lanes plus the `_llvm` lane, exactly as `chars/`, `blocks/`, `enums/` did
  (`techdesign-03-core-types.md` §6, `abi-addendum-lv-block.md` §9). Must cover: literals
  (incl. the `0.1`-is-exact proof — the canonical `0.1 + 0.2 == 0.3` test that fails for float),
  each operator, rounding modes, overflow/divide-by-zero throws, `toString`/`fromString`
  round-trip, `decimal?`/union/`match`, `Map`/`Set` keys, and `1.0 == 1.00` equality.
- **If a struct rep:** likely **zero new ABI/runtime work** → the LLVM lane is free (like enum).
  **If a new tag:** the tag + ARC (if heap) + `LlvmGen` construction land in **one atomic
  STOP-gated pass** (`abi-addendum-lv-block.md` §1/§8), never partially.

---

## 12. Open questions the design must resolve (the checklist)

1. **Representation** (the gate — acceptance #1): fixed-scale int64 · per-value-scale
   fixed-point struct · C#-128 struct · IEEE decimal64/128 · arbitrary-precision. Recommended to
   evaluate first: **C#-style 128-bit as a value `struct`** (money-grade, fixed-size, zero ABI,
   full coverage) or **per-value-scale fixed-point struct**.
2. **Packaging:** prelude value `struct` (enum/DateTime path, zero ABI, recommended spine) vs.
   dedicated ABI tag (char/Block path, faster, STOP-gated). Can stage: struct first, tag later.
3. **Literals:** target-typing only (char-style) vs. suffix (`9.99m`) vs. both; and **the
   parse-from-source-text-not-double rule** (non-negotiable for correctness). Call-arg-position
   target-typing in v1 or deferred (char deferred it).
4. **Mixed arithmetic:** disallow all mixing · allow `decimal⊕int` only · explicit conversion
   methods. And the bare-literal-overload precedence rule (char picked back-compat).
5. **Rounding & division:** default rounding mode, whether it is selectable, the division
   result-scale/precision-context story, overflow = loud, divide-by-zero = loud.
6. **Equality vs. scale:** does `1.0 == 1.00`? Make `(==)`, `Map`/`Set` key equality, and
   `toString` mutually consistent (normalize, or compare by numeric value).
7. **Conversions:** `toFloat()`/`toInt()` (loud on range), `ofInt()`, whether `ofFloat()` exists
   at all or float→decimal is refused as inherently lossy.
8. **Precision bound:** for fixed-size reps, the max significant digits / scale range, and what
   happens at the boundary (throw vs. saturate vs. round).
9. **Staging:** is v1 the full surface, or a minimal `+ - * /`, compare, `toString`/`fromString`
   (acceptance #2's floor) with rounding-mode selection and richer methods as a follow-on?

---

## 13. Acceptance criteria (from the ticket) + doc duties

**From `request-decimal-type.md`:**
1. Representation decision recorded **before** implementation.
2. If implemented: `+ - * /`, comparison, `toString()`/`parse()` round-trip, correct to the
   chosen precision, **on every active engine**.
3. `info.md` §2/§9 gains a `decimal` entry (or an explicit "still deferred, here's why" note).

**Documentation duties the design inherits** (the Track-03 pattern,
`techdesign-03-core-types.md` §7): `docs/reference.md` §2.2 (primitive) *or* §4.2b (if a
struct) + §6.1 (methods table) + §1.4 (if a literal form is added); `info.md` §9 (numeric
story) and §2 (the acceptance-#3 entry); and — since this is a design *for* a ticket — on
completion move the request to `designs/requests/accepted/` (already done by this task) and the
finished design to `designs/complete/` (`[[design-workflow-file-moves]]`). If any sub-feature is
punted, file a deferral in `designs/` (the `deferal-track03-type-surface.md` shape) rather than
leaving a silent gap.

---

## 14. Implementation surface map (files & anchors — the layer tour)

If a **struct** rep (recommended to evaluate first — mirrors enum/DateTime, mostly front-end):

| layer | file(s) | work |
|---|---|---|
| prelude | `src/Resolver.cpp` (prelude string segments `kPreludeCore/Std/…`) | declare `struct Decimal { … }` with operator methods, labeled ctors, `toString`/`fromString`, in-language arithmetic (DateTime/Duration are the model) |
| literal typing | `src/Checker.cpp:604-635` region (char `isCharLiteral`/`markCharLiteral`/`expectsChar`) + `:683` (FloatLit typing) | add `decimal`-expected target-typing that **parses the literal source span**, not the double |
| lexer (only if a suffix) | `src/Lexer.cpp:63` `lexNumber` + `src/Token.hpp` | optional `m`/`d` suffix token form |
| AST flag | `src/Ast.hpp` (`:173`/`:179` charLit/singleQuoted precedent) | a `decimalLit` flag if using target-typing |
| engines | none new for a pure struct rep (structs already run on all four engines) | differential corpus is the proof |
| corpus | `tests/corpus/decimal/decimal.lev` + `.expected` + ctest lanes incl. `_llvm` | per §11 |

If a **new ABI tag** rep (char/Block path — only if the fast path is wanted in v1):

| layer | file(s) | work |
|---|---|---|
| value model | `src/RuntimeValue.hpp` (`VKind`), `src/Eval.cpp`, `src/IrInterp.cpp` | new `VKind::Decimal` + plumbing |
| emit-C++ | `src/CGen.cpp` | mirror the value model in the embedded runtime |
| ABI (STOP-gated, atomic) | `runtime/lv_abi.h` (tag enum `:39-49`), `runtime/lv_runtime.c` (ARC `lv_is_counted`/`lv_recursive_free`, `to_string` rows if heap) | a `LV_DEC` tag — immediate if ≤64-bit, heap+ARC if wider (Block is the heap template) |
| LLVM | `src/LlvmGen.cpp` | construct/dispatch (char retag-inline `abi-addendum-lv-block.md` §7 for immediate; Block ARC scaffolding for heap) |
| natives | `src/RuntimeNatives.cpp` + `runtime/lv_runtime.c` | arithmetic/format native cores |

**ELF (`src/X64Gen.cpp`): no lane, ever. Never gate the design on it.**

---

## 15. Foreseeable problems & risk register

| # | risk | mitigation / note |
|---|---|---|
| 1 | **Literal precision leak** — a decimal literal parsed via `double` silently loses exactness (defeats the whole type). | Parse from the literal **source span** (coefficient + `.` position), never `parseFloatLiteral`. This is the #1 correctness gate; make it a corpus test (`0.1+0.2==0.3`). |
| 2 | **Reopening integer-promotion special cases** via mixed arithmetic. | State mixed-type rules up front (§8.2); prefer disallow-or-explicit-convert. The narrow-int request flags this as the trap to avoid. |
| 3 | **`1.0` vs `1.00` equality** inconsistent across `(==)`, `Map`/`Set` keys, `toString`. | Decide numeric-value equality; normalize or define `(==)` by value and make key-equality match (info.md §11 C3). |
| 4 | **ABI-tag STOP hazard** — a half-added tag to the closed set is worse than none. | If a tag is chosen, land it atomically per `abi-addendum-lv-block.md` §1②/§8. Prefer the struct rep to sidestep entirely. |
| 5 | **Division non-termination / rounding under-specified.** | Mandatory rounding-mode/precision parameter on `/` and scale reduction; loud divide-by-zero. |
| 6 | **Scale-as-type-parameter (`decimal<2>`)** temptation — primitive-with-generics collides with §6.6 "singletons/primitives hold no T-typed state." | Dismiss explicitly, or the designer re-derives and re-rejects it. |
| 7 | **Arbitrary-precision ≠ dense-storable** — kills the columnar/data-thrust use. | If the data thrust matters, favor a fixed-size rep (C#-128 / fixed-scale). |
| 8 | **Taking a C decimal-FP dependency** (for IEEE decimal64/128) contradicts dependency-minimalism unless behind a sanctioned seam. | If IEEE conformance is wanted, follow the libm/OpenSSL seam precedent (LA-2) — an owner-level call, not a quiet import. |
| 9 | **Float-formatting wart bleed-through** — reusing `float.toString()`'s `%f` path. | Decimal `toString()` is independent and must be exact/clean from day one (§3.2); do not block on the deferred global float-formatting work. |
| 10 | **Bare-literal overload ambiguity** (`f(decimal)` vs `f(float)`/`f(int)`). | State a precedence rule + checker test, mirroring char's "back-compat/string wins" ruling. |

---

## 16. The one-line recommendation for the design's spine (not a decision — a starting point)

Evaluate **decimal as a prelude value `struct`** holding a **fixed-size numeric model**
(C#-style 128-bit, or per-value-scale fixed-point), with **arithmetic written in the language**
via operator methods, **literals via char-style target-typing that parses the source text
directly**, a **loud** overflow/divide-by-zero posture, an explicit **`decimal⊕int`-only**
mixed rule, a **`RoundingMode` enum**, and a **dedicated ABI fast-path tag explicitly deferred**
as a later optimization. This is the lowest-risk path that satisfies "correct on every engine"
(acceptance #2) on day one, exactly as `enum` and `DateTime`/`Duration` did — while leaving the
door open to the immediate-tag performance path once the semantics are proven. Whether the
owner instead wants full IEEE-754 fidelity or arbitrary precision is the one question above this
research that only the owner can answer, and it is the true content of acceptance criterion #1.

---

*Pre-design research only. Every `file:line` was verified against the working tree at the time
of writing; `info.md`/`reference.md` section numbers are cited for design intent. The tech
design that consumes this doc owns the actual rulings.*
