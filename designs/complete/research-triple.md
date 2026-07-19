# Research: `Triple<A, B, C>` — the `Pair` counterpart

**Status:** research input for the `Triple` tech design. Not a design; carries no rulings.
**Feeds:** `designs/requests/request-tuple-triple.md` (the ask).
**Date:** 2026-07-19. **Author target:** whoever writes the `Triple` tech design.

This document assembles everything needed to write that design: `Pair`'s exact,
verified current shape (the precedent `Triple` must either mirror or deliberately
diverge from), the generic-struct/class mechanism that makes adding a new prelude
type mechanical across all four active backends, and — most importantly — the one
genuine architectural decision the request ticket does not resolve and gets factually
wrong evidence for: **whether `Triple` should be a `class` (exactly matching what
`Pair` is today) or a `struct` (matching what the request ticket's pseudocode and
prose actually ask for)**. Each is evidenced below with file:line citations so the
design can pin the choice instead of re-deriving it.

---

## 0. One-paragraph statement of the problem

`Pair<A, B>` is a two-field generic prelude type used as the element of relational
joins and `Map` iteration. The request asks for a mechanical three-field sibling,
`Triple<A, B, C>`, with "the same construction/equality/printing conventions as
`Pair`." That instruction is underspecified in a way that matters: **`Pair` is
declared `class Pair<A, B>`, not `struct`** (`src/Resolver.cpp:397`), so today it has
reference identity, no synthesized `==`, and no `toString` — none of the value-type
guarantees ("copied not aliased, field-wise equality, no identity") the request's own
prose claims `Pair` already has. The request's literal code sample
(`struct Triple<A, B, C> { A first; B second; C third; }`) is therefore not actually
"the same shape as `Pair`" — it is a different, stronger kind of type. Both readings
are legitimate outcomes; the tech design must pick one on purpose. Everything else
about `Triple` — prelude placement, backend impact, testing, docs — is mechanical and
low-risk regardless of which reading wins, and is covered in full below.

---

## 1. Correcting the request ticket's premises

Two claims in `designs/requests/request-tuple-triple.md` do not hold up against the
current source and should not be carried into the design as established fact:

1. **"Mirroring `Pair`'s existing shape (`struct`, value type...)"** (request line 37)
   — `Pair` is `class Pair<A, B>` (`src/Resolver.cpp:397`), confirmed alongside the
   two design docs that reference it: `designs/complete/techdesign-05-stdlib-collections.md:9`
   ("`class Pair (43–47)`") and `designs/complete/techdesign-00-overview.md:88`
   ("`class Array / class Map / class Pair / new class Set`"). `Pair` has never been a
   `struct`. See §2 for the concrete behavioral consequences.
2. **The cited source, `designs/suggested-features.md` §2.5/§13** — this file does not
   exist and has no trace in this repository's git history (`git log --all
   --diff-filter=A -- '*suggested-features*'` returns nothing). The request itself
   says the source doc "is being retired," so this is likely accurate as a secondhand
   summary, but the design should treat the "add `Triple`, defer tuple syntax"
   recommendation as **unverifiable from this repo**, not as a citable prior ruling.
   Also unverifiable: whether Track 05
   (`designs/complete/techdesign-05-stdlib-collections.md`) "landed" `Pair`, as the
   request's summary claims (line 4) — that track's own scope line lists `Pair` as an
   already-present region it *consumes* (`techdesign-05-stdlib-collections.md:9`), not
   something it designed or introduced. `Pair` predates Track 05.

Neither correction blocks the feature. Both matter because they're exactly the kind
of thing a design doc's readers will assume is settled and build on.

---

## 2. `Pair`, in full — the precedent

### 2.1 Declaration

The entire prelude is embedded as a C++ raw string literal in `src/Resolver.cpp`
(`kPreludeCore`, starting `src/Resolver.cpp:22`) — there is no separate `.lev` stdlib
file. `Pair`'s declaration, verbatim, `src/Resolver.cpp:394-401`:

```lev
// Array<T>: a tiny native core (length/at/add — add is pure, returns a new
// array) with the LINQ/JS surface written in the language on top of it.
// A 2-tuple, used as the result element of relational joins.
class Pair<A, B> {
    A first;
    B second;
    new Of(A a, B b) { first = a; second = b; }
}
```

Notable properties, each with a direct behavioral consequence:

| property | mechanism | consequence |
|---|---|---|
| `class`, not `struct` | `it->isValue = false` by default (`src/Parser.cpp:1935`, contrast `:1936` for `struct`) | reference identity; `copyValue()` (`src/RuntimeValue.hpp:118-120`) only deep-copies when `isValue` is true — a `Pair` aliases like any other class |
| no explicit `(==)` | — | compares by **reference identity** in every engine (see §2.3) |
| no explicit `toString()` | — | prints as the generic fallback string `"<object>"` in every engine (see §2.4) |
| one named constructor, `Of` | ordinary constructor-overload dispatch by `(label, arity)` (`src/Eval.cpp:380-430`) | called everywhere as `Pair::Of(a, b)`, never `new Pair(a, b)` |
| two public fields, `first`/`second` | ordinary field slots (`Shape::slots`, `src/Symbols.hpp:36-66`) | direct access, no accessor methods |

### 2.2 Every stdlib usage (all in `src/Resolver.cpp`, prelude source)

```lev
// Array<T>.join
Array<Pair<T, U>> join<U>(Array<U> other, (T, U) => bool pred) {
    Array<Pair<T, U>> r = [];
    for (T x in this)
        for (U y in other)
            if (pred(x, y)) r = r.add(Pair::Of(x, y));
    return r;
}                                                            // Resolver.cpp:510-516

