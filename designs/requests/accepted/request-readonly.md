# Request: `readonly` — Runtime Write-Once Field Modifier (LA-28)

**From:** Atlantis (all tracks — every DI-held service field). **Date:** 2026-07-07.
**Priority:** P1 — pervasive in the canonical example (`designs/atlantis/example/`) and in
idiomatic framework code (immutable service references, entity keys, config).
**Owner ruling (2026-07-07):** *"It needs to be readonly, it is not a const. A const is
compile time, readonly is run time. It will not be const."*

## 1. The distinction (owner-ruled — this is the whole point)

| | `const` (exists) | `readonly` (this ask) |
|---|---|---|
| **When** | compile time | **run time** |
| **Value** | known and folded at compile time | any **runtime** value (a DI-injected service, a parsed config, a computed id) |
| **Write window** | its initializer | its **initializer + the constructor** |
| **After** | it was never a mutable slot — it's a constant | a real slot, now **immutable** |

`readonly` is **write-once at runtime**: the field is assigned exactly once, during
construction (initializer or constructor body), from a value that need not — and usually
does not — exist at compile time. Every later write is a **compile-time error**. This is
the C#/Java `readonly`/`final`-field semantics. It is emphatically **not** `const`:
`const` is a compile-time constant and cannot hold an injected service or a runtime value.

## 2. Requested surface

```
class UserController : Controller {
    private readonly IUserService userService;         // set once in ctor, from DI

    new UserController(IUserService userService) {
        this.userService = userService;                // the one legal write
    }

    void reassign(IUserService other) {
        this.userService = other;                      // COMPILE ERROR: write to readonly after construction
    }
}
```

- **Placement:** a field modifier, combinable with access modifiers
  (`private readonly`, `public readonly`) and inline or sectional (§2 of info.md).
- **Write rule:** writable in the field initializer and inside the declaring class's
  constructor(s); read-only in every other context (methods, other classes, after
  construction). Enforced by the checker (compile time), like the write-guard `const`
  already applies to its own window.
- **Not transitive** (matching `const`'s stance, info.md §4.3b): `readonly MyClass m`
  fixes the binding `m`, not `m.field`. A `readonly` reference to a mutable object still
  lets you mutate the object; you just can't rebind the field.
- **Not a type:** `readonly` does not participate in assignability, overload resolution,
  or generics — it is a slot write-view modifier, exactly the axis info.md §1's mutation
  table calls "slot" (the same column `const` occupies, but scoped to
  initializer+constructor rather than to a compile-time window).
- **Default construction (§3):** a `readonly` field the constructor does not assign is a
  compile-time error (write-once means write-*once*, not write-*zero-or-once*) — unless it
  has an initializer. (Owner's call whether an unassigned `readonly T?` defaults to `None`
  or is likewise an error; recommend: error, to keep "exactly once" honest.)

## 3. Interaction with the existing `const` field semantics (for the owner)

Flagging factually, not arguing: `const`-on-fields today is documented (info.md §4.3b,
reference §4.3b, `designs/complete/const.md`) as *"write-view scoped to
initializer + declaring-class constructor bodies"* — which is runtime-write-once in
mechanism. Per this ruling the two are **distinct concepts** (`const` = compile-time,
`readonly` = runtime), so the owner will want to decide how they coexist — three shapes,
owner's choice:
  (a) **`const` narrows to compile-time-only** for fields (its value must be a
      compile-time constant), and `readonly` takes over the runtime-write-once role the
      example uses;
  (b) both keywords exist with `readonly` as the runtime-write-once field modifier and
      `const` retaining its current field behavior (some overlap, distinguished by intent
      + whether the initializer is a constant);
  (c) some other split the owner prefers.
Atlantis only needs **(a)-or-(b)**'s outcome: that `readonly` exists with runtime
write-once field semantics. The reference-doc update that lands this ticket should state
the `const`/`readonly` division explicitly (info.md §1 mutation table + §4.3b).

## 4. Acceptance

1. Corpus: a `readonly` field assigned once in a constructor from a runtime value reads
   back correctly; a second write (in a method, or a second ctor-body assignment after the
   first) is a **compile error** naming the field; oracle/IR/LLVM identical.
2. `readonly` field holding an injected/reference value: the field cannot be rebound, but
   the referenced object's own mutation still works (non-transitive).
3. A `readonly` field left unassigned by every constructor (and without an initializer) is
   a compile error.
4. reference.md + info.md §4.3b document the `const` (compile-time) vs `readonly`
   (runtime write-once) division.

## 5. Interim fallback (Atlantis, until this lands)

Plain mutable fields (`private IUserService userService;`), assigned in the constructor by
convention. **Never `const`** — the values are runtime-injected, which `const` (compile-time)
cannot express; substituting `const` would be a semantic error, not a spelling choice. The
example's `readonly` spelling is preserved in all Atlantis source and docs so the flip to
the real modifier is mechanical (delete the interim note, nothing else changes).
