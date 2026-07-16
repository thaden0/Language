# Tech Design: Covariant-Return Interface Satisfaction

**Status:** implemented. **Date:** 2026-07-12. **Feature ID:** F6 (Sonar prerequisite).
**Depends on:** none. **Unblocks:** Sonar's fluent contracts (anchor C5: `IContainer add(IComponent)` satisfied by `Container add(IComponent)`).
**Difficulty:** M. **Risk:** MED — tiny code delta, but the design IS the reconciliation with slot merging and runtime dispatch.

Frozen ruling: anchor §4/F6. Ground truth: infodemp §7 (re-verify lines at implementation).

---

## 1. Motivation

Fluent builder chains must stay concrete-typed (`box.add(a).add(b)` returning `Container`, not decaying to the interface), while the interface still declares the verb. Today that combination is a hard error: interface satisfaction compares **full canonical signature strings by exact equality** (`Resolver.cpp:5280`), and a method canonical embeds its return type (`slotOf`, `:5138-5144`) — so `add : (IComponent) -> Container` fails the requirement `add : (IComponent) -> IContainer` with *"does not satisfy interface: missing ..."*. The framework fallback (queries-only interfaces) exists, but the language feature is the chosen path (aggressive-pursuit posture).

## 2. Current state (the three interacting constraints)

1. Interface bases contribute **`interfaceReqs`, not merged slots** (`Resolver.cpp:5258-5261`) — requirements never allocate and never create slots. This is the load-bearing fact: relaxing the *comparison* cannot create a second slot, because the requirement side never contributed one.
2. `mergeSlot` (`:5152-5184`) keys on name + full canonical; same-name **different**-canonical members coexist as separate slots (`:5183`).
3. Runtime dispatch is name+arity only (`IrInterp.cpp:27-47`); overridden overload sets sharing an arity are a compile error (§3.4a, `Checker.cpp:269-291`). Any design leaving two same-arity `add` slots on one class would be broken; the frozen ruling avoids that by construction.

The subtype predicate needed (`assignable`/`isSubclass`) exists on **Checker** (`Checker.cpp:296-318`, `:212-219`; interfaces live in `decl->bases`, so `isSubclass(Container, IContainer)` is already true) — but satisfaction runs in **Resolver** over canonical strings, and `Slot` carries no structured return type. That representation gap is the implementation question.

## 3. Design (the frozen ruling, made mechanical)

**Ruling:** an interface method requirement is satisfied — **and consumed, contributing no second slot** — by a class method with the same name, identical parameter canonicals, and a return type **assignable to** the required return. Parameters stay invariant. Scope v1: **interface satisfaction only**; class-override covariance is an explicit non-goal (today a derived same-name different-return method silently *coexists* as a separate slot rather than overriding — a footgun boundary this design documents with an example test, and deliberately does not extend).

### 3.1 Mechanics — Resolver-local assignability over bare names (chosen)

Options: (a) a Resolver-local subtype walk; (b) thread structured return `Type`s into `Slot`; (c) move satisfaction checking post-resolve into Checker. **(a) chosen**: no representation change, no phase reshuffle; (b) recorded as the v2 path if variance work grows.

