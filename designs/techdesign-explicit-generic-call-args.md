# Tech Design — Explicit Generic Type Arguments at Call Sites

**Status:** READY FOR IMPLEMENTATION. **Date:** 2026-07-19.
**Source:** `docs/complete/research-explicit-generic-call-args.md` (the research input,
archived when this decision-complete design was created).
**Trigger:** LA-31 Stage 2's R8 forced deviation in
`designs/complete/techdesign-02-reifier.md`: the reifier could not emit an explicit
generic argument on its `expr::Expr` construction because expression-position generic
arguments do not exist in the landed grammar.
**Track:** one front-end track, ordered M0 → M4, one implementer. Parser, AST,
resolution, overload selection, rule cloning, specialization cloning, and source
printing are one representation story and must land together.
**Primary files:** `src/Ast.hpp`, `src/Parser.hpp`, `src/Parser.cpp`,
`src/Resolver.cpp`, `src/Checker.hpp`, `src/Checker.cpp`, `src/Rules.cpp`, and
`src/AstPrinter.cpp`.
**Verify-only files:** `src/Eval.cpp`, `src/Lower.cpp`, `src/IrInterp.cpp`,
`src/CGen.cpp`, and `src/LlvmGen.cpp`. No IR, runtime value, ABI, or backend concept is
introduced.

---

## 0. Decision summary — all research choices are closed here

| ID | ruling |
|---|---|
| **R1 — spelling** | Explicit call-site type arguments use the unambiguous postfix spelling **`callee::<T1, ...>(args)`**. The bare expression spelling `callee<T>(args)` is not claimed. |
| **R2 — ownership** | The type-argument list belongs to the **`Call` AST node**, not to a synthetic `Member` node. It is invocation metadata and is valid only when immediately followed by the call's `(`. |
| **R3 — position** | The list is written immediately before the argument list, after the complete callee: `Box::<int>()`, `Box::From::<int>()`, `obj.map::<string>(fn)`, `N::f::<int>()`. `Box::<int>::From()` is not a second spelling. |
| **R4 — arity** | Explicit application is all-or-nothing. The caller supplies exactly the declared type-parameter count. There are no partial lists, `_` inference holes, named type arguments, trailing commas, or empty `::<>()` lists. |
| **R5 — authority** | An explicit binding is authoritative. It seeds substitution before argument, target, or lambda-last inference. Later inference may fill only unbound slots and never replaces an explicit value. |
| **R6 — disagreement** | A value or target that disagrees with an explicit binding is checked against the pinned type and gets the ordinary local type error. There is no separate “explicit conflicts with inferred” error class. |
| **R7 — overloads** | Explicit arity filters an overload set, and the substituted parameter types participate in applicability and scoring. `f::<int>(1)` cannot select a non-generic `f`, nor can a pinned `T=string` make an `int` argument silently applicable. |
| **R8 — callable classes** | Construction binds the constructed class's type parameters. A function/method call binds that callable's own type parameters. An instance receiver continues to supply its class-level arguments independently. |
| **R9 — declared callees only** | Explicit arguments require a resolved declared construction, function, or method. They do not apply to a closure variable, a call result, an indexed function value, an immediately-invoked lambda, or an expression macro call. |
| **R10 — bare syntax remains stable** | `Box<int>(5)` and `identity<int>(5)` keep their current comparison parse. No parser symbol table, speculation, backtracking, or checker rewrite is added. |
| **R11 — erasure** | The list is checker-only. A successful checked call reaches Eval/Lower/backends as the same ordinary resolved positional call as today. No type descriptor is emitted. |
| **R12 — generated source** | `--ast`, `--expand`, rule/procedural-macro clones, default-expression clones, and LA-18 specialized clones must preserve and render the list. LA-31 is updated to emit `expr::Expr::<Fn>(...)` and its round-trip test pins that spelling. |
| **R13 — callee resolution** | The explicit list never changes what the callee name denotes. Existing construction-first, namespace, alias, member, and method lookup chooses the declaration category/candidate set; explicit arity and types filter only within that already-selected set. |

These rulings deliberately choose research Option A, close Option B on ambiguity and
architecture cost, and close Option C on soundness. Nothing remains for an implementer
to decide at the language-surface level.

---

## 1. Problem, scope, and fit with the language

### 1.1 The missing inference source

Leviathan currently has three generic binding situations:

1. a type-position instantiation such as `Box<int>`;
2. a construction whose class arguments are inferred from value arguments or a target;
3. a generic function/method call whose callable arguments are inferred from values.

Only the first has an explicit source spelling. The other two fail when neither values
nor a target carry the needed type:

```lev
class Box<T> { new Box() { } }
var b = Box();       // cannot infer T; no inline way to provide it today

Array<T> empty<T>() => [];
var xs = empty();    // cannot infer T; a target-typed local is the only workaround
```

The feature adds the missing, explicit inference source without changing what a generic
is at runtime:

```lev
var b  = Box::<int>();
var xs = empty::<int>();
```

### 1.2 Philosophy fit

The governing rule is:

> A generic call binds declared type slots from explicit arguments when supplied;
> otherwise it binds them from the existing inference sources.

That is the same slot-binding model `info.md` §9 already uses for class-, function-,
and method-level generics. The new syntax does not introduce specialization,
reification, a second generic system, or a runtime calling convention. It only lets a
caller name a binding that is currently recoverable solely from context.

The `::<` marker is intentionally visible. It follows the repository's “explicit over
implicit” rule and claims a token adjacency that is a hard error today, protecting the
ordinary `<`/`>` comparison grammar from guesswork.