// Array<T>.groupJoin
Array<Pair<T, Array<U>>> groupJoin<U>(Array<U> other, (T, U) => bool pred) {
    Array<Pair<T, Array<U>>> r = [];
    for (T x in this) {
        Array<U> matches = [];
        for (U y in other) if (pred(x, y)) matches = matches.add(y);
        r = r.add(Pair::Of(x, matches));
    }
    return r;
}                                                            // Resolver.cpp:517-525

// Array<T>.zip
Array<Pair<T, U>> zip<U>(Array<U> other) {
    Array<Pair<T, U>> r = [];
    int n = length() < other.length() ? length() : other.length();
    for (int i in 0 .. n - 1) r = r.add(Pair::Of(at(i), other.at(i)));
    return r;
}                                                            // Resolver.cpp:553-558

// Array<T>.withIndex
Array<Pair<int, T>> withIndex() {
    Array<Pair<int, T>> r = [];
    int i = 0;
    for (T x in this) { r = r.add(Pair::Of(i, x)); i = i + 1; }
    return r;
}                                                            // Resolver.cpp:581-586

// Map<K, V> : IIterable<Pair<K, V>>, iterator() => MapIterator(this)
//                                                             Resolver.cpp:710, 727

// Map<K, V>.entries
Array<Pair<K, V>> entries() {
    Array<Pair<K, V>> r = [];
    for (Pair e in this) r = r.add(e);
    return r;
}                                                            // Resolver.cpp:732-736

// Map<K, V>.withAll / .mapValues / .whereEntries — consume Pair via for(Pair e in ...)
//                                                             Resolver.cpp:740-754

// MapIterator<K, V> : IIterator<Pair<K, V>>
class MapIterator<K, V> : IIterator<Pair<K, V>> {
    Map<K, V> m; Array<K> ks; int i = 0;
    new MapIterator(Map<K, V> src) { m = src; ks = src.keys(); }
    bool hasNext() => i < ks.length();
    Pair<K, V> next() { K k = ks.at(i); i = i + 1; return Pair::Of(k, m.at(k)); }
}                                                            // Resolver.cpp:760-771

