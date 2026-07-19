# Value-type tuples: `Triple<A, B, C>`, and flipping `Pair` to a `struct`

**Status:** ready. **Date:** 2026-07-19.
**Depends on:** nothing unlanded. Rides on already-shipped infrastructure — the
generic-class/struct machinery behind `Pair`, ordinary operator-overload and
`toString()` method dispatch (all four active engines), Track 05's `keyEquals`
struct-recursive `Map`-key rule, and the landed dense/columnar `Array<struct>` layout
(`designs/complete/techdesign-columnar-arrays.md`). No new natives, no new IR ops, no
backend edits.
**Source (the ask):** `designs/requests/accepted/request-tuple-triple.md`.
**Source (the evidence):** `designs/complete/research-triple.md` — the precedent survey
and the class/struct option analysis. Two follow-up investigations extended it and are
folded in here with fresh citations: (1) generic value-struct columnar eligibility, and
(2) the blast radius of flipping `Pair` from class to struct. Where this design's line
numbers differ from the research doc's, the source drifted since it was written and the
numbers here were re-verified against current `src/`.
**Owns (regions):**
- kPrelude `Pair<A, B>` — **flips `class` → `struct`**, gains a hand-written `(==)` and
  `toString()` (`src/Resolver.cpp:397-401`).
- kPrelude `Triple<A, B, C>` — new value `struct`, inserted immediately after `Pair`.
- `docs/reference.md` §6.4 neighborhood.
- New/extended corpus assertions and a new green churn/density regression file (§8).

**Does NOT own / touches nothing in:** `RuntimeNatives.cpp`, `CGen.cpp`, `LlvmGen.cpp`,
`IrInterp.cpp`, `Eval.cpp`, `Checker.cpp`, `runtime/lv_runtime.c`, `runtime/lv_abi.h`,
`X64Gen.cpp`, and the `synthesizeStructEquality` pass. Every one was checked (§4, §6) and
is deliberately left untouched. The columnar *monomorphization* work discussed in §7 is
**out of scope** and named as its own future track.