### 1.3 In scope

- generic class construction, including nullary constructors;
- labeled construction;
- free, namespace, static-side, and instance generic calls;
- qualified/nested names and imported aliases;
- method calls whose receiver already carries class-level generic arguments;
- nested, qualified, nullable, union, function, and higher-kinded type arguments, using
  the existing `TypeRef` grammar;
- named/default/injected value arguments on the same call;
- optional chaining and base-qualified method calls;
- source expansion, rule/procedural-macro cloning, and LA-18 specialization cloning;
- the LA-31 reifier's originally requested explicit `Fn` construction argument.

### 1.4 Explicitly not in scope

- the ambiguous bare spelling `f<T>(x)` in expression position;
- generic callable references in value position (`var r = f::<int>`);
- partial generic application or placeholders (`f::<_, string>`);
- named generic arguments (`f::<T: int>`);
- default type parameters;
- runtime/dynamic application of type arguments to a function value;
- expression-macro type arguments (`macroName::<T>!(payload)`);
- any change to generic variance, erasure, LA-18 specialization rules, or backend ABI.

These are closed boundaries, not implementation deferrals. A later proposal may add a
different surface, but this implementation must neither approximate nor silently accept
one.

---

## 2. Surface syntax and normative semantics

### 2.1 Canonical examples

| operation | spelling | binding owner |
|---|---|---|
| default generic construction | `Box::<int>()` | `Box`'s class parameters |
| labeled generic construction | `Box::From::<string>("x")` | `Box`'s class parameters |
| qualified construction | `pkg::Box::<int>()` | `pkg::Box`'s class parameters |
| generic free function | `identity::<int>(5)` | `identity`'s callable parameters |
| namespaced function | `mathx::convert::<float>(n)` | `convert`'s callable parameters |
| generic instance method | `box.remap::<string>("x")` | `remap`'s method parameters; `box` supplies class parameters |
| class/static-side function | `Factory::make::<User>(row)` | `make`'s callable parameters |
| base-qualified method | `this.Base::convert::<string>(x)` | `convert`'s callable parameters |
| optional call | `maybe?.map::<string>(fn)` | `map`'s callable parameters; optional short-circuit unchanged |

The list always follows the **complete callee** and immediately precedes its value
arguments. This one placement rule avoids a special grammar for constructors or labels.
In particular, a labeled generic construction is `Box::From::<int>(...)`, not
`Box::<int>::From(...)`.

### 2.2 Grammar delta

The expression grammar gains one combined postfix form:

```ebnf
postfix          := primary postfixPart*
postfixPart      := '.' memberName
                  | '?.' memberName
                  | '::' memberName
                  | '(' valueArgs? ')'
                  | '::' '<' typeArgList '>' '(' valueArgs? ')'
                  | '[' expr ']'
                  | '!' '(' macroArgs? ')'               // existing macro call

typeArgList      := type (',' type)*                      // one or more
```

`type` is exactly the existing type grammar used in annotations. It therefore already
handles:

```lev
f::<Array<int>>()
f::<pkg::Thing?>()
f::<int | string>()
f::<(User) => bool>()
f::<Array<Array<int>>>()       // existing `>>` splitting closes both lists
```

Whitespace is insignificant (`f :: < int > (1)` tokenizes the same way), matching
other operators. No adjacency-only lexer rule is added.

### 2.3 Parse commitment and recovery

`parsePostfix` must test `ColonColon` + lookahead `Lt` **before** its ordinary
`.`/`::` member branch. Once that pair is seen, the parser commits to explicit generic
application; it never falls back to comparisons. The branch:

1. consumes `::` and `<`;
2. rejects an immediate `>`/`>>` as an empty list;
3. parses one or more ordinary `TypeRef`s separated by commas;
4. closes with the existing `expectGt()` helper, retaining nested-`>>` behavior;
5. requires an immediate `(` token;
6. constructs a `Call` whose `a` is the already-parsed callee, whose
   `explicitTypeArgs` is the parsed list, and whose `list` comes from `parseArgs()`.

If `(` is absent, report `expected '(' after explicit type arguments` at the next token
and recover as one malformed call. Do not create a fake zero-name `Member`: that shape
would leak into member resolution, method-reference logic, and printers.

### 2.4 Call-only, not a general generic-application expression

The grammar deliberately combines the type list and the call. These are errors:

```lev
var r = identity::<int>;         // generic callable references remain unsupported
var c = Box::<int>;              // no constructed value until a constructor is called
identity::<int>!(payload);       // macro calls are a different surface
```

This keeps LA-25's existing rule intact: a reference to a generic callable has unbound
type slots and is rejected. Supporting a generic function value would require a type
application expression and a representation/rule beyond this ticket.

### 2.5 Exact arity; no mixed explicit/inferred list

The list length must equal the binding owner's declared parameter count:

```lev
R choose<R, S>(R x, S y) => x;

choose::<int, string>(1, "x");  // valid
choose::<int>(1, "x");          // error: expected 2, got 1
choose::<int, string, bool>(1, "x"); // error: expected 2, got 3
```

All-or-nothing is chosen because the language has neither default type parameters nor a
placeholder syntax. Inferring “the rest” by position would make future `_`/named/default
type-argument work inherit an accidental v1 rule. An omitted list continues to mean full
inference exactly as today.

An overload set may contain callables with different generic arities. The explicit count
is an applicability filter: only candidates declaring exactly that many callable type
parameters remain. If none remain, the error names the count rather than pretending the
value arguments failed.