// regex::apiParseReplacement / apiApplyReplacementParts — Pair<int, string>
// tags literal-text vs. group-substitution parts.             Resolver.cpp:4114-4169
// regex::apiWithoutKey — Pair<string, Array<int>> while iterating a Map.
//                                                              Resolver.cpp:4259-4266
```

`Pair` is otherwise an ordinary generic prelude class — nothing here is compiler
magic; every one of these is just a method body using `Pair` the way user code would
use any other generic type.

### 2.3 Equality today — reference identity, always

No backend synthesizes or special-cases `Pair` equality:

- **Eval.cpp** (`src/Eval.cpp:1130-1146`): `findMethod(l.obj->cls, "==")` →
  `callFunction`; falls back to `l.obj == r.obj` (pointer identity) only if no method
  exists.
- **IrInterp.cpp** (`src/IrInterp.cpp:730-737`): identical fallback, with the comment
  "a struct gets a synthesized field-wise `(==)` at resolve time... falls through and
  [is] loud if it ever does [not]."
- **CGen.cpp** (`src/CGen.cpp:1550-1607`): operator dispatch built from
  `collectMembers(cls, mem)`, same reference-identity fallback at `:1599-1602`.
- **LlvmGen.cpp** (`src/LlvmGen.cpp:1243-1260`): per-class method table keyed by
  `selector.text`, same fallback contract.

Because `Pair` is a `class` and defines no `(==)`, it never enters the struct-equality
synthesis pass at all (that pass only runs `for (StmtPtr& it : items) ... if (it->kind
!= StmtKind::Class || !it->isValue ...) continue;` — `src/Resolver.cpp:6604`, guarded
on `isValue`). **`Pair::Of(1, 2) == Pair::Of(1, 2)` is `false` today**, in every
engine — two distinct objects.

### 2.4 Printing today — the generic fallback, `"<object>"`

`src/RuntimeValue.hpp:193-234` (`valueToString`, shared by Eval.cpp/IrInterp.cpp), its
CGen twin `src/CGen.cpp:214-241` (`ts()`), and the LLVM runtime helper `rtToString`
(`src/LlvmGen.cpp:1685`) all special-case exactly one class by name — `Range` (prints
`start..end`) — and fall back to the literal string `"<object>"` for everything else,
`Pair` included:

```cpp
case VKind::Object:
    if (v.obj && v.obj->cls && v.obj->cls->name == "Range") { ... }
    return "<object>";                                    // RuntimeValue.hpp:200-202
```

So "the same printing convention as `Pair`" (request acceptance criterion 1) is, as
stated, a nearly trivial bar — `Pair` has no printing convention beyond the universal
class fallback. `console.writeln(Pair::Of(1,2))` prints `<object>` today, in every
engine. This is a real gap `docs/reference.md` does not surface (§6 below) and worth
the design explicitly deciding whether `Triple` should do better (a hand-written
`toString()` costs nothing and is the obvious next step either way — see §4.4).

### 2.5 `docs/reference.md` — the entire documented surface for `Pair`

`docs/reference.md:1106-1108`, three lines, no equality note, no printing note, no
value/reference-semantics note:

```
### 6.4 `Pair<A, B>`
Fields `first`, `second`; constructor `Pair::Of(a, b)`. The element type of relational
joins and of Map iteration.
```

(The section numbering here is already out of sequence — `### 6.4` appears in the
file before `### 6.3.5` and before `### 6.4.4` — evidently inserted without a full
renumbering pass. Worth noting so a new `Triple` subsection doesn't feel obligated to
fix pre-existing numbering as part of this ticket.)

---

## 3. Why `Triple` is backend-mechanical, regardless of `class`/`struct`

`grep -rn "Pair" src/` turns up exactly one category of *compiler* (not stdlib) hit:
**Map iteration's built-in `for (Pair e in m)` fast path**, which looks `Pair` up by
name to construct the loop's per-iteration entry object, once per engine:

- `src/Checker.cpp:4060-4061` — Map iteration's static element type resolves to
  `classType(scope_->lookup("Pair"))`.
- `src/Eval.cpp:1764-1769` — constructs via `construct(pairSym, "Of", pargs)`.
- `src/IrInterp.cpp:759-765` — builds a `Pair` object generically
  (`obj->fields["first"]/["second"]`).
- `src/CGen.cpp:1667-1670` (`CGen::pairId()`), `:1641-1649` — generated C++ builds a
  Pair via `mkobj(pairId())` + `objset`.
- `src/LlvmGen.cpp:769-771, 3249-3266` — `classIdByName("Pair")`, builds the entry via
  `rtObjNew` + **raw hardcoded payload offsets `i64C(16)`/`i64C(32)`** rather than the
  generic named-field mechanism used everywhere else (contrast the fully generic field
  codegen at `LlvmGen.cpp:2910`: `payAddr(b, regs[in.b], i64C(16 + (in.d - 1) * 16))`).
  This is a hand-optimized micro-path exploiting `Pair`'s known fixed 2-field layout
  for `Map`'s inherently-binary iteration protocol.
- `src/X64Gen.cpp:1586-1625` — analogous frozen-backend native path.

**None of this is triggered by, or relevant to, `Triple`.** `Map` iteration will
always yield `Pair` (a `Map` is intrinsically a 2-tuple); nothing proposes making it
yield `Triple`. This is the *only* place `Pair` is special-cased by name anywhere in
the C++ compiler source — everywhere else, it is used exactly like a user-defined
generic type. `Triple` needs none of this wiring.

### 3.1 The generic mechanism that makes a new prelude type free

