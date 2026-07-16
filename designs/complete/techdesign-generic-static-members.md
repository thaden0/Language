# Tech Design — Static-Shaped Member Resolution on Generic Type Parameters (LA-18)

**Status:** **COMPLETE** — implemented 2026-07-12. **Date:** 2026-07-10.
**Review (2026-07-11):** approach confirmed (monomorphization over reification stands); probes
P-1/P-2/P-3 re-verified against the tree at `2cb4888` (Jul-6 binary). Review added a v1 scope
restriction the original draft missed — `T::…` on a **class-level** type parameter is not
enumerable even whole-program because the raw form legally erases the static tuple (§7.6, a
soundness hole, not a nice-to-have) — plus the CallDyn/override interaction for instance methods
(§7.7), the pass-2 demand-collection note (§4.1), and the LA-25 composition restriction (§8).
**Track:** single track, ordered milestones M0 → M4, **one implementer**. This introduces a **new
compilation mode** (demand-driven monomorphization) — it is not a fan-out and must not be split
across implementers; the checker deferral, the specialization table, and the per-engine
consumption are one story.
**Source (the *why*):** `designs/requests/request-generic-static-members.md` (LA-18); Atlantis Track 07
(generic MCP/tool adapters — `designs/atlantis/techdesign-07-mcp-openapi.md`, probe T7-P1) and
Track 03 (P-4 decode helpers — `designs/atlantis/techdesign-03-serialization.md`, the witness
idiom). Indexed in `designs/atlantis/techdesign-00-overview.md` (LA table, `:233`).
**Consumers:** Track 07 (one generic adapter per shape instead of per-type generated adapters),
Track 03 (decode helpers without the witness-parameter contortion). **Priority P1, wanted by
AG-5 (2026-11-25).**
**Convention docs:** `designs/complete/techdesign-00-overview.md` governs this work — read it
first. This track edits compiler files (`Checker.cpp`, `Lower.cpp`, and the function-emission
paths of the backends); per overview §2 it **serializes** behind any other in-flight
compiler-file track and rebases on the current tree before starting.

### The `::`-generalization pair (read this)

This design and its sibling `designs/complete/techdesign-method-references.md` (LA-25) both relax a
restriction on the **same operator** — `::`, "the non-instantiated side" (reference §3.4). They are
**two designs, not one**, split on every axis that matters (mechanism, risk, size, timeline). The
short version:

| | LA-18 (this doc) | LA-25 (sibling) |
|---|---|---|
| axis of `::` it generalizes | the **left operand** — a type *variable* (`A::…`), not a concrete type | the **result position** — value, not just call |
| mechanism | **demand-driven monomorphization** (new compilation mode) | checker synthesizes an eta-expansion lambda the backends already emit |
| why it is hard | **type arguments are erased on all five engines** (§3) — `A::` has no runtime carrier | none — reuses closures |
| risk / size | high, ~weeks | low, ~days |
| wanted by | AG-5 (Nov 25) | AG-2 (Oct 5) |

They **compose** (`A::FromJson` as a *value* in a generic body needs both) but neither needs the
other; the seam is §8. Full rationale for the split is in the sibling doc's "the `::`-generalization
pair" section.

---

## 1. The ask, and the trap in its framing

Inside a generic body, allow `::`-reached members of a **type parameter**, resolved when the
concrete type is known (request §1):

```
McpTool jsonTool<A>(string name, (A) => JsonValue handler) {
    return McpTool(name,
        (JsonValue raw) => handler(A::FromJson(raw)),   // labeled ctor via type param
        A::Empty().schemaJson());                        // any ::-member, same rule
}
```

The request describes this as "the C++-template model the language already uses for HKT bodies …
resolution-by-type with the type arriving via substitution, not a new mechanism." **That framing is
half right and the wrong half is load-bearing.** The *checking* is duck-typed-at-instantiation
today (info.md §9, HKT bodies). The *code generation* is **not** C++-template monomorphization —
this compiler compiles each generic body **once** and dispatches dynamically, and **it erases type
arguments** (§3). So `A::FromJson` — a *static* call needing only the type `A`, with no instance —
has, at runtime, **nothing that carries `A`**. This is why the request's own interim fallback is a
*witness parameter* (`fromDb<T>(DbValue, T witness)`): the witness exists solely to smuggle the
type in as a value, precisely because the *type* is gone at runtime. LA-18 therefore **does need a
new mechanism.** This design supplies it: **demand-driven monomorphization, above the IR.**