### 2.6 Which parameters the list binds

#### Construction

For `C::Label::<A...>(values)`, bind `C`'s `Stmt::generics`. Constructors do not
declare independent generic lists in the current grammar. Every constructor overload for
the class shares this seed.

#### Function or method

For `callee::<A...>(values)`, bind the selected callable declaration's
`Stmt::generics` positionally. For an instance method on `C<X>`, the receiver still seeds
`C`'s class parameters and the explicit list seeds only the method's own parameters:

```lev
class Box<T> {
    Pair<T, U> pairWith<U>(U other) => Pair(item, other);
    T item;
}

Box<int> b = Box(1);
Pair<int, string> p = b.pairWith::<string>("x");
```

Class/static-side functions have no instance tuple; their explicit list likewise binds
the callable's signature parameters. This matches `info.md` §6.6: a static side cannot
carry class-parameterized state.

### 2.7 Explicit bindings are authoritative

Substitution begins with the explicit tuple, then runs existing inference only to fill
unbound sources (receiver class parameters and, when there is no explicit list, callable
parameters). `unify()` must never overwrite an existing explicit entry.

```lev
T identity<T>(T x) => x;

int a = identity::<int>(5);          // T = int
string b = identity::<string>(5);    // error at 5: expected string
```

The second call does not infer `T=int` and then report an “explicit/inferred conflict.”
It pins `T=string`; the value `5` is simply invalid for the resulting parameter type.

Targets behave the same way:

```lev
Box<int> a = Box::<int>();       // valid
Box<int> b = Box::<string>();    // existing initializer error: Box<string> is not Box<int>
```

The explicit result is computed first and ordinary invariant assignment checks compare it
to the target. This preserves one type-error vocabulary and points at the place whose type
is wrong.

### 2.8 Existing syntax is intentionally unchanged

The following continues to parse as relational operators:

```lev
identity<int>(5)    // (identity < int) > (5)
Box<int>(5)         // (Box < int) > (5)
```

The feature does not reserve new words, change `<`/`>` precedence, consult symbols while
parsing, or reinterpret an existing AST after resolution. A negative parser/checker test
must pin this non-change.

---

## 3. AST, parser, resolver, cloning, and printing

### 3.1 AST representation (`src/Ast.hpp`)

Add one field to `Expr`:

```cpp
// Call only: authored `callee::<T, U>(args)`. Empty means no explicit list.
std::vector<TypeRefPtr> explicitTypeArgs;
```

No new `ExprKind`, token kind, `Member` flag, or inferred-type cache is needed.
`TypeRefPtr` is deliberately retained rather than immediately converting to checker `Type`:

- the Resolver owns namespace/type-parameter lookup and canonical spelling;
- rules can splice a type-position hole through existing `cloneType` hygiene;
- `--expand` needs the authored type syntax;
- LA-18 specialization can substitute a type parameter in the list structurally;
- runtime passes can ignore the vector after checking.

An empty vector unambiguously means “no explicit list” because `::<>()` is a parse error.

### 3.2 Parser implementation (`src/Parser.hpp`, `src/Parser.cpp`)

Factor the type-list loop into a small helper if it keeps `parsePostfix` readable, for
example `parseExplicitTypeArgs()`. The helper must reuse `parseType()` and `expectGt()`;
do not duplicate the type grammar or angle-closing logic.

The explicit branch precedes this current branch:

```cpp
if (at(Dot) || at(ColonColon) || at(QuestionDot)) { ... }
```

Conceptually:

```cpp
if (at(ColonColon) && peek(1).kind == Lt) {
    auto call = mkExpr(Call, base->span);
    call->a = move(base);
    call->explicitTypeArgs = parseExplicitTypeArgs();
    require LParen;
    call->list = parseArgs();
    base = move(call);
    continue;
}
```

The actual implementation should let `parseArgs()` consume the required `(` once the
targeted absence check has fired, so there is one argument-list parser. Macro `!(args)`
ordering remains unchanged.

### 3.3 Type resolution (`src/Resolver.cpp`)

`Resolver::resolveExprTypes` currently descends expression children primarily to find
types carried by `match` arms. Extend its generic descent:

```cpp
for (TypeRefPtr& t : e->explicitTypeArgs)
    resolveType(t.get(), scope);
```

This is mandatory, not a checker convenience. It ensures:

- namespace-qualified type arguments use the same lookup as annotations;
- an enclosing callable's type parameter resolves to `SymbolKind::TypeParam`;
- canonical strings and nested generic arguments are available to `fromTypeRef`;
- pass 2 re-resolves rule-generated lists against the generated expression's lexical
  scope.

Update the function's comment so it no longer claims only match-carried types are
resolved. `is`/`inject` may retain their existing lazy checker path; explicit call
arguments are eagerly resolved because overload selection consumes them immediately.

### 3.4 Clone discipline — every manual expression clone is part of the feature

`Expr` uses unique ownership, so every field-by-field clone must copy the new vector.
This repository has already had silent rule-clone field loss; this is a hard acceptance
gate.

