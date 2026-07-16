# Tech Design — Method References as Values (LA-25)

**Status:** design accepted, ready for implementation. **Date:** 2026-07-10.
**Review (2026-07-11):** approach confirmed; probes P-1/P-2/P-5 re-verified against the tree at
`2cb4888` (Jul-6 binary) and all line references re-checked. Review added the expected-type
mechanism note (§2.2), the argument-position deferral consequence for M3 (§6), and two decided
restrictions: generic-callable references (§8.6) and function-value identity (§8.7).
**Track:** single track, ordered milestones M1 → M4, **one implementer**. Not a fan-out —
value-position resolution, overload disambiguation, and eta-expansion lowering are one small
checker story that must land together.
**Source (the *why*):** `designs/request-method-references.md` (LA-25); Atlantis Track 02 routing
ruling R10 (`designs/atlantis/techdesign-02-routing-controllers.md` §0R), Track 07 client
aspiration. Indexed in `designs/atlantis/techdesign-00-overview.md` (LA table, `:238`).
**Consumers:** Atlantis Track 02 (the R10 canonical route table
`Route('/login', AuthController::Login)`), Track 07 (typed client method binding). **Priority
P1, wanted by AG-2 (2026-10-05).**
**Convention docs:** `designs/complete/techdesign-00-overview.md` governs this work
(contracts, engine-coverage policy, STOP-and-escalate, probe-before-code, commit discipline) —
read it first.

### The `::`-generalization pair (read this)

This design and its sibling `designs/complete/techdesign-generic-static-members.md` (LA-18) both relax a
restriction on the **same operator** — `::`, "the non-instantiated side" of a type/namespace
(`docs/reference.md` §3.4). They are deliberately **two designs, not one**, because they split on
every axis that matters:

| | LA-25 (this doc) | LA-18 (sibling) |
|---|---|---|
| axis of `::` it generalizes | the **result position** (value, not just call) | the **left operand** (a type *variable*, not a concrete type) |
| mechanism | checker recognizes the form + **synthesizes an eta-expansion lambda** the existing lowerer already emits | **demand-driven monomorphization** (a new compilation mode) |
| new runtime machinery | **none** — reuses closures + `CallDyn` (probe P-5 proves the target lowering already runs) | specialized function bodies per concrete type argument |
| risk / size | low, ~days | high, ~weeks |
| wanted by | AG-2 (Oct 5) | AG-5 (Nov 25) |

Bundling a days-long checker feature with a weeks-long monomorphization feature into "one work
unit, one implementer" (the overview's design-unit norm) would be wrong packaging and would couple
a November deliverable to an October one. They **compose** (`A::FromJson` as a *value* inside a
generic body needs both) but neither needs the other; the seam is one section (§9). Both touch
`Checker.cpp`, so per overview §2 they serialize anyway — two sequenced designs is the honest
shape.

---

## 1. The one rule (philosophy fit)

info.md §6.5 is the whole design in one sentence: **a member is a typed slot at a label; some of
those types are executable.** Reading a field member in value position yields the field's value.
Reading a *method* member — whose type is callable — must therefore yield the *callable value*.
Today it doesn't: a `::`-reached method is usable **only if immediately applied** (`Type::m(args)`).
That "must apply immediately" rule is the special case. LA-25 deletes it:

> A `::`-reached **callable member in value position is a first-class function value** — the same
> way a `::`-reached field in value position is a first-class value. "Resolution is by type"
> (info.md §1) reaches the slot; the slot's value is what a read yields, executable or not.

There is no new construct and no new runtime concept. The reference is **honesty about intent plus
arity-safe brevity** (request §1): `AuthController::Login` *is* the function
`(AuthController c, ILoginRequest r) => c.Login(r)` — the design just lets you name it instead of
restating its signature by hand.

### 1.1 Unbound is the honest reading

An instance method carries an implicit `this`. Reached through the **type** (`AuthController::Login`),
there is no instance to bind `this` to — so the receiver becomes the function value's **first
parameter** (request §1):

```
C::m        where  m : (P...) => R  declared on receiver C
            yields  (C, P...) => R                 // receiver-first, UNBOUND
```

This is not a convention imposed on `::`; it is the same rule as everywhere else — `::` is "the
non-instantiated side" (reference §3.4), so a method reached through it has no bound instance, so
the receiver is an ordinary parameter. A **bound** reference (`someInstance.Login` as a value, the
receiver captured) is the natural `.`-sibling and is **explicitly out of scope** (§8.4): the
request asks only for the unbound `::` form, and the bound form raises capture/lifetime questions
this design need not open.

