# `Triple<A, B, C>` — a three-field value sibling of `Pair`

**Status:** ready. **Date:** 2026-07-19.
**Depends on:** nothing unlanded. Rides entirely on already-shipped infrastructure —
the generic-class/struct machinery that backs `Pair`, ordinary operator-overload and
`toString()` method dispatch (all four active engines), and Track 05's `keyEquals`
struct-recursive `Map`-key rule. No new natives, no new IR ops, no backend edits.
**Source (the ask):** `designs/requests/accepted/request-tuple-triple.md`.
**Source (the evidence):** `docs/research-triple.md` — the verified precedent survey and
the three-way option analysis this design rules on. Read that first; this document
decides what it laid out.
**Owns (regions):**
- kPrelude `Triple<A, B, C>` — new class in the `kPreludeCore` raw-string literal,
  inserted immediately after `Pair`'s declaration (`src/Resolver.cpp`, after the block
  at `:397-401`).
- `docs/reference.md` §6.4 neighborhood (new `Triple` subsection beside `Pair` at
  `:1106-1108`).
- New/extended corpus assertions (§7) and new unit-test `OUT` cases (§8).

**Does NOT own / touches nothing in:** `RuntimeNatives.cpp`, `CGen.cpp`, `LlvmGen.cpp`,
`IrInterp.cpp`, `Eval.cpp`, `Checker.cpp`, `runtime/lv_runtime.c`, `runtime/lv_abi.h`,
`X64Gen.cpp`, `src/Resolver.cpp`'s `synthesizeStructEquality` pass. See §3 and the
Prime directive — every one of these was checked and is deliberately left untouched.

**Prime directive:** `Triple` is expressible **entirely in prelude source text**, using
the same generic-class machinery that already backs `Pair`, `Array`, `Map`, and the
non-generic prelude structs (`RegexOptions`/`Group`/`Match`). **Zero C++ changes to any
backend.** The one code path that could have forced a compiler change — the
struct-equality synthesis pass, which explicitly bails on generic structs
(`src/Resolver.cpp:6880-6885`) — is sidestepped by design, not touched (§4, §5). This is
the central engineering decision of this ticket and it is a *scoping* decision, not an
implementation one.

---

## 1. What this design decides

The research doc (`docs/research-triple.md`) establishes, with file:line evidence, that
adding `Triple<A, B, C>` is backend-mechanical and low-risk **regardless** of how it is
declared, and isolates exactly one real fork the request ticket does not resolve:

> **Is `Triple` a `class` (a literal mechanical copy of what `Pair` *is* today) or a
> `struct` (what the request's own pseudocode and prose actually ask for)?**

The request ticket gets its own evidence wrong on this point in two ways the design must
not inherit as fact (both proven in `research-triple.md` §1):

1. It claims `Triple` should "mirror `Pair`'s existing shape (`struct`, value type)."
   **`Pair` is `class Pair<A, B>`** (`src/Resolver.cpp:397`), not a struct. It has
   reference identity, no synthesized `==`, and prints `<object>`. So "mirror `Pair`"
   and "be a value type" are two *different* asks, and the ticket conflates them.
2. It cites `designs/suggested-features.md` §2.5/§13 as prior authority. That file does
   not exist in this repo or its history; treat the "add `Triple`, defer tuple syntax"
   recommendation as a sound but **uncitable** secondhand summary, not a standing ruling.

**This design rules: `Triple` is a `struct` with a hand-written `(==)` and `toString()`
— Option C in `research-triple.md` §4.4.** Rationale, decision record, and the two
rejected alternatives are in §4. Everything downstream (§5–§9) follows from that ruling.

### 1.1 Non-goals (explicit, per the request's Known Warnings)

- **Anonymous tuple syntax stays untouched.** Nothing here introduces `(int, string,
  bool)` as a type or bare `(a, b, c)` literal construction. `Triple` is only ever
  spelled `Triple<A, B, C>` and built with `Triple::Of(a, b, c)`, exactly as `Pair` is
  only ever `Pair<A, B>` / `Pair::Of(a, b)`. Approving `Triple` does **not** decide the
  tuple-syntax question by default; that remains a separate, larger design.
- **No `Quad`, no `zip3`, no arity-4+.** A `zip3`-shaped combinator and any four-field
  successor are named in the request as "natural next asks" but are explicitly out of
  scope. If a four-field case is later wanted, that ticket decides then whether to
  continue the pattern (`Quad`) or make it the trigger to design tuples — this design
  does not pre-decide it.
- **`Map` iteration keeps yielding `Pair`.** A `Map` is intrinsically a 2-tuple; nothing
  here proposes making it yield `Triple`. The one place `Pair` is special-cased by name
  in the compiler (Map's `for (Pair e in m)` fast path) is untouched and irrelevant
  (`research-triple.md` §3).

---

## 2. Surface — the exact prelude source that lands

Inserted into `kPreludeCore` (`src/Resolver.cpp`), immediately after `Pair`'s closing
brace at `:401`:

```lev
// A 3-tuple sibling of Pair. Unlike Pair (a reference `class`), Triple is a
// `struct` value type — copied not aliased, field-wise equality, no identity
// (info.md §9). The (==) and toString() are hand-written rather than synthesized:
// the struct-equality synthesis pass bails on generic structs by design
// (Resolver.cpp:6880), so a derived (==) is unavailable here — and a hand-written
// relation is the recommended path anyway (see designs/techdesign-triple.md §4).
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

That is the whole of it. No natives, no new `Symbol`/`Shape` C++, no new IR ops — pure
prelude text, exactly like `Pair`, `RegexOptions`, `Group`, and `Match`.

### 2.1 Line-by-line intent

| line | intent | why it is safe today |
|---|---|---|
| `struct Triple<A, B, C>` | value type; three type parameters | `Stmt::generics` is an arbitrary-length vector (`src/Ast.hpp:349`) — arity is never hardcoded. `struct` sets `isValue = true` (`src/Parser.cpp:1936`). |
| `A first; B second; C third;` | three public data slots | `Shape::slots` is an arbitrary-length vector (`src/Symbols.hpp:36-66`); every backend generates field metadata by iterating it. |
| `new Of(A a, B b, C c)` | one named constructor, mirroring `Pair::Of` | ordinary ctor dispatch by `(label, arity)` (`src/Eval.cpp:380-430`); called `Triple::Of(a, b, c)`, never `new Triple(...)`. |
| `bool (==)(Triple<A, B, C> other) => …` | field-wise equality, hand-written | detected by `hasExplicitEq(cls)` (`src/Resolver.cpp:6755`), so the synthesis pass skips this class entirely (`:6871`, "the author's relation wins"). Ordinary generic-method dispatch — no arity/generic special-casing anywhere (`research-triple.md` §2.3). |
| `string toString() => "(" + … + ")"` | printable, closing the gap `Pair` left open | ordinary method dispatch; the engines' `<object>` fallback (`src/RuntimeValue.hpp:200-202`) only fires when *no* `toString()` exists. |

### 2.2 Why the self-type spelling `Triple<A, B, C>` in the `(==)` parameter is correct

A user-written operator overload on a generic prelude class is already a solved, tested
shape — this is not new ground. The hand-written `(==)` spells its parameter
`Triple<A, B, C>` (fully parameterized with the enclosing type parameters), which is the
ordinary way a generic class refers to itself in a method signature and resolves through
the normal checker path. This is *precisely the spelling the synthesis pass could not
safely auto-generate* — the generated source uses the bare, unparameterized class name
`N` (`src/Resolver.cpp:6926-6927`), whose behavior for a generic self-type was the
deferred, never-validated question (§4.2). By writing the fully-parameterized form by
hand, we get the correct relation with zero dependence on that unresolved path.

---

## 3. Backend impact — confirmed zero, per facet

Reproduced from `research-triple.md` §6, re-verified against current source. Every facet
`Triple` touches is already arity- and generics-agnostic; the one facet that is *not*
(`synthesizeStructEquality`, the generic-struct bail-out) is the one this design routes
around instead of into.

| facet | mechanism | evidence | Triple-safe? |
|---|---|---|---|
| declaration (3 type params) | `Stmt::generics`, arbitrary length | `src/Ast.hpp:349` | yes — no arity assumption |
| field storage (3 fields) | `Shape::slots`, arbitrary-length vector | `src/Symbols.hpp:36-66` | yes |
| field access, all 4 engines | slot-index IR op, generic over slot count | `src/Lower.cpp:100-126`; `src/LlvmGen.cpp:2910` | yes |
| construction (`Of`) | ctor dispatch by `(label, arity)`; positional slot fallback | `src/Eval.cpp:380-430` | yes |
| value copy (`struct` → deep) | `copyValue()`, gated on `isValue`, loops `fields` | `src/RuntimeValue.hpp:118-125` | yes — general over field count |
| `(==)` hand-written | ordinary method dispatch, any arity/generics | per-engine method tables (`Eval.cpp:1130-1146`; `IrInterp.cpp:730-737`; `CGen.cpp:1550-1607`; `LlvmGen.cpp:1243-1260`) | yes |
| `(==)` **synthesized** (struct only) | `synthesizeStructEquality`, **bails on generics** | `src/Resolver.cpp:6880-6885` | **not used — routed around, §4** |
| `Map` key equality (if used as a key) | `keyEquals`, generic + recursive, no synthesis dependency | `src/RuntimeValue.hpp:277-296`; `CGen.cpp:260`; `LlvmGen.cpp:329,1912` | yes — works today, independent of §4 |
| `toString()` hand-written | ordinary method dispatch | (same tables) | yes |
| LA-18 `T::` monomorphization | triggered only by `T::member`; excludes class-level type params | `docs/reference.md:242-245` | n/a — `Triple` is a plain data holder, never engages |
| Map-iteration `Pair` fast path | `classIdByName("Pair")` etc. — the one name special-case | `Checker.cpp:4060-4061`; `Eval/IrInterp/CGen/LlvmGen/X64Gen` | irrelevant — `Map` never yields `Triple` |

**Frozen ELF backend (`X64Gen.cpp`):** no changes, no design consideration, per the
standing project rule (ELF is never extended). Its corpus runner self-skips
(`tests/run_elf.sh`, prints `SKIP (beyond ELF coverage)`) on anything it can't lower, so
a new `Triple` corpus file cannot break that lane. In practice it will likely pass
outright — `Triple` exercises the identical generic-`struct` machinery that `Pair`-using
corpus already runs clean under `--emit-elf`.

**A free correctness win worth stating explicitly:** because `Triple` is a `struct`
(value type), using it as a `Map` key **already works correctly today** via the
`keyEquals` field-recursive path (`src/RuntimeValue.hpp:277-296`), which never touches
the `(==)`-synthesis pass. This holds independently of the `(==)` operator itself. The
hand-written `(==)` in §2 governs the *source-level* `t1 == t2` operator; `keyEquals`
governs `Map`-key identity. They are separate mechanisms and both are correct under this
design (`research-triple.md` §5).

---

## 4. Decision record: `struct` + hand-written `(==)`/`toString()` (Option C)

### 4.1 The three candidates (from `research-triple.md` §4)

- **Option A — `class Triple`, a literal copy of `Pair`.** Zero risk, but inherits
  `Pair`'s *actual* (not claimed) semantics: reference identity, `Triple::Of(1,2,3) ==
  Triple::Of(1,2,3)` is **`false`**, prints `<object>`. Satisfies acceptance criterion 1
  ("same conventions as `Pair`") only in its most literal, bug-for-bug reading.
- **Option B — `struct Triple` relying on a *synthesized* `(==)`.** Matches the
  request's literal pseudocode, but `Triple` would be the **first generic value struct
  ever to reach** `synthesizeStructEquality`, which explicitly bails on generic structs
  (`src/Resolver.cpp:6880-6885`) because the self-type spelling in its generated source
  (`N`, bare and unparameterized, `:6926-6927`) was never validated for a generic self.
  Pursuing B means lifting that guard and fixing the generated spelling to include the
  type parameters — a change to the synthesis pass's *general* behavior (it would then
  attempt synthesis for **any** future generic struct), validated by new corpus proof.
  Larger and riskier than "add `Triple`."
- **Option C — `struct Triple` with a hand-written `(==)` and `toString()`.** Real value
  semantics (copied not aliased, field-wise equality) with **zero** dependence on the
  synthesis pass: `hasExplicitEq(cls)` makes the pass skip the class outright. Ordinary
  generic-method dispatch, which already works. Closes the printing gap `Pair` itself
  never closed, as a free side effect.

### 4.2 Ruling and rationale

**Chosen: Option C.**

1. **It delivers what the request actually wants.** The request's prose asks for value
   semantics — "copied not aliased, field-wise equality, no identity" (info.md §9's
   `struct` rule). Option C gives exactly that. Option A does not (it gives reference
   identity and `false`-on-equal). Between the request's *literal* citation ("mirror
   `Pair`") and its *stated intent* (value semantics), the intent is the real
   requirement; the citation is factually mistaken about what `Pair` is (§1).
2. **It carries zero new compiler risk.** Everything in Option C is already-exercised
   machinery: user-written operator overloads and `toString()` on generic classes work
   today across all four active engines (`research-triple.md` §2.3, §4.4). Nothing here
   is a first-of-its-kind code path.
3. **It is strictly less work and less risk than Option B**, while producing the same
   observable behavior at every `Triple` call site. Option B's only advantage — deriving
   `(==)` instead of writing it — is worthless for a type whose field list is
   *permanently fixed at three*: there is nothing to keep in sync, so the "derived stays
   correct as fields change" benefit of synthesis never applies.
4. **It closes a latent documentation/UX gap** (`Pair` prints `<object>`), rather than
   faithfully reproducing it — the better default for a brand-new type, and one that
   costs a single hand-written line.

### 4.3 Why not B, stated as a boundary

Option B is worth doing **only if** the real goal is to land generic-struct-`(==)`
*synthesis* as a general compiler capability, with `Triple` as its first corpus case.
That is a legitimate feature — but it is a **different, larger ticket** than "add
`Triple`," it changes the synthesis pass's behavior for every future generic struct, and
it must not ride along here by default (the same "don't decide the bigger question by
accident" discipline the request applies to tuple syntax). If that capability is later
wanted, this design's Option-C `Triple` becomes the natural first migration target and
regression oracle for it: the hand-written `(==)` documents exactly what a correct
synthesized relation must produce.

### 4.4 Consequence for the acceptance criteria

- Criterion 1 ("same construction/equality/printing conventions as `Pair`"): satisfied
  in the sense that matters — `Triple::Of(a,b,c)` construction mirrors `Pair::Of(a,b)`
  exactly, and equality/printing are *at least as good as* `Pair`'s (strictly better, in
  fact). We are consciously **not** reproducing `Pair`'s reference identity and
  `<object>` printing, because those are gaps, not conventions. This deviation is
  deliberate and documented here and in `docs/reference.md` (§9 below).
- Criterion 2 (corpus on every active engine): §7.
- Criterion 3 (documented alongside `Pair`): §9.

---

## 5. Interaction with the struct-equality synthesis pass — proof of non-interference

`Triple` has a hand-written `(==)`. The synthesis pass, in order:

1. `synthesizeStructEquality` (`src/Resolver.cpp:6829`) iterates candidate structs.
2. For each, `if (hasExplicitEq(cls)) continue;` (`:6871`) — **`Triple` is skipped here**,
   before the generics guard is ever reached. `hasExplicitEq` (`:6755`) returns true
   because `Triple` declares `bool (==)(…)`.
3. The generic-struct bail-out at `:6880-6885` is therefore **never evaluated for
   `Triple`** — it is dead with respect to this type.

So this design touches the synthesis pass **not at all**, and does not alter its behavior
for any other struct. The guard at `:6880` remains exactly as-is; the deferred
generic-synthesis question remains deferred, cleanly out of this ticket's scope. This is
the whole reason Option C was chosen over B: it makes the risky path *unreachable* rather
than *fixed*.

**Regression note for future editors:** if someone later lands generic-struct-`(==)`
synthesis (Option B's capability) and removes the `hasExplicitEq` early-out or the
generics guard, `Triple`'s hand-written `(==)` must still win (author's relation beats
derived). That invariant is already the pass's stated contract (`:6871` comment); a
corpus assertion in §7 pins the *observable* result (`Triple::Of(1,2,3) ==
Triple::Of(1,2,3)` is `true`) so any future regression in that area is caught.

---

## 6. Prelude placement and ordering

Single insertion point: `src/Resolver.cpp`, inside the `kPreludeCore` raw string,
immediately after `Pair`'s declaration (after `:401`). `Triple` references only
already-declared, primitive-or-earlier machinery (`bool`, `string`, `+`, `==`, `&&`), so
placement anywhere after the core operators is valid; placing it adjacent to `Pair` keeps
the two tuple types together for readers and matches how `research-triple.md` §7 framed
it. No forward-reference hazard — `Triple` is not consumed by any earlier prelude
declaration (unlike `Pair`, which `Array`/`Map` methods return; nothing returns `Triple`
in the prelude itself as of this ticket).

No new C++ symbol registration, no `CMakeLists` change for the prelude — `kPreludeCore`
is compiled as one string.

---

## 7. Testing plan

### 7.1 Corpus (acceptance criterion 2 — every active engine)

Following `Pair`'s own precedent, `Triple` needs **no dedicated corpus directory and no
new CMake registration** — every top-level `.ext`/`.lev` file is auto-discovered by the
directory-scanning targets `corpus_treewalk`, `corpus_ir`, `corpus_native` (emit-C++),
and `corpus_llvm_full` (`CMakeLists.txt:201-207, 828-831, 1125`). `corpus_elf_full`
scans the same tree and self-skips gracefully (§3).

Extend the existing `Pair`-bearing file that already does explicit construction:

- **`tests/corpus/arrays_ext.ext`** — the richest template; already builds `Pair` via
  `Pair::Of(...)` and reads `.first`/`.second`. Add a short `Triple` section that
  exercises the full surface of this design:

  ```lev
  // --- Triple: construction, field access, value equality, printing ---
  Triple<int, string, bool> t = Triple::Of(1, "x", true);
  console.writeln(t.first);              // 1
  console.writeln(t.second);             // x
  console.writeln(t.third);              // true
  console.writeln(t);                    // (1, x, true)   <- toString(), NOT <object>
  Triple<int, string, bool> u = Triple::Of(1, "x", true);
  console.writeln(t == u);               // true           <- field-wise (==)
  Triple<int, string, bool> w = Triple::Of(1, "x", false);
  console.writeln(t == w);               // false
  // value semantics: copy on bind, no aliasing
  Triple<int, string, bool> v = t;
  console.writeln(v == t);               // true (equal by value)
  ```

  These four behaviors — field access, `toString()`, equal-`true`, unequal-`false` —
  are the direct observable encodings of the §4 ruling, and running them on all four
  active engines is the byte-for-byte acceptance bar.

- **Optional second touch:** if a nested/heterogeneous case is wanted for depth, add a
  `Triple<int, Array<int>, string>` line to confirm a non-primitive middle field
  round-trips (parallels `Pair<T, Array<U>>` from `Array.groupJoin`). Not required for
  acceptance; cheap to include.

**Scale precedent** (from `Pair`, which has no dedicated file and added no CMake
targets): expect `Triple` to touch **1–2 existing corpus files**, no new directory, no
new CMake target.

### 7.2 Unit tests (`tests/test_eval.cpp`, `OUT` macro)

Mirror `Pair`'s existing `OUT` coverage shape. Add cases that pin construction + field
access and the equality contract from §4/§5:

```cpp
// Triple construction + field access
OUT("void run() { Triple t = Triple::Of(1, \"x\", true); "
    "console.writeln(t.first); console.writeln(t.second); "
    "console.writeln(t.third); } run();", "1\nx\ntrue\n");

// Triple field-wise (==): equal and unequal
OUT("void run() { Triple a = Triple::Of(1, 2, 3); Triple b = Triple::Of(1, 2, 3); "
    "Triple c = Triple::Of(1, 2, 4); "
    "console.writeln(a == b); console.writeln(a == c); } run();", "true\nfalse\n");

// Triple toString() (closes the <object> gap Pair left open)
OUT("void run() { console.writeln(Triple::Of(1, \"x\", true)); } run();",
    "(1, x, true)\n");
```

No new `ERRORS` (negative) case is needed: all three type parameters are ordinary and
always comparable transitively through the same rule that already governs `Pair`/`Array`
element comparability. (Should the design ever want to pin a specific diagnostic for a
non-comparable field inside `Triple`, that would be a follow-up; it is not part of
acceptance and Option C's hand-written `(==)` gives an ordinary "no `(==)` on field type"
error at the comparison site anyway, not a silent `false`.)

### 7.3 Regression anchor for §5

The `a == b` → `true` corpus/unit assertions double as the regression anchor named in §5:
they pin the observable result of `Triple`'s hand-written `(==)`, so any future change to
the struct-equality synthesis pass that accidentally shadowed or overrode the
author-written relation would fail here.

---

## 8. Rollout / implementation checklist

Single, self-contained landing — no phasing needed (no unlanded dependencies, no backend
edits).

1. Insert the `struct Triple<A, B, C>` block from §2 into `kPreludeCore`
   (`src/Resolver.cpp`, after `:401`).
2. Extend `tests/corpus/arrays_ext.ext` with the §7.1 `Triple` section.
3. Add the three `OUT` cases from §7.2 to `tests/test_eval.cpp`.
4. Add the `docs/reference.md` §6.4a subsection (§9).
5. Build; run `corpus_treewalk`, `corpus_ir`, `corpus_native`, `corpus_llvm_full`, and
   the `test_eval` unit target. All four active engines must produce byte-identical
   output on the new corpus section (criterion 2). ELF lane self-skips or passes.
6. Commit. Per the known-bugs/known-request workflow, this closes the accepted request —
   no known-bug entry is involved (this is a feature, not a bug fix).

**Estimated blast radius:** one prelude declaration (~11 lines), one corpus section, three
unit `OUT`s, one docs subsection. No C++ logic changes anywhere.

---

## 9. Documentation

Add a `Triple` subsection to `docs/reference.md` beside `Pair` (currently
`docs/reference.md:1106-1108`). Unlike `Pair`'s three terse lines, `Triple`'s note
records the semantics that are actually true of it (and that `Pair`'s doc silently omits
for `Pair`):

```
### 6.4a `Triple<A, B, C>`
Fields `first`, `second`, `third`; constructor `Triple::Of(a, b, c)`. A value type
(`struct`, info.md §9): copied not aliased, with field-wise `==` and a `toString()` that
prints `(first, second, third)`. The three-field sibling of `Pair` — use it wherever
three values need to travel together (a coordinate + a label, a key + two related
values) instead of a purpose-built struct or a nested `Pair<A, Pair<B, C>>`.
```

Do **not** renumber the pre-existing `### 6.4` / `### 6.3.5` / `### 6.4.4` disorder in
that file — it predates this ticket and fixing it is out of scope (`research-triple.md`
§2.5). `6.4a` slots in without disturbing it.

**Deliberate divergence to call out in review:** the doc states `Triple` has value
semantics, field-wise `==`, and real printing — which `Pair` (a `class`) does *not*.
This is the §4 ruling made visible to users. It is intentional that the two "tuple"
types differ here; the alternative (making `Triple` a `class` to match `Pair`
bug-for-bug) was Option A and was rejected (§4.1). If uniformity between the two is later
judged more important than `Triple`'s better semantics, the cheaper reconciliation is to
*upgrade `Pair`* (give it a hand-written `(==)`/`toString()` too) rather than downgrade
`Triple` — but that is a separate ticket touching `Pair`'s existing call sites and is not
proposed here.

---

## 10. Summary

- `Triple<A, B, C>` is a **`struct`** with hand-written `(==)` and `toString()` (§2) —
  Option C of `research-triple.md` §4, chosen for real value semantics with zero new
  compiler risk (§4.2).
- **Zero backend edits.** Every facet `Triple` touches is already generics/arity-agnostic
  (§3); the one facet that is not (generic-struct `(==)` synthesis) is made *unreachable*
  by the hand-written `(==)`, not modified (§5).
- Ships as ~11 lines of prelude text, 1–2 corpus files, three unit `OUT`s, one docs
  subsection (§8).
- **Scope is held:** no anonymous tuple syntax, no `Quad`/`zip3`, no change to `Map`'s
  `Pair` iteration, no change to the struct-equality synthesis pass (§1.1).
- The one deliberate divergence from `Pair` — `Triple` has value semantics and prints
  itself, `Pair` does neither — is intentional, documented, and reversible in the
  direction of upgrading `Pair` rather than degrading `Triple` (§9).