| clone site | required operation | reason |
|---|---|---|
| `RuleEngine::cloneExpr` in `src/Rules.cpp` | clone each entry through `cloneType` | preserves type holes, def-site qualification, and pass-2 hygiene in rules/procedural macros |
| `SpecializationCloner::expr` in `src/Checker.cpp` | clone each entry through its substitution-aware `type()` | `helper::<T>` inside an LA-18 specialized body must become `helper::<Concrete>` |
| `cloneDefaultExpr` in `src/Checker.cpp` | deep-copy each entry through `copyTypeRef` | synthesized default expressions must not drop authored call metadata |
| `cloneForReify` in `src/Checker.cpp` | deep-copy entries through `copyTypeRef` | best-effort structural clones remain honest even when a captured/reified shape grows |

Any new clone found by `rg` during M0 joins this table before code lands. Synthesized calls
that do not author an explicit list naturally leave the vector empty.

### 3.5 Source and AST printers (`src/AstPrinter.cpp`)

Both `exprStr` (debug/`--ast`) and `srcExpr` (`--expand`) render a Call using the
following concatenation:

```text
<callee> + "::<" + <type list> + ">(" + <value args> + ")"
```

when `explicitTypeArgs` is non-empty, otherwise exactly as today. Use the existing
`typeList`/`typeStr` helpers so qualified, nested, union, nullable, and function types
round-trip consistently.

Examples:

```lev
identity::<int>(5)
pkg::Box::From::<Array<int>>([])
expr::Expr::<(User) => bool>((u) => u.active, tree, binds, 0)
```

The marker is emitted by the **Call** printer, after printing its complete callee. The
Member printer stays unchanged. Macro calls always have an empty explicit list. A Call with
both `isMacroCall` and a non-empty `explicitTypeArgs` is an internal AST invariant violation;
assert it in debug builds rather than inventing a source spelling.

---

## 4. Checker design — seed, select, validate, substitute

### 4.1 Keep the no-explicit-list path stable

Calls without `explicitTypeArgs` must use today's inference and overload behavior. In
particular, do not globally change the `mentionsTypeParam` scoring rule while adding this
feature; the regex/lambda overload family around bug #34 is sensitive to apparently small
applicability changes.

Implement explicit handling as an optional candidate seed that becomes active only when the
Call vector is non-empty. Shared helpers are required so the two construction entry paths
(`typeOfCallInner` and target-aware `typeInitExpr`) cannot drift.

### 4.2 Materialize the authored type tuple once per call

After Resolver has populated each `TypeRef`, convert the list to checker `Type`s in source
order. Preserve the `TypeRef`s on the AST for printing; the converted vector is temporary
checker state.

An explicit argument naming an enclosing, still-abstract type parameter follows the
language's existing generic-body leniency: it may materialize as `Unknown` at the definition
site and becomes concrete if an LA-18 specialization substitutes it. It is still an
authoritative occupied slot; later definition-site inference must not replace it with a
different concrete type.

Unknown/non-type names are Resolver errors using the existing type diagnostic; the call
checker must not issue a second arity/applicability cascade for the same malformed list.

### 4.3 One candidate-substitution model

Resolve the callee category exactly as today before forming a seed. In particular, if a
bare name currently resolves as construction rather than a same-spelled free function,
adding `::<...>` does not reverse that priority. For each candidate considered within the
selected category, form a substitution seed:

```text
receiver bindings     class parameters <- receiver.args          (instance method only)
explicit bindings     owner parameters <- call.explicitTypeArgs  (when present)
```

The “owner parameters” are selected by R8:

- construction: `ctorClass->decl->generics`;
- function/method: `candidate->generics`.

Validate explicit arity before adding entries. Candidate overloads may therefore have
different seeds or be rejected solely by generic arity.

It is useful to represent this locally as a small `CandidateGenericSeed`/`Subst` structure
rather than mutating declarations or the Call. The same map then feeds:

- overload applicability and scoring;
- default/injection fill typing;
- deferred lambda/method-reference target typing;
- `inferConstruction` or `genericReturn`;
- LA-18 `recordSpecialization`.

### 4.4 Overload applicability with explicit types

`pickInjecting` currently treats any parameter mentioning a type variable as generically
applicable before inference. That remains correct when there is no explicit list. With an
explicit seed, each parameter is first substituted as far as the seed permits:

```lev
T id<T>(T x) => x;
id::<string>(5);
```

The candidate parameter is `string`, not an unconstrained `T`, so the `int` argument is
inapplicable. Structural forms work the same way:

```lev
int count<T>(Array<T> xs) => xs.length();
count::<string>([1, 2]);      // Array<int> is not Array<string>
```

Candidate evaluation order:

1. reject the candidate if explicit count != its binding owner's count;
2. map positional/named value arguments to parameters as today;
3. substitute each parameter through receiver + explicit seed;
4. fill omissions from defaults/injection using the substituted parameter type;
5. score supplied values against substituted types (exact > widening);
6. preserve fewer-fills then first-declared tie-breaking among applicable candidates.

If a substituted parameter still mentions an unbound type parameter, retain today's generic
leniency for only that unbound portion. Under R4 a callable's own parameters are all bound
when an explicit list is present; remaining unknowns can come from an abstract generic
receiver/body and follow existing rules.

A non-generic overload declares zero type parameters and is filtered out by any non-empty
explicit list. This makes the caller's intent deterministic:

```lev
int f(int x) => 1;
T f<T>(T x) => x;

f(1);          // existing overload rules
f::<int>(1);   // generic overload only
```

### 4.5 Precise sole-candidate value mismatch

When one generic-arity-compatible candidate remains and an explicit substitution makes a
supplied argument inapplicable, emit the local mismatch at the argument span:

```text
argument for parameter 'x' has type 'int', expected 'string'
```