There is no per-instantiation monomorphization of class/struct field layout anywhere
in the pipeline — generics are erased at the object-representation level, and every
piece of machinery that touches "how many fields does this type have" iterates a
runtime vector rather than assuming an arity:

- **AST**: `Stmt::generics` (`src/Ast.hpp:349`) is an arbitrary-length
  `std::vector<std::string_view>` — a class's type-parameter count is never hardcoded.
- **Fields**: `Shape::slots` (`src/Symbols.hpp:36-66`) is an arbitrary-length
  `std::vector<Slot>` per class symbol. Every backend's field metadata is generated by
  iterating this vector — `src/CGen.cpp:1447-1489` (`fieldKeys`, `genFieldSlot`,
  `genFieldCount`), `src/Lower.cpp:100-126` (`fieldSlotOf`, `packedSlot` — the shared
  compile-time slot index every backend's IR op reads).
- **Runtime object shape**: `src/RuntimeValue.hpp:56-60` — the Eval.cpp/IrInterp.cpp
  `Object` uses a **name-keyed** `unordered_map<std::string, Value> fields`, arity- and
  generic-agnostic by construction.
- **Construction**: `Evaluator::runCtor` (`src/Eval.cpp:380-430`) finds the constructor
  by `(label, arity)` and executes it as ordinary statements; the no-explicit-ctor
  fallback populates fields positionally from `cls->shape.slots` — no 2-field
  assumption anywhere.
- **Copy semantics**: `copyValue()` (`src/RuntimeValue.hpp:118-125`) deep-copies
  *iff* `v.obj->cls->isValue`, looping `v.obj->fields` generically (arbitrary count).
- The only real "monomorphization" pass in the language — LA-18's demand-driven
  specialization of generic *callables* that do `T::member` static dispatch
  (`src/Checker.hpp:155-193`; `docs/reference.md:228-245`) — is triggered only by
  `T::` member access inside a generic body, is unrelated to field count, and
  explicitly excludes class-level type parameters
  (`docs/reference.md:242-245`: "A class-level type parameter is rejected"). `Triple`
  is a plain data holder with no `T::` usage, so this pass never engages.

**Conclusion: adding `Triple<A, B, C>` as a new prelude class/struct — however many
fields, however many type parameters — requires zero C++ changes to any of the four
active backends' generic machinery.** This confirms the request's own framing
("`Triple` is cheap... not a new design, it's `Pair` with one more type parameter and
one more field").

---

## 4. The one open decision: `class Triple` or `struct Triple`

### 4.1 What the language's `struct` keyword actually buys, per `info.md` §9

`info.md:810-848` ("Value types: `struct` (the object mask, generalized)"), the
authoritative statement of what distinguishes a `struct` from a `class`:

> A `struct` differs from a reference `class` on exactly the axes that make it a
> *value*:
> - **Copied, not aliased.** Binding, passing, returning, or storing a struct copies
>   it (deep...). A `class` keeps reference identity.
> - **No identity.** ... Equality is **field-wise by default via a synthesized `(==)`
>   method**... a struct with a field that has no comparison is a **compile error** at
>   the comparison site — never a silent `false`.
> - **`mutating` methods.** ... a method that writes `this` must be marked `mutating`.
> - **Flat.** A struct may implement interfaces but not inherit implementation...
>   value types are final.

This is precisely the semantic the request's prose describes ("copied not aliased,
field-wise equality, no identity") — but it is **not** what `Pair` (a `class`) has
today (§2.3-2.4). The request conflates "mirror `Pair`'s shape" with "get these value
semantics"; those are two different asks once `Pair`'s actual declaration is checked.

### 4.2 Option A — `class Triple<A, B, C>`, a literal mechanical copy of `Pair`

```lev
class Triple<A, B, C> {
    A first;
    B second;
    C third;
    new Of(A a, B b, C c) { first = a; second = b; third = c; }
}
```

- **Risk: none.** Exactly the same shape as an already-working, already-tested
  generic prelude class. Zero new code paths exercised anywhere.
- **Cost:** inherits `Pair`'s actual (not claimed) semantics — reference identity,
  `Triple::Of(1,2,3) == Triple::Of(1,2,3)` is `false`, prints `<object>`.
- Satisfies acceptance criterion 1 ("same construction/equality/printing conventions
  as `Pair`") **literally and exactly**, because it reproduces what `Pair` actually
  does today, bug-for-bug.
- Does **not** satisfy the request's own prose gloss on that criterion ("copied not
  aliased, field-wise equality, no identity").

### 4.3 Option B — `struct Triple<A, B, C>` relying on synthesized `(==)`

This is the request's literal code sample. It hits a real, documented, and — per the
research below — **never-exercised** gap.

The struct-equality synthesis pass, `synthesizeStructEquality`
(`src/Resolver.cpp:6564-6693`, landed 2026-07-15/17 per
`designs/complete/struct-equality/techdesign-struct-equality.md:1-8`, "Status:
IMPLEMENTED (M1-M3)"), explicitly bails out for any generic value struct, verbatim
(`src/Resolver.cpp:6612-6619`):

```cpp
// v1: no derived (==) for generic structs (the self-type spelling
// with type params is deferred; no existing corpus exercises it).
if (!cls->generics.empty()) {
    rec.badKindNote = "a generic struct (derived '(==)' is not synthesized in v1)";
    program.structEqSynths.push_back(std::move(rec));
    continue;
}
```

If this guard fires, any later `t1 == t2` on a `Triple` value hits the checker's
comparability gate (packet 03, `designs/complete/struct-equality/03-checker-gate.opus.md:4`)
as a **compile error naming the first non-comparable field** — the exact "loud, never
a silent `false`" contract `info.md` §9 promises, just triggered at the *wrong* time
(because `Triple` itself, not one of its fields, is the non-comparable thing).

Why the guard exists — the literal generated source, `src/Resolver.cpp:6662-6663`:

```cpp
std::string src = "struct __eq_" + N + " {\n"
                  "    bool (==)(" + N + " other) => " + body + ";\n"
                  "}\n";
```

`N` is the **bare class name** (`cls->name`, no type-parameter list). For a
non-generic struct like the prelude's `RegexOptions`/`Group`/`Match`
(`src/Resolver.cpp:3873-3908` — the closest existing 3-5-field `struct` precedent,
all non-generic), `N` alone is an unambiguous self-type. For `Triple<A, B, C>`, the
generated parameter type would be the bare, unparameterized `Triple` — whether the
parser/checker resolves a bare generic name as "self, with the enclosing type
parameters bound" inside the class body, or rejects/misresolves it, is **exactly the
open question the packet-02 design flagged and never subsequently closed** (checked
every later packet, `03`–`08` and the roll-up doc — "generic" is never mentioned
again). No corpus file anywhere exercises `==` on a generic value struct today.

- **Risk: real, and unquantified.** `Triple` would be the *first* generic value
  struct to ever reach this code path, in either direction (works cleanly, or needs a
  fix to how the self-type is spelled in the synthesized source).
- **To pursue this option**, the design needs one of:
  (a) lift the `!cls->generics.empty()` bail-out (`Resolver.cpp:6612-6619`) and fix
      the self-type spelling at `Resolver.cpp:6662-6663` to include `cls->generics`
      (e.g. emit `Triple<A, B, C>` instead of bare `Triple`), verified against a new
      corpus case before trusting it; or
  (b) leave the compiler-side gate exactly as-is and use Option C instead (§4.4) —
      strictly less total work, and zero risk to any existing struct-equality
      guarantee.
- **Note the guard is per-file scoped**: it fires on *any* struct with
  `!cls->generics.empty()`, so touching it is a change to the synthesis pass's
  general behavior, not a `Triple`-only carve-out — it would silently start attempting
  synthesis for any *other* future generic struct too. That is very likely desirable
  (removing an artificial restriction) but is a bigger-than-`Triple` change and should
  be named as such if chosen.

### 4.4 Option C — `struct Triple<A, B, C>` with a hand-written `(==)` and `toString()`

```lev
struct Triple<A, B, C> {
    A first;
    B second;
    C third;
    new Of(A a, B b, C c) { first = a; second = b; third = c; }
    bool (==)(Triple<A, B, C> other) =>
        first == other.first && second == other.second && third == other.third;
    string toString() => "(" + first + ", " + second + ", " + third + ")";
}
```

- Gets real value semantics (`struct` → copied, not aliased, per §9 — this part is
  fully general and needs no change for any field count, per `copyValue()`,
  §3.1) **without** touching the equality-synthesis pass at all: a hand-written
  `(==)` is detected by `hasExplicitEq(cls)` (`src/Resolver.cpp:6577`) and the
  synthesis pass skips the class entirely — "the author's relation wins."
- **Risk: none new.** User-written operator overloads and `toString()` on a generic
  class already work today (e.g. any user `class Box<T> { ... (==) ... }` — ordinary
  method dispatch, §2.3/§3's per-backend method tables, no arity/generic
  special-casing anywhere in that dispatch).
- **Cost:** the equality/print logic is written by hand once, in the prelude, rather
  than derived — three extra lines, forever in sync by inspection (unlike a
  synthesized method there's no automatic re-derivation if a field is added, but
  `Triple`'s field list is permanently fixed at three, so this is not a live risk).
- Satisfies both the request's literal acceptance criterion (equality/printing
  "convention... alongside `Pair`" — trivially, since `Pair` has neither) **and** its
  prose gloss (real field-wise equality, real value semantics) **and** closes the
  printing gap `Pair` itself never closed (§2.4) — `console.writeln(Triple::Of(1,2,3))`
  would print something better than `<object>`.

### 4.5 Recommendation surface for the design (not decided here)

Options A and C are both low-risk and fully achievable with today's compiler; the
choice between them is a **product** decision (does `Triple` need value semantics? do
call sites actually rely on `==`/printing?), not a technical one. Option B is
strictly dominated by C for any team that wants generic-struct `==` in general
(because C ships today with existing guarantees, while B requires fixing and
validating an admittedly-deferred compiler code path) — Option B is worth pursuing
**only if** the design's real goal is closing the generic-struct-equality-synthesis
gap itself, with `Triple` as the first corpus case, which is a materially larger and
riskier scope than "add `Triple`." The recommendation surfaced by this research: pick
**Option C** unless the team specifically wants to also land generic-struct `(==)`
synthesis as part of this ticket.

---

## 5. A relevant asymmetry: `Map`-key comparison already works for generic value structs, today

Independent of the `(==)`-operator synthesis gap in §4.3, `Map`'s own key-equality
comparator, `keyEquals`, is a **separate, more primitive, already-fully-generic**
mechanism — it compares fields by raw name-keyed iteration, with no synthesis step
and no generics guard at all (`src/RuntimeValue.hpp:277-296`):

```cpp
// --- key equality for Map (Track 05 C3: primitives by value; structs
// field-wise recursive (a struct IS its fields, §9); classes by identity) -----
inline bool keyEquals(const Value& a, const Value& b) {
    ...
    case VKind::Object:
        if (a.obj && b.obj && a.obj->cls && a.obj->cls == b.obj->cls && a.obj->cls->isValue) {
            if (a.obj->fields.size() != b.obj->fields.size()) return false;
            for (const auto& [k, av] : a.obj->fields) {
                auto it = b.obj->fields.find(k);
                if (it == b.obj->fields.end() || !keyEquals(av, it->second)) return false;
            }
            return true;
        }
        return a.obj == b.obj;
```

This is mirrored identically in `src/CGen.cpp:260` and `lvrt_keyeq`
(`src/LlvmGen.cpp:329, 1912`) — landed as part of the `keyEquals` struct-recursive
rule (`designs/complete/techdesign-05-stdlib-collections.md:187-190, 222`), on 4 of 5
engines (frozen ELF/X64Gen permanently excluded by design, unrelated to `Triple`).
**This path is arity- and generics-agnostic by construction** — it never goes through
`synthesizeStructEquality` at all. Practical consequence: **if `Triple` is a `struct`
(Option B or C), using it as a `Map` key already works correctly today**, regardless
of whether the `==` operator synthesis question (§4.3) is ever resolved — the two
mechanisms are independent, and only the *source-level* `t1 == t2` operator is gated
on synthesis/hand-writing.

---

## 6. Backend impact map (confirms §3's "zero backend work" conclusion per-facet)

| facet | mechanism | Pair evidence | generic/arity-agnostic? |
|---|---|---|---|
| declaration | `Stmt::generics` (arbitrary length) | `Ast.hpp:349` | yes |
| field storage | `Shape::slots` (arbitrary length vector) | `Symbols.hpp:36-66` | yes |
| field access (all 4 engines) | slot-index IR op, generic over `in.d` | `Lower.cpp:100-126`; `LlvmGen.cpp:2910` | yes |
| construction | ctor dispatch by `(label, arity)`; positional slot fallback | `Eval.cpp:380-430` | yes |
| copy semantics | `copyValue()`, gated on `isValue`, loops `fields` | `RuntimeValue.hpp:118-125` | yes |
| `==` (no explicit method) | reference-identity fallback, all 4 engines | `Eval.cpp:1130-1146`; `IrInterp.cpp:730-737`; `CGen.cpp:1599-1607`; `LlvmGen.cpp:1243-1260` | yes (uniform fallback) |
| `==` (synthesized, `struct` only) | `synthesizeStructEquality`, **bails on generics** | `Resolver.cpp:6612-6619` | **no — the one real gap, §4.3** |
| `==` (hand-written) | ordinary method dispatch, any arity/generics | same tables as above | yes |
| `Map` key equality | `keyEquals`, generic + recursive, no synthesis dependency | `RuntimeValue.hpp:277-296` | yes |
| `toString`/print (no explicit method) | fixed `"<object>"` fallback, all engines | `RuntimeValue.hpp:200-202`; `CGen.cpp:214-241` | yes (uniform fallback) |
| `toString` (hand-written) | ordinary method dispatch | — | yes |
| LA-18 `T::` monomorphization | triggered only by `T::member`, excludes class-level type params | `docs/reference.md:242-245` | n/a — never engages for a data holder |
| **Map-iteration fast path (the one place `Pair` is special-cased by name)** | `classIdByName("Pair")` etc. | `Checker.cpp:4060-4061`; `Eval.cpp:1764-1769`; `IrInterp.cpp:759-765`; `CGen.cpp:1667-1670`; `LlvmGen.cpp:769-771,3249-3266`; `X64Gen.cpp:1586-1625` | **irrelevant to `Triple`** — `Map` never yields `Triple` |

The frozen ELF backend (`X64Gen.cpp`) needs **no changes and no design consideration**
per the standing project rule ("ELF is not a project target — never extended, no
feature gated on it"). Its corpus runner (`tests/run_elf.sh:8-11`) already
self-skips gracefully — printing `SKIP (beyond ELF coverage)` — on any construct it
can't lower, rather than hard-failing, so a new `Triple` corpus file cannot break that
lane even if `X64Gen` has some undiscovered gap. In practice it likely passes anyway:
`Pair`-using corpus files already run clean on `--emit-elf` today (per
`techdesign-05-stdlib-collections.md:267`: "`arrays_ext.ext` part 2 ... every engine
incl. `--emit-elf`"), and `Triple` exercises the identical generic-class machinery.

---

## 7. Prelude placement

Insert immediately after `Pair`'s declaration, same file, same string literal
(`src/Resolver.cpp`, inside `kPreludeCore`, right after line 401):

```lev
// A 3-tuple sibling of Pair — see docs/research-triple.md for why this is
// [class|struct] rather than the other choice.
[class|struct] Triple<A, B, C> {
    A first;
    B second;
    C third;
    new Of(A a, B b, C c) { first = a; second = b; third = c; }
    [ + hand-written (==)/toString if Option C is chosen, §4.4 ]
}
```

No natives, no new `Symbol`/`Shape` C++ code, no new IR ops — this is pure prelude
source text, exactly like `Pair`, `RegexOptions`, `Group`, or `Match`.

---

## 8. Testing plan

### 8.1 Corpus (the acceptance criterion's "every active engine" bar)

There is **no dedicated `tests/corpus/pair/` directory** — `Pair` rides along inside
general-purpose corpus files as the return type of existing `Array`/`Map` methods,
confirmed by `grep -rln "Pair" tests/corpus/`:

- `tests/corpus/arrays_ext.ext` — the richest template; **explicitly constructs**
  `Pair` via `Pair::Of(...)` (not just implicit method returns), e.g.:
  ```lev
  Array<Pair<int, string>> z = a.zip(["a", "b", "c"]);
  console.writeln(z.at(0).first);
  console.writeln(z.at(0).second);
  ...
  Array<Pair<int, string>> tagged = [
      Pair::Of(1, "first-one"), Pair::Of(0, "zero"), Pair::Of(1, "second-one")
  ];
  ```
  This is the closest existing template for a hand-built `Triple::Of(a, b, c)` smoke
  test — add a short new section exercising `Triple::Of(...)`, field access
  (`.first`/`.second`/`.third`), and (per whichever equality option is chosen) an
  `==` assertion.
- `tests/corpus/collections.ext` — `Pair` via `Map` iteration
  (`for (Pair e in ages) total += e.second;`) and `Array.join`.
- `tests/corpus/maps_set.ext` — `Pair` as `Map.entries()`'s element type, sorted via
  `.sortBy((p) => p.first)`.
- `tests/corpus/iterator.lev:80-93` — cross-checks the built-in `for (Pair e in m)`
  fast path against the explicit `IIterator<Pair<K,V>>` protocol path for
  byte-identical output. `Triple` has no analogous iteration role, so no equivalent
  file is needed unless a future ticket gives it one.

Every top-level `.ext`/`.lev` corpus file is auto-discovered by every
directory-scanning CMake `add_test` — `corpus_treewalk`, `corpus_ir`, `corpus_native`
(emit-C++), and `corpus_llvm_full` (`CMakeLists.txt:201-207, 828-831, 1125`) — with
**no new CMake registration needed**. (`corpus_elf_full` also scans the same
directory but is not a design concern, per §6.)

**Scale precedent** (from `Pair`'s own history — no dedicated corpus file, no new
CMake targets): expect `Triple` to touch **2-3 existing corpus files** (extend
`arrays_ext.ext`-style construction/field-access assertions; no new directory).

### 8.2 Unit tests (CTest-registered custom harness, `OUT`/`ERRORS`/`CLEAN` macros)

`Pair`'s existing coverage, as a template:

```cpp
// tests/test_eval.cpp:137-139 (relational joins, iterated as Pair)
OUT("void run() { Array a = [1,2,3]; Array b = [2,3,4]; "
    "Array j = a.join(b, (x, y) => x == y); console.writeln(j.length()); "
    "for (Pair p in j) console.writeln(p.first); } run();", "2\n2\n3\n");

// tests/test_eval.cpp:207-208 (Map iteration as Pair)
OUT("void run() { Map<string, int> m; m[\"a\"] = 30; m[\"b\"] = 26; int t = 0; "
    "for (Pair e in m) t += e.second; console.writeln(t); } run();", "56\n");

// tests/test_checker.cpp:239 (HKT negative test using Pair<int,int> as a
// deliberately-wrong container head)
ERRORS("F<A> keepIt<F, A>(F<A> c) => c; "
       "void f() { Array<int> a = [1]; Pair<int, int> p = keepIt(a); }");
```

Add analogous `OUT` cases in `tests/test_eval.cpp` for `Triple::Of(...)` construction
+ field access, and (if Option B or C) an `OUT` case asserting `==` behavior. No new
`ERRORS` case is obviously needed unless the design wants to pin a specific
diagnostic for `Triple`'s comparability (unlikely, since all three type parameters are
ordinary and always comparable transitively through the same rule as `Pair`/`Array`).

### 8.3 Docs

Add a `Pair`-adjacent subsection to `docs/reference.md` (near `### 6.4 Pair<A, B>`,
`docs/reference.md:1106-1108`), matching its existing terseness unless Option C's
extra semantics warrant a line each for equality/printing (recommended, since
`Pair`'s silence on those points is a documentation gap this ticket can quietly avoid
repeating rather than must repeat for "consistency"):

```
### 6.4a `Triple<A, B, C>`
Fields `first`, `second`, `third`; constructor `Triple::Of(a, b, c)`.
[If Option C: "A value type (`struct`): copied not aliased, field-wise `==`,
prints as `(first, second, third)`."]
```

(Note the existing numbering disorder at `### 6.4`/`### 6.3.5`/`### 6.4.4` predates
this ticket — don't feel obligated to fix it as a side effect.)

---

## 9. Summary — what the design doc actually needs to decide

Everything in §§3, 6, 7, 8 is mechanical, evidenced, and low-risk regardless of
outcome. The design has exactly one real fork to rule on:

1. **`class Triple` (Option A, §4.2)** — zero risk, exact behavioral mirror of
   `Pair` as it actually exists (reference identity, no `==`, `<object>` printing).
2. **`struct Triple` with synthesized `(==)` (Option B, §4.3)** — matches the
   request's literal code sample, but is the first-ever exercise of a compiler code
   path (`Resolver.cpp:6612-6619, 6662-6663`) explicitly deferred and never validated
   for generic value structs; requires a small, out-of-`Triple`-scope compiler fix
   plus new corpus proof before it can be trusted.
3. **`struct Triple` with hand-written `(==)`/`toString()` (Option C, §4.4)** — real
   value semantics, zero new compiler risk, closes `Pair`'s own printing gap as a free
   side effect. **Recommended** unless the team wants to use this ticket to also land
   generic-struct-equality synthesis.

Whichever is chosen, flag explicitly in the design (per the request's own warning,
lines 45-49) that **anonymous tuple syntax remains untouched** — nothing here
proposes `(int, string, bool)` type syntax or literal tuple construction; `Triple` is
only ever spelled `Triple<A,B,C>` / `Triple::Of(...)`, same as `Pair`.