Static-side and namespace functions (info.md §6.6) have no `this`, so they yield their signature
directly with no receiver parameter:

```
App::Routes::addRoutes    yields  (Atlantis::App) => void
```

Labeled constructors (info.md §2) are callable members selected by label, so they fall out of the
same rule for free (request §1, "nice-to-have"): `User::FromJson` yields `(JsonValue) => User`.
This is the exact seam LA-18 reuses (§9), so this design **includes it** rather than deferring it.

---

## 2. Surface syntax

No new tokens. The reference is an ordinary `::`-qualified name (already a postfix operator,
reference §3.1 `:187`) appearing where a value is expected instead of before a `(`.

```
// instance method -> UNBOUND reference (receiver is the first parameter)
var h = AuthController::Login;              // (AuthController, ILoginRequest) => LoginResponse

// namespace / static-side function -> plain function value (no receiver)
var f = App::Routes::addRoutes;            // (Atlantis::App) => void

// labeled constructor -> (params) => ConstructedType   (composes for LA-18, §9)
var g = User::FromJson;                    // (JsonValue) => User

// the motivating consumer: the R10 route table names handlers, not lambdas
app.AddRoute(Route('/login', AuthController::Login).requestType(ILoginRequest));

// stored, passed, kept in a typed container — an ordinary function value
Array<(HttpRequest) => HttpResponse> chain = [ Logging::before, Auth::guard ];
```

### 2.1 Grammar deltas

**None required in the abstract grammar** — `Type::method` already parses to
`ExprKind::Member{colon:true}` (Ast.hpp `:111,:151`), and `::name` is already postfix
(reference §3.1). M1's first task (§6) is a probe to confirm whether *value-position* `Type::m`
(no trailing `(`) reaches the checker as a `Member` node on every path, or whether one parser
site assumes a call follows. Probe **P-2** below shows the current *end-to-end* result is a silent
runtime failure, not a parse error, on the field-assignment path — evidence the node already
arrives at the checker.

### 2.2 What resolves the reference

A method reference is resolved by **ordinary overload resolution by type** (reference §3.7,
info.md §9), with one twist: the selector is a **target function type**, not an argument list.

- **Unambiguous name** (one candidate): resolved directly; the reference's type is that
  candidate's (unbound) function type.
- **Overloaded name + a target type in context** (the field/parameter/assignment-target type, or
  an explicit annotation): pick the candidate whose unbound function type **equals** the target.
  This is the same "expected type drives resolution" the checker already runs for constructor and
  return inference.
- **Overloaded name + no target type** (a bare `var` with nothing to disambiguate against): a
  **compile error** — `"ambiguous method reference 'C::m'; annotate the target function type"` —
  the same "refuse to guess" stance as a bare `distinct`-collided read (reference §3.4). *Not* a
  silent pick.
- **Missing member**: a compile error **at the reference site** (request acceptance #2) —
  `"no method 'X' on type 'C'"` / `"no function 'X' in namespace 'M'"`.

**Where the expected type comes from (mechanism note — review 2026-07-11).** The checker has
**no ambient expected-type channel** (verified: nothing downward-flowing in `Checker.cpp`; lambdas
get parameter types only from call context, `checkLambdaBody`). The target type therefore arrives
per syntactic position, and the implementation differs by position:

- **Declared-type var-decl / assignment / field store**: the slot's declared type is at hand at
  the check site — pass it into the reference's resolution directly. Cheap.
- **Argument position** — the motivating consumer's actual shape
  (`Route('/login', AuthController::Login)` is a reference as a *constructor argument*) — the
  target type is only known **after** the outer call's overload resolution. An **overloaded**
  reference here must ride the existing **lambda-deferral machinery**: `typeOfCallInner` types
  lambda arguments `unknown` during overload choice and walks them afterward against the chosen
  candidate's declared parameter types (Checker.cpp `:766–785`, ctor args `:854–866`, calls
  `:1589–1611`). An overloaded method reference in argument position is deferred exactly the same
  way and resolved against the chosen overload's parameter type. **Accepted consequence** (same as
  lambdas today): a deferred overloaded reference does not help choose the *outer* overload. A
  **single-candidate** reference needs no deferral — its type is known immediately from
  `typeOfMember` and participates in outer overload choice like any typed argument.

---

## 3. Current behavior (verified against this tree — `build/lang`, 2026-07-06 binary)

House rule (overview §4.1): *"verified against this tree; nothing assumed."* Probes run with
`build/lang --run <file>`:

| # | program | result today | reading |
|---|---|---|---|
| **P-1** | `M::dbl(21)` (namespace fn, **called**) | prints `42` | the called form works — LA-25 is specifically the *value* position |
| **P-2** | `class Box { (int)=>int fn; } b.fn = M::dbl; b.fn(21)` | **compiles**, then `Uncaught RuntimeException: cannot resolve call target 'fn'` | value-position `::`-callable is silently accepted and lowered to a **non-closure** — a latent correctness hole LA-25 closes |
| **P-3** | `var h = Greeter::hi;` at top-level | parse error `expected expression` at `var` | the derail is **`var` at top-level statement position**, *not* `::` (P-5 confirms `var` works in a function body) |
| **P-5** | `class Box { (int)=>int fn; } b.fn = (x) => M::dbl(x); b.fn(21)`; also `Array<(int)=>int>` stored and called | prints `42`, `42` | **the exact eta-expansion LA-25 synthesizes already runs on every engine** — the lowering target is proven |

The load-bearing finding is **P-2 + P-5**: the target lowering (a non-capturing closure whose body
is the existing call) is already correct and green; LA-25's whole job is to make the checker *emit*
that closure for a value-position `::`-callable instead of accepting a non-callable that fails at
runtime.

(`var` at top-level statement position, P-3, is an orthogonal pre-existing quirk — `var` works
inside function/method bodies, which is where the route table lives. Examples here use `var` inside
methods or explicit function types; LA-25 does not depend on fixing top-level `var`.)

---

## 4. The mechanism

Two touch points: **Checker** (recognize + type + resolve) and **Lower** (synthesize the
eta-expansion closure). No IR op, no backend change.

### 4.1 Checker — `typeOfMember` gains the callable-in-value-position case

The checker already routes a `Member` that is a call's callee through `typeOfCall`
(Checker.cpp `:830–931`); a `Member` reached **anywhere else** goes through `typeOfMember`
(`:664`). So value-position detection is **structural and already present** — if `typeOfMember`
is asked to type a `::` Member whose selector names a callable member, it is *by construction* not
a call callee. Add there:

1. `base = typeOf(e->a)`.
2. If `e->colon` and the selector `e->text` names a **callable** member of `base`:
   - `base` is `TKind::TypeValue` for class/struct `C`, `e->text` a **method** → unbound
     instance ref; synthesize function type `(classType(C), param₀…paramₙ) => ret`.
   - `base` is `TKind::TypeValue`, `e->text` a **labeled ctor** → `(param₀…paramₙ) => classType(C)`.
   - `base` is `TKind::TypeValue`, `e->text` a **static-side function** (§6.6) → `(params…) => ret`.
   - `base` is a **namespace**, `e->text` a **function** → `(params…) => ret`.
3. Overload disambiguation by the expected type (§2.2); ambiguous-without-target → error;
   missing → error at `e->span`. Note `typeOfMember` alone can only see the expected type in
   slot-target positions; in **argument position** an overloaded reference is deferred and
   resolved by the call path against the chosen overload's parameter type, riding the existing
   lambda-deferral walk (§2.2 mechanism note) — the call path must treat an unresolved overloaded
   `isMethodRef` Member the way it treats a Lambda argument.
4. Record on the `Member` node, for the lowerer: the resolved target decl (`Expr` already carries
   a checker-resolved-target field, Ast.hpp `:183`), an `isMethodRef` flag, and a
   `receiverIsFirstParam` bool. The node's `Type` is the synthesized function type.

This reuses existing helpers: `functionOverloads`/`methodOverloads`/`ctorOverloads`, the resolver
(`resolve(...)`), and `fromTypeRef`/function-type construction the checker already builds for
lambda params.

### 4.2 Lower — synthesize the eta-expansion closure

When the lowerer meets a value-position `Member` flagged `isMethodRef`, it builds the same closure
`lowerLambda` would for the equivalent hand-written lambda (which P-5 proves runs today):

```
instance method  C::m      ->   (recv, p₀…pₙ) => recv.m(p₀…pₙ)     // body = CallDyn by name
namespace fn     NS::f      ->   (p₀…pₙ)       => NS::f(p₀…pₙ)      // body = existing CallFn
labeled ctor     C::Label   ->   (p₀…pₙ)       => C::Label(p₀…pₙ)   // body = existing construction
```

- The synthesized parameters get **fresh, hygienic register/names** (the §16.5 alpha-renaming
  precedent).