This is not a conflict diagnostic; it is the ordinary parameter contract after substitution.
With multiple compatible overloads, retain the aggregate
`no overload of '<name>' matches the arguments`, because per-candidate mismatch lists are
noise and current overload diagnostics follow that policy.

### 4.6 Construction (`inferConstruction`)

Change `inferConstruction` (or a shared helper it calls) to accept the Call/explicit seed,
not just a span. Its map is populated in this order:

1. explicit class bindings;
2. constructor-argument unification, filling only absent entries;
3. target-type unification, filling only absent entries.

The returned `Type` is therefore the explicit class instantiation even when the constructor
has no type-bearing arguments:

```lev
var b = Box::<int>();      // returns checker type Box<int>, not raw Box
```

Both construction routes must use the same helper:

- ordinary `typeOfCallInner`, where no target is supplied;
- `typeInitExpr`, where a declared initializer/return/field target is supplied.

The target-aware route must not infer over an explicit entry. A target disagreement is left
for its existing assignment/return check as required by R6.

Constructor overload matching must use the same explicit class seed before selection. A
constructor parameter `(T) => R`, `Array<T>`, or other compound type therefore receives the
pinned target during lambda/method-reference deferral and applicability, not only after the
constructor has already been selected.

### 4.7 Generic functions and methods (`genericReturn`)

`genericReturn` already seeds class parameters from the receiver, unifies value arguments,
walks deferred lambdas last, records LA-18 specializations, and substitutes the return. Insert
explicit callable bindings after receiver bindings and before value unification:

```text
receiver class tuple → explicit callable tuple → value unification → lambda-last → return
```

Because `unify` is first-binding-wins, explicit values remain pinned. Deferred lambdas use
the substituted function parameter shape, so this works:

```lev
U apply<T, U>(T x, (T) => U fn) => fn(x);
string s = apply::<int, string>(1, (n) => n.toString());
```

If the lambda returns an `int` while `U` is pinned to `string`, the ordinary lambda/return
assignability check reports the mismatch; lambda-last may not overwrite `U`.

`recordSpecialization` receives the final seeded substitution exactly as it receives inferred
tuples today. An explicit call to an LA-18 specialization-required generic therefore creates
the same concrete demand as an inferred call.

### 4.8 Named/default/injected value arguments

Explicit type arguments are orthogonal to value-argument binding. The existing normalization
still rewrites a successful call to a full positional `list`, while
`explicitTypeArgs` stays untouched for diagnostics and printing.

- **Named arguments:** map first, then compare each supplied value to its substituted
  parameter.
- **Defaults:** defaults on type-variable-typed parameters remain forbidden by the existing
  v1 rule. Other defaults clone normally; the clone must preserve nested explicit calls.
- **Injection:** lookup uses the substituted canonical parameter type. If `T` is explicitly
  `ILogger`, an omitted `T` parameter consults the `ILogger` bind and the synthesized
  `Inject` node carries a concrete copied `TypeRef` for that substituted type.
- **Char/lambda/method-reference retyping:** retain existing special paths, but feed them the
  substituted expected type for the winning candidate.

This ordering avoids selecting a candidate under an unconstrained `T` and discovering only
after normalization that a default, bind, lambda, or method reference cannot satisfy the
pinned type.

### 4.9 Checker re-entry / idempotence

`argsNormalized` makes checker re-entry use a shorter overload path today. That path must not
drop explicit seeds. On re-entry:

- do not append defaults/injections again;
- rebuild or reuse the same receiver/explicit candidate seed;
- apply explicit arity filtering and substituted applicability;
- leave `explicitTypeArgs` intact.

Add a regression that checks the same AST through the two-pass metaprogramming/checker flow;
passing only on the first check is not acceptance.

### 4.10 No lowering payload

After `resolved`, `resolvedClass`, normalized `list`, and result type have been recorded, the
explicit vector has no semantic consumer below the Checker. Eval and Lower already execute the
selected declaration with positional value arguments. They must not append type arguments to
runtime arg vectors or introduce a new IR operation.

---

## 5. Diagnostics (normative strings)

All diagnostics use the explicit-list/argument span and avoid cascades after an unresolved
type.

| ID | condition | required diagnostic |
|---|---|---|
| **E1** | `::<` immediately closes | `expected type argument after '<'` |
| **E2** | parsed list is not followed by `(` | `expected '(' after explicit type arguments` |
| **E3a** | one resolved construction owner has wrong count | `construction of 'Box' expects 1 explicit type argument, got 2` |
| **E3b** | one resolved callable owner has wrong count | `call to 'identity' expects 1 explicit type argument, got 2` |
| **E3c** | overload set has no candidate with that count | `no overload of 'f' accepts 2 explicit type arguments` |
| **E4** | explicit substitution makes the only arity-compatible candidate's supplied value invalid | `argument for parameter 'x' has type 'int', expected 'string'` |
| **E5** | callee is a dynamic/function value rather than a declared target | `explicit type arguments require a declared function, method, or constructor` |

Use singular/plural grammar for `argument(s)` in E3a/E3b, but tests should pin the full
singular and plural examples so wording cannot drift.

Existing diagnostics remain authoritative for:

- unknown/non-type type arguments (`Resolver::resolveType`);
- target/result mismatch (`cannot initialize ...`, return mismatch);
- multiple overloads with matching generic arity but no value match;
- unsupported generic callable references (the parser now fails first because `(` is
  required);
- generic construction with no inference source when no explicit list is supplied.

---

## 6. LA-31 integration — close the feature's trigger

