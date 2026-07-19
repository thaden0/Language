# Columnar for generic value structs — per-instantiation shape monomorphization

**Status:** LANDED 2026-07-19 — eligible all-scalar instantiations of a generic value
struct now go columnar on LLVM-native; full suite green (4 engines × `--columnar`/
`--no-columnar`, ELF/X64 frozen backend, churn-leak, checker/eval). Implemented by an
Opus-class agent. See the implementation log (§12) for the as-built approach, which
deliberately simplifies the plan below (§1/§4) without changing the observable contract.
**Date:** 2026-07-19.
**Difficulty:** L. **Risk:** MED/HIGH — it mints a new *kind* of specialization (class
shapes, not just callables) and threads an instantiation-specific classId through
construction and the runtime layout flip. Milestones carry STOP checkpoints (§9).
**Depends on:** landed columnar `Array<struct>` (`designs/complete/techdesign-columnar-arrays.md`),
landed LA-18 demand-driven monomorphization (`src/Checker.cpp:4317-4551`), landed
dense/value-struct discipline. Nothing unlanded.
**Source (origin):** `designs/techdesign-triple.md` §7 (the finding that generic tuple
arrays get only row-major dense, never columnar) and
`designs/requests/accepted/request-columnar-dense-array-struct.md` §10.2 ("widening the
eligibility line is future work").
**Owns (regions):** the LA-18 specialization machinery (`src/Checker.cpp` around
`:4502-4551`, `:4317-4442`, and `SpecializationCloner` at `:167`), class-shape building for
specialized classes (`src/Resolver.cpp` `buildShape`/`slotOf` reuse), classId + descriptor
emission (`src/LlvmGen.cpp:505-514, 1180-1235`; CGen twin), and the go-columnar runtime
linkage (read-only understanding of `runtime/lv_runtime.c:2055-2066`; no ABI change).
**Does NOT own / must not change:** the `lv_abi.h` columnar contract (bit 62, descriptor
signatures — §4 of the columnar design is frozen), oracle semantics (`src/Eval.cpp` —
stays boxed/generic, the differential reference), `X64Gen.cpp` (frozen; columnar-disabled),
value semantics (`copyValue`/`keyEquals` are already generic and untouched).

**One-line goal:** make `Array<Pair<int,int>>`, `Array<Triple<int,int,bool>>`, and every
other all-scalar instantiation of a generic value struct store **columnar** (8-byte
tag-free columns, ~2×, column-selective locality) — the tier they cannot reach today
because eligibility and the runtime descriptor table are resolved off the shared generic
symbol, whose fields are type parameters, not scalars.

---

## 0. The problem, precisely (verified)

Columnar eligibility (`columnarEligibleStruct`, `src/Symbols.hpp:98-108`) inspects each
field slot's `canonical` string and accepts only `int`/`float`/`bool`/`char`
(`columnarTypecodeOf`, `:86-92`). For a generic value struct the slots hold the **declared**
type-parameter spelling — `Pair<A,B>`'s slots are `{canonical:"A"}, {canonical:"B"}` —
because:

- shapes are built **once per class symbol** (`buildShape`, short-circuits on
  `shape.built`, `src/Resolver.cpp` ~`:6531`), and a slot's canonical is copied verbatim
  from the declared type (`slotOf`, `src/Resolver.cpp:6340`: `s.canonical =
  member->type->canonical`), so `first : A` yields canonical `"A"`;
- the **only** slot substitution that runs (`substituteSlotGenerics`,
  `src/Resolver.cpp:6441`) is applied solely to *inherited base-class* slots (`:6575,
  :6580`), never to a class's own declared fields;
- the checker hands the lowerer `valueClass = t.sym` — the shared generic symbol, with the
  instantiation's `Type.args` **discarded** (`src/Checker.cpp:787`; decision sites
  `src/Lower.cpp:937, 1937`);
- the runtime tables `lv_col_eligible(classId)` / `lv_col_typecode(classId, k)` are keyed
  **per-classId**, and `classId` is one id per symbol (`classIdOf`, `src/LlvmGen.cpp:505`),
  so all instantiations of a generic class share one id and one (type-param-derived) row.

`columnarTypecodeOf("A") == 0`, so `columnarEligibleStruct` returns false at the first
field and every generic-tuple array falls to boxed or (for a value struct) row-major dense.
**There is no runtime handle that distinguishes `Pair<int,int>` from `Pair<float,bool>`.**
Relaxing the predicate is insufficient: the per-classId descriptor table would have no
correct single row to emit. The fix must give each columnar-eligible **instantiation** its
own identity — a monomorphized shape and classId.

