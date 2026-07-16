# Request: Static-Shaped Member Resolution on Generic Type Parameters (LA-18)

**Design:** `designs/complete/techdesign-generic-static-members.md` (**LANDED 2026-07-12**) — pairs with
`designs/complete/techdesign-method-references.md` (LA-25); see that doc's "`::`-generalization pair" note
for why the two are separate designs.

**From:** Atlantis Track 07 (interop; also helps Track 03's P-4). **Date:** 2026-07-07.
**Priority:** P1 for AG-5 (2026-11-25); small ask — probe first (T7-P1), it may partially
work already given duck-typed-at-instantiation generic bodies.

## 1. The ask

Inside a generic body, allow `::`-reached members of a type parameter, resolved at
monomorphization when the concrete type is known (the C++-template model the language
already uses for HKT bodies):

```
McpTool jsonTool<A>(string name, (A) => JsonValue handler) {
    return McpTool(name,
        (JsonValue raw) => handler(A::FromJson(raw)),   // labeled ctor via type param
        A::Empty().schemaJson());                        // any ::-member, same rule
}
```

Bodies are duck-typed at instantiation, so `A::FromJson` has a concrete target at every
use — this is resolution-by-type with the type arriving via substitution, not a new
mechanism. (No constraints system requested; a missing member at instantiation is a
compile error at the use site, exactly like HKT bodies today.)

## 2. What it unlocks

One generic adapter per shape instead of per-type generated adapters: MCP tool dispatch
(`JsonValue → A::FromJson → handler → toJson`), typed client response decoding
(`Promise<A>` from `A::FromJson(body)`), Track 03's decode helpers without the
witness-parameter contortion (`fromDb<T>(DbValue, T witness)` exists only because
`T::...` doesn't resolve).

## 3. Acceptance

1. Corpus: generic function calling `A::SomeLabel(...)` (labeled ctor) and a plain
   `::`-member on 2+ instantiations; oracle/IR/LLVM identical; missing-member
   instantiation = clean compile error naming the use site and the concrete type.
2. Works for value structs AND reference classes as `A`.

## 4. Interim fallback (designed in)

Per-arity/per-type generated adapter members via rules (Track 07 §3.5 ladder), witness
parameters (Track 03). Both deletable on landing.