- The instance body is a `.`-call on a receiver whose static type is the referenced class —
  so the reference dispatches **exactly as the equivalent hand-written, explicitly-typed lambda
  `(C c, ...) => c.m(...)` does**, under the language's ordinary receiver-dispatch rule. Today
  that rule is: an **interface**-typed receiver dispatches on the runtime object's class
  (checker leaves `resolved` null → `CallDyn`-by-name); a **concrete-class**-typed receiver is
  statically bound to that class's own declaration (the deliberate perf choice recorded in
  `designs/complete/techdesign-07-iteration.md`'s log, "dynamic dispatch is interface-only").
  Consequence: `IAnimal::speak` invoked on a `Cat` runs `Cat`'s override; `Animal::speak`
  invoked on a `Dog` runs `Animal`'s own body. **Ruled 2026-07-11 (§8.8):** this is correct —
  the reference must be exactly as polymorphic as its expansion, no more; dispatch semantics
  belong to the language's call rule, not to this feature. (An earlier draft of this bullet
  claimed override dispatch for the concrete-class case too; that claim was wrong against the
  landed language and was the STOP #2 escalation in the implementation log, §12.)
- These closures **capture nothing** (receiver and args are parameters), so they are the cheapest
  closure kind — no capture retain/release, no lifetime surprise (info.md §15).

**Placement choice (implementation, not architecture):** synthesize either (a) as an AST rewrite
the checker attaches (`Member` → `Lambda`), so the lowerer needs *zero* new code, or (b) in the
lowerer directly from the annotation. (a) maximizes reuse (the whole lambda path — typing, closure
capture analysis, all five backends — runs unchanged); (b) keeps the AST faithful. **Recommend
(a)**; if attaching a synthesized subtree during checking proves awkward against how `Checker.cpp`
annotates (it does not currently rewrite), fall back to (b). This choice is a **STOP point only if
neither is clean** (§8.1) — both reuse `lowerLambda` and add no backend code.

---

## 5. Engine coverage

Per overview §4.3, and trivial here because there is **no new lowering** — every engine already
executes the synthesized closure (P-5 is green on the interpreters; the C++/ELF/LLVM closure paths
are the same ones `lowerLambda` feeds):

- **oracle + IR**: mandatory, full coverage.
- **emit-C++**: full (closures covered).
- **ELF**: full — no `X64Gen.cpp` edit needed (closures already emit); the freeze (overview §2.1)
  is not touched.
- **LLVM**: full (closures + `CallDyn` already covered, info.md §17).

Because the feature lowers to an existing construct, "all engines it claims" is "all engines,"
with the P-5 lowering already differential-green.

---

## 6. Milestones (ordered; one implementer)

- **M1 — surface + checker typing.** Probe P-1/P-2/P-3/P-5 first (confirm the value-position
  `Member` reaches the checker; isolate any single parser site that assumes a following `(`).
  Then: `typeOfMember` types a value-position `::`-callable as its (unbound) function type for
  namespace functions and single (non-overloaded) instance methods. Corpus: a namespace-function
  reference and an instance-method reference stored in a typed field and invoked; oracle + IR
  green. Checker tests: missing member = error at the reference site.
- **M2 — eta-expansion lowering.** Synthesize the closure (§4.2); replace P-2's silent-runtime-fail
  with a real closure. Instance references dispatch via `CallDyn` (override-correct). Corpus grows
  to emit-C++ + ELF + LLVM; all differential-green.
- **M3 — overloads + labeled ctors.** Target-type disambiguation of an overloaded reference (§2.2);
  ambiguous-without-target = clean error; `C::Label` labeled-ctor references
  (`(params) => C`). Overloaded references in **argument position** ride the lambda-deferral path
  (§2.2 mechanism note) — corpus must include one (the route-table shape), not only the
  field-target shape. Corpus: an overloaded method disambiguated by the target field type; an
  overloaded method disambiguated as a call argument; a labeled-ctor reference. This closes
  request acceptance #1–#3 and lands the LA-18 seam (§9).
- **M4 — Atlantis smoke + reference.md.** The R10 route-table line
  (`Route('/login', AuthController::Login)`) compiles and dispatches in the one-process loopback
  corpus; `docs/reference.md` §3.4 updated in the same commit (overview §2.2).

Each milestone ends on the overview §4.3 acceptance block (clean build, `ctest`, corpus, ELF,
native, churn 13/13 + XFAIL, lcurl smoke).

---

## 7. Acceptance (maps request §3 one-to-one)

1. **Corpus** — a reference to (a) an instance method, (b) a namespace function, and (c) an
   overloaded method disambiguated by target type; each invoked through the value;
   **oracle/IR/LLVM identical** (plus emit-C++ and ELF here, since coverage is free).
2. A reference to a **missing or ambiguous** method is a **compile error at the reference site**
   (missing → M1; ambiguous-without-target → M3).
3. The reference's type **spells as an ordinary function type** (`(A, B) => R`) — assignable to
   fields/params of that type and storable in `Array<(A,B)=>R>` (P-5 confirms the container works;
   M2/M3 make the *reference* produce the value).