---

## 1. Approach: ride the LA-18 cloner, specialize *class shapes*

LA-18 already clones a generic `Stmt` under a type-param→concrete-`Type` substitution,
names it `$spec.Name<int, int>`, clears its generics, marks `isSpecialization`, and caches
it keyed on `(generic ptr, arg-canonical tuple)` — `materializeSpecializations`,
`src/Checker.cpp:4502-4551`, via `SpecializationCloner{program, subst}` (`:167, 4521`).
Today this fires **only for callables** with `specializationRequired`, triggered by
`T::member` static access (`markSpecializationSites`, `:4317-4442`). The cloner already
rewrites type annotations through the whole subtree — so cloning `struct Pair<A,B>` under
`{A→int, B→int}` yields a `struct` whose field declarations read `int first; int second;`,
and building *that* clone's shape produces slots with canonical `"int"` — which
`columnarEligibleStruct` accepts unchanged, and which the descriptor emitter
(`emitColumnarDescriptors`, `src/LlvmGen.cpp:1188-1231`, reads `sym->shape.slots[k].canonical`)
describes correctly with **zero edits**.

So the whole design is: **extend the demand/materialize machinery to also specialize
columnar-eligible generic value-struct classes, register each clone as a class symbol with
a built shape and its own classId, and route construction + array-element typing of eligible
concrete instantiations to the specialized symbol.** Everything downstream — eligibility,
descriptor emission, the go-columnar flip, fused reads, gather — is already generic over
"whatever symbol has these slots" and needs no change.

### 1.1 Why full monomorphization, not a side-channel layout id

The object payload header stores exactly one classId, used for **both** method dispatch and
the columnar decision (the runtime flip reads `classId = lv_ld_i64(val->payload, 0)`,
`runtime/lv_runtime.c:2062`). A "keep the generic classId for dispatch, add a separate
layout id for columnar" split would require either widening the ABI header (forbidden — the
columnar contract is frozen) or abandoning the dynamic go-dense model for a static one.
Minting a distinct classId per eligible instantiation keeps the single-id invariant: the
specialized symbol carries **both** its own descriptor row **and** its method table (cloned
from the generic), so dispatch and columnar agree by construction. This is also the
cheapest fit to the existing cloner, which already produces a fully re-checkable clone.

---

## 2. Scope of what gets specialized (kept small and demand-driven)

Only instantiations that can actually be columnar are specialized — the set is naturally
bounded:

- generic class is a **value struct** (`isValue`), and
- every concrete type argument bound to a field is a **columnar scalar**
  (`int`/`float`/`bool`/`char`), such that the resulting shape is columnar-eligible.

`Pair<int,string>`, `Pair<int, Array<int>>`, `Triple<int, SomeClass, bool>` are **not**
specialized — they are ineligible regardless (heap field), keep the generic symbol, and
stay boxed exactly as today. This is the same eligibility line as `columnarEligibleStruct`,
evaluated against the *substituted* shape. A generic value struct with a non-scalar field
in *every* instantiation is never specialized. The specialization count is bounded by the
number of distinct all-scalar instantiations actually constructed in the program — for the
tuples, a handful (`Pair<int,int>`, `Pair<int,float>`, …).

**Non-goals:** non-value generic classes (never columnar); nested-struct fields (columnar
design defers these, §10.1 there); anonymous tuples (separate surface, `techdesign-triple.md`
§7.2 — though they land *directly* eligible with no work here, being concrete by
construction); mutation-granularity/column-selective COW (columnar design §10, still
deferred).

---

## 3. Where the demand comes from

A class-specialization demand for an eligible instantiation `C<τ…>` must be recorded when
the program can construct or store such a value. Two trigger sites, both static and already
type-resolved:

1. **Construction** — a call `C::Of(...)` (or any ctor) whose receiver type resolves to an
   eligible concrete `C<τ…>`. The checker already knows the concrete `Type{sym,args}` at the
   call. Record the demand and rebind the construction's resolved class to the specialized
   symbol so `NewObject`/`classIdOf` stamps the instantiation classId.
2. **Array element type** — an `Array<C<τ…>>` for eligible `C<τ…>` appearing as a
   declared/inferred type (the element type the lowerer's go-columnar and fused-read paths
   key on). Record the demand and set the array-element `valueClass` to the specialized
   symbol.

