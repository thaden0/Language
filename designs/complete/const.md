# `const` — Technical Design

**Status:** implemented and verified. **Date:** 2026-07-04. **Verified:** 2026-07-10.
**Companion:** `argv.md` (probed in the same pass; its `env::args()` surface ultimately
chose the function form over a const global — argv.md §3 — so const's first prelude
consumers are `console` and the `OpenMode` flags, §6).

Everything below was originally verified against this tree (`build/lang`, Jul 4) by reading
source and running probe programs; file/line pointers are to that working copy. The completed
implementation has since been verified against the current tree with the full CTest suite.

---

## 0. Motivation

Four concrete holes, all found while building real programs in the language:

1. **Globals cannot state "fixed after init" — and today's behavior is a silent no-op.**
   Probed: `std::read = std::OpenMode(9);` compiles, runs, produces **no error and no
   effect** — `std::read.bits` still reads `1` afterwards. Silently-ignored assignment
   is the exact failure mode the design bans everywhere else (info.md §16, reference
   §3.7: "runtime failures are loud"). **Owner ruling (2026-07-04): this must be a
   compile-time error — filed as bug.md #7.** `const` is what makes that rejection
   principled rather than ad-hoc: prelude globals get marked `const`, and the error
   falls out of the general rule (`cannot assign to const 'read'`) instead of a special
   no-assigning-globals case.
