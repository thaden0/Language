# Request: Method References as Values (LA-25)

**Design:** `designs/techdesign-method-references.md` (accepted 2026-07-10) — pairs with
`designs/complete/techdesign-generic-static-members.md` (LA-18); the two are deliberately separate designs
(see the "`::`-generalization pair" note in either doc).

**From:** Atlantis Track 02 (routing — ruling R10), Track 07 (client aspiration).
**Date:** 2026-07-07. **Priority:** P1 — the R10 canonical route table is written in
terms of them (`Route('/login', App::Controllers::AuthController::Login)`), wanted by
AG-2 (2026-10-05).

## 1. The ask

A `::`-qualified reference to a method, in value position, yields a **typed function
value** without calling it:

```
// instance method → UNBOUND reference: receiver is the first parameter
var h = AuthController::Login;     // (AuthController, ILoginRequest) => LoginResponse

// namespace / static-side function → plain function value (no receiver)
var f = App::Routes::addRoutes;    // (Atlantis::App) => void
```

- The reference is resolved statically (ordinary overload resolution by target type or
  explicit disambiguation when the name is overloaded — same rules as a call).
- No new runtime machinery: the value is the same callable a lambda wrapping the call
  would be; `AuthController::Login` ≡ `(AuthController c, ILoginRequest r) => c.Login(r)`
  — the reference form is honesty about intent plus arity-safe brevity.
- Labeled constructors compose for free if the same rule applies to them
  (`User::FromJson` as a `(JsonValue) => User` value) — nice-to-have, not required.

## 2. Why Atlantis needs it

R10's route table binds handlers by naming them, not wrapping them:
`app.AddRoute(Route('/login', AuthController::Login).requestType(...))`. The framework
pairs the unbound reference with a controller factory (per-request activation, Track 02
§R10): construct the controller via DI, apply the reference. Without this, every route
line becomes a hand-written lambda that restates the signature — noise that defeats the
table's readability, or forces attribute-only routing.

## 3. Acceptance

1. Corpus: reference to an instance method, a namespace function, and an overloaded
   method (disambiguated by target type); each invoked through the value; oracle/IR/LLVM
   identical.
2. A reference to a missing/ambiguous method is a compile error at the reference site.
3. The reference's type spells as an ordinary function type (`(A, B) => R`) — assignable
   to fields/params of that type, storable in `Array<(A,B)=>R>`.

## 4. Interim fallback (designed in)

Track 02 ships a `Route(path, factory, lambda)` overload
(`Route('/login', () => AuthController(inject IUserService), (c, req) => c.Login(req))`)
and the `@InjectRoutes();` splice path — both work today; both get replaced by the
reference spelling the day this lands.