Both reuse the existing demand queue (`specializationDemands_`, `src/Checker.hpp:168`) and
its fixed-point drain, so transitively-constructed instantiations (a specialized body that
itself builds another eligible tuple) are picked up on the same worklist. The dedup key is
the existing `(generic ptr, arg-canonical tuple)` (`:4505-4511`) — no new cache.

**Determinism invariant carried forward** (columnar design §3): within one compiled
program an `Array<C<τ…>>` for eligible `C<τ…>` is boxed-empty or columnar, never row-major —
because every such element is stamped with the specialized classId, and that classId is
`lv_col_eligible`. The generic symbol's classId (used only by ineligible instantiations)
stays row-major/boxed.

---

## 4. Materializing a specialized *class* (the new plumbing)

`materializeSpecializations` today calls `checkFunction` on each clone — correct for
callables, insufficient for a class, which additionally needs a **registered class Symbol
with a built Shape** and a **classId**. The class path adds:

1. **Clone** the class `Stmt` under `subst` via the existing `SpecializationCloner`
   (`src/Checker.cpp:4521`). Field type annotations rewrite to concrete scalars; the cloned
   `(==)`/`toString`/`Of` bodies come along (they are generic-body-erased and dispatch the
   same way). Name `$spec.Pair<int, int>`, `generics.clear()`, `isSpecialization = true`,
   `specializationOf = generic` (mirrors `:4524-4530`).
2. **Register a class Symbol** for the clone in the appropriate scope and **build its
   shape** (reuse `Resolver::buildShape`/`slotOf`, `src/Resolver.cpp:6340`+, over the cloned
   decl). The resulting slots have canonical `"int"`/… so `columnarEligibleStruct` accepts
   it. This is the one genuinely new step — the specialization pipeline has never built a
   class shape before; it must run shape-building for the clone the same way the resolver
   does for a declared class.
3. **Assign a classId** on demand at codegen via the existing `classIdOf(Symbol*)`
   (`src/LlvmGen.cpp:505`) — the specialized symbol is just another symbol; it gets the next
   id when first referenced. `emitColumnarDescriptors` then iterates `clsIds` and emits its
   eligibility + typecode rows **with no change** (`:1195-1231`).
4. **Emit its class-info + method table.** The specialized class shares the generic's method
   *bodies* (cloned). Class-info (`{name, classId, nslots, isValue}`, `src/LlvmGen.cpp:275`)
   and the method dispatch table must exist for the specialized classId so `(==)`, `toString`,
   and `Of` dispatch on a `Pair<int,int>` object resolve. Simplest correct form: emit them
   from the specialized symbol like any class; dedup identical method bodies at the IR level
   if it matters (optimization, not correctness).

CGen mirrors LlvmGen (its descriptor/class-info twin). Oracle/IR interp: see §5.

---

## 5. Engine neutrality — oracle and IR stay generic

The columnar design's cornerstone invariant is that **layout is value-semantically
unobservable**: oracle and IR interp stay boxed and produce the reference output
(`techdesign-columnar-arrays.md` §2, engine-impact table). This design preserves that:

- The oracle (`src/Eval.cpp`) constructs objects carrying `Symbol* cls`; `copyValue`/
  `keyEquals` are gated on `isValue` and iterate fields by name — **generic-agnostic, no
  classId, untouched.** Whether the oracle stamps the generic or the specialized symbol on
  a `Pair<int,int>` is unobservable to it (same fields, same `isValue`, same methods). To
  keep the oracle maximally undisturbed, specialization is a **checker→native-codegen**
  concern: the oracle may continue to see the generic symbol. The only requirement is that
  the *native* construction/array-typing sites resolve to the specialized symbol.
- **Differential corpus is the proof** (§8): every columnar `.lev`/`.ext` must produce
  byte-identical output on oracle/IR (boxed, generic) and emit-C++/LLVM (columnar,
  specialized). Any divergence is a bug in the specialization's neutrality, caught before
  the flip.

The single subtlety to pin (STOP-worthy if it bites): the specialized symbol must not leak
into a place that makes *identity* or *dispatch* observably differ from the generic — e.g.
`keyEquals` compares `a.obj->cls == b.obj->cls`. If two `Pair<int,int>` values are ever
constructed under *different* symbols (one generic, one specialized), key equality would
wrongly diverge. Resolution: within one engine, all eligible `Pair<int,int>` constructions
resolve to the **same** symbol (the cache guarantees one specialized symbol per key), so
`cls == cls` holds. Cross-engine differences are fine (engines are compared by *output*, not
by symbol identity). This must be asserted by a `Map<Pair<int,int>, …>` corpus case (§8).

