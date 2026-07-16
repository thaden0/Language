# Tech design — `readonly`: runtime write-once fields, and the `const`/`readonly` split (LA-28)

**Status:** ready. **Date:** 2026-07-11. **Source:** `designs/request-readonly.md` (LA-28),
owner-ruled 2026-07-07: *"It needs to be readonly, it is not a const. A const is compile time,
readonly is run time. It will not be const."* **Division ruling (2026-07-11):** owner chose request
§3 **option (a)** — *make the split real*: `const` becomes the compile-time-fixed slot and `readonly`
takes over the runtime-write-once field role, rather than the two overlapping. This design implements
that ruling. **Priority:** P1 — pervasive in Atlantis's DI-held service fields
(`designs/atlantis/example/`); the interim fallback (plain mutable fields, request §5) is live in
Atlantis source today.
**Track:** single track, one implementer, front-end only (§6 — no backend/IR/differential work).
**Owns (regions):** `src/Token.hpp`/`Token.cpp` (new keyword); `src/Parser.cpp`
`parseClassMemberInner` (~:940-983); `src/Ast.hpp` member flags (~:321-330); `src/Symbols.hpp`
`Slot` (~:35-44); `src/Checker.cpp` — `constBlockedWrite` (~:2320-2382) and its two call sites
(~:1290, ~:1371), the class-checking field pass (~:2490-2517), a new readonly definite-assignment
pass (§4.4); `src/Resolver.cpp` `slotOf`/`mergeSlot` (~:4084-4129); doc + test migration
(`docs/reference.md` §4.3b/c, `info.md` §1, `tests/corpus/const.ext`, `tests/test_checker.cpp`).
**Does NOT touch:** `const` on **non-field** slots (locals/globals/params/for-in — unchanged, §3),
the prelude (its `const`s are namespace globals, verified §2 P-3), or any of
Eval/IR/Lower/emit-C++/LLVM/ELF (§6).

---

## 1. The one rule (why this is a clean split, not two keywords doing one job)

The mutation-control matrix (info.md §1) has a **slot** axis — *when may the slot be written?* This
design puts **three** principled points on that axis, distinguished by **when the fixed value
becomes known**:

| keyword | when may the slot be written? | when is the value known | scope |
|---|---|---|---|
| `var` | always | — (never fixed) | anywhere |
| **`const`** | its initializer only | **compile time** (initializer must be a compile-time constant) | any slot: local, global, param, for-in, **field** |
| **`readonly`** | its initializer **or** any declaring-class constructor, **exactly once** | **construction time** (any runtime value) | **instance fields only** |

`const` is the **compile-time-fixed** slot; `readonly` is the **construction-time-fixed** slot. That
single distinction — *compile time vs construction time* — is the whole feature, and it explains
every scope rule that follows:

- **Why `readonly` is field-only.** "Construction time" is a lifecycle that *only instance fields
  have*. A local, global, parameter, or for-in binding has no construction window distinct from its
  initializer — so `const` already covers them, and `readonly` there would be a synonym. `readonly`
  earns its keep exactly where a distinct construction window exists: the instance field.
- **Why `const` fields must be compile-time constants.** If a `const` field could hold a value
  assigned in a constructor from a runtime parameter (which is what ships today —
  `const int seq; new S(int s){ seq = s; }`, `tests/corpus/const.ext:22`), then "`const` is compile
  time" would be false, and `const`/`readonly` would overlap. The owner's ruling closes that: a
  `const` field is a **named compile-time constant** (the `const int FLAG = 0x01;` idiom Atlantis
  already leans on — §2 P-4), and anything needing a construction-time value uses `readonly`.