---

## 8. Foreseeable problems & STOP points

### 8.1 Elaboration placement (soft) — §4.2
Checker-side AST rewrite vs lowerer-side synthesis. Both reuse `lowerLambda`. **STOP only if
neither is clean**; log which was chosen in the implementation log.

### 8.2 Overload disambiguation without an expected type (decided)
Bare `var h = C::overloaded;` with no target type is a **compile error**, not a pick. This is the
info.md §1 "a bare read with no type in context cannot resolve" rule. Do **not** default to
first-declared here — first-declared breaks *ties among applicable candidates at a call*
(reference §3.7); a value-position reference has no arguments to make candidates applicable, so
there is nothing to tie-break. Require the annotation.

### 8.3 Keyword selectors after `::` (already handled)
`client::get`, `x::set` — `get`/`set`/`is` are names after `::` (reference §3.2, the Track-09
"keyword member names" landing). Method references inherit this for free; add one corpus case to
pin it.

### 8.4 Bound references (`.`-value-position) — OUT OF SCOPE
`someInstance.method` as a value (receiver captured) is a separate feature: it needs a *capturing*
closure and opens receiver-lifetime/aliasing questions (info.md §15). The request asks only for the
unbound `::` form. Named on the roadmap as a sibling; **not** built here. If a consumer asks for it,
file a new ticket — do not widen this design silently.

### 8.5 Reference to a `distinct`-collided or interceptor member (decided)
A `distinct`-collided member (info.md §4) reached by a *qualified* `::` is unambiguous (that is what
`::` qualification is *for*) — a reference to `this.Counter::value`-style qualified members is fine.
A reference to a member on an interception-directive class (info.md §7) that has *no slot* is a
compile error at the reference site (there is no concrete target to eta-expand), consistent with
"missing member." No special case.

### 8.6 Reference to a generic callable (decided: v1 compile error)
A method-level-generic callable (`identity<R>`, `Array<T>::map<U>`, info.md §9) has **unbound type
parameters** in its would-be function type, and value position supplies no arguments to infer them
from. v1: a reference to a generic callable is a **compile error at the reference site** —
`"cannot reference generic function 'm' — its type parameters are unbound in value position"`.
A target function type *could* pin them by unification (the same head-unification HKT inference
uses); that is a follow-up, not v1 — no consumer needs it (route handlers are monomorphic), and
silently supporting half of it (target present) while erroring on the other half (bare `var`)
without designing the unification rule first would be guesswork. Note the LA-18 seam is unaffected:
inside a specialized copy every type param is concrete, so §9's composed case never hits this error.

### 8.7 Function-value identity (decided: documented, not special-cased)
Each *evaluation* of a reference synthesizes a fresh closure, so two evaluations of `C::m` are
distinct function values — exactly as two identical hand-written lambdas are. Do **not** intern or
memoize references in v1 (a cache keyed by resolved target is a pure optimization, available later
without semantic change since the closures capture nothing). Consequence to document in
reference.md (§11): comparing two references for identity is not a way to test "same handler" —
compare by the route key or store the value once.