LA-31's `reifyLambda` currently rewrites a lambda into an `expr::Expr(...)` Call and records
in its completed design that the desired generic spelling was impossible. Once M1–M3 are
green, the same track must:

1. deep-copy the concrete `fnRef` into the rewritten Call's `explicitTypeArgs`;
2. keep the existing `resolvedClass`, `resolved` constructor, normalized value list, and
   concrete checker result unchanged;
3. make `--expand` emit:

   ```lev
   expr::Expr::<(User) => bool>(<lambda>, <tree>, <binds>, <siteId>)
   ```

4. reparse and execute the expanded source byte-identically;
5. add a dated note to `designs/complete/techdesign-02-reifier.md` closing its R8 forced
   deviation through this feature (do not rewrite the historical log entry).

This is part of definition-of-done, not an optional follow-up: the missing LA-31 spelling is
the concrete consumer that justified the ticket and exercises a synthesized, already-normalized
Call.

---

## 7. Engine coverage and performance

### 7.1 Active engines

The feature must run byte-identically on all four active paths:

- tree-walk oracle;
- IR interpreter;
- emit-C++;
- LLVM (primary AOT).

Coverage proves the front end erased the new metadata correctly. No backend file should need a
semantic edit; each sees the same resolved Call it saw when the types were inferred.

### 7.2 Frozen ELF policy

`X64Gen.cpp` is frozen and not a project target (`info.md` §0/§17). Do not edit it and do not
gate completion on an ELF result. A non-gating smoke is allowed if the already-lowered call
works for free, but any ELF-only failure is recorded as reference behavior, not a reason to
extend the backend.

### 7.3 Cost model

- parse cost: linear in the authored type list, using the existing type parser;
- check cost: one small substitution map per applicable candidate only when a list is present;
- emitted code size: unchanged (except LA-18's already-existing specialization behavior when an
  explicit tuple creates a concrete demand);
- runtime instructions/data: unchanged;
- ABI and serialization: unchanged.

---

## 8. File-by-file implementation inventory

| file | required change |
|---|---|
| `src/Ast.hpp` | add `Expr::explicitTypeArgs` (Call-only) with a precise comment |
| `src/Parser.hpp` | declare a type-list helper if factored |
| `src/Parser.cpp` | recognize `ColonColon` + `Lt` before ordinary member parsing; parse types with `parseType`/`expectGt`; require `(`; create Call directly |
| `src/Resolver.cpp` | resolve every explicit Call `TypeRef` in lexical scope during both resolver passes; update stale traversal comment |
| `src/Checker.hpp` | thread Call/explicit seed through construction and call-candidate helpers; declare any seed/diagnostic helpers |
| `src/Checker.cpp` | explicit arity filtering; authoritative seed; substituted overload applicability/default/inject/lambda handling; construction + generic-return consumption; precise diagnostics; LA-31 reifier attachment; copy field in every local cloner |
| `src/Rules.cpp` | clone explicit types via `cloneType` in `RuleEngine::cloneExpr` |
| `src/AstPrinter.cpp` | render `::<...>` in both AST and source Call printers |
| `tests/test_parser.cpp` | structural positive cases, nested `>>`, call-only errors, and bare-comparison non-change |
| `tests/test_resolver.cpp` | qualified and enclosing-type-parameter argument resolution |
| `tests/test_checker.cpp` | construction/function/method success; arity, dynamic callee, pinned value mismatch, target mismatch, overload filtering, named/default/inject/lambda interactions |
| `tests/test_meta.cpp` | a rule or procedural macro that emits a call with explicit types, proving clone + pass-2 preservation |
| `tests/corpus/explicit_generic_call_args/` | active-engine runtime corpus and `.expected` output |
| `tests/corpus/expr_reify/` | update/extend LA-31 expansion acceptance to require `expr::Expr::<Fn>` |
| `CMakeLists.txt` | register focused tree-walk, IR, emit-C++, LLVM, and expand-round-trip lanes |
| `docs/reference.md` | §§2.5, 3.1, 3.4, 3.7, 4.4/4.5: distinguish type-position `Name<T>` from call-position `callee::<T>(args)` and document authority/arity |
| `info.md` | §0 landed log when implemented; §9 generics examples/rule corrected from ambiguous “Explicit `<...>`” wording |
| `designs/complete/techdesign-02-reifier.md` | append the R8 closure note after the new spelling lands |

Verify-only: `Eval.cpp`, `Lower.cpp`, `IrInterp.cpp`, `CGen.cpp`, `LlvmGen.cpp`, runtime
sources, and ABI headers. A need to change one is a STOP condition (§11).

---

## 9. Ordered milestones

### M0 — re-probe current truth and freeze the baseline

Before editing:

1. rebuild `build/leviathan` from this checkout;
2. rerun the research repros for `Box<int>(5)`, `identity<int>(5)`, uninferable `Box()`,
   and rejected `Box::<int>(5)`;
3. confirm `::<` remains a free parser slot;
4. locate every field-by-field `Expr` clone with `rg` and update §3.4 if the list changed;
5. run `parsertests`, `resolvertests`, `checkertests`, `metatests`, and the LA-31 expand
   lane to capture the pre-change baseline.

If current behavior contradicts R1's free-slot premise, STOP and amend the design; do not
switch to speculative bare-angle parsing.

### M1 — representation, parser, resolver, printer, clone plumbing

- add the AST field and parser branch;
- resolve the TypeRefs;
- copy them through Rules, specialization, defaults, and reifier clones;
- print them in AST/source modes;
- add parser/resolver/meta structural tests;
- make `--expand` → reparse preserve a hand-authored explicit call even before checker
  semantics are enabled.