2. **The prelude's value-globals are facts that can't say so.** `console`
   ([Resolver.cpp:679](src/Resolver.cpp#L679)) and the `OpenMode` flags
   (`std::read/write/append/binary`, [Resolver.cpp:500-503](src/Resolver.cpp#L500)) are
   fixed values by intent, with no way to declare it — which is what makes the silent
   no-op in (1) possible at all. Marking them `const` is the principled end-state of the
   bug #7 ruling. (`argv.md`'s `env::args` was considered as a const consumer too, but
   chose the function form for comptime-hermeticity and init-order reasons — argv.md §3;
   const is not load-bearing there.)
3. **Narrowing invalidation is coarser than it needs to be.** Assignment invalidates flow
   narrowing ([Checker.cpp:745](src/Checker.cpp#L745), `invalidatePath`). A binding that
   *cannot* be assigned never needs invalidating — `const` bindings keep their narrowing
   for their whole scope, for free.
4. **The concurrency story leans on immutability** (info.md §14: "immutable data shares
   freely — cannot race; shared mutable access is the gated, marked exception") but the
   language currently has no way to *declare* an immutable binding. `const` is the
   declaration side of that guarantee.

And one gap in the existing constant story: `comptime` (reference §6.9) covers values
fixed **at build time**, hermetically — it is *forbidden* from touching the process
environment. Nothing covers values fixed **at startup** and immutable thereafter. argv is
the poster child: it cannot be `comptime` (hermeticity) and shouldn't be `var`
(mutable). `const` is the missing middle of the spectrum:

```
comptime  —  value fixed at compile time (folded to a literal)
const     —  binding fixed at initialization time            <- this design
var       —  binding free to change
```

---

## 1. The one rule

> **`const` scopes a slot's write view to its initialization window.** After the window
> closes, only the read view remains.

That is the entire feature. It is not a type, not a property of the value, and not a new
kind of member — per the one-member rule (info.md §6.5), a binding is a typed slot with
views over it (§6's get/set machinery). `const` states *when the write view exists*:
during initialization, and never again.

This completes a mutation-control matrix the language already has two-thirds of, each
axis orthogonal and already principled:

| axis | question | mechanism | status |
|---|---|---|---|
| **slot** | when may the binding be written? | `const` | this design |
| **value** | does the value alias or copy? | `struct` / pure `Array`/`Map` (§9, §11) | exists |
| **view** | which access views are exposed? | `get`-only accessors (§6) | exists |

There is deliberately **no fourth axis** — no type-qualifier const. See §2.

---

## 2. What `const` is NOT (the honesty section)

- **Not transitive.** `const MyClass m = MyClass();` fixes the *binding* `m`; the object
  it names remains a mutable reference object — `m.field = 5;` is legal. This is C#
  `readonly` semantics, chosen deliberately: deep immutability is already the job of the
  other two axes (want an immutable value? use a `struct`/`Array`/`Map`; want read-only
  access? expose only `get` views). One axis per concern, no overlap.
- **Not a type.** `const int` is not a different type from `int`. `const` never appears
  in a type position, never participates in assignability, overload resolution, or
  generic instantiation; `Array<const T>` does not exist. This is what kills
  const-contagion (the C++ experience: const-correctness colors every signature it
  touches) **by construction** — the same anti-contagion stance that rejected `any`,
  async coloring, and `Result`-threading (info.md §9, §12.6, §14). A const slot's value
  assigns to a var slot and vice versa, freely; only writes to the const slot itself are
  restricted.
- **Not `comptime`.** A `const` initializer runs at normal initialization time with full
  runtime capability (natives included — that's the point, for argv). `comptime` remains
  the build-time tier; the two compose (`comptime` vars are conceptually const already —
  they reify as literals).

---

## 3. Surface and semantics

`const` is a declaration modifier, written where `distinct` and `mutating` already sit
(before the type / before the inference marker):

```
const int maxRedirs = 50;                    // local: fixed at declaration
const var limit = 100;                       // composes with inference
const Array<string> args = std::sysArgs();   // namespace global: fixed at startup

class Session {
    public const string id = "s-1";          // field, initializer form
    const int seq;                           // field, constructor form
    new Session(int s) { seq = s; }          //   ...the ctor window may write it
    void bump() { seq = seq + 1; }           // ERROR: outside the window
}

void handle(const Options o) { ... }         // parameter (allowed; rarely needed)
for (const string a in args) { ... }         // per-iteration binding
```

Per slot kind, the initialization window is:

1. **Locals** — the declaration initializer, exactly. `const int x;` (no initializer) is
   a **compile error**: bare declaration auto-constructs (§3 of info.md), and
   auto-constructing a const would freeze the default forever — a provably useless slot.
   Refusing provably-useless code is an error, not a special case (same family as "no
   applicable overload"). (Definite-single-assignment relaxation — `const int x; if (c)
   x = 1; else x = 2;` — is §9 Open Question 1; the flow engine could support it later.)
2. **Fields** — the field initializer plus **the declaring class's own constructor
   bodies** (the synthesized `$init` and each `new`). Multiple writes *within* the window
   are fine (C# readonly semantics — the window is what's enforced, not once-ness).
   Writes from ordinary methods, from other classes, or after construction are compile
   errors. Under multiple inheritance, a base's const field is writable only by the
   base's own constructors — the derived ctor reaches it by calling `Base::Ctor(...)`,
   which is already the construction model (§2/§4 of info.md).
3. **Namespace / class-static globals** — the initializer, only. Any later assignment is
   a compile error. (Today's silent no-op on prelude globals becomes a loud refusal.)
4. **Parameters** — the call binding. Reassignment in the body is an error. This falls
   out of the local rule; it's permitted for completeness but rarely worth writing.
5. **`for (const T x in ...)`** — each iteration is a fresh binding; `const` documents
   that the body doesn't reassign it.

---

## 4. Interactions — each falls out of the one rule

- **Compound assignment.** `a op= b` ≡ `a = a op b` (reference §12.7) → a write → error
  on a const slot. The checker already routes compound assignment through the same
  branch as plain assignment ([Checker.cpp:734](src/Checker.cpp#L734)).
- **Indexer sugar on pure values.** On a pure array, `a[i] = v` is *rebind* sugar
  (reference §3.6) → a write to the slot → `const Array<T> a; a[i] = v;` is an error.
  On a mutable *object* with a `([])` set accessor, `obj[i] = v` mutates the object in
  place and never touches the slot → allowed through a const binding. Both answers come
  from the same one-rule reading of `a[i] = v` (§6.7) — no new decision needed.
- **Struct `mutating` methods.** A `mutating` method writes `this` — i.e., writes the
  value in the receiver's slot. Calling one through a const binding therefore requires a
  write view that doesn't exist: compile error ("cannot call mutating method
  'translate' on const 'p'"). Swift-identical, and it lands at the same enforcement
  family as the existing value-struct write check
  ([Checker.cpp:732-739](src/Checker.cpp#L732), helper `writesThisField` at
  [:1126](src/Checker.cpp#L1126)).
- **Narrowing.** Assignment is what invalidates narrowing
  ([Checker.cpp:745](src/Checker.cpp#L745)). Const bindings can't be assigned, so their
  narrowing persists for the entire scope: `const string? h = env::variable("HOST");
  if (h != None) { /* h : string from here on, unconditionally */ }`. Zero new
  machinery — invalidation simply never fires for const paths.
- **Closures.** Capture is by snapshot (reference §7.1). Capturing a const is naturally
  coherent — the snapshot can never drift from the origin. (Capturing a `var` and
  mutating inside the closure already doesn't propagate out; const just makes the
  fixedness explicit at the declaration site.)
- **MI / `distinct` / collisions.** Collision detection stays keyed on **name + type**
  (§4 of info.md) — const is not part of the type. If two same-name same-type inherited
  members would collapse into one slot but disagree on constness, the merged slot's
  constness is ambiguous → **refuse to guess**: compile error; resolve with `distinct`
  or by matching the declarations. (The same shape as every other ambiguity rule.)
- **Accessors.** A const field may carry a `get` view (a read view over a
  ctor-window slot — fine). Declaring a `set` accessor on a const field is a compile
  error: a set view is a write view that would outlive the window.
- **Interfaces.** v1: `const` is not allowed in interface requirements. An interface
  wanting to *offer* read-only access already requires a `get` view (§6/§8); requiring
  the implementer's backing slot to be const would constrain internals rather than
  surface. Deferred (§9 Open Question 3).
- **DI `bind`.** No interaction: a binding's value flows through ordinary parameters,
  which are fresh slots per call.

---

## 5. `let` — resolving the two-words-one-meaning duplication

Today `var` and `let` are **both** pure inference markers with identical meaning
(reference §2.1; the parser treats them interchangeably at
[Parser.cpp:200](src/Parser.cpp#L200), [:579](src/Parser.cpp#L579),
[:742](src/Parser.cpp#L742), [:1225](src/Parser.cpp#L1225)). Two words with one meaning
is the mirror image of the §11 rule ("one word never means two things") — an
unprincipled duplication waiting for a distinction.

**Proposal: `let` ≡ `const var`** — a single-assignment inferred binding. `var` stays the
mutable inferred binding. This is the Swift split, and it lands on the sugar shelf next
to `=>` (return) and `T?` (`T | None`): `let` is not a new construct, it is the composed
spelling of two existing ones.

Migration cost, measured: there is exactly **one** `let` in the entire tree
([tests/corpus/churn/closures.ext:12](tests/corpus/churn/closures.ext#L12),
`let f = (x) => x + i;`) — and `f` is never reassigned, so it compiles unchanged under
the new meaning. The migration is free today and only gets more expensive with time.

Severability: `const` stands alone if the `let` repurposing is declined — `let` would
simply remain an alias of `var`.

---

## 6. Prelude adoption (immediate consumers)

- `const Console console = Console();` ([Resolver.cpp:679](src/Resolver.cpp#L679)) — the
  global console can no longer be silently "reassigned" (aliasing stays fine:
  `Console c = console;`).
- `const OpenMode read/write/append/binary`
  ([Resolver.cpp:500-503](src/Resolver.cpp#L500)) — the open-mode flag values are facts,
  not variables.
- **Not** `std::env::args` — argv.md ultimately chose a *function* (`env::args()`) over
  a const global, for comptime-hermeticity and init-order reasons (argv.md §3). Its
  return value is protected by array purity, not by const.
- Internal mutable state (Timer ids, stream buffers, TcpStream watch flags) stays `var`,
  correctly.

---

## 7. Implementation inventory

`const` is a **pure front-end feature**: after the checker, it does not exist.

1. **Tokens** — add `KwConst` to the enum and `{"const", TokenKind::KwConst}` to the
   keyword table ([Token.cpp:91](src/Token.cpp#L91), `keywordKind`); reference §1.3
   keyword list gains `const`.
2. **Parser** ([Parser.cpp](src/Parser.cpp)) —
   - member declarations: accept it where `mutating`/`distinct` are accepted
     (`parseClassMemberInner`, [:807-812](src/Parser.cpp#L807)) → `m->isConst`;
   - local/global declarations: accept an optional leading `KwConst` before the type or
     the `var`/`let` marker at the decl sites ([:579](src/Parser.cpp#L579),
     [:742](src/Parser.cpp#L742), and `looksLikeVarDecl` [:1225](src/Parser.cpp#L1225));
   - parameter lists: optional `KwConst` per param;
   - if §5 is adopted: `KwLet` sets `inferred + isConst`.
3. **AST** ([Ast.hpp](src/Ast.hpp)) — one `bool isConst` alongside the existing
   `distinct` / `isMutating` flags on the declaration nodes.
4. **Checker** ([Checker.cpp](src/Checker.cpp)) — the real work, and it is localized:
   - **assignment site** ([:734-752](src/Checker.cpp#L734), where the value-struct
     `mutating` check already lives): resolve the assignment target to its declaring
     slot (the path machinery narrowing already uses — `pathOf`/`invalidatePath`
     [:745](src/Checker.cpp#L745), `writesThisField` [:1126](src/Checker.cpp#L1126));
     if the slot is const and the site is outside its window → error. Window test for
     fields: `curMember_->isCtor && thisClass_ == declaring class` — both already
     tracked at [:735](src/Checker.cpp#L735). Compound assignment already flows through
     this branch;
   - **mutating-call site**: when a resolved callee `isMutating`, classify the receiver
     expression; a const slot receiver → error (extends the same receiver
     classification the §9 check uses);
   - **declaration checks**: const local without initializer → error; `set` accessor on
     a const field → error; shape-collapse constness mismatch → error (in the resolver's
     shape build, where same-name+type merge already happens);
   - **narrowing**: skip `invalidatePath` for const targets (they can't be reached by
     assignment anyway once the write check errors — strictly a precision/no-op detail).
5. **Eval / IR / Lower / all four backends — zero changes.** No runtime representation,
   no IR bit, no emission change. (Optional follow-on, not required: `Ownership.cpp`
   could exploit "const ⇒ never rebound" for escape precision.)
6. **Prelude & docs** — §6 markings; reference §1.3 + a new §4.x "const declarations" +
   §2.1 var/let note; info.md gets the §1 matrix.

## 8. Testing

All compile-accept / compile-reject — there is no runtime behavior to test, which is
itself the point.

- **Positive corpus**: const local / global / field (both initializer and ctor forms) /
  param / for-in; `const var`; `let` single-assignment (if §5 adopted); narrowing
  persistence on a const optional across branches; non-mutating struct method called
  through a const binding; indexer *reads* through const arrays; aliasing a const
  binding into a var binding and mutating the alias's own slot.
- **Negative (checkertests)**: reassign const local; compound-assign const; `const int
  x;` without initializer; field write from a non-ctor method; field write from another
  class; derived-ctor direct write to a base const field; `set` accessor on const
  field; `mutating` call on const struct binding; `a[i] = v` on const pure array;
  collision constness mismatch; assignment to const namespace global (the `std::read`
  probe case, now loud); reassigned `let` (if adopted).
- **Differential**: run the positive corpus on `--run`/`--ir` (outputs identical by
  construction — no runtime change).

## 9. Open questions

1. **Definite single assignment for locals** (`const int x; if (c) x = 1; else x = 2;`)
   — the flow-narrowing engine could carry it; deferred to keep v1 to one rule, one
   window.
2. **Sectional `const:`** — access modifiers have a sectional form (§2 of info.md); a
   const section for blocks of constants is plausible sugar. Deferred.
3. **Interface const requirements** — see §4; deferred in favor of get-view contracts.
4. **Deep-freeze** — explicitly out of scope, likely forever: the value axis (structs,
   pure collections) is the language's answer to immutable *data*; a gated `freeze`
   would be a fourth mechanism duplicating it.
5. **Naming** — `const` is an abbreviation in a full-words-trajectory language (§9 of
   info.md). Like `std` and `env`, it has crossed into term-of-art status, and the
   alternatives drag in other languages' different semantics (`final` = Java's
   non-override; `readonly` = C#'s field-only; `val` = Kotlin). Keeping `const`.