---

## 2. The one rule (philosophy fit)

`::` reaches "the non-instantiated side" of a type (reference §3.4). Today the left operand must be
a **statically-known concrete** type/namespace/base. LA-18 lifts that to a **type variable**,
resolved the same way generic *bodies* are already checked — leniently, then pinned at
instantiation (info.md §9). The general rule the two make ordinary:

> `T::member` resolves against whatever concrete type `T` is. When `T` is concrete, that is today's
> resolution. When `T` is a type parameter, "whatever concrete type" is **each type the generic is
> instantiated with** — and because the program is compiled **whole** (info.md §12/§17), that set
> is closed and enumerable, so each `T::member` resolves to a concrete target with **zero runtime
> cost** ("cost-identical to hand-written," info.md §16.5). No constraints system is requested; a
> missing member at instantiation is a compile error at the use site (request §1), exactly like an
> HKT body's duck-typed call that fails to resolve.

Monomorphization lives **above the IR**: specialization produces N *concrete* IR functions, and all
five engines already execute concrete functions. No engine learns a new concept — the single
semantic-lowering layer (info.md §17) is preserved.

---

## 3. Current behavior (verified against this tree — `build/lang`, 2026-07-06 binary)

House rule (overview §4.1): *"verified against this tree; nothing assumed."*

| # | probe / inspection | result | reading |
|---|---|---|---|
| **P-1** | `Foo make<A>(int n) => A::FromInt(n); make<Foo>(7)` | compile error **`unknown name 'A'`** at the `A::` site | a type-parameter name is **not a value/`::`-base name** today — LA-18 breaks first at *name resolution*, before any codegen question |
| **P-2** | `grep -niE 'typeArg\|reif' Ir.hpp RuntimeValue.hpp Eval.cpp` | **no matches** | **type arguments are erased** — no runtime value carries `A`, on *any* engine incl. the tree-walk oracle |
| **P-3** | `Checker.cpp:843–869` (`T(...)` / `T::Label(...)` resolution) | binds `ctorClass = bt.sym` (a **concrete class Symbol**) and records a concrete `Stmt*` in `call->resolved` | the existing static-call path **requires** a concrete symbol at check time; a type variable supplies none, and there is **no `CallStatic`-by-descriptor op** to fall back to (Ir.hpp has `NewObject`/`CallFn`/`CallDyn`/`CallNativeFn` only) |
| **P-4** | `Checker.cpp:58` `mentionsTypeParam`, `:1478` `substitute`, `SymbolKind::TypeParam` | present | the **type-level** substitution scaffolding LA-18 needs already exists — the gap is value-position resolution + specialized *lowering*, not type arithmetic |

The three facts that shape the whole design: **(P-1)** the checker must learn to resolve a
type-param name in `::`-base position; **(P-2 + P-3)** erasure + concrete-`Stmt*` static calls mean
there is no runtime path — the concrete target must be chosen *at compile time*, per instantiation;
**(P-4)** the checker's substitution machinery is already there to lean on.

---

## 4. The mechanism — demand-driven monomorphization, above the IR

Only generic functions/methods whose body **syntactically contains `X::…`** where `X` is one of
their own type parameters are affected. Everything else compiles once, exactly as today — **no
regression, no code-size cost where the feature is unused.**

**v1 scope (review 2026-07-11): `X` must be a type parameter of the enclosing *callable* — a
free/namespace/static-side function or a method-level parameter.** Two shapes are v1 **compile
errors**, not silent gaps: (a) `T::…` where `T` is a **class-level** type parameter (§7.6 — a
genuine soundness hole for monomorphization, not a deferral of convenience), and (b) `X::…`
inside an instance method that participates in **override dispatch** (§7.7). Neither consumer
(Track 07's `jsonTool<A>`, Track 03's decode helpers) uses either shape — both are free
functions.

### 4.1 Checker (M1) — resolve the type-param operand, defer the member