---

## 6. What does NOT change (the leverage)

Because the design produces "a symbol whose slots are concrete scalars," everything the
columnar track already built consumes it unchanged:

| facet | mechanism | change |
|---|---|---|
| eligibility predicate | `columnarEligibleStruct` reads slot canonicals | **none** — accepts substituted `"int"` slots |
| descriptor tables | `emitColumnarDescriptors` iterates `clsIds`, reads slots | **none** — new symbol → new row automatically |
| go-columnar flip | `lv_arr_go_columnar` gated on `lv_col_eligible(classId)` | **none** — specialized classId is eligible |
| fused `arr[i].field` | `ColGet`, gated on `columnarEligibleStruct(valueClass)` | **none** — valueClass is the specialized symbol |
| gather (escape) | builds a standalone value struct | **none** |
| ABI (`lv_abi.h`) | bit 62, column layout, descriptor sigs | **none** — frozen, untouched |
| value semantics | `copyValue`/`keyEquals`, `isValue`-gated, by-name | **none** — already generic |
| the demand cloner | `SpecializationCloner`, `(generic, tuple)` cache | **reused**, extended to classes |

The **new** code is concentrated in: demand recording at the two trigger sites (§3), class
clone + shape-build + registration (§4.1-4.2), and routing construction/array-typing to the
specialized symbol (§3). Descriptor/runtime paths are pure reuse.

---

## 7. Milestones (single track — one owner, ordered, STOP protocol per §9)

- **G-M0 — spike / neutrality proof.** Hand-specialize `Pair<int,int>` (or a throwaway
  `struct P2<A,B>`): mint one specialized symbol with concrete slots, route
  `Array<P2<int,int>>` construction + element typing to it, confirm `lv_col_eligible` returns
  1 for its classId and an `arr[i].x` scan hits the columnar `ColGet` path. **Gate:** the
  fused columnar read fires and output is byte-identical to boxed oracle. If the classId
  linkage can't be made to agree between construction stamp and descriptor row — **STOP**,
  the single-id assumption (§1.1) is wrong and a Fable-class revise is needed.
- **G-M1 — demand recording.** Add class-specialization demand at the construction and
  array-element sites (§3), gated on eligibility-of-substituted-shape. No materialization yet
  — just log the demand set on the corpus and confirm it's the expected bounded set.
- **G-M2 — materialize class specializations.** Clone + register symbol + build shape (§4.1-
  4.2); wire `classIdOf`/descriptor emission (automatic) and class-info/method-table emission
  (§4.4). **Gate:** descriptor tables contain a correct row per specialized instantiation;
  method dispatch (`(==)`, `toString`, `Of`) resolves on the specialized classId.
- **G-M3 — routing + go-columnar end to end.** Eligible tuple arrays actually flip columnar
  on native; fused reads and gather work. **Gate:** four-engine differential green on the
  full corpus with `--columnar` on; oracle/IR byte-identical.