M1 is green when the AST shape is stable, nested `>>` works, the invalid call-only forms have
targeted parse errors, and bare `f<T>(x)` still has its old Binary tree.

### M2 — generic construction

- bind class parameters from the explicit list;
- filter/score constructor overloads using the substituted class seed;
- share handling between ordinary and target-aware construction paths;
- implement construction arity and sole-candidate mismatch diagnostics;
- cover default and labeled/qualified constructors.

M2 acceptance includes the formerly impossible `var b = Box::<int>();` plus target conflict,
named/default/injected value arguments, and `Box::From::<T>`.

### M3 — generic functions, methods, and overloads

- bind callable parameters in `genericReturn` before inference;
- apply explicit seeds during overload applicability and re-entry;
- preserve receiver class bindings separately;
- feed substituted types to lambda-last/method-reference handling and injection;
- record explicit LA-18 specialization demands;
- cover free, namespace, static-side, instance, base-qualified, and optional method calls.

M3 is green only if calls with no explicit list show no test churn outside newly precise
diagnostics.

### M4 — LA-31 closure, docs, all active engines

- attach `fnRef` in `reifyLambda` and update the completed design log;
- update `docs/reference.md` and `info.md`;
- land the runtime/negative/expand corpus;
- run all four active engine lanes and the full ctest suite;
- inspect the emitted C++/IR only to confirm no type-argument payload exists.

The design remains in `designs/` until M4 passes. Only then may an implementer move it to
`designs/complete/`.

---

## 10. Test and acceptance matrix

### 10.1 Parser/AST tests

Required structural cases:

```lev
Box::<int>()
N::Box::From::<Array<int>>(xs)
identity::<(User) => bool>(pred)
obj.remap::<string>("x")
outer::<Array<Array<int>>>()
```

Assert each is one `Call` carrying the expected type list and the expected untouched callee
tree. Also assert:

- `identity<int>(5)` remains a `Binary(Gt)` over `Binary(Lt)`;
- `f::<>()`, `f::<int>`, and `f::<int>!(x)` produce E1/E2-style parse failures;
- a normal `NS::f(x)` and macro `name!(x)` AST dump is byte-for-byte unchanged.

### 10.2 Checker-positive matrix

At minimum:

1. nullary generic construction without target/value inference;
2. value-bearing and labeled generic construction;
3. generic function whose type appears only in its return (`empty::<int>()`);
4. explicit identity/function call;
5. namespace-qualified function;
6. instance generic method on a generic receiver (class and method tuples both matter);
7. nested/qualified/function type arguments;
8. generic arity selecting the generic member of a mixed overload set;
9. named arguments after the type list;
10. a default/injected omitted value whose parameter type is concretized by the explicit list;
11. a lambda whose parameter/return targets come from explicit types;
12. an explicit concrete tuple that records and executes an LA-18 specialization;
13. `helper::<T>` inside an LA-18-specialized body, proving specialization cloning substitutes
    the new vector;
14. a rule/procedural macro emitting `helper::<int>(...)`, proving rule cloning + pass 2;
15. LA-31 emitted `expr::Expr::<Fn>`.

### 10.3 Checker-negative matrix

Pin exact/substantial diagnostics for:

- too few and too many construction type arguments;
- too few and too many callable type arguments;
- explicit args on a non-generic declaration (expected 0);
- no overload accepting the explicit count;
- pinned direct and compound parameter mismatch;
- explicit result vs incompatible target (existing initializer diagnostic);
- explicit args on closure variable, call result, indexing result, and IIFE;
- unknown/non-type/incorrectly qualified type arguments;
- a non-matching lambda return under a pinned callable result type;
- a macro-call attempt;
- generic reference without `(`;
- bare `f<T>(x)` remaining unaffected.

### 10.4 Runtime corpus

Create `tests/corpus/explicit_generic_call_args/explicit_generic_call_args.lev` with one
deterministic stdout golden covering construction, labeled construction, free/namespace
function, instance method, overload filtering, nested types, named/default value args, and the
LA-31 output shape where practical. Register:

- `corpus_explicit_generic_call_args_treewalk`;
- `corpus_explicit_generic_call_args_ir`;
- `corpus_explicit_generic_call_args_cpp`;
- `corpus_explicit_generic_call_args_llvm` (inside `LLVM_FOUND`).

The output must be byte-identical. Do not add an ELF gate.

### 10.5 Expansion and cloning corpus

Run `tests/run_expand_roundtrip.sh` over the new corpus. The expanded source must visibly
contain `::<...>` and recompiling it must produce identical output.

Add a meta fixture whose quasiquote contains an explicit generic call and a specialization
fixture whose authored list contains the specialized type parameter. These tests exist because
both clone paths can otherwise lose the field while all hand-authored calls remain green.

Update the LA-31 expand assertion from `expr::Expr(...)` to
`expr::Expr::<(User) => bool>(...)`, while retaining its no-double-reification and byte-identity
checks.

---

## 11. Hazards and STOP conditions

### H1 — `::<` branch ordering

If ordinary `::` member parsing runs first, it reports “expected member name” and loses the free
slot. The explicit branch must be first and direct-to-Call.

### H2 — nested `>>` token splitting

The parser mutates a `GtGt` token one closer at a time. Reuse `expectGt`; duplicating angle logic
will regress nested types or shifts. The nested test is mandatory.