**Prime directive:** both tuple types are expressible **entirely in prelude source text**,
using machinery that already backs value structs today. **Zero C++ changes to any
backend.** The two code paths that could have forced a compiler change — the
struct-equality synthesis pass (which bails on generic structs,
`src/Resolver.cpp:6880-6884`) and the columnar-eligibility path (which cannot see through
a generic's type parameters, §7) — are both *routed around*, not touched. That routing is
the central engineering decision of this ticket.

---

## 1. What this design decides

Two rulings, one of them a correction to the original scope:

1. **`Triple<A, B, C>` lands as a value `struct`** with a hand-written `(==)` and
   `toString()` — Option C of `research-triple.md` §4.
2. **`Pair<A, B>` flips from `class` to `struct`**, gaining the same hand-written `(==)`
   and `toString()`. The rule this establishes, per the owner's direction: **all tuple
   types are value structs — when one is, all are.** `Pair` shipping as a reference
   `class` (`src/Resolver.cpp:397`) was a mistake, and §2 shows it is a *performance*
   mistake, not merely a cosmetic one.

The original design (v1 of this file) ruled only on `Triple` and deferred `Pair`'s
inconsistency to "a separate future ticket." That deferral was wrong: the consistency gap
between a value-type `Triple` and a reference-type `Pair` is not cosmetic — it is the
difference between dense and boxed storage on the stdlib's data hot paths (§2). This
version folds the `Pair` flip in, because the density analysis makes it the point.

### 1.1 Non-goals (explicit)

- **No anonymous tuple syntax.** No `(int, string, bool)` type, no bare `(a, b, c)`
  literal. Tuples are only `Pair<A,B>` / `Triple<A,B,C>` and `::Of(...)`. See §7.2 for why
  anonymous tuples are a genuinely separate (and, for the density story, *cleaner*)
  design — and why they are still deferred.
- **No `Quad`/`Quint`/`zip3`.** Per the request's Known Warning, a four-field need is the
  trigger to *design anonymous tuples*, not to extend the named-arity ladder. Adding
  `Quad`/`Quint` now would deepen commitment to a pattern with sharply diminishing
  ergonomic returns (`.first`…`.fifth`) and create more legacy to reconcile if tuples
  ever land. Held at three.
- **No columnar monomorphization of generic value structs (§7).** This design delivers
  the *row-major dense* win for tuple arrays for free; the further *columnar* win for
  generic tuples requires per-instantiation shape monomorphization, which is a larger
  architectural track named but not undertaken here.
- **`Map` iteration keeps yielding `Pair`.** Untouched; the one place `Pair` is
  special-cased by name (Map's `for (Pair e in m)` fast path) is verified safe under the
  flip (§4.3), not modified.

---

## 2. Why this is a performance decision, not a consistency one

The native `Array<value-struct>` storage has **three tiers**, established by reading the
runtime append-flip (`runtime/lv_runtime.c:2055-2066`) and the columnar design
(`designs/complete/techdesign-columnar-arrays.md`, landed 2026-07-12, default-on):

| element type | `Array<…<int,int>>` (all-scalar) | `Array<…<int,string>>` (heap field) |
|---|---|---|
| **reference `class`** (`Pair` today) | **boxed** — one heap alloc + pointer *per element* | boxed |
| **value `struct`, generic** (this design) | **row-major dense** — flat 16-byte-slot records, no per-element heap alloc, no pointer chase | boxed (heap field — unchanged, correct) |
| **value `struct`, concrete-scalar** (§7) | **columnar** — 8-byte tag-free columns, ~2× smaller, column-selective locality | n/a |

The append flip (`runtime/lv_runtime.c:2063-2065`) gates the dense/columnar decision on
`lvrt_isvalueclass(classId)` plus a **per-instance** `lv_struct_has_heap_field` check —
*not* on anything a reference class can satisfy:

```c
if (lv_struct_has_heap_field(val->payload))          lv_arr_boxed_append(...);
else if (lv_cfg_columnar && lv_col_eligible(classId)) lv_arr_go_columnar(...);
else                                                  lv_arr_go_dense(...);   // row-major
```

**Consequence.** `Pair` is a `class`, so `lvrt_isvalueclass` is false and *every*
`Array<Pair<…>>` — the return type of `Array.zip`, `.withIndex`, `.join`, `.groupJoin`,
and `Map.entries` (`src/Resolver.cpp:510-586, 732-736`) — is stored **boxed**: a heap
allocation and a pointer chase per element, on precisely the bulk-data paths the language
positions as its differentiator. Flipping `Pair` to a `struct` moves the all-scalar case
(`Array<Pair<int,int>>` from `withIndex`) straight to **row-major dense** with **zero new
infrastructure** — the single largest locality improvement available on the stdlib today,
bought with a keyword. Heterogeneous pairs carrying a `string` (`zip`, `entries`) stay
boxed, exactly as they must (the dense record can't own a heap field) — no regression.

That is why the two tuple types must not diverge on value-ness: the divergence *is* a
storage-class divergence on the hot path.

---

## 3. Surface — the exact prelude source that lands

### 3.1 `Pair` (flip + explicit methods), replacing `src/Resolver.cpp:397-401`

```lev
// A 2-tuple, the result element of relational joins and Map iteration. A value
// `struct` (NOT a reference class): copied not aliased, field-wise ==, and — as
// a value type — its arrays store dense/row-major instead of boxed (see
// designs/techdesign-triple.md §2). The (==) is hand-written because the struct-
// equality synthesis pass bails on generic structs (Resolver.cpp:6880).
struct Pair<A, B> {
    A first;
    B second;
    new Of(A a, B b) { first = a; second = b; }
    bool (==)(Pair<A, B> other) => first == other.first && second == other.second;
    string toString() => "(" + first + ", " + second + ")";
}
```

### 3.2 `Triple`, inserted immediately after `Pair`

```lev
// A 3-tuple sibling of Pair. Same value-struct conventions (§2, §4).
struct Triple<A, B, C> {
    A first;
    B second;
    C third;
    new Of(A a, B b, C c) { first = a; second = b; third = c; }
    bool (==)(Triple<A, B, C> other) =>
        first == other.first && second == other.second && third == other.third;
    string toString() => "(" + first + ", " + second + ", " + third + ")";
}
```

No natives, no new `Symbol`/`Shape` C++, no new IR ops — pure prelude text.

### 3.3 Why the fully-parameterized self-type in `(==)` matters

Both `(==)` methods spell their parameter `Pair<A, B>` / `Triple<A, B, C>` — fully
parameterized. This is the ordinary, tested way a generic class names itself in a method
signature. It is *also precisely the spelling the synthesis pass could not safely
generate*: that pass emits the bare, unparameterized class name `N`
(`src/Resolver.cpp:6926-6927`), whose resolution for a generic self was the deferred,
never-validated question. Writing the parameterized form by hand gives the correct
relation with **zero dependence** on that unresolved path (§4.2, §5).

---

## 4. Blast radius of the `Pair` flip — verified corpus-safe

Investigated exhaustively against current source. **Verdict: no existing corpus or unit
test changes output or breaks; no `.expected` file moves.** One latent behavior change
exists (not exercised in-tree) and is closed by the hand-written `(==)`.

### 4.1 The one latent break — closed by §3.1

A value struct with no `(==)` hits the comparability gate at the comparison site
(`src/Checker.cpp:2324-2331`): the synthesis pass records a generic struct with
`synthesized=false` (`src/Resolver.cpp:6880-6884`), so `Pair == Pair` would become a
**compile error** ("define an explicit `bool (==)(Pair other)`"). Today `Pair` compares by
reference identity (`src/Checker.cpp:2335-2338`, the `!isValue` branch) and
`Pair::Of(1,2) == Pair::Of(1,2)` is legal-and-`false`. **No corpus or test compares
`Pair`s** (grep: zero `Pair ==` sites; `iterator.lev:93` compares *strings*), so nothing
in-tree breaks — but user code doing `Pair::Of(...) == ...` would. The hand-written
`(==)` in §3.1 preserves comparability and upgrades its result from identity to
field-wise. This is a strict improvement, made deliberately.

### 4.2 Equality/printing semantics change (intended)

- `==` goes from reference-identity (`false` on equal values) to **field-wise** (`true`).
- Printing stays `<object>` for a *bare* `Pair` today (no `toString` synthesis exists);
  the hand-written `toString()` makes it print `(first, second)`. No corpus prints a bare
  `Pair` (all sites read `.first`/`.second`), so this is invisible to existing tests and
  purely additive.

### 4.3 Verified SAFE (no change)

- **Map-iteration fast-path offsets stay correct.** Standalone (heap/arena) object slot
  layout is **`isValue`-independent**: header 0–16, field 0 at 16, field 1 at 32 — the
  same offsets `RawGet` computes as `16+(d-1)*16` for *all* classes
  (`src/LlvmGen.cpp:2956`). `isValue` changes only *where* `NewObject` allocates (arena
  vs heap) and the copy/eq semantics — and the fast path calls `rtObjNew` directly, so
  even allocation is unchanged (`src/LlvmGen.cpp:3295-3318`; `X64Gen.cpp:1607-1620`). The
  Eval/IrInterp/CGen fast paths build the `Pair` by field *name* via `mkobj`/`objset`,
  which is layout-agnostic (`src/Eval.cpp:1764`; `IrInterp.cpp:759`; `CGen.cpp:1697`).
- **No aliasing/mutation break.** Grep of prelude + `tests/` finds `Pair` field assignment
  *only* in the constructor (`src/Resolver.cpp:400`). Nothing mutates `.first`/`.second`
  post-construction, so nothing relies on mutation-through-alias that value-copy severs.
- **`Array<Pair<int,int>>` re-routes boxed → row-major dense** (§2). Layout is
  value-semantically unobservable (`src/CGen.cpp:958`), so `arrays_ext.ext:42-44`
  (`wi.at(3).first`) produces identical text — but now exercises a dense path `Pair` never
  hit as a class. This is why §8 adds a dedicated regression file. (`Pair<int,string>`
  from `zip`/`entries` keeps a heap field → stays boxed → wholly unchanged.)
- **Churn/ARC corpus not at risk.** `tests/corpus/churn*`, `columnar/`, `dense_array_at/`
  contain no `Pair` usage. Flipping `Pair` to a struct pulls it under `copyValue`
  (`src/RuntimeValue.hpp:118-125`) and `keyEquals` (`:286-294`) — a self-balancing
  per-bind deep copy, semantically correct, previously untested for `Pair` (hence §8's new
  green test).

### 4.4 Corpus/test files touching `Pair` — all confirmed no `.expected` change

`iterator.lev`, `arrays_ext.ext`, `collections.ext`, `elf/collections.ext`,
`maps_set.ext` (read `.first`/`.second`, sort/build `Array<Pair>`); `test_eval.cpp:139,208`
(`for (Pair …)`); `test_checker.cpp:239` (type-mismatch, value-ness-independent). Output
identical in every case; some LLVM/CGen paths re-route to dense for `Pair<int,int>` with
byte-identical results.

---

## 5. Decision record: value `struct` + hand-written `(==)`/`toString()`

Three candidates (from `research-triple.md` §4), applied to *both* tuples:

- **A — reference `class`** (status quo for `Pair`, mechanical copy for `Triple`): boxed
  arrays (§2), reference identity, `<object>`. Rejected — it is the performance bug.
- **B — `struct` with *synthesized* `(==)`**: the first-ever exercise of the generic-struct
  branch of `synthesizeStructEquality`, which explicitly bails (`src/Resolver.cpp:6880`)
  because its generated source uses the bare class name `N` (`:6926-6927`), never
  validated for a generic self. Lifting the guard changes the pass's behavior for *every*
  future generic struct and needs new corpus proof. Rejected for this ticket — larger,
  riskier scope; the derive-vs-write benefit is nil for a permanently-fixed field list.
- **C — `struct` with hand-written `(==)`/`toString()`** *(chosen)*: real value semantics
  and dense-array eligibility (§2) with **zero** dependence on the synthesis pass —
  `hasExplicitEq(cls)` (`src/Resolver.cpp:6755`) makes the pass skip the class outright
  (`:6871`, "the author's relation wins"). Ordinary generic-method dispatch, already
  working on all four engines. Closes the `<object>` printing gap for free.

Option C is the same ruling for `Pair` and `Triple`, which is the point: **uniform value
semantics across all tuple arities.** Between the request's literal "mirror `Pair`" and its
stated intent (value semantics), the intent governs — and the density analysis (§2) shows
the intent is also the performant choice.

---

## 6. Non-interference with the struct-equality synthesis pass — proof

Both tuples have a hand-written `(==)`. In `synthesizeStructEquality`
(`src/Resolver.cpp:6829`): `if (hasExplicitEq(cls)) continue;` (`:6871`) skips each **before**
the generic-struct guard at `:6880` is ever evaluated. So this design touches the pass
**not at all** and does not change its behavior for any other struct; the deferred
generic-synthesis question stays cleanly out of scope. If someone later lands generic
`(==)` synthesis (Option B's capability), the author's hand-written relation must still win
(`:6871`'s contract) — §8's `== → true` assertions pin the observable result as a
regression anchor.

---

## 7. What this does NOT get you: columnar for generic tuples

### 7.1 The finding

The *row-major dense* win (§2) is free. The *columnar* win (8-byte tag-free columns,
column-selective locality) is **not**, for the generic tuple types. `columnarEligibleStruct`
(`src/Symbols.hpp:98-108`) inspects each field's **declared** canonical type; for
`Pair<A,B>` the slots are `"A"`/`"B"`, and `columnarTypecodeOf("A") == 0`, so the predicate
returns false at the first field. There is **no per-instantiation value-struct shape
cloning**: the checker hands the lowerer `valueClass = t.sym` — the single shared generic
symbol, `Type.args` discarded (`src/Checker.cpp:787`; decision sites `src/Lower.cpp:937,
1937`) — and `substituteSlotGenerics` (`src/Resolver.cpp:6441`) rewrites only *inherited
base-class* slots, never own fields. The runtime descriptor table is keyed per-classId,
one id shared by every instantiation of a generic class, so `Pair<int,int>` and
`Pair<float,bool>` cannot even be given distinct typecode rows.

**Net: `Array<Pair<int,int>>` (as a struct) is row-major dense, not columnar.** Getting it
columnar needs per-instantiation monomorphization of eligible generic value structs —
mint a distinct symbol+classId per concrete `(class, args)` with substituted field
canonicals, and thread the concrete `Type` (not the bare symbol) from `Checker.cpp:787`
through the three `Lower.cpp` sites. That is a real track, sized well beyond "add a tuple,"
and is **explicitly not undertaken here**. It is the natural next step if profiling shows
the row-major tuple arrays want the further 2× and column-selectivity.

### 7.2 Why anonymous tuples are the *cleaner* columnar path (and still deferred)

An anonymous tuple `(int, string, bool)` has **concrete** field types by construction — no
type parameters. If it desugars to a concrete-field value struct, it is columnar-eligible
*immediately*, with none of the monomorphization work §7.1 requires, because it never
introduces the generic-classId-sharing problem. So for the density story, anonymous tuples
are **cheaper**, not harder, than making generic `Pair<int,int>` columnar. The genuinely
hard part of anonymous tuples was never layout — it is the *surface*: variadic-arity type
syntax (the language has no variadic generics), structural-vs-nominal identity, and
inference. Those are why they remain deferred, per the request's Known Warning. This design
records the inversion so the deferral is made on the real cost (surface), not a phantom one
(layout). If anonymous tuples are ever taken up, the monomorphization track in §7.1 and the
tuple surface should be scoped together — they share the "concrete-typed value struct →
columnar" payoff.

---

## 8. Testing plan

### 8.1 Corpus (acceptance criterion 2 — every active engine)

No new directory or CMake registration — top-level `.ext`/`.lev` files are auto-discovered
by `corpus_treewalk`/`corpus_ir`/`corpus_native`/`corpus_llvm_full`
(`CMakeLists.txt:201-207, 828-831, 1125`); `corpus_elf_full` self-skips (columnar is
ELF-disabled anyway, `src/main.cpp:317`).

Extend `tests/corpus/arrays_ext.ext` (already builds `Pair` explicitly) with a `Triple`
section exercising the full surface, and add `Pair` equality/printing now that both are
real:

```lev
Triple<int, string, bool> t = Triple::Of(1, "x", true);
console.writeln(t.first); console.writeln(t.second); console.writeln(t.third);
console.writeln(t);                              // (1, x, true)  <- toString, not <object>
console.writeln(t == Triple::Of(1, "x", true));  // true
console.writeln(t == Triple::Of(1, "x", false)); // false
Pair<int, int> p = Pair::Of(1, 2);
console.writeln(p);                              // (1, 2)
console.writeln(p == Pair::Of(1, 2));            // true  <- was false (identity) as a class
```

### 8.2 New green density/churn regression file — `tests/corpus/tuple_value.lev`

Recommended by the blast-radius investigation: **no existing test exercises `Pair` under
value-copy / dense-array discipline**, and the flip newly routes `Array<Pair<int,int>>`
through the row-major dense path. Add a `.lev` file (per the `.lev`-for-new-corpus
convention) that:

- builds `Array<Pair<int,int>>` via `withIndex`/explicit `Pair::Of`, iterates it, and reads
  fields (exercises the dense record path and value-copy binds);
- rebinds a `Pair`/`Triple` to a second variable and mutates neither, asserting equality
  holds by value (exercises `copyValue`);
- uses a `Pair<int,int>` and a `Triple<int,int,int>` as `Map` keys, asserting field-wise
  key equality (exercises `keyEquals`, already generic-safe, `src/RuntimeValue.hpp:286`).

Must be byte-identical across oracle/IR/emit-C++/LLVM.

### 8.3 Unit tests (`tests/test_eval.cpp`, `OUT` macro)

```cpp
OUT("void run() { Triple t = Triple::Of(1, \"x\", true); "
    "console.writeln(t.first); console.writeln(t.second); "
    "console.writeln(t.third); } run();", "1\nx\ntrue\n");
OUT("void run() { console.writeln(Triple::Of(1,2,3) == Triple::Of(1,2,3)); "
    "console.writeln(Pair::Of(1,2) == Pair::Of(1,2)); } run();", "true\ntrue\n");
OUT("void run() { console.writeln(Pair::Of(1,2)); "
    "console.writeln(Triple::Of(1,\"x\",true)); } run();", "(1, 2)\n(1, x, true)\n");
```

The `Pair::Of(1,2) == Pair::Of(1,2)` → `true` case is the visible marker of the semantic
flip (was `false` as a class) and doubles as the §6 regression anchor.

---

## 9. Documentation

Replace `docs/reference.md:1106-1108` (`### 6.4 Pair<A, B>`) and add a `Triple` subsection,
recording the value semantics both now have:

```
### 6.4 `Pair<A, B>`
Fields `first`, `second`; constructor `Pair::Of(a, b)`. A value type (`struct`, info.md §9):
copied not aliased, field-wise `==`, prints `(first, second)`. The element type of
relational joins and of Map iteration. Its arrays store densely (no per-element boxing).

### 6.4a `Triple<A, B, C>`
Fields `first`, `second`, `third`; constructor `Triple::Of(a, b, c)`. The three-field
sibling of `Pair`, same value-struct conventions. Use it wherever three values travel
together (a coordinate + a label, a key + two related values) instead of a purpose-built
struct or nested `Pair<A, Pair<B, C>>`.
```

Do not touch the pre-existing `### 6.4` / `### 6.3.5` / `### 6.4.4` numbering disorder — it
predates this ticket.

---

## 10. Rollout / checklist

1. Flip `Pair` to a `struct` and add its `(==)`/`toString()` (§3.1); insert `Triple` (§3.2)
   — both in `kPreludeCore`, `src/Resolver.cpp` around `:397`.
2. Extend `tests/corpus/arrays_ext.ext` (§8.1); add `tests/corpus/tuple_value.lev` (§8.2);
   add the `OUT` cases (§8.3); update `docs/reference.md` (§9).
3. Build; run `corpus_treewalk`, `corpus_ir`, `corpus_native`, `corpus_llvm_full`, and
   `test_eval`. All four active engines byte-identical on the new corpus. ELF self-skips.
4. Commit. Feature, not a bug fix — no known-bug entry involved.

**Blast radius:** two prelude declarations (~11 lines each), one corpus section, one new
corpus file, three `OUT`s, one docs edit. Zero C++ logic changes.

---

## 11. Summary

- **Both tuples are value `struct`s** (§1, §3): `Triple` lands new; `Pair` flips
  `class`→`struct`. Uniform value semantics, hand-written `(==)`/`toString()` on each.
- **This is a performance decision** (§2): a reference-class tuple forces boxed
  `Array<Pair>` storage on the stdlib's data hot paths; the flip moves the all-scalar case
  to row-major dense for free. That is why the two arities must not diverge on value-ness.
- **The flip is verified corpus-safe** (§4): no `.expected` changes; the one latent break
  (`Pair == Pair`) is closed by the hand-written `(==)`; the Map fast-path offsets are
  `isValue`-independent and stay correct.
- **Zero backend edits** (§6): both risky compiler paths (struct-eq synthesis; columnar
  eligibility) are routed around, not touched.
- **Scope held** (§1.1): no tuple syntax, no `Quad`/`zip3`, no columnar monomorphization.
- **The columnar tier for generic tuples is a named future track** (§7), and anonymous
  tuples — deferred for their *surface*, not their layout — are the cleaner path to it if
  ever taken up.