- **G-M4 — churn + bench + flip.** Churn-corpus coverage of specialized value-struct
  arrays under COW; a column-selective aggregate benchmark vs row-major (columnar request
  Acceptance #3); then, once green, the tuples inherit columnar with no further work. Retain
  `--no-columnar` one cycle.

Value structs that are *not* generic already work today; this track strictly widens
eligibility to generic instantiations. Ineligible instantiations are provably untouched
(they never enter the demand set).

---

## 8. Testing

- **Differential corpus (the neutrality contract).** Extend the tuple corpus
  (`tests/corpus/tuple_value.lev`, added by `techdesign-triple.md` §8.2) and add
  `tests/corpus/generic_columnar.lev`: build `Array<Pair<int,int>>` /
  `Array<Triple<int,int,bool>>`, run a **column-selective scan** (`for … s = s + arr[i].first`)
  that exercises the fused `ColGet` path, read every field, and use a `Pair<int,int>` as a
  `Map` key (the §5 identity check). Byte-identical across oracle/IR/emit-C++/LLVM.
- **Descriptor unit assertion.** A C-level or IR-level check that `lv_col_eligible` returns
  1 and `lv_col_typecode` returns the right scalar codes for a specialized tuple classId
  (mirrors `runtime/selftest.c` columnar tests).
- **Ineligibility regression.** `Array<Pair<int,string>>` stays boxed (no specialized symbol,
  no columnar row) — assert unchanged output and, if observable, unchanged storage class.
- **Bench.** `sum` over one field of a large `Array<Pair<int,int>>` (columnar) vs the same
  as `Array<Pair<int,string>>`-shaped row-major control, on LLVM — demonstrates the
  cache-locality win (columnar request Acceptance #3). State the regime honestly (columnar
  wins on column-selective scans of wide/large arrays; parity-to-slight-loss on
  whole-row access — same caveat as the base columnar design §9).

---

## 9. STOP protocol / do-not-touch

Same discipline as the columnar-arrays implementation
(`designs/complete/techdesign-columnar-arrays-2.md` §0). A Sonnet-class implementer **STOPs**
and escalates to a Fable-class revise if: the classId single-id assumption fails (G-M0); the
`lv_abi.h` columnar contract would need any change (it must not — frozen); class-shape
building for a clone conflicts with a resolver invariant it can demonstrate; or the oracle/IR
neutrality (§5) cannot be preserved without an observable semantic change. Mechanical
adaptation (renamed symbol, corrected line, verified offset) — proceed and log.

Frozen / untouched: `runtime/lv_abi.h` columnar contract; `src/X64Gen.*` (columnar-disabled,
no lane, no ELF gate); oracle semantics as the differential reference; never edit `.expected`
to green a lane; all existing lanes green at every merge, and with `--columnar` off the output
is byte-identical to today. New corpus/churn files are `.lev`.

---

## 10. Relationship to the tuple design

`designs/techdesign-triple.md` flips `Pair`/`Triple` to value structs, which delivers
**row-major dense** tuple arrays with zero new infrastructure. This track delivers the
**columnar** tier on top. The two are **independent**: the tuple flip does not require this
work, and this work does not require the tuple flip (it widens eligibility for *every*
generic value struct). If this track lands **first** (the intended order), the tuples become
columnar the instant they flip to structs — no additional tuple-specific work. If the tuple
flip lands first, tuple arrays are row-major dense in the interim and upgrade to columnar
when this track lands. Either order is correct; sequencing this first means the tuple ticket
inherits the top tier immediately.

---

## 11. Summary

- Generic value structs can't go columnar because eligibility and the runtime descriptor
  table resolve off the shared generic symbol, whose fields are type parameters (§0, verified).
- Fix: **monomorphize columnar-eligible generic value-struct instantiations** — ride the
  existing LA-18 cloner (`Checker.cpp:4521`) to clone the class under the concrete subst,
  build the clone's shape (concrete scalar slots), give it its own classId, and route
  construction + array typing to it (§1, §3, §4).
- Everything downstream is **pure reuse**: `columnarEligibleStruct`, `emitColumnarDescriptors`,
  the go-columnar flip, fused reads, and value semantics all already work on "a symbol with
  scalar slots" (§6). No `lv_abi.h` change.
- Bounded, demand-driven scope (all-scalar instantiations only); ineligible instantiations
  provably untouched (§2). Oracle/IR stay boxed and generic; differential corpus is the
  neutrality proof (§5, §8). STOP checkpoints on the two real risks — the single-classId
  linkage and engine neutrality (§9).
- Lands the columnar tier for the tuples and for every concrete-scalar generic struct; the
  tuple design (`techdesign-triple.md`) inherits it for free if sequenced first (§10).

---

## 12. Implementation log (as built, 2026-07-19)

The landed implementation reaches the same observable contract as §1/§4 by a **simpler,
lower-risk route** than the cloner+`buildShape`+demand-queue plan. The §9 STOP protocol was
not tripped: the single-classId linkage (§1.1) and engine neutrality (§5) both held on the
first pass, and no `lv_abi.h` / resolver invariant needed changing. Net diff: **~113 lines**
across four files, no new AST fields, no new ABI, no runtime edit, no CGen/X64Gen edit.

**What changed vs. the plan — and why it is equivalent:**

1. **Eager monomorphization instead of the LA-18 demand queue (§3, §4.1).** The plan cloned
   the class `Stmt` through `SpecializationCloner` and re-`checkFunction`'d it. Instead,
   `Checker::specializeValueStruct(generic, args)` (`src/Checker.cpp`) mints the specialized
   `Symbol` **on demand inside `typeOf`**, the first time an eligible concrete `C<τ…>` value
   struct is typed. No demand queue, no clone, no re-check. It is memoized on
   `(generic ptr, arg-canonical tuple)` (`valueStructSpecs_`), which delivers the §5
   single-symbol identity invariant directly (verified: exactly one `$spec.P2<int,int>` per
   program; `Map<P2<int,int>,_>` keys compare correctly on all engines).

2. **Substitute the generic's already-built shape instead of `buildShape` on a clone
   (§4.2).** The generic's `Shape` is already built by the resolver. The specialized shape is
   that shape with each **non-method field slot's `canonical` substituted** through
   `{param→arg.canonical}`; eligibility is the existing `columnarTypecodeOf(...)!=0` test
   applied to the substituted canonical (a heap/param-bound-to-`string` field ⇒ ineligible ⇒
   keep the generic symbol, boxed). This is exactly the observable result §4.2 wanted ("slots
   have canonical `int`") without invoking the resolver from the checker — avoiding the
   cross-pass seam the plan glossed. Method slots are copied verbatim.

3. **Share the generic's `decl` for methods (§4.4).** The specialized `Symbol` sets
   `decl = generic->decl`, so `collectMembersLG`/`fieldKeysOf`/`collectBases` all resolve to
   the generic's **already-emitted** method bodies and bases. The class-info + method-table +
   descriptor rows for the specialized classId are therefore emitted by `emitClassRegistration`/
   `emitColumnarDescriptors` **with zero backend change** — they iterate `clsIds`, and the
   specialized symbol enters `clsIds` automatically via `classIdOf(in.sym)` at the stamped
   `NewObject`. Dispatch (`==`, `toString`, `Of`) on a `P2<int,int>` resolves through the
   specialized classId's table to the generic's fnIndexes. No cloned methods, no new IR
   functions.

4. **One routing channel: `Expr::valueClass` (native-only).** The checker points
   `valueClass` at the specialized symbol for eligible instantiations (`typeOf`, the same
   site that already carried `t.sym`). The lowerer's construction `NewObject` stamp
   (`src/Lower.cpp` `lowerCall`) prefers `valueClass` over `ctorClass`, and the fused `ColGet`
   / gather sites already keyed on `valueClass` — so they specialize for free. Because the
   **oracle never reads `valueClass`**, engine neutrality (§5) is automatic and required no
   care: the oracle keeps constructing under the generic symbol, native stamps the specialized
   classId, and the differential corpus proves byte-identical output. (Consequently the
   for-in-gather fast path, whose gate reads the declared loop-var `TypeRef::resolvedSymbol`
   rather than `valueClass`, still fires correctly for these arrays via the runtime gather
   path and is leak-flat under `corpus_churn_leak_columnar_llvm` — confirmed green — even
   though its symbol resolves to the generic; no further routing was needed.)

**Ownership plumbing.** Specialized symbols are minted from a `const Sema&`, so `Sema` gained
a `mutable` append-only arena (`lateSymbols_` + `nameArena_` for stable `string_view` name
backing) with `const` minting methods (`newLateSymbol`/`intern`, `src/Symbols.hpp`). These
symbols live as long as the `Sema` (owned by the `Resolver`, which outlives codegen) and are
never registered in a scope, so name resolution is untouched — they are reached only through
IR `Op::….sym` pointers.

**Tests.** `tests/corpus/columnar/generic_columnar.lev` (+`.expected`) is the neutrality
contract per §8: `Array<P2<int,int>>` column-selective fused scan + gather + for-in, an
`Array<P3<int,int,bool>>` three-column scan, a `Map<P2<int,int>,string>` key (§5 identity),
and an ineligible `Array<P2<int,string>>` boxed control — byte-identical across
oracle/IR/emit-C++/LLVM under both `--columnar` and `--no-columnar`. `lv_col_eligible`
returns 1 for the specialized classId on native (disassembly-verified); escaping-tier peak
drops under `--columnar` (2496→1376 bytes on the spike), confirming the SoA flip. Full
regression: `corpus_columnar`, `corpus_native`, `corpus_llvm`, `corpus_elf{,_core,_full}`,
`corpus_churn_leak{,_columnar_llvm}`, `runtime_selftest`, `checkertests`, `evaltests`,
`corpus_treewalk`, `corpus_ir` — 100% green.

**Deferred (unchanged from §2 non-goals / §10).** Pair/Triple are still prelude *classes*;
flipping them to structs is the separate `techdesign-triple.md` track, which inherits this
tier the instant it lands. Nested-struct fields and the column-selective aggregate bench
(§8) remain future work. emit-C++/X64 keep their dense (layout-unobservable) representation —
columnar SoA is an LLVM-native + runtime optimization, exactly as in the base columnar design.