- **Canonical split:** compare `"(params) -> ret"` structurally. Splitting the STRING at the last top-level `" -> "` is fragile against nested function types (`((KeyEvent) => void) -> int`) — instead, **record the boundary at construction**: `slotOf` builds the canonical by concatenation; capture `paramsCanon` and `retCanon` on the `Slot` at that moment (two extra strings, no structural `Type`). Requirements get the same treatment via the identical construction path.
- **Assignability, v1 containment:** covariance applies only when BOTH return canonicals are **bare declared-type names** (a class or interface identifier, plus the one sanctioned suffix form below). Resolution: look up each name among resolved decls; accept if `provided == required` or `required` appears in `provided`'s transitive `decl->bases` walk (mirroring `isSubclass`). Everything else — unions, generics-parameterized returns, function types, primitives — stays exact-equality. This containment is the safety valve: no string-parsing of complex canonicals, ever.
- **Optional returns:** `retCanon == required + "?"`-shaped satisfaction (`T` provided where `T?` required) is admitted — recognized as the suffix special case (`provided + "?" == required` or provided's base-walk hits `required`-minus-`?`). Cheap, useful (e.g. a class returning a concrete value where the interface admits absence). Anything more union-shaped stays exact.
- **Generic interfaces:** the comparison runs on requirement canonicals AFTER `substituteSlotGenerics` (`:5211-5215/:5253-5257`) — i.e., exactly where the exact-equality check runs today; ordering unchanged. Test: `interface R<T> { R<T> self(); } class C : R<int> { C self(); }` satisfies (C's bases-walk reaches `R<int>` post-substitution — the walk compares substituted base spellings; pinned by test).
- **Multiple assignable candidates** (a class declaring both `Container add(IComponent)` and `IContainer add(IComponent)` — legal today as coexisting slots): the requirement is satisfied if ANY candidate's return is assignable; **first-declared wins** as the satisfying slot (the language's universal tie-break). Note honestly: such a class already trips the §3.4a shared-arity error at dynamic call sites, so the pattern is near-unusable regardless — the tie-break rule just keeps satisfaction deterministic.

### 3.2 The three constraints, discharged

1. `mergeSlot` untouched — requirements still contribute no slots; only the comparison at `:5277-5285` changes.
2. §3.4a: exactly one `add` slot exists (the class's); calls through an `IContainer`-typed receiver dispatch name+arity to it; through `Container`-typed: static/devirtualized per landed rules. No new runtime ambiguity is possible because no new slot is possible.
3. Runtime never disambiguates by return type — and never needs to, per (2).

### 3.3 Diagnostics

Genuine misses keep the current error. Near-miss gets a note: `"'Widget' does not satisfy interface: 'add : (IComponent) -> Widget' found, but return type 'Widget' is not assignable to required 'IContainer'"`. The distinction costs one extra comparison pass and pays for itself in every fluent-API onboarding.

## 4. Implementation plan

| M | step | difficulty |
|---|---|---|
| M1 | `Slot` gains `paramsCanon`/`retCanon` captured in `slotOf` (+ the requirement path) | S |
| M2 | Resolver-local `returnAssignable(providedRet, requiredRet)` (bare-name bases walk + the `?` suffix case) + relax the `:5280` comparison to params-equal ∧ return-assignable | M |
| M3 | Near-miss diagnostic + multiple-candidate tie-break | S |
| M4 | Corpus (incl. generic-substitution ordering) | S/M |

Zero engine changes (Resolver/Checker only); all four lanes agree by construction.

## 5. Edge cases

`distinct` on the satisfying method (distinct slots still satisfy — satisfaction reads slots, distinct affects merging between BASES; pinned); an interface extending an interface with a REFINED return for the same method (two requirements, one class method satisfying both via assignability — walks independently, both pass); a requirement whose return is the interface ITSELF (`IContainer add(...)` — the Sonar case; self-referential names resolve normally); primitives/void returns (exact-equality path, unchanged); parameter differences of any kind → still "missing" (invariance).

## 6. Potential issues & mitigations

1. **Pure relaxation, but error-text regressions** — genuinely-missing-member errors must not degrade; M4 includes red tests asserting the classic message survives.
2. **Bare-name resolution ambiguity** (same type name in two namespaces): the canonical strings are produced by the resolver with its own qualification conventions — the bases-walk must compare canonicals AS PRODUCED (same-namespace-qualification on both sides), not re-resolve names loosely. Implementation note: compare against the base-decl canonical spellings recorded during resolution, not raw source names.
3. **Silent behavioral widening** — previously-erroring code now compiles; that is the feature. No compat risk (nothing working changes meaning).
4. **The class-override non-goal being mistaken for an oversight** — the coexistence-semantics example test (a derived class "narrowing" a base method's return produces two slots + the arity hazard) documents the boundary executably.

## 7. Testing plan

Green: the Sonar fluent case (interface verb + concrete return, chain stays concrete-typed); `T` for `T?`; generic-substituted covariance; two interfaces/one method; distinct-satisfier; multi-candidate first-declared. Red: unrelated return (near-miss note text pinned); param mismatch (classic message pinned); union-return attempt (exact path). Runtime: dispatch-through-interface on all four engines (byte-identical); the class-override boundary pin. All in `tests/corpus/` per house layout.

## 8. Open questions

1. Structured `Type` on `Slot` (option b) — revisit if parameter variance or richer return shapes are ever wanted.

## 9. Implementation log

- 2026-07-12 — design written; not started.
- 2026-07-12 — implemented M1–M4 in `Slot`/`Resolver`: callable boundaries are
  recorded without reparsing signatures; interface satisfaction walks resolved declared-type
  bases with transitive generic substitutions; optional `T?` uses its canonical `T | None`
  spelling; near-miss return diagnostics preserve the classic missing-member diagnostic for
  parameter mismatches. Added checker/resolver boundary tests and focused corpus coverage for
  fluent/interface dispatch, optional returns, substituted generics, refined/twin interfaces,
  `distinct`, and deterministic multiple candidates. Tree-walk, IR, emit-C++, and LLVM pass;
  no frozen-ELF lane.