### 8.8 Dispatch through a concrete class (RULED 2026-07-11, owner escalation)
The language's receiver-dispatch rule is **interface-dynamic, class-static** (a concrete-class-
typed receiver statically binds the named class's own declaration; only an interface-typed
receiver dispatches on the runtime object — `techdesign-07-iteration.md` log, `Checker.cpp`'s
method-call branch). This design's original §4.2 asserted override dispatch for the
concrete-class case, contradicting that rule; the escalation and analysis are in the
implementation log (§12, STOP #2). **Ruling:**

- **A method reference carries NO dispatch behavior of its own.** `C::m` is *definitionally*
  the eta-expansion lambda `(C c, ...) => c.m(...)`; it dispatches exactly as that lambda does
  under whatever the language's receiver-dispatch rule is — today and after any future change
  to that rule. The equivalence (P-5) is the invariant this feature guarantees; making
  references a dynamic-dispatch exception would be a value that behaves differently from its
  own definition ("hidden magic," info.md §1) and was rejected.
- **Want polymorphic dispatch? Reference the interface** (`IAnimal::speak`, yielding
  `(IAnimal) => string`) — the identical choice every ordinary call site already makes.
  The R10 route-table consumer references concrete controllers monomorphically and is
  unaffected either way.
- **The concrete-class static-dispatch rule itself is separately under reconsideration** —
  see `designs/proposal-class-method-dispatch.md` (filed with this ruling): the slot model
  (info.md §6.5) argues class-receiver calls should be *semantically* dynamic with
  whole-program devirtualization as the fast path (info.md §7's own closed-set doctrine).
  If that pivot lands, references become override-correct **automatically and for free**,
  because the equivalence does the work — no change to this feature. That layering is exactly
  why the ruling puts dispatch in the language's call rule and not in LA-25.
- Corpus pins the ruled behavior explicitly (both directions: concrete-class static,
  interface dynamic), so the pivot — if taken — flips a deliberate, documented expectation
  rather than silently changing an untested one.


---

## 9. Composition with LA-18 (the seam)

Once **both** this design and `designs/complete/techdesign-generic-static-members.md` land, a method/ctor
reference on a **type parameter** in value position works:

```
(JsonValue) => A  decoder<A>() => A::FromJson;      // LA-25 value position + LA-18 type-param operand
```

There is **no new interaction to design**. LA-18 monomorphizes the generic body per concrete `A`
(§ that doc); inside each specialized copy `A::FromJson` is a *concrete* labeled-ctor reference, so
LA-25's eta-expansion (§4.2) produces an ordinary concrete closure — exactly the `User::FromJson`
case M3 already covers. The seam is: **LA-25 owns "callable-in-value-position → function value";
LA-18 owns "`::` on a type variable → concrete member per instantiation"; their product is one
concrete closure per instantiation.** Neither design blocks on the other:

- LA-25 alone: references on **concrete** types/namespaces — the whole R10 route table. Fully
  useful with zero LA-18.
- LA-18 alone: `A::FromJson(raw)` **called immediately** — the MCP/decode path. Fully useful with
  zero LA-25.

If LA-25 lands first (it is scheduled to, AG-2 < AG-5), LA-18 inherits value-position type-param
references for free; if LA-18 landed first, it would inherit them when LA-25 lands. Order-independent.

---

## 10. Interim fallback (already in the tree — request §4)

Both work today and both are deleted the day LA-25 lands:

- **Track 02** ships a `Route(path, factory, lambda)` overload —
  `Route('/login', () => AuthController(inject IUserService), (c, req) => c.Login(req))` — and the
  `@InjectRoutes();` splice path (`designs/atlantis/techdesign-02-routing-controllers.md` §0R).
- The hand-written lambda `(c, req) => c.Login(req)` *is* what LA-25 synthesizes (P-5), so
  migration is a mechanical spelling swap with byte-identical output — the acceptance for deletion.

---

## 11. `docs/reference.md` deltas (ship in the feature commit — overview §2.2)

- **§3.4 (Member access and qualification):** add that a `::`-reached callable member **in value
  position** is a typed function value; instance methods are **unbound** (receiver = first
  parameter); namespace/static functions and labeled constructors yield their signature directly;
  resolution is by target function type, ambiguous-without-target and missing-member are compile
  errors at the reference site.
- **§2.5 / a new §3.2 note:** the reference's type is an ordinary function type, assignable and
  storable like any function value.
- **v1 limits, same §3.4 block:** a reference to a generic callable is a compile error (§8.6);
  each evaluation of a reference yields a fresh function value — identity comparison is not a
  "same handler" test (§8.7).

---

## 12. Implementation log

### 2026-07-11 — M1-M3 mechanism implemented; STOPPING on a real dispatch-semantics discrepancy (§4.2)

**What's built and verified correct** (re-probed P-1/P-2/P-3/P-5 first, per house rule — all
matched the design's table before writing any code). All of it is **checker-only**, exactly the
recommended §4.2 option (a): `tryResolveMethodRef` recognizes a value-position `::`-callable
(namespace function, instance method, or labeled constructor), resolves overloads by an expected
function type threaded in from three call sites (declared-type var/field-init via `typeInitExpr`,
assignment via `typeOfBinary`, and argument position via the same lambda-deferral machinery
`genericReturn`/the ctor-arg loop already used — `isDeferredMethodRefArg` mirrors the existing
`a->kind == ExprKind::Lambda` deferral check), and — on a single resolved candidate — rewrites the
`Member` node **in place** into the exact eta-expansion `Lambda` a hand-written equivalent would
be (`e->kind = Lambda`, synthesized params, a synthesized `Call` body). No new Expr fields, no
Lower/Eval code, no IR op — every engine's existing Lambda handling picks it up unchanged, exactly
as recommended. Verified working end-to-end (`--run`, oracle) for:
- namespace-function reference (P-1/P-2 shape), instance-method reference, labeled-constructor
  reference, all invoked through the value;
- overload disambiguation by the declared-type slot (var-decl, field-init, and plain assignment
  all thread the expected type in) and by a constructor-argument position (the literal R10
  route-table shape: `Route(Greeter::hi)` picks the right overload from the ctor's declared
  parameter type);
- keyword selector after `::` (§8.3) — inherited for free, confirmed with a method named `get`;
- an unbound reference through an **interface** type dispatches dynamically (correct — see below);
- missing member → error at the reference site; ambiguous-without-target → error (§8.2); a
  reference to a method-level-generic callable → error (§8.6), on both a namespace function and a
  class method.

**STOP #1 (logged, not blocking — worked around): bug.md #2.** Calling a closure stored in a
FIELD via *direct* dot-syntax (`obj.field(args)`) is broken on every non-oracle engine — loud on
IR (`cannot resolve call target`), **silent wrong output (exit 0, no diagnostic) on emit-C++**,
loud on LLVM (compile error), silent on ELF too. Reproduced with a **hand-written lambda**, zero
LA-25 code involved — `Lower.cpp`'s `lowerCall` (and `IrInterp.cpp`'s `CallDyn`/`CGen.cpp`'s
`callnative`/`LlvmGen.cpp`) has no fallback from "no method found by that name" to "read it as a
field and `CallValue` it," unlike `Eval.cpp`'s `evalCall`, which has exactly that fallback. Filed
as its own defect (bug.md #2, P1 via the emit-C++ silent-failure marker P1.1) rather than folded in
here, since it predates and is independent of this design. **Workaround used everywhere in this
track's testing:** bind the field's value to a `var`-inferred local before calling it
(`var h = obj.field; h(args);`) — confirmed correct on oracle AND IR AND emit-C++ for every
scenario above. Corpus for this track should use that pattern for "stored in a field, then
invoked" cases until bug.md #2 is fixed.

**STOP #2 (blocking — needs a ruling before M2/M4 corpus can be written): §4.2's override-dispatch
claim doesn't hold for a CONCRETE-class-typed unbound reference; it only holds for an
INTERFACE-typed one.**

§4.2 states: "The instance body is a `.`-call → `CallDyn`-by-name ... so the reference dispatches
to the receiver's actual runtime type — an unbound reference to a **base method** invoked on a
**derived instance** runs the override, exactly as a hand-written `(c) => c.m()` would." This is
false as written when the base is a concrete `class` (not an `interface`). Verified with three
programs, no LA-25 code involved in any of them:

```
class Animal { string speak() => "..."; }
class Dog : Animal { string speak() => "Woof"; }
string callSpeak(Animal c) => c.speak();     // c's static type is the CONCRETE class Animal
callSpeak(Dog());                            // prints "...", NOT "Woof"
```
```
interface IAnimal { string speak(); }
class Dog : IAnimal { string speak() => "Woof"; }
string callSpeak(IAnimal c) => c.speak();    // c's static type is the INTERFACE IAnimal
callSpeak(Dog());                            // prints "Woof" — correct, dynamic dispatch
```

This matches an **existing, already-documented project fact** the LA-25 design didn't cross-check:
`designs/complete/techdesign-07-iteration.md`'s implementation log (2026-07-06, "Bug 21" entry)
already states this in so many words — *"this language's dynamic dispatch is genuinely
interface-only by design (`Checker.cpp`'s own comment: 'A call through an INTERFACE-typed receiver
must dispatch on the runtime object's class ... Leave `resolved` unset' ...); a CLASS-typed
receiver bakes in the statically-resolved declaration, a deliberate perf choice, not a bug."* The
`(c) => c.m()` example in §4.2 only behaves as claimed when the checker happens to leave `c`'s type
`unknown` (e.g. an untyped lambda param with no target-type context to infer from, the accidental
gap that made the ORIGINAL P-2/P-5 probes look override-correct) — it is not a guarantee for an
explicitly/statically class-typed receiver, and my synthesized reference's receiver parameter
**is** explicitly typed (`bt.canonical`, the concrete class), so it hits the static path, same as
`callSpeak(Animal c)` above.

**Why this needs a ruling, not a unilateral fix:** the two readings produce different, both-legal
behavior, and choosing between them is a language-semantics decision, not an implementation bug:
1. **Match the language's existing static-dispatch-for-classes rule** (what my current
   implementation already does, unchanged): an unbound reference through a concrete class is
   statically bound to THAT class's own declaration, exactly like an ordinary explicitly-typed call
   through the same class would be. §4.2's override-dispatch sentence would need correcting to say
   "through an interface" specifically, not "a base method" generally. No code change needed —
   `rewriteAsMethodRef` already only nulls `resolved` for the interface case (mirroring
   `Checker.cpp`'s own existing method-call branch precedent, ~line 993).
2. **Make method references a deliberate exception**: always leave `resolved` null for an unbound
   instance-method reference (interface or concrete class alike), forcing dynamic `CallDyn`-by-name
   dispatch unconditionally. Mechanically trivial (drop the `isInterface` condition in
   `rewriteAsMethodRef`) and requires no Lower/Eval changes either (the by-name fallback already
   works — proven by the interface case and the untyped-lambda-param case). But it makes a method
   *reference* behave differently from an equivalent hand-written, explicitly-typed lambda
   (`(Animal c) => c.speak()`), breaking the design's own "the eta-expansion IS what a hand-written
   lambda would produce" equivalence (§4.2, P-5's whole justification) — a real, deliberate
   divergence this design doesn't currently ask for or justify.

Not deciding this myself per the overview §4.2 STOP-and-escalate protocol (never improvise a
design/semantics change) and this session's explicit instruction to stop and detail rather than
resolve a diverging issue. Everything else in this log is unaffected by the choice — namespace
functions and labeled constructors have no dispatch-polymorphism question at all, and the
interface-typed case is correct either way (already dynamic). Only the concrete-class
instance-method-reference case is in question. Pausing here; the checker implementation as
committed reflects reading #1 (no code change) since that requires zero further work either way
this is decided, and is the smaller diff from "do nothing" if reading #1 is confirmed.

**Not yet done, pending the ruling above:** M3's remaining corpus (the parts not blocking on this
question could still be written, but weren't, to keep this stopping point clean — see
STOP #1's workaround note for the pattern to use), M4 (Atlantis-shaped smoke test + reference.md
delta), and moving this file to `designs/complete/`.

### 2026-07-11 — STOP #2 ruling landed (owner escalation, Fable); M3/M4 completed; design closed

**Ruling: reading #1, formalized as §8.8.** A reference is definitionally its eta-expansion
lambda and carries no dispatch behavior of its own — `Animal::speak` statically binds Animal's
declaration (like any explicitly-`Animal`-typed call site today); `IAnimal::speak` dispatches
dynamically (like any interface-typed call site today). Reading #2 (references as a
dynamic-dispatch exception) was rejected as hidden magic that would make a value behave
differently from its own definition and would diverge from lambdas — in both directions — the
moment the underlying language rule changes.