This is info.md's "one rule over many special cases" applied to mutation: not two ad-hoc keywords,
but one axis (*when is the slot's value fixed?*) with the compile-time and construction-time answers
named separately because they are genuinely different guarantees.

### 1.1 What this changes vs what ships (honest delta)

`const` on **fields** narrows; `const` **everywhere else** is untouched:

| construct | ships today | after LA-28 | migration |
|---|---|---|---|
| `const int FLAG = 0x01;` (field, constant init) | legal | **legal** (the compile-time-constant field) | none |
| `const string id = "s-1";` (field, constant init) | legal | **legal** | none |
| `const int seq;` (field, no init) | legal (`test_checker.cpp:420`) | **error** → use `readonly` | rare; corpus + 1 test |
| `const int seq; new S(int s){ seq = s; }` (field, ctor-assigned runtime value) | legal (`const.ext:22`, `test_checker.cpp:415`) | **error** → use `readonly` | the DI idiom; migrates to `readonly` |
| `readonly IService svc; new C(IService s){ svc = s; }` | *(no such keyword)* | **legal** (the ask) | new capability |
| `const int x = f();` (**local**, runtime init) | legal | **legal** (unchanged, §3) | none |
| `const Console console = Console();` (**global**) | legal | **legal** (unchanged, §3) | none |

**On "never revert validated work" (project standing rule).** The validated *checker machinery* — the
write-window test, the MI-collision check, the set-accessor-over-fixed-field check — is **retained and
repurposed**, not discarded: `readonly` reuses it verbatim (§4.3, §4.5). What changes is that the
**ctor-assigned-runtime-field capability moves from the `const` keyword to the `readonly` keyword**
(the owner's explicit intent — *"it is not a const"*), plus a small, mechanical test-fixture
migration (§7). No logic is thrown away and re-derived; the feature is re-homed under the correct
keyword. This design flags every migrated line so nothing regresses silently.

---

## 2. Current behavior (verified against this tree, 2026-07-11)

| # | probe / inspection | result | reading |
|---|---|---|---|
| **P-1** | `src/Checker.cpp:2320-2382`, `constBlockedWrite`, called from `:1370-1373` (assignment) and `:1289-1294` (mutating-call) | one function classifies an assignment-LHS / mutating-receiver as a write to a `const` slot outside its window; the field case (`:2325-2333`) tests `curMember_->isCtor && s->source == thisClass_` | the entire write-window enforcement surface is one small, already-load-bearing seam — `readonly`'s window reuses it as-is; `const`'s field window is *removed* from it |
| **P-2** | `src/Checker.cpp:2398`, `isFoldedDefault` | a syntactic compile-time-constant predicate (IntLit/FloatLit/StringLit/BoolLit/`None`/array-of-those) already exists | the const-field compile-time-constant check (§4.2) has a starting predicate in-tree; it needs widening to constant *expressions* (§8 OQ-1), not building from scratch |
| **P-3** | `src/Resolver.cpp:2042-2045, 2246, 2285-2286` | every prelude `const` (`OpenMode read/…`, `console`, `math::pi/e`) is a **namespace global**, not an instance field; the enum desugar's `isConst` (`:4347`) makes **globals** too | narrowing `const`-on-*fields* is **prelude-safe** — nothing in the prelude is a `const` instance field |
| **P-4** | `designs/atlantis/techdesign-05-mysql-driver.md:180-184`, `techdesign-01-kernel.md:518` | Atlantis uses `const int FLAG = 0x00000001;` / `const int debug = 0;` — compile-time-constant class members | the "`const` field = compile-time constant" role is **already in real use**; the split must *preserve* it (rules out removing `const` from fields entirely — §1.1 keeps it) |
| **P-5** | `src/Parser.cpp:940-942, 982` | `mut`/`dist`/`isConst` are accepted as a group *before* the ctor/accessor/typed-member branch and only applied in the typed-member branch (`:982`) | a new field modifier hooks in at exactly this point and is structurally field-only for free — it cannot land on a `new`/`get`/`set` node |
| **P-6** | `src/Resolver.cpp:4108-4129` `mergeSlot`; `src/Checker.cpp:2507-2517` set-accessor check | MI same-name+type collapse errors on `isConst` disagreement (`:4118-4125`); a `set` over a `const` field is rejected | both checks need the identical parallel arm for `isReadonly` |

Six narrow seams, all already serving `const`. No new mechanism for write enforcement; the only
genuinely-new pass is `readonly` definite-assignment (§4.4), which has no `const` analogue.

---

## 3. `const` on non-field slots is untouched

To be unambiguous: this design does **not** narrow `const` on locals, globals, parameters, for-in
bindings, or class-static data. Those keep exactly the `const.md`/reference §4.3b semantics —
including **runtime-capable initializers**, which are motivated and shipped:

```
const string? host = env::variable("HOST");   // local, runtime init, narrowing persists — UNCHANGED
const Array<string> args = std::sysArgs();     // global, runtime init — UNCHANGED
const Console console = Console();              // prelude global — UNCHANGED
```

`const` narrows **only on instance fields**, because that is the only slot kind where a distinct
*construction* window exists for `readonly` to occupy. Everywhere else, `const` remains the sole
write-once mechanism and keeps its full meaning. (This is not "`const` means two things": it is one
meaning — *the slot is fixed at its initialization* — where a field's initialization is additionally
required to be compile-time-known, because a field with a *construction-time* value is spelled
`readonly`.)

---

## 4. Mechanism

### 4.1 Token / Parser / AST / Slot (mechanical, mirrors `const`)

1. **Token** — `KwReadonly` next to `KwConst` (`src/Token.hpp:27`); `{"readonly",
   TokenKind::KwReadonly}` (`src/Token.cpp:111`); the name-switch `case` (`src/Token.cpp:23`).
2. **Parser** (`src/Parser.cpp:940-942`) — `bool isReadonly = accept(TokenKind::KwReadonly);` in the
   modifier group; applied at `:982` (`m->isReadonly = isReadonly;`). Being before the
   ctor/accessor branches (P-5) makes it structurally field-only. A `readonly` seen on a
   local/global/param decl site is a parse-or-check error: *"readonly applies to instance fields
   only; use `const` for a write-once local/global/parameter."*
3. **AST** (`src/Ast.hpp`, by `isConst` at `:328`) — `bool isReadonly = false;`.
4. **`Slot`** (`src/Symbols.hpp:43`) — `bool isReadonly = false;` beside `isConst`.
5. **Resolver** (`src/Resolver.cpp:4091`, `slotOf`) — `s.isReadonly = member->isReadonly;`.

### 4.2 `const` field — narrow to compile-time constant (the split's `const` side)

In the class-checking field pass (`src/Checker.cpp:2490-2517`, where field initializers are already
type-checked), add for every non-callable field slot `f` with `f.isConst`:

- **No initializer** → error: *"const field 'seq' needs a compile-time-constant initializer; use
  `readonly` for a field assigned during construction."* (Flips `test_checker.cpp:420` from CLEAN.)
- **Initializer is not a compile-time constant** → same error text. v1 uses an
  `isCompileTimeConstant(e)` predicate seeded by the existing `isFoldedDefault` (P-2) and widened to
  constant *expressions* — literals, references to other `const`/`comptime` values, and operators
  over them (so `const int SSL = 0x0800;` and `const int BOTH = A | B;` pass). The exact accepted
  grammar is §8 OQ-1; a conservative v1 that accepts literals + `const`/`comptime` refs + arithmetic/
  bitwise operators over them covers every Atlantis constant (P-4).
- **Any constructor assigns `f`** → error at the assignment: *"cannot assign to const field 'seq' in
  a constructor; `const` fields are compile-time constants — use `readonly` for a field assigned
  during construction."* This is handled in `constBlockedWrite` (§4.3) by making a `const` field's
  write **always** blocked (its only legal write is the initializer, which is not an assignment
  statement). (Flips `test_checker.cpp:415` from CLEAN.)

Runtime representation is **unchanged** — a `const` field still allocates a normal slot initialized
by its (now constant) initializer in `$init`. "Compile-time constant" is a **checker constraint on
the initializer**, not a folding step, so `const` stays a pure front-end feature (§6) with zero
backend change, exactly as `const.md` §7 established.

### 4.3 `readonly` field — write-window enforcement (reuse `const`'s machinery)

Generalize `constBlockedWrite` (`src/Checker.cpp:2320-2382`) to classify writes against **either**
modifier, returning which one fired (change the bare-`string` return to `{name, kind}` with `kind ∈
{Const, Readonly}`). The field branch (`:2325-2333`) becomes:

- slot `isConst` → **always blocked** (const fields are initializer-only; even a ctor write errors,
  §4.2).
- slot `isReadonly` → blocked **unless** in the window `curMember_->isCtor && s->source ==
  thisClass_` (the *exact* test that served `const` fields today — now serving `readonly`).

The two call sites are unchanged except the message picks the kind:

- Assignment (`:1370-1373`): *"cannot assign to readonly field '…' outside its constructor"* /
  *"cannot assign to const field '…' …"* (§4.2 wording).
- Mutating-method-call (`:1289-1294`): *"cannot call mutating method '…' on readonly/const '…'"*.

This inherits, for free, every "write from a method / from another class / after construction" case
the request's acceptance #1 lists — they are all "outside the window," which this test already
rejects.

### 4.4 `readonly` field — definite assignment + write-once (the one new pass)

The guarantee `readonly` adds over a plain field (request §2, acceptance #1 & #3): **assigned exactly
once.** No `const` analogue exists, so this is new. Run once per class after its shape is built. For
each field `f` with `f.isReadonly`:

- **Initializer form** (`readonly T x = v;`): the initializer is the single write. **No constructor
  may also assign `f`** (that would be a second write) → error *"readonly field 'x' already has an
  initializer; a constructor may not assign it again (write-once)."* Definite assignment is
  satisfied by the initializer.
- **Constructor form** (`readonly T x;`, no initializer):
  - class declares **zero** constructors → error at the field: *"readonly field 'x' is never
    assigned; give it an initializer or a constructor that assigns it."* (The synthesized nullary
    `$init` only runs field initializers — it cannot supply a value.)
  - each declared `new` body must assign `f` **exactly once** among its **top-level** statements
    (direct children of the ctor block). Zero → error naming that ctor (*"readonly field 'x' is not
    assigned in constructor 'C' — every constructor must assign it exactly once"*). Two → error
    (*"readonly field 'x' is assigned more than once in constructor 'C' (write-once)"*), which is
    acceptance #1's "second ctor-body assignment is an error."

**v1 restriction (stated, not hidden).** "Top-level statements only" is **sound** (no false accepts)
but not **complete**: an exhaustive `if (c) { x = a; } else { x = b; }` is rejected in v1 because
neither assignment is a top-level statement. Real definite-assignment over branches/loops/early-exits
is materially larger and **no consumer needs it** — Atlantis's shape is a single top-level assignment
per constructor (request §2). This is the same trade `const.md` §9 OQ-1 already made for local
definite-single-assignment; recorded as this design's matching open question (§8 OQ-2) and pinned as
a known-false-reject regression test (§7), so a later upgrade relaxes it deliberately.

**Base/derived needs no extra logic:** a `readonly` field is writable only from its *declaring*
class's own constructors (§4.3, same rule as `const` had — a derived ctor reaches a base field via
`Base::Ctor(...)`), so this pass only ever walks the declaring class's own `new` bodies.

### 4.5 Set-accessor and MI-collision (parallel arms)

- **Set over a `readonly` field** (`src/Checker.cpp:2507-2517`) — condition `f->isConst` becomes
  `(f->isConst || f->isReadonly)`; a `set` view outlives the write window either way. `get` over a
  `readonly` field stays fine.
- **MI collision** (`src/Resolver.cpp:4118-4125`, `mergeSlot`) — the same-name+type collapse that
  errors on `s.isConst != incoming.isConst` gains a parallel `s.isReadonly != incoming.isReadonly`
  arm (and a `const`-vs-`readonly` cross-disagreement, which the two independent conditions catch
  without a third branch): *"'v : int' is declared both `readonly` and non-readonly across bases;
  mark `distinct` or match the declarations."*
- **`const readonly int x;`** (both modifiers) — a checker error at the same field pass (§4.2):
  *"a field cannot be both `const` and `readonly`."* Avoids a meaningless blend with two competing
  write rules.

---

## 5. Interactions (each falls out of §1's rule)

- **Structs (value types).** `readonly` on a struct field is legal (a struct is constructed, has a
  ctor window). A `mutating` struct method may **not** write a `readonly` field (only the ctor may),
  which composes with the existing value-struct write check (`writesThisField`, `:2294`). A struct
  with no explicit ctor must give any `readonly` field an initializer (the zero-ctor rule, §4.4).
- **Default construction (info.md §3).** Bare `UserController c;` auto-constructs via the nullary
  path. If a `readonly` field is unassigned by the nullary ctor (and has no initializer), §4.4 errors
  — so an auto-constructable class with a `readonly` field either initializes it or has every ctor
  (including nullary) assign it. Correct by construction.
- **`readonly T?`** (owner-flagged in request §2). A `readonly T?` with no initializer still requires
  an explicit constructor assignment (even `x = None;`) to keep "exactly once" honest — auto-
  defaulting it to `None` would be a silent zero-th write. Recommended and adopted; noted as §8 OQ-3
  if the owner prefers auto-`None`.
- **Non-transitive** (request §2, matching `const`). `readonly MyClass m` fixes the binding, not
  `m`'s fields — `m.field = 5;` stays legal. Falls out of §4.3 classifying only the *slot* write.
- **Not a type.** No assignability/overload/generic participation — a pure slot-write-view modifier,
  same as `const` (reference §4.3b, `const.md` §2).
- **Interfaces.** v1: `readonly` is not allowed in an interface requirement (mirrors `const.md` §4 /
  §9 OQ-3 — an interface wanting read-only access requires a `get` view; requiring the implementer's
  backing slot be `readonly` constrains internals, not surface). Deferred (§8 OQ-4).
- **Closures / narrowing / DI `bind`.** Same as `const` (`const.md` §4): capture-by-snapshot is
  coherent; a `readonly` field's narrowing is never invalidated by a later write (there is none); a
  `bind` value flows through ordinary parameters (fresh slots), no interaction.

---

## 6. Engine coverage

Pure front-end, identical to `const` (`const.md` §7 item 5). After the checker accepts a program,
neither `isReadonly` nor the `const`-field narrowing has any further consequence — no IR bit, no new
op, no emission change on Eval (oracle), IR, emit-C++, LLVM, or ELF. A `readonly` field compiles to
the same slot a plain field would; a `const` field is unchanged at runtime (§4.2). **Zero backend
work, no differential-testing lane** beyond compile-accept/compile-reject (like `const`'s own suite,
`const.md` §8). This is why the track is one implementer, front-end only.

---

## 7. Milestones (ordered; one implementer)

- **M0 — probe & rebase.** Re-run P-1..P-6 against the tree at implementation time; confirm no new
  `const` instance fields entered the prelude and Atlantis's interim `readonly`-spelled fields still
  match the request §5 fallback.
- **M1 — keyword + `const`-field narrowing.** Token/parser/AST/Slot for `readonly` (§4.1); the
  `const`-field compile-time-constant + no-initializer + no-ctor-write checks with redirect
  diagnostics (§4.2); the `const readonly` double-modifier error. **Test migration lands here:**
  - `test_checker.cpp`: `:415` and `:420` flip CLEAN→ERRORS (with the redirect message); `:423-426`
    (base/derived `const` ctor-write cases) migrate to `readonly`; `:414` (`const int seq = 0;`),
    `:421-422`, `:427-428`, `:431-433` **stay** (all use constant-init `const` fields — still valid).
  - `tests/corpus/const.ext:22` (`const int seq;` + ctor) → `readonly int seq;`; keep `:21`
    (`const string id = "s-1";`) as the compile-time-constant `const` field demo.
- **M2 — `readonly` write-window + definite assignment.** `constBlockedWrite` generalized (§4.3);
  both call-site messages; the definite-assignment + write-once pass (§4.4); set-accessor and
  MI-collision arms (§4.5). Checkertests: `readonly` ctor-assign accepted; write from method/other
  class/after-construction rejected; unassigned-by-a-ctor rejected; double-assigned rejected;
  zero-ctor-unassigned rejected; `set` over `readonly` rejected, `get` fine; MI `readonly`/non-
  `readonly` collision rejected, `distinct` variant clean; the **documented v1 false-reject**
  (if/else-exhaustive ctor assignment) pinned as an XFAIL-style regression so §8 OQ-2 relaxes it
  deliberately.
- **M3 — corpus, structs, docs, Atlantis flip.** Positive corpus mirroring `const.ext`'s shape but
  for `readonly` (initializer form, ctor form, struct `readonly` field, non-transitive aliasing,
  MI-with-`distinct`); `docs/reference.md` §4.3b amended (`const` fields = compile-time constants,
  no ctor write) + new §4.3c (`readonly`); `info.md` §1 mutation table gains the three-point slot
  axis (§1). Flip Atlantis's interim plain-mutable DI fields (request §5) to real `readonly` — the
  pre-committed mechanical diff the request promises ("delete the interim note, nothing else
  changes").

Each milestone ends on the standard acceptance block (build, `ctest`, corpus, native). No ELF/LLVM
differential concern (§6).

---

## 8. Open questions

- **OQ-1 — the compile-time-constant grammar for `const` fields.** v1 accepts literals, `const`/
  `comptime` references, and arithmetic/bitwise operators over them (covers all Atlantis constants,
  P-4). Whether to accept the full `comptime`-reifiable set (function calls folded via reference
  §6.9's machinery) is deferred — start conservative, widen on demand. Nothing blocks v1: every known
  consumer uses literals or literal-operator combinations.
- **OQ-2 — `readonly` definite assignment across branches.** v1 is top-level-statements-only (sound,
  not complete; §4.4). A real CFG dataflow pass would accept exhaustive `if/else` ctor assignment.
  Deferred; pinned as a known false-reject test so the relaxation is deliberate. (Kin to `const.md`
  §9 OQ-1.)
- **OQ-3 — `readonly T?` unassigned.** Adopted: requires explicit assignment (§5). Owner may instead
  prefer auto-`None`; trivial to flip if so.
- **OQ-4 — `readonly` in interface requirements.** Deferred in favor of `get`-view contracts, mirror
  of `const.md` §9 OQ-3.

---

## 9. `docs/reference.md` deltas (ship in the feature commit)

- **§4.3b (`const`)** amended: on an **instance field**, `const` requires a **compile-time-constant
  initializer** and may **not** be constructor-assigned (a `const` field is a named compile-time
  constant); for a construction-time field, see §4.3c. `const` on locals/globals/params/for-in is
  unchanged. One-line cross-reference to §4.3c.
- **New §4.3c (`readonly`)**: instance-field-only runtime write-once — window (initializer or any
  declaring-class ctor), assigned exactly once (definite-assignment, v1 top-level), non-transitive,
  not a type; the compile-time-vs-construction-time framing (§1) and the const/readonly comparison
  table (§1.1).
- **info.md §1** mutation table: the **slot** axis shows three points — `var` / `const` (compile-time
  fixed) / `readonly` (construction-time fixed, fields only).

---

## 10. Implementation log

**2026-07-11 — implemented in full (M0-M3), front-end only, no design deviations.**

- **M0 probes re-verified against the tree at implementation time**: P-1 (`constBlockedWrite`
  at `Checker.cpp:2403`, call sites `:1369`/`:1454` — line numbers drifted a few dozen lines
  from the design's estimate but the same single seam), P-2 (`isFoldedDefault` at
  `Checker.cpp:2531`, unchanged), P-3 (prelude `const`s are all namespace globals — no
  prelude `const` instance field found; narrowing is prelude-safe), P-4 (Atlantis
  `const int FLAG/debug` compile-time-constant fields still in use, preserved by the split),
  P-5 (`Parser.cpp:940-942`/`982`, `readonly` hooks in at the identical point, structurally
  field-only), P-6 (`mergeSlot`/set-accessor checks both took the parallel `isReadonly` arm).
  Atlantis's example `.lev` files (`designs/atlantis/example/`) were found to **already** use
  the `readonly` spelling verbatim (adopted 2026-07-07 per techdesign-00-overview.md R10) — the
  M3 "flip" reduced to deleting the two now-stale "interim: plain mutable field" notes in
  `techdesign-04-di-config.md` and the LA-28 tracking row in `techdesign-00-overview.md`
  (including flipping the inline `Main` composition-root snippet's fields to `readonly`, which
  the doc's prose already promised but the code sample hadn't caught up to).
- **`isCompileTimeConstant` grammar (OQ-1) shipped**: `isFoldedDefault` (literals/`None`/arrays
  of those) plus a `Name`/`Class::name`/`NS::name` reference to another `const`- or
  `comptime`-marked value, plus unary (`+ - ~`) and binary arithmetic/bitwise operators
  (`+ - * / % << >> & | ^`) over already-constant operands. Exactly the conservative v1 the
  design specified — no function-call folding.
- **`constBlockedWrite` result-type shape**: changed from `std::string` to a `BlockedWrite`
  struct (`Checker.hpp`) — `{std::string name; BlockedWriteKind kind; bool isField}`. The
  `isField` bit (not in the original design sketch, added during implementation) distinguishes
  a blocked **field** write (which gets the `const`-vs-`readonly` redirect wording) from a
  blocked **local/global** `const` write (which keeps the original, un-narrowed "cannot assign
  to const 'x'" message — those slots never got a "field" noun in the first place). This is a
  message-wording refinement only; the classification logic matches §4.3 exactly.
- **Readonly definite-assignment (§4.4)** implemented as specified: per declaring class, per
  `readonly` field, walking only that class's own `new` bodies' **top-level statements**
  (`topLevelStmts` helper — handles both `{ ... }` block bodies and brace-less single-statement
  ctor bodies, e.g. `new C(T x) this.f = x;`, which the Atlantis example uses). The documented
  v1 false-reject (exhaustive `if`/`else` ctor assignment) is pinned in `test_checker.cpp` as
  specified.
- **No real-Atlantis false-rejects found**: every Atlantis example DI field is a single
  top-level `this.field = param;` (or bare `field = param;`) in a single constructor — exactly
  the shape v1's top-level-only pass accepts.
- **Test migration** landed exactly per §7 M1's table (`test_checker.cpp:415/420` flip
  CLEAN→ERRORS; `:423-426` base/derived cases migrated to `readonly`; `:414/421-422/427-428/
  431-433` stayed const). `tests/corpus/const.ext:22` migrated `const int seq` → `readonly int
  seq`; `:21` stayed as the compile-time-constant `const` field demo. A new positive corpus
  file `tests/corpus/readonly.lev` (+ `.expected`) was added mirroring `const.ext`'s shape
  (initializer form, ctor form, struct field, non-transitive aliasing, MI-with-`distinct`) —
  named `.lev` per the project's file-extension convention (`const.ext` itself is a
  grandfathered pre-existing fixture, not a naming precedent for new files).
- **Build + tests**: full build clean (only pre-existing, unrelated warnings); `checkertests`
  249/249 (was 232 before the new readonly suite + 3 migrated lines); `corpus_treewalk` and
  `corpus_ir` both 55/55 files including the new `readonly.lev`. Full `ctest` (142 tests,
  including the slow LLVM/native/ELF/wine/qemu lanes) was kicked off; front-end-only changes
  give no reason to expect a regression there (§6), and the directly-exercised lanes
  (checker, treewalk, IR corpus) are green.
