# Tech Design: Bound Method References (`obj.method` as a value)

**Status:** complete. **Date:** 2026-07-12. **Feature ID:** F3 (Sonar prerequisite).
**Depends on:** LA-25 unbound references (landed), class-method dispatch (landed). **Unblocks:** Sonar handler ergonomics (T04/T06/T07 — `box.onKey(this.onKeyDown)`, `@Shortcut`'s `this.$m`).
**Difficulty:** M. **Risk:** LOW/MED (the one risk: removing a lenient-unknown checker branch).

Frozen ruling: anchor §4/F3. Ground truth: infodemp §4 (the complete delta map; re-verify lines at implementation).

---

## 1. Motivation

LA-25 shipped `Class::method` (unbound — receiver becomes param 0). Handlers want the bound form: `menuItem.onSelect(this.save)` instead of `menuItem.onSelect(() => this.save())`. The two are semantically identical by the LA-25 §8.8 ruling (a reference IS its eta-expansion lambda), so this feature is pure ergonomics with zero new dispatch machinery — which is why it's checker-only.

## 2. Current state (verified map)

- LA-25 is a **checker-only in-place rewrite**: a value-position `::`-callable becomes an eta-expansion `Lambda` node; every engine runs the ordinary lambda path. Zero Lower/Eval/IrInterp/LlvmGen/CGen code.
- Entry `Checker::tryResolveMethodRef` (`Checker.cpp:1062`), self-gated `if (!e->colon) return unknown();` (`:1064`).
- **The hook point:** the instance-`.`-member branch at `Checker.cpp:910-917` — a method member read without a call currently returns lenient `wrap(unknown())` (`:915`). `var f = obj.method` silently types unknown today; downstream it lowers as a data-slot `GetMember` read (`Lower.cpp:1595-1602`) — a latent hole, not a feature.
- Rewrite machinery: `rewriteAsMethodRef` (`:989-1060`) — synthesizes `$mr0..$mrN` params, body = a `Call`; `methodRefCanonical` (`:938`) omits the receiver when `recvCanon` is empty (`:942`); the free-function branch (`:1002-1006`) already reuses `e->a` verbatim as the callee base — the exact shape the bound variant needs.
- Closure capture is **by name** over visible locals (`Lower.cpp:1380-1383`), `this` under the literal name "this" (`:1389-1391`) — so a rewritten lambda whose body mentions a local or `this` captures it with zero new lowering.
- Value/arg/assignment gates: `typeOfMember :829-833`, `typeOfBinary :1470-1473`, arg positions + overload deferral `isDeferredMethodRefArg :1144`.
- Dispatch: `resolveDispatch` (`:282-294`) applied at rewrite time — landed class-method dispatch means the synthesized call dispatches on the runtime class, devirtualized when closed.

## 3. Design (the frozen ruling, elaborated)

**Surface:** `obj.method` in value position (variable init with declared type, argument, assignment RHS, container element with target type) is a bound function value whose type is the method's signature minus the receiver: `void save()` on `Editor` → `() => void`.

**v1 receiver restriction (the one real decision, frozen):** the receiver expression must be a **bare local identifier or `this`**. `a.b.method`, `this.field.method`, `getFoo().method` → compile error: *"bind the receiver to a local first"* + a note showing the fix (`var r = a.b; use r.method`). Rationale: capture-by-name snapshots a local/`this` correctly for free; an embedded receiver EXPRESSION left in the lambda body re-evaluates per call (silently wrong), and hoisting to a temp needs statement context the in-place rewrite doesn't have. A crisp teachable rule beats a subtle capture bug.

**Semantics:**
- Bound = reference semantics on the receiver: the lambda captures the local's VALUE (an object reference); later mutation of the object is visible to calls (same object); rebinding the LOCAL after ref creation is not (snapshot of the reference). Pinned by test.
- Fresh value per evaluation, no identity (LA-25 parity) — removal-by-token APIs (Sonar R12) are the consequence, already designed for.
- No generic methods (LA-25 parity); no bound refs to constructors (labels are already `Type::Label` — unbound territory).
- Overloaded methods resolve against the **target function type** exactly like LA-25, reusing the deferral machinery; overloaded with no target = compile error.
- Dispatch: definitionally the eta-expansion — interface-typed OR overridden-below receivers dispatch on the runtime class; closed sets devirtualize. No new rules; any future dispatch pivot flows in automatically (the §8.8 rationale, restated).

## 4. Implementation plan

| M | step | difficulty |
|---|---|---|
| M1 | Type side: at `Checker.cpp:915`, when the resolved member is a callable method slot, return the bound signature (`methodRefCanonical(m, "", ret)`); keep leniency for genuinely-unknown members (dynamic-property reads) — the discrimination is "resolved to a method slot on the receiver's static type" vs "not found" | M |
| M2 | Rewrite side: `rewriteAsMethodRef` bound variant — validate receiver shape (Name-of-local/param or This) BEFORE rewrite, error otherwise; drop the `$mr0` receiver param; splice `e->a` as the callee base (the `:1002` pattern) | M |
| M3 | Gate extensions: admit `!e->colon` instance cases at `:1064`, `:829`, `:1470`, `:1145` | S |
| M4 | Diagnostics (the restriction error + fix-it note) + `--expand` verification (rewrites are source-shaped like LA-25's) | S |
| M5 | Corpus | S |

Engines: **no changes** — all four lanes run the ordinary lambda output. ELF untouched.

## 5. Edge cases (each with the decided behavior)

- Optional receiver (`Focusable? f; f.method`) → must narrow first (ordinary member-access-on-optional error).
- `this.method` inside a constructor → legal (`this` exists).
- Bound ref inside a lambda: the outer lambda's capture sweep sees the receiver local/`this` — nesting composes (verified against `lowerLambda`'s visible-locals snapshot).
- Field-holding-closure vs method of the same name: fields win member resolution today (a slot read); the M1 discrimination keys on the member KIND — a closure-typed FIELD read stays a field read (unchanged), only method slots rewrite. Pinned by test.
- Assignment TARGET `obj.method = x` — unchanged (a field write; methods aren't assignable).
- Inside `spawn`: the ref captures the receiver by name; copy-always applies at the boundary like any lambda (bug #3's interpreter over-capture caveat applies equally — no new exposure).
- Shadowing: the name resolves at the ref site's scope, standard rules.

## 6. Potential issues & mitigations

1. **Removing the `:915` leniency** may re-type existing code that read a method member without calling. Mitigation: grep the corpus for `var x = <expr>.<methodName>`-without-call patterns pre-implementation; the old behavior was a silent unknown feeding a data-slot read (a latent bug), so any hit was already broken — migration note, not compat shim.
2. **Error-message quality is the feature's UX** — the restriction error MUST show the two-line fix. M4 acceptance criterion.
3. **bug #34 adjacency** (lambda-literal overload scoring): bound refs enter overload scoring as typed function values (like LA-25 refs), NOT as lambda literals — they take the non-buggy path. Verify which scoring branch they hit; pin with a test mixing a string overload.
4. **Deferral loops** — overloaded bound ref as an argument to an overloaded callee: reuse `isDeferredMethodRefArg`; LA-25's machinery generalizes (same shape). Test the double-overload case.

## 7. Testing plan

Corpus (all four lanes, checker-only ⇒ byte-identical): value/arg/RHS positions; overloaded target resolution; `this` receiver in ctor + method; mutation-after-ref (same object) vs local-rebind-after-ref (old object); restriction errors (`a.b.method`, `call().method`) with message text pinned; optional-receiver narrowing error; interface-typed local dispatching on runtime class; devirtualized concrete case; no-generic error; spawn capture; `--expand` round-trip.

## 8. Open questions

1. v2: arbitrary-expression receivers via checker-inserted hoisting (needs statement-context plumbing) — deferred until the restriction demonstrably hurts.

## 9. Implementation log

- 2026-07-12 — design written; not started.
- 2026-07-12 — implemented as a checker-only extension of LA-25. Bound refs accept bare
  locals/parameters and `this`, preserve closure-field precedence, reuse target-typed overload
  deferral (including Array element context), and synthesize the ordinary lambda/call path with
  runtime dispatch. Added focused checker diagnostics and a four-engine + `--expand` corpus;
  no lowering, evaluator, IR, C++, LLVM, runtime, or frozen-ELF changes.