**The long-term question was answered separately, at the right layer:** the escalation exposed
that the *language's* concrete-class static-dispatch rule conflicts with the slot model
(info.md §6.5: a method IS a per-object slot; a statically-bound call on a `Dog` reads a value
the Dog's slot does not hold) and with §7's own "closed candidate set → point right at it;
open set → cached lookup" doctrine (an overridden method's candidate set is open). Filed as
`designs/proposal-class-method-dispatch.md`: make class-receiver calls semantically dynamic,
recover the fast path via whole-program devirtualization (the compiler sees every class — §12 —
so any method with no override in the program keeps the direct call; only genuinely-overridden,
base-typed call sites pay dispatch, which is exactly where dynamic behavior is *wanted*). If
accepted, LA-25 references become override-correct with zero changes here. LA-25 deliberately
does not gate on that proposal.

**M3/M4 closed out:**
- Corpus: `tests/corpus/method_refs/` — one comprehensive program (namespace-fn / unbound
  instance-method / labeled-ctor references; overload disambiguation by field type, by
  assignment, and as a constructor argument — the R10 route-table shape; keyword selector
  `Client::get`; §8.8's ruled dispatch pinned in BOTH directions) plus checker-error corpus in
  `tests/test_checker.cpp` (missing member, ambiguous-without-target, generic-callable §8.6).
  Engine lanes recorded in CMakeLists per the evidence (see below).
- Invocation-through-a-field uses the `var h = obj.field; h(args)` binding pattern throughout,
  per bug.md #2 (pre-existing, independent of LA-25); the direct `obj.field(args)` spelling
  stays pinned ONLY in bug.md #2's repro, not here.
- The design's M4 wording ("the one-process loopback corpus") predates reality — no Atlantis
  code exists in-tree yet (designs only), so the R10 acceptance is pinned as the route-table
  *shape* (`Route(path-analog, Controller::method)` constructor-argument disambiguation +
  dispatch through the stored value), which is the exact line R10 needs.
- reference.md §3.4 gained the method-reference block (value-position `::`-callables, unbound
  receiver-first form, resolution by target type, both error forms, v1 limits incl. §8.6/§8.7,
  and the §8.8 dispatch note); §3.3 cross-references it.
- `techdesign-method-references.md` + `request-method-references.md` → `designs/complete/`
  (request-file precedent: `request-comptime-template-import.md`).