### H3 — overload scoring adjacency

Bug #34 and LA-31's lambda/Expr tie logic make generic applicability sensitive. Calls with an
empty explicit list must stay on the old path. If implementation requires globally changing
score tiers, STOP and correct the design before proceeding.

### H4 — duplicated construction typing

`typeOfCallInner` and `typeInitExpr` both select/check constructors. A fix in only one makes
`var x = C::<T>()` behave differently from `C<T> x = C::<T>()`. Shared explicit-seed logic is
mandatory.

### H5 — normalization re-entry

`argsNormalized` cannot mean “generic work complete.” A pass-2 or repeated check must reapply
the authoritative tuple without cloning fills again.

### H6 — clone loss

Rules and LA-18 specialization are manual field-by-field clones. A hand-authored corpus alone
will miss loss. The two clone-specific fixtures are hard gates.

### H7 — abstract explicit arguments in generic bodies

An enclosing `T` may be unknown during erased definition-site checking. Preserve it as an occupied
explicit slot and retain existing leniency; do not infer a contradictory concrete type. If the
current `Type` representation cannot distinguish occupied-unknown from absent in the seed map,
add that distinction locally to the seed structure. Do not add runtime type descriptors.

### H8 — backend leakage

If Eval/Lower/IR/backend code appears to require the explicit list, STOP. That indicates the
front-end failed to finish substitution/resolution or an unrelated pre-existing bug was exposed.
File the backend issue separately; do not invent an IR type-argument payload.

### H9 — source printer fidelity

Printing the list on Member rather than Call produces the wrong placement for labels/chains and
cannot cover a Name callee uniformly. Both AST and source printers must use Call ownership.

### Mandatory STOP protocol

Stop, append evidence to §14, and seek a design correction if:

- `::<` is no longer a free grammar slot in the rebased tree;
- a parser symbol table/backtracking becomes necessary;
- explicit binding would require runtime reification or a backend edit;
- an unruled overload/partial-application semantic choice appears;
- all four active engines disagree after the front end emits an otherwise ordinary resolved Call.

Do not improvise a bare-angle heuristic or silently narrow feature scope.

---

## 12. Rejected alternatives (closed, so they are not re-litigated)

### 12.1 Bare `callee<T>(args)` with speculative parsing

Rejected. `f(a < b, c > (d))` and a generic call can be syntactically indistinguishable without
symbol knowledge. Backtracking would add a parser mode the tree does not otherwise use and still
leave genuinely ambiguous valid token streams.

### 12.2 Parser-side symbol table

Rejected. Parser and Resolver are deliberately separate; adding declaration knowledge to parse
`<` would be an architectural change affecting forward declarations, imports, aliases, and the
existing declaration lookahead. The ergonomic benefit does not justify that blast radius when a
free explicit marker exists.

### 12.3 Checker reinterpretation of comparison ASTs

Rejected as unsound. Local values can shadow type/function names, and the same Binary tree can
honestly mean a comparison. Resolution cannot recover user intent without a syntax marker.

### 12.4 Store type args on a synthetic Member

Rejected. `::<T>` selects no member and a fake empty selector would contaminate namespace,
constructor-label, method-reference, and dynamic member logic. The list affects invocation, so
Call owns it.

### 12.5 Runtime type descriptors / reified generics

Rejected. This feature supplies compile-time bindings to an erased system. Reification would add
an ABI and engine-wide cost while solving a different, open-world problem.

### 12.6 Explicit bindings merely compared with inference

Rejected. Running inference independently and then comparing creates a second diagnostic class and
can let overload selection happen under the wrong type. Seeding first makes explicit intent
authoritative and lets existing assignability rules identify the actual bad value/target.

---

## 13. Definition of done

The feature is complete only when all of the following are true:

1. R1–R13 are implemented with no unrecorded deviation.
2. The AST owns the list on Call and every manual clone preserves it.
3. Resolver pass 1 and pass 2 resolve the list in lexical scope.
4. Construction, free/namespace/static/instance method calls, overload filtering, named/default/
   injected args, lambda-last, and LA-18 specialization pass focused tests.
5. All diagnostics in §5 have coverage.
6. Bare `f<T>(x)` behavior is explicitly regression-pinned.
7. `--ast` and `--expand` render the canonical spelling; expansion reparses and runs.
8. LA-31 emits `expr::Expr::<Fn>` and its historical deviation is closed by an appended log note.
9. Tree-walk, IR, emit-C++, and LLVM corpus lanes are byte-identical.
10. No runtime/IR/backend semantic file was changed.
11. `docs/reference.md` and `info.md` describe the landed syntax and authority rule.
12. Only after all gates pass is this file moved to `designs/complete/`.

Focused validation before the full suite:

```sh
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure -R 'parsertests|resolvertests|checkertests|metatests'
bash tests/run_corpus.sh build/leviathan --run tests/corpus/explicit_generic_call_args
bash tests/run_corpus.sh build/leviathan --ir tests/corpus/explicit_generic_call_args
bash tests/run_native.sh build/leviathan tests/corpus/explicit_generic_call_args
bash tests/run_expand_roundtrip.sh build/leviathan tests/corpus/explicit_generic_call_args
```

Then run the full configured `ctest --test-dir build --output-on-failure`, including the LLVM
lane when LLVM is configured. Test counts are intentionally not frozen in this document.

---

## 14. Implementation log

No implementation has started. Append dated findings, STOP events, rulings, milestone evidence,
and final validation here; do not rewrite the normative sections to hide deviations.