1. **Name resolution:** in `::`-base position, a name that resolves to `SymbolKind::TypeParam` is
   legal (fixes P-1's `unknown name 'A'`). `typeOf(A)` yields a *type-parameter type value*
   (`TKind::TypeValue` over the `TypeParam` symbol), not "unknown."
2. **Member typing (duck-typed):** `A::Label(args)` and `A::member` are typed **leniently at the
   definition site** — reusing the HKT-body leniency (info.md §9; the same path `substitute`
   already degrades gracefully, Checker.cpp `:1529`). The result type is the substituted type where
   recoverable, else the raw head (compatible with any instantiation), matching how HKT return
   types already degrade.
3. **Record the demand:** mark the enclosing function `specializationRequired` and record, per
   `X::member` site, the selector and whether it is a labeled-ctor / static / instance form.
4. **Collect concrete bindings:** the checker already infers concrete type arguments at every call
   of a generic (`inferConstruction`/`genericReturn`, Checker.cpp `:869,:887`). For a
   `specializationRequired` callee, record the concrete type tuple at each call site into a
   **specialization set** keyed by (function, type-arg-tuple). Collection runs on the **pass-2
   (post-rule-injection) tree** — rules (info.md §16.5) inject code before the re-resolve/check
   pass, so rule-generated call sites contribute tuples like hand-written ones; collecting on
   pass 1 would silently miss them.

### 4.2 Lower (M2) — emit one concrete body per instantiation

Instead of lowering one generic body, the lowerer, for each `specializationRequired` function,
emits **one specialized copy per distinct concrete type tuple** in its specialization set:

- In a copy with `A = Foo`, `A::FromJson` resolves through the **ordinary** concrete path
  (Checker.cpp `:843` logic, now run with `A` pinned to `Foo`) → a concrete `Stmt*` → an ordinary
  `NewObject`/`CallFn`. Zero runtime dispatch, zero erasure problem.
- Each **call site** is rewired to its specialized target (the (fn, type-tuple) key selects the
  copy). Non-`::` generic calls are untouched.
- **Dedup** by the (fn, type-tuple) key: two `make<Foo>` calls share one copy.
- **Transitive specialization:** if a `specializationRequired` function is called from *another*
  generic with `A` still abstract (the concrete type only known further up), the caller becomes
  specialization-required too, propagating the concrete tuple down. The graph is finite (finite
  functions × finite whole-program types) so it terminates; §7.1 is the STOP guard for a
  pathological non-terminating shape.

Because specialization happens **above the IR**, each specialized copy is just another concrete
function in the function table — **oracle, IR, emit-C++, LLVM, and ELF all consume it with no new
op** (info.md §17). This is the design's core leverage.

### 4.3 Diagnostics (M3) — missing member at instantiation

When a specialization for `A = Foo` finds no `Foo::FromJson`, emit a compile error **naming both
the use site and the concrete type** (request acceptance #1):

```
error: type 'Foo' (instantiating 'A' of 'jsonTool<A>') has no labeled constructor 'FromJson'
  --> the reference: jsonTool.lev:12  A::FromJson(raw)
  --> instantiated here: main.lev:40  jsonTool<Foo>(...)
```

Two spans — the `A::` site and the instantiation site — because the fault is the *pairing*, exactly
as the request's acceptance demands. This is the duck-typed-body failure mode made legible (info.md
§16.5 notes span attribution for synthesized/late-resolved nodes is a known soft spot; this design
must not regress it — §7.3).

### 4.4 Why not the alternatives (recorded, so they are not re-litigated)

- **Type-argument reification** (pass a runtime type descriptor for `A`; `A::FromJson` dispatches
  through it): adds a calling-convention change + a descriptor value kind + a static v-table on
  **all five engines**, and it carries **runtime cost** — contradicting "cost-identical" (info.md
  §16.5). Only justified if the instantiation set were *open* (separate compilation). The language
  is **whole-program** (info.md §12/§17), so the set is closed and monomorphization wins.
  **Fallback only** if §7.1's termination guard fails in a way specialization cannot express.
- **Formalized witness passing** (auto-generate Track 03's manual `T witness`): still a
  calling-convention change + runtime cost; a strictly weaker version of reification. **Not
  chosen.** (It is, however, exactly the *interim fallback* that already exists in the tree, §9.)

---

## 5. Engine coverage (overview §4.3)

- **oracle + IR:** mandatory. The specialization set is built from whole-program call sites; both
  execute the concrete copies. Full coverage — this is where M2/M3 land first.
- **emit-C++:** full (concrete functions).
- **LLVM:** full — request acceptance #1 names oracle/IR/**LLVM** as the differential triple.
  Concrete copies emit through the existing function path (info.md §17).
- **ELF:** the whole-language backend, but per the **freeze** (overview §2.1, the portable-backend
  ruling): **do not extend `X64Gen.cpp`/`X64.hpp` for new work.** If specialization needs no
  X64Gen change (it should not — specialized copies are ordinary functions the ELF backend already
  lowers), ELF coverage is free; if any site *would* require an X64Gen edit, **exclude ELF with an
  explicit diagnostic** and an isolated corpus dir, following Track 04's pattern (overview §2.1).
  Never silence.

---

## 6. Milestones (ordered; one implementer)

- **M0 — probe & scope (before code).** Re-run P-1..P-4 against the *current* tree (the binary here
  is the Jul-6 build; rebase first). Additionally probe: is the whole-program instantiation set for
  the Track 07 MCP shape (`jsonTool<A>` called with 2+ concrete `A`) actually enumerable at lower
  time, and does a `specializationRequired` function ever get called only through *another* generic
  (forcing transitive specialization, §4.2)? If the instantiation set is **not** statically
  enumerable for a required shape → **STOP** (§7.1) and escalate; the reification fallback (§4.4) is
  the escalation's subject, not an improvised pivot.
- **M1 — checker: operand + deferral.** Type-param name legal in `::`-base position (P-1 fixed);
  `A::Label(args)`/`A::member` typed duck-style; specialization demand + per-call concrete tuples
  recorded. No lowering yet — a compile-only milestone whose test is: the def-site checks, and the
  instantiation set is correctly collected (assert via a `--specializations` dump, mirroring
  `--why`/`--namespaces`, overview §12.8-style tooling).
- **M2 — lower: specialization.** Emit concrete copies per tuple; rewire call sites; dedup;
  transitive propagation. `A::FromJson` (labeled ctor) and a plain `A::member` on **2+
  instantiations**; oracle + IR differential-green (request acceptance #1, first two engines).
- **M3 — value structs AND classes + diagnostics.** Prove `A` = a value `struct` and `A` = a
  reference `class` both work (request acceptance #2); missing-member instantiation = the
  two-span error (§4.3). LLVM added to the differential triple. Checker tests for the error.
- **M4 — emit-C++ + ELF + reference.md.** emit-C++ coverage; ELF per §5 (free, or excluded with a
  diagnostic + isolated corpus); `docs/reference.md` §2.5 updated in the same commit (overview
  §2.2). Track 07's generic MCP adapter compiles and dispatches over 2 concrete tool types in the
  hermetic corpus.

Each milestone ends on the overview §4.3 acceptance block (build, `ctest`, corpus, ELF, native,
churn 13/13 + XFAIL, lcurl smoke).

---

## 7. Foreseeable problems & STOP points

### 7.1 Non-enumerable / non-terminating instantiation set (**hard STOP** — §4.2, M0)
Monomorphization assumes the (fn, type-tuple) set is finite and statically known. Whole-program
compilation guarantees this for direct calls; transitive specialization through nested generics
must **terminate** (finite functions × finite types). If a shape produces an unbounded set (e.g. a
generic that instantiates itself with a strictly larger type each level — `A` → `Box<A>` → …), that
is genuinely unrepresentable by monomorphization. **STOP**, log the shape, escalate: the reification
fallback (§4.4) is the design correction, not an in-place improvisation (overview §4.2). The Track
07/03 consumers do **not** exhibit this shape (they instantiate with concrete DTO types), so v1 may
**bound the specialization depth** and error clearly past it, deferring unbounded recursion to a
reification follow-up.

### 7.2 HKT interaction (decided: forbid in v1)
`::` on a **higher-kinded** type parameter (`F` of kind `* -> *`, info.md §9) has no member surface
until applied. v1 permits `::` only on `*`-kinded params; `F::something` is a clear compile error
("`::` on a type-constructor parameter is not supported"). Revisit only if a consumer needs it.

### 7.3 Span attribution for specialized bodies (soft — §4.3)
Specialized copies are late-materialized; diagnostics inside them (a runtime throw, a secondary type
error) must attribute to the **original source span**, not a synthesized location. info.md §16.5
flags this as an existing soft spot for `$init`/rule-injected nodes — reuse whatever span-carrying
mechanism those use; do not regress it. Add a corpus case whose specialized body throws and assert
the report points at the original line.

### 7.4 Interaction with the metaprogramming interim fallback (coordinate, don't collide)
Track 07/03's current adapters are **generated by rules** (per-arity/per-type, info.md §16.5). LA-18
replaces them. During the overlap, a class may have **both** a hand/rule-generated `FromJson` and be
used as `A::FromJson` — ensure specialization resolves to the *declared/generated* member (they are
the same concrete `Stmt*` by then), and that deleting the rule post-landing is a clean removal. This
is a sequencing note for the Atlantis tracks, not new compiler work.

### 7.5 Bare auto-construction of `A` is a *different* open question (do not conflate)
info.md §3/§19 #2 asks "does every `T` have a default for bare `A x;` construction?" — a *constraint*
question. LA-18 is **not** that: `A::FromJson(raw)` is an **explicit** labeled call, not bare
auto-construction. Keep them separate; LA-18 requires no defaultability constraint on `A`.

### 7.6 Class-level type parameters (**decided: v1 compile error** — soundness, not scope-trimming)
Monomorphization requires the concrete tuple to be **statically known at every call site**. For
*function-level* params, whole-program compilation guarantees that (the checker infers the tuple
at each call). For **class-level** params it does not, because class generics are erased **and the
raw (unparameterized) form is legally compatible with any instantiation** (info.md §9):

```
class Holder<T> { Foo mk() => make<T>(7); }     // T::… demanded transitively via make<T>
Array<Holder> mixed = [ holderOfFoo, holderOfBar ];   // raw form — legal today
mixed[i].mk();                                   // static type: raw Holder. Tuple: unknowable.
```

The instance carries no tuple at runtime (erasure, P-2), and the static type has been legally
widened to raw — so the demand set is **not enumerable** at that call site even with whole-program
visibility. Supporting this would mean monomorphizing the *class* (per-tuple shapes, banned raw
widening) or per-instance type descriptors — i.e. reification (§4.4), a different design. v1:
`T::…` where `T` resolves to a **class-level** type parameter is a compile error —
`"'T' is a class-level type parameter; '::' on type parameters is supported for function-level
parameters only (v1)"` — at the `T::` site. The M1 checker must make this distinction explicitly
(a `SymbolKind::TypeParam` alone does not say which kind it is; check the declaring scope).
Revisit only via the reification fallback if a consumer needs it.

### 7.7 Instance methods under override dispatch (**decided: v1 compile error**; mangled-copy plan recorded)
Instance-method calls lower to **`CallDyn`-by-name** (Lower.cpp `:1172,:1287,:1324`) — dispatch
picks the body from the *receiver's runtime class*. "Rewire the call site to the specialized
copy" (§4.2) is straightforward for `CallFn` free functions; for a method it means dispatching on
a **mangled selector** (`m$Foo`), which stays override-correct only if **every override in the
hierarchy** is specialized for every demanded tuple and registered under the same mangled name.
That set is closed and enumerable whole-program, but it is real machinery (demand propagation
across the override set) that no consumer needs. v1: a `specializationRequired` **method-level
generic on an instance method** is allowed only when the method neither overrides nor is
overridden anywhere in the program (the checker knows the hierarchy); the lowerer then emits
mangled-name copies and rewires the checker-resolved `CallDyn` (`in.decl` is already recorded,
Lower.cpp `:881`). If the method participates in override dispatch → compile error naming the
method and the overriding class. STOP and escalate if Track 07/03 turn out to need it; the fix is
the override-set propagation above, not reification.


---

## 8. Composition with LA-25 (the seam)

Once **both** land, `A::member` in **value position** inside a generic yields a function value:

```
(JsonValue) => A  decoder<A>() => A::FromJson;   // LA-18 type-param operand + LA-25 value position
```

**No new interaction to design.** LA-18 specializes the body per concrete `A`; inside each copy
`A::FromJson` is a *concrete* labeled-ctor reference, so LA-25's eta-expansion produces an ordinary
concrete closure — the `User::FromJson` case LA-25 already covers. Division of labor: **LA-18 owns
"`::` on a type variable → a concrete member per instantiation"; LA-25 owns "callable-in-value-
position → a function value."** Their product is one concrete closure per instantiation. Neither
blocks the other:

- LA-18 alone: `A::FromJson(raw)` **called immediately** — the MCP/decode path (request §2). Fully
  useful with zero LA-25.
- LA-25 alone: references on **concrete** types — the R10 route table. Fully useful with zero LA-18.

LA-25 is scheduled first (AG-2 < AG-5), so LA-18 inherits value-position type-param references for
free when it lands. Order-independent.

One composed shape is a **compile error, not a seam** (review 2026-07-11): a reference *to* the
`specializationRequired` generic itself (`var f = jsonTool;` — LA-25 value position, LA-18 callee)
has no concrete type tuple, so there is no copy to point the closure at. This falls out of LA-25's
§8.6 rule (references to generic callables are v1 errors) with no LA-18-side work — noted here so
neither implementer thinks the other handles it.

---

## 9. Interim fallback (already in the tree — request §4)

Both work today; both are **deletable on landing**:

- **Per-arity/per-type generated adapters via rules** (Track 07 §3.5 ladder, info.md §16.5): a rule
  generates a concrete adapter member per tool type instead of one generic `jsonTool<A>`.
- **Witness parameters** (Track 03's `__atlDec(src, keys, idx, T witness)` overload set,
  `designs/atlantis/techdesign-03-serialization.md` §3): the type is smuggled as a *value* argument
  and overloads dispatch on it — the exact contortion LA-18 removes. That the fallback exists *is*
  the proof (P-2) that type arguments are erased: nobody threads a witness for information the
  runtime already had.

Acceptance for deletion: the generic form produces output byte-identical to the generated/witnessed
form on oracle/IR/LLVM (request acceptance #1).

---

## 10. `docs/reference.md` deltas (ship in the feature commit — overview §2.2)

- **§2.5 (Generics):** add that inside a generic body, `T::member` (any `::`-reached member of a
  type parameter — labeled constructor, static function, method) is legal and resolves **per
  instantiation** (duck-typed at the definition site, pinned when the concrete type is known); a
  member absent on an instantiating type is a compile error naming the use site and the concrete
  type. Note the whole-program-monomorphization implementation ("cost-identical to hand-written")
  and the v1 limits (§7.1 bounded depth, §7.2 `*`-kinded params only, §7.6 function-level type
  parameters only — class-level `T::…` is an error, §7.7 no override-dispatched methods).
- **§3.4 (Member access):** cross-reference — `::`'s left operand may be a type parameter inside a
  generic body.

---

## 11. Implementation log

- **2026-07-12 — pre-work sync / M0.** Rebasing `agent3` on `origin/master` was a no-op;
  implementation started at `a6448f7`. P-1 reproduced on the current binary with the equivalent
  inference-supported spelling `A make<A>(A witness, int n) => A::FromInt(n); make(foo, 7)`:
  `unknown name 'A'` at the `A::` site. (The design's explicit `make<Foo>(7)` probe spelling is
  stale for function calls in this parser; function type arguments are inferred.) P-2 found no
  type-argument/reification carrier in `Ir.hpp`, `RuntimeValue.hpp`, or `Eval.cpp`; P-3 confirmed
  construction still records a concrete `Stmt*`; P-4's `mentionsTypeParam` / `substitute` /
  `SymbolKind::TypeParam` substrate remains present.
- **M0 enumeration result:** direct calls produce a closed `(callable, concrete tuple)` set.
  The required transitive shape is enumerable too: an abstract `outer<A> -> inner<A>` edge marks
  `outer` specialization-required; checking `outer<N>` records `inner<N>`. Dedup rewires recursive
  or repeated demands to the existing copy. A v1 depth guard errors after 32 specialization edges.
  No hard STOP condition fired.
- **M1–M3:** `Checker` marks callable-level `T::member` sites, checks definitions duck-style,
  records post-rule-pass concrete demands, clones/substitutes concrete callable bodies to a fixed
  point, and rewires calls to those copies. The original erased body is never emitted. Concrete
  copies preserve authored spans; a missing labeled constructor/member emits an error at the
  `T::` site plus an instantiation note at the concrete call. `--specializations` dumps the
  deduplicated set. Class-level params and HKT heads are rejected as designed; generic instance
  methods work only outside override sets and lower as direct specialized calls.
- **M4 / engines and docs:** the concrete copies are ordinary evaluator functions / IR functions.
  No IR opcode or backend-specific emitter change was needed. Added
  `tests/corpus/generic_static_members` for reference classes, value structs, two tuples,
  LA-25 value-position composition, transitive specialization, and an instance method; added
  checker coverage for missing members and every v1 restriction. Updated `docs/reference.md`
  §§2.5/3.4.
- **Acceptance:** build green; unit/checker/eval/meta plus all five dedicated corpus lanes green
  (oracle, IR, emit-C++, LLVM-native, frozen ELF); churn **15 guarded PASS + 2 expected XFAIL**;
  lcurl plan/check green and the localhost smoke reached its expected connection-refused path.
  A full unfiltered `ctest` was attempted, but current master stalls in the unrelated legacy
  root `tests/corpus/async.ext` tree-walk entry (isolated with `bash -x`; still stalls under
  `LANG_PUMP=1`) after the LA-30 suspension flip. The LA-18 lanes and all front-end suites pass;
  no LA-18 program is involved in that aggregate-lane stall.
