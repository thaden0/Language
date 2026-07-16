# Tech Design — Class-receiver method dispatch: semantically dynamic, statically devirtualized

**Status:** design accepted, ready for implementation (owner greenlit 2026-07-11). **Date:** 2026-07-11.
**Track:** **single track**, ordered milestones M1 → M3, **one implementer**. This is one small,
tightly-coupled checker change (a devirtualization predicate + the dispatch-decision flip at three
sites) plus cross-engine corpus — not a fan-out. Do **not** split it.
**Provenance:** promoted from `designs/proposal-class-method-dispatch.md` (this file replaces it),
which was filed from the LA-25 STOP #2 escalation
(`designs/complete/techdesign-method-references.md` §8.8 / §12).
**Prior art in-tree:** `designs/complete/techdesign-07-iteration.md` log (2026-07-06, "Bug 21") —
"dynamic dispatch is genuinely interface-only by design ... a CLASS-typed receiver bakes in the
statically-resolved declaration, a deliberate perf choice, not a bug" — plus the prelude `Seq`
workarounds (interface-typed fields to *get* dispatch). This design flips that deliberate choice,
now that its costs have a body of evidence.
**Convention docs:** `designs/complete/techdesign-00-overview.md` governs this work (§4.1 probes,
§4.2 STOP-and-escalate, §4.3 acceptance, §4.4 commit discipline) — read it first. `src/` edits are
in scope (fixer-role, overview §4.4). `X64Gen.cpp`/`X64.hpp` are **frozen** (overview §2.1, portable
pivot) — this design touches neither.

---

## 1. The one rule (philosophy fit)

Today a method call on a **concrete-class-typed** receiver statically binds the *named class's own*
declaration; only an **interface-typed** receiver dispatches on the runtime object. So an override
is silently inert through any base-class-typed binding — field, parameter, local, or (since LA-25) a
method reference. The proposal's §2 lays out why this contradicts three of the language's *own*
load-bearing rules; the short form:

- **§6.5 one-member rule.** A method is a *per-object typed slot*. §4's collapse rule says a derived
  same-name+same-type member *overrides* — a `Dog` object's `speak` slot **holds Dog's speak**. A
  statically-bound `c.speak()` on that object invokes a value the object's slot does not hold. Static
  class dispatch quietly reintroduces the field-vs-method dichotomy §6.5 exists to erase (field reads
  per-object, method reads per-static-type).
- **§7 dispatch doctrine.** "Closed candidate set → point right at the slot; **open** candidate set →
  cached pointer/lookup." A method some subclass overrides has, at a base-typed call site, an *open*
  candidate set — by §7's own words it belongs on the dynamic path.
- **§16 loudness.** The failure is the canonical silent-distant footgun: override written in one file,
  base-typed call far away, nothing errors, the wrong body runs. The language makes exactly this shape
  loud everywhere else.

> **The rule this design lands:** an unqualified instance-method call dispatches on the receiver's
> **runtime** slot value — uniformly for interface-typed and class-typed receivers. The checker
> statically binds (direct call, zero cost) whenever the candidate set is **provably closed** — no
> class anywhere in the whole-program gather (§12) overrides the resolved method below the receiver's
> static type. **Devirtualization is a pure optimization: it can never change what runs, only how
> fast.**

This is the smaller diff *to the language's own stated rules*: §6.5, §7, §12, and §16 already argue
for it; the current behavior is the special case.

### 1.1 What stays static (unchanged)

- **Qualified access** (`this.Base::m()`, `obj.Counter::value`) — qualification *names a specific
  slot* (§4); `distinct` members are unaffected.
- **Overload resolution, constructor selection, operator resolution** — still resolved by static type
  at compile time (§9). Only *which override body* runs moves to the runtime slot, and only when an
  override actually exists. **Type-checking is 100% unchanged** — the statically-resolved method `m`
  is still what types the call (return type, argument checking, `mutating`-const check); we change only
  the *dispatch target* recorded for the engines, exactly as the interface path already does.
- **Operators are deliberately out of scope for v1** (§5.4) — an operator is a method (info.md §5) and
  the same argument applies to an overridden operator, but operator dispatch is a distinct resolution
  path (`typeOfBinary`, Checker.cpp:1440) with its own runtime lowering; extending the rule to
  operators is a clean, separate follow-up placed on the roadmap (§10), not smuggled in here.
- **Value structs** — final, nothing inherits (§9), so every struct method devirtualizes by
  construction. Zero cost, no code path reached.
- **Namespace / static-side functions, labeled constructors** — no receiver, no
  dispatch-polymorphism question at all.

---

## 2. The rule precisely

Let a method call resolve (by the existing static overload resolution) to method declaration `m` on
receiver static type `T` (a class or interface `Symbol`). Define:

```
dispatchesDynamically(T, m)  ≡  isInterface(T)  ∨  isOverriddenBelow(T, m)

isOverriddenBelow(T, m)      ≡  ∃ class X :  X is a strict subclass of T
                                          ∧  X declares its own method with the
                                             same (name, signature) as m
```

- `dispatchesDynamically` **true**  → leave `resolved` **null** → the engine dispatches by name at
  runtime on the object's actual class (the existing interface path).
- `dispatchesDynamically` **false** → set `resolved = m` → direct call, **byte-identical IR to
  today** (§5.3).

Interfaces fold in as a special case (`isInterface(T)` short-circuits true — an interface method
requirement always has its implementers "below" it, so `isOverriddenBelow` would return true anyway;
we keep the explicit disjunct so a zero-implementer interface still dispatches dynamically exactly as
today, no behavior change). "Same signature" means the merged-shape **`Slot::canonical`** string
(`"(params) -> ret"`, Symbols.hpp:37) — same name + same canonical, which is precisely the
override/collapse test the Resolver already uses when merging shapes. A same-name **different-signature**
declaration is an *overload*, not an override: `m` still applies and the call stays static for that
signature (§5.2 works a full example).

---

## 3. Current behavior (verified against this tree, 2026-07-11, all engines agree)

House rule (overview §4.1). Probes run with `build/lang --run <file>` (see §6 for the full probe set).

```
class Animal { string speak() => "..."; }
class Dog : Animal { string speak() => "Woof"; }
string callSpeak(Animal c) => c.speak();
callSpeak(Dog());        // prints "..."  (static class bind — the bug)

interface IAnimal { string speak(); }
class Cat : IAnimal { string speak() => "Meow"; }
string callSpeak2(IAnimal c) => c.speak();
callSpeak2(Cat());       // prints "Meow" (dynamic — already correct)
```

### 3.1 The three dispatch-decision sites (the proposal named only one)

`Expr::resolved` (Ast.hpp:183) is the single value threaded to every engine — non-null = "direct
call this exact decl"; null = "dispatch by name on the runtime class." `Lower.cpp:1341` copies it into
`IR CallDyn.decl` (Ir.hpp:44), feeding all five engines from one point. Method dispatch sets `resolved`
at **three** sites in `Checker.cpp`, all of which must apply `dispatchesDynamically`:

| # | site | code today | shape |
|---|---|---|---|
| **S1** | `Checker.cpp:1299-1300` — `base.method(...)` branch | `bool ifaceRecv = …isInterface; call->resolved = ifaceRecv ? nullptr : m;` | the primary path (parameters, fields, locals) |
| **S2** | `Checker.cpp:1242-1246` — bare `method()` on `this` | `call->resolved = m;` (no interface guard at all) | an inherited method calling another via `this`; **also buggy** — `this` flowing through a base method is not necessarily the most-derived class |
| **S3** | `Checker.cpp:896-897` — `rewriteAsMethodRef` (LA-25) synthesized call | `if (recvClass && …isInterface) call->resolved = nullptr;` | the eta-expansion lambda body for a method reference `C::m` |

**S3 is the correction to the proposal's impact inventory.** The proposal (§4) claims "LA-25 method
references become override-correct automatically … no changes." That is false: `rewriteAsMethodRef`
sets `resolved` on its *own* synthesized call directly (Checker.cpp:889) and nulls it for interfaces
only (896-897) — it does **not** route that call back through S1. So `Animal::speak` used as a value
would keep binding `Animal::speak` statically unless S3 gets the same predicate. All three sites must
call one shared helper. (This is why the design extracts a helper rather than editing S1 inline.)

**S2** was missed by the proposal too. Repro that is currently correct only by accident (no override
exists), but breaks the moment one does:

```
class Base    { string who() => "base";  string tag() => who(); }   // bare this-call
class Mid : Base { string who() => "mid"; }
Mid m; m.tag();      // TODAY binds Base::who statically inside Base::tag → "base" (WRONG once dynamic lands elsewhere; inconsistent)
```

---

## 4. The mechanism

Two additions to `Checker`, then the three-site flip. **No IR op, no Lower/Eval/backend code** — the
null-`resolved` `CallDyn`-by-name path already exists and is exercised by interface dispatch on every
engine (§5).

### 4.1 The override index (precompute once, O(1) per call site)

The proposal hand-waves "a trivial subclass walk" *per call site* — that is O(classes × slots) on
every method call, which is needless. Precompute a program-wide index once (the whole-program gather,
§12, makes this a single pass), then answer each call site in O(1).

`sema_.symbols` already enumerates every class (Symbols.hpp:90); `Checker::hygienicClass`
(Checker.cpp:1076) already scans it exactly this way. Add to `Checker` (`Checker.hpp`):

```cpp
// (T, "name\x1f canonical-sig") is present  ⟺  some strict subclass of T
// declares its own method with that name+signature (i.e. m is overridden below T).
std::unordered_map<const Symbol*, std::unordered_set<std::string>> overriddenBelow_;
bool overrideIndexBuilt_ = false;
void buildOverrideIndex();
bool isOverriddenBelow(const Symbol* T, const Stmt* m) const;
bool dispatchesDynamically(const Symbol* T, const Stmt* m) const;
```

```cpp
void Checker::buildOverrideIndex() {
    if (overrideIndexBuilt_) return;
    overrideIndexBuilt_ = true;
    for (const std::unique_ptr<Symbol>& xp : sema_.symbols) {
        const Symbol* X = xp.get();
        if (!X || X->kind != SymbolKind::Class) continue;
        for (const Slot& s : X->shape.slots) {
            if (!s.isMethod || s.source != X) continue;   // ONLY X's own declarations/overrides
            std::string key = std::string(s.name) + "\x1f" + s.canonical;
            // mark every proper ancestor A of X (isSubclass walks bases upward, Checker.cpp:166)
            walkProperAncestors(X, [&](const Symbol* A){ overriddenBelow_[A].insert(key); });
        }
    }
}

bool Checker::isOverriddenBelow(const Symbol* T, const Stmt* m) const {
    if (!T || !m) return false;
    for (const Slot& s : T->shape.slots)          // find m's own slot to read its canonical
        if (s.decl == m) {
            auto it = overriddenBelow_.find(T);
            return it != overriddenBelow_.end()
                && it->second.count(std::string(s.name) + "\x1f" + s.canonical) != 0;
        }
    return false;
}

bool Checker::dispatchesDynamically(const Symbol* T, const Stmt* m) const {
    if (T && T->decl && T->decl->isInterface) return true;   // interfaces: existing behavior verbatim
    return isOverriddenBelow(T, m);
}
```

`walkProperAncestors(X, f)` recurses `X->decl->bases[i]->resolvedSymbol` (the same field
`isSubclass` uses, Checker.cpp:167), calling `f` on each ancestor (not `X` itself); a diamond visits
an ancestor twice, harmless for a set insert; the resolver's `building` cycle guard (Shape,
Symbols.hpp:49) means the base graph is acyclic here.

**Why `s.source == X` is exactly right.** The Resolver's shape merge sets `Slot::source` to the
*winning declarer* ("later/overriding wins", info.md §4). A class that overrides `B::speak` has its
`speak` slot's `source == itself`; an inherited-unchanged method has `source ==` the ancestor. So
`s.source == X` isolates precisely X's *own* method declarations — which are the only ones that can
constitute an override of an ancestor.

**Why the index has no false positives.** A mark `(A, name, sig)` is only ever *queried* when some
call site resolves method `m` with that (name, sig) on static type `A` — which requires `A` to have
that method on its shape. If `A` has it and a subclass `X` declares its own same-(name, sig) method,
that *is*, by definition (info.md §4), an override of `A`'s method → dynamic is correct. If `A` lacks
it, the mark is never queried. Either way the answer is correct. (Both the build side and the query
side read `Slot::canonical`, so the strings match byte-for-byte — do not recompute the signature via
`fromTypeRef` on one side only.)

**When to build.** Lazily, guarded by `overrideIndexBuilt_`, on first entry to any of S1/S2/S3 (the
first method-typed call the checker sees). Shapes are fully built by the time checking runs (they are
a Resolver product, and the Resolver builds shapes for **every** class incl. the prelude, so the index
is complete). Note the prelude is *not checked* (Checker runs only on the user program), but its
classes are still in `sema_.symbols` with built shapes, so a user program that statically types a
receiver as a concrete prelude class is indexed correctly.

**Cost.** Build: O(Σ_X ownMethods(X) · ancestorDepth(X)) once — negligible. Query: one shape scan for
`m`'s slot (shapes are small) + one hash lookup. If a program's shapes are large enough for the scan to
matter, capture `m`'s canonical when it is first resolved and pass it in — but do not optimize
speculatively; the simple form is fine and matches the codebase's canonical-string idiom.

### 4.2 The three-site flip

Each site replaces its ad-hoc interface test with `dispatchesDynamically`:

```cpp
// S1  Checker.cpp:1299-1300
call->resolved = dispatchesDynamically(bt.sym, m) ? nullptr : m;

// S2  Checker.cpp:1244  (was: call->resolved = m;)
call->resolved = dispatchesDynamically(thisClass_, m) ? nullptr : m;

// S3  Checker.cpp:896-897  (replace the isInterface-only null)
if (recvClass && dispatchesDynamically(recvClass, target))
    call->resolved = nullptr;
```

S1 already computes `m` via `resolve(methodOverloads(bt.sym, …))` and keeps using it for
`genericReturn`/mutating checks — unchanged. S2 already has `m` and `thisClass_`. S3 has `target`
(the method decl) and `recvClass`; `isOverriddenBelow` reads `target`'s canonical from
`recvClass->shape`. That is the entire dispatch change.

### 4.3 `?.` optional-chain calls — free

The `?.` call path narrows the union then falls into the same `bt.kind == TKind::Class` branch at S1
(Checker.cpp:1274-1300), so optional-chained method calls inherit the new rule with no extra code.

---

## 5. The one sharp edge: overridden overload sets under name+arity runtime lookup

This is the one place the rule meets a **pre-existing** runtime limitation. It is scoped, understood,
and made **loud** rather than left silent.

### 5.1 The runtime lookup is name+arity, not full-signature

When `resolved` is null, every engine dispatches by **name + arity** (`findMethod` Eval.cpp:215,
`findMethodByName` IrInterp.cpp:24, `genCallM` CGen.cpp:1179 / LlvmGen.cpp:1267 — all cite bug.md
#13/#27; the frozen X64Gen.cpp:2180 matches by **name only**, see §7). None disambiguate two
same-arity overloads by parameter *type*. Interface dispatch has ridden this every day; this design
routes overridden **class** calls onto the same path, so the same limitation now reaches them.

### 5.2 Signature-precise decision keeps the common cases correct

Because `isOverriddenBelow` matches on **full signature**, the devirtualization *decision* is always
right, and the vast majority of shapes dispatch correctly at runtime:

```
class Animal { string speak() => "a"; string speak(int n) => "a-int"; }
class Dog : Animal { string speak() => "WOOF"; }        // overrides ONLY speak()

Animal a = Dog();
a.speak();      // m = Animal::speak();    overridden below? yes  → dynamic → runtime name+arity(0) → Dog::speak()   ✓ "WOOF"
a.speak(5);     // m = Animal::speak(int); overridden below? NO   → static  → Animal::speak(int)                    ✓ "a-int"
```

Different-arity overloads and single (non-overloaded) overridden methods — the 99% case — are
correct on oracle/IR/emit-C++/LLVM *and* frozen ELF (a single arity/name candidate can't be picked
wrong).

### 5.3 Devirtualized calls are byte-identical to today

For a method with **no** override below the receiver's static type, `resolved = m` exactly as before
→ the same `CallDyn.decl` → the same direct call in every backend. There is **no codegen delta** on
the fast path — perf preservation is a construction guarantee, not a measured hope. The only new cost
is compile-time (the one-shot index). A §10.2-style timed smoke over a hot devirtualizable loop should
show zero regression by construction; include it in M2 as evidence, not discovery.

### 5.4 The hazard, and the loud diagnostic (recommended)

The hazard is exactly: an overridden method whose runtime class *also* carries a **same-arity,
different-type sibling overload**, so name+arity cannot pick the intended body:

```
class Animal { string speak(string s) => "a-str"; string speak(int n) => "a-int"; }
class Dog : Animal { string speak(int n) => "d-int"; }   // overrides speak(int); sibling speak(string) shares arity 1

Animal a = Dog();
a.speak(5);   // decision: overridden below (speak(int)) → dynamic.  runtime name+arity(1) on Dog: TWO arity-1 slots → ambiguous
```

Today this program *silently runs the wrong (base) overload* because class calls are static — the very
bug this design removes. Going dynamic without a fix would trade silent-wrong-static for
silent-maybe-wrong-runtime. Neither is acceptable under §16. **Recommended design decision:** at the
moment a site decides to dispatch a *class* receiver dynamically **because of an override** (not the
interface case), the checker verifies the runtime lookup is unambiguous — i.e. no *other* method slot
on the resolved method's own class shares its (name, arity). If it is ambiguous, emit a **loud
compile error at the call site**:

> `method 'speak' is overridden below 'Animal' but shares its arity with another overload 'speak' —
> the runtime dispatch cannot disambiguate by type; give the overloads distinct arities or names, or
> qualify the call explicitly`

This converts the one genuinely-unsafe corner from silent-distant into loud-local (§16), and — because
it fires in the **checker**, before any backend — it is *also what keeps the frozen ELF path sound*
(§7) without touching X64Gen. It is a new compile error on a program that compiles today, but that
program is *already silently buggy* (runs the base overload); refusing it is the language-honest trade.

**Decided default: emit the error (§5.4).** Marked as a **STOP/owner-checkpoint** only in that if the
owner prefers to *pin the ambiguity as an XFAIL corpus entry* instead of erroring, that is a one-line
swap (skip the diagnostic, add the XFAIL) — but it is strictly worse (§16) and not recommended. Either
way, the real long-term fix — **signature-aware runtime dispatch** (carry the resolved parameter
signature to the by-name lookup, benefiting interface dispatch identically) — goes on the roadmap
(§10); it is out of scope here because it is a cross-engine runtime change equally owed to the
*existing* interface path, not something this checker-local change should smuggle in.

---

## 6. Probes (run before writing code — overview §4.1)

Tiny `.lev` programs; run each on `build/lang --run` (and note the current result) *before* touching
`Checker.cpp`. If any diverges from the table, STOP (overview §4.2) — the mechanism assumes these.

| # | program (essence) | expected result **today** | what it pins |
|---|---|---|---|
| **P-1** | `Animal`/`Dog:Animal`, `callSpeak(Animal c)=>c.speak()`, `callSpeak(Dog())` | prints `...` | the bug S1 fixes (baseline to flip) |
| **P-2** | a method with **no** override, called through its own type in a hot loop | prints correctly; **static-bound** today | the fast path that must stay byte-identical (§5.3) |
| **P-3** | overloaded-overridden, **same-arity sibling** (§5.4 program) | prints the **base** overload (silent-wrong, static) | the hazard M3's diagnostic makes loud |
| **P-4** | bare this-call: `Base.tag()` calls `who()`; `Mid:Base` overrides `who`; `Mid m; m.tag()` | prints `base` today | S2 (the second missed site) |
| **P-5** | user **generic** class `Box<T>` with `Sub<T>:Box<T>` overriding a method, called base-typed | prints the base body today | canonical-match under generics (verify the index keys line up) |
| **P-6** | interface case (`IAnimal`/`Cat`) — unchanged | prints `Meow` | the fold-in must not regress interfaces |

P-3/P-6 also double as the M3/M1 corpus seeds.

---

## 7. Engine coverage

Per overview §4.3. Trivial for the fast path (no codegen change, §5.3); the dynamic path reuses the
interface machinery on every engine:

- **oracle + IR** — mandatory, full. `findMethod`/`findMethodByName` name+arity already run for
  interfaces; class overrides join them.
- **emit-C++** — full. `genCallM` (CGen.cpp:1179) name+arity guard already present.
- **LLVM** — full. `callmCandidates` (LlvmGen.cpp:1267) exact-arity + own-override-preferred already.
- **frozen ELF (X64Gen)** — **do not edit** (overview §2.1). Its `genCallM` (X64Gen.cpp:2180) matches
  by **name only** (no arity guard — a pre-existing frozen-backend characteristic, already true for
  interface dispatch there). Consequence: the common (single-candidate) override case is correct on
  ELF; a **same-arity overloaded** override is not disambiguable there. §5.4's checker diagnostic
  fires *before* any backend, so it prevents that unsound shape from reaching ELF at all — ELF stays
  sound without a X64Gen edit. Pin the common override case green on ELF; the ambiguous shape is
  rejected at check time on every backend uniformly (no ELF-specific silent gap).

Engine-coverage policy is thus satisfiable in full, because the one shape ELF's frozen dispatch can't
serve is exactly the one the checker refuses program-wide.

---

## 8. Milestones (ordered; one implementer)

Each milestone ends on the overview §4.3 acceptance block (clean build zero new warnings, `ctest`,
`run_corpus.sh` oracle-vs-IR, `run_elf.sh`, `run_native.sh`, `churn_leak.py` 13/13 + XFAIL, lcurl
smoke).

- **M1 — the predicate + the flip (oracle + IR).** Run P-1…P-6 first; confirm the table. Add
  `buildOverrideIndex`/`isOverriddenBelow`/`dispatchesDynamically` (§4.1) and apply it at **all three**
  sites S1/S2/S3 (§4.2). Flip the corpus pin: `tests/corpus/method_refs.lev` (the `Animal::speak`
  through `Dog` line, currently pinned to `...` by LA-25 §8.8) and its `.expected` (line 2 `...` →
  `Woof`); update the anticipating comment (method_refs.lev:63-66) to record that this design landed
  the flip. New corpus program `tests/corpus/class_dispatch.lev` (+`.expected`): base/derived override
  through a parameter (S1), through a `this`-relative call (S2, the P-4 shape), and through a method
  reference `Animal::speak` stored + invoked (S3) — oracle + IR green. Interface case (P-6) pinned
  unchanged. Checker test: none new yet.
- **M2 — full engine coverage + fast-path identity proof.** Extend `class_dispatch.lev` coverage to
  emit-C++, LLVM, and ELF (common single-candidate overrides; §7). Add the §5.3 evidence: a
  non-overridden hot-loop call, confirmed still `resolved`-bound (a timed smoke shows no regression;
  optionally diff the emitted IR/`.o` against the pre-change build to demonstrate byte-identity). All
  five engines differential-green on the covered shapes.
- **M3 — the overload hazard made loud + reference.md.** Add the §5.4 checker diagnostic
  (ambiguous-arity override → compile error at the call site) with a `tests/test_checker.cpp` case;
  add a corpus program for a *safe* overridden overload set (different arities — §5.2, must stay
  correct and partly static) to prove the diagnostic is narrow and does not over-fire. Update
  `docs/reference.md` §3.4 (dispatch semantics) in the **same commit** (overview §2.2). This closes
  the design; move this file to `designs/complete/`.

---

## 9. Impact inventory (corrections to the proposal in **bold**)

- **Checker** (`Checker.cpp` + `Checker.hpp`): the index (§4.1) and **three** site flips —
  **S1 (1299-1300), S2 (1244), and S3 (`rewriteAsMethodRef`, 896-897)**. The proposal named only S1;
  S2 (bare this-call) and **S3 (LA-25 method references — the proposal wrongly said "no changes")** are
  required or the override rule is incomplete. One shared helper covers all three.
- **Engines:** nothing new — the null-`resolved` `CallDyn`-by-name path is interface-proven on every
  engine (§7). No IR op, no Lower/Eval/CGen/LlvmGen edit; **no X64Gen edit** (frozen).
- **Overload sets under override:** the pre-existing name+arity (name-only on frozen ELF) runtime
  limitation (§5). Made **loud** by the M3 checker diagnostic rather than left silent; the real fix
  (signature-aware runtime dispatch) is roadmapped (§10), shared with the interface path.
- **Corpus:** `tests/corpus/method_refs.lev` + `.expected` flip the LA-25 §8.8 pin (an explicit,
  reviewed change the pin's own comment anticipates); new `tests/corpus/class_dispatch.lev`; checker
  test in `tests/test_checker.cpp`. A sweep for other override-inertness-dependent corpus programs was
  done at design time — only the `method_refs.lev` line depends on it; the prelude `Seq` family
  *works around* the old rule (interface-typed fields) and stays correct (just non-load-bearing).
- **Prelude:** Track 07's interface-typing workarounds remain correct; they stop being load-bearing.
  No prelude edit is required (the prelude is not checked; its dispatch is already interface-routed).
- **LA-25 method references:** become override-correct via **S3** (not "automatically" — S3 is the
  change that makes the proposal's claim true).
- **bug.md #2** (field-closure dot-call) shares the `CallDyn` fallback region but is **independent** —
  this design neither needs nor fixes it; the corpus uses the `var h = obj.field; h(args)` binding
  workaround (as LA-25's does) for any "reference stored in a field, then invoked" case until #2 is
  fixed. Do not fold #2 in.

---

## 10. Foreseeable problems & STOP points

- **10.1 Overloaded-override ambiguity (decided; §5.4).** Loud checker error is the recommended
  default. STOP/owner-checkpoint *only* to choose error-vs-XFAIL; error is recommended. Do not
  silently go dynamic on the ambiguous shape.
- **10.2 Canonical-string match under generics (verify, P-5).** The index and the query both read
  `Slot::canonical`; confirm a user generic subclass override produces matching canonicals on both
  sides (P-5). If a generic instantiation's slot canonical differs between base and override in a way
  that misses a real override, STOP and log — the fix is to normalize on the same canonical source,
  not to loosen the match to name+arity (that would reintroduce the §5 hazard into the *decision*).
- **10.3 Operators (deferred, roadmapped).** Overridden operators still bind statically after this
  design. Same slot-model argument applies; extending `typeOfBinary` (Checker.cpp:1440) is a clean
  follow-up. Explicitly on the roadmap — not silently dropped.
- **10.4 Signature-aware runtime dispatch (roadmapped).** The real fix for §5's name+arity limitation,
  owed equally to interface dispatch. Out of scope (cross-engine runtime change). Roadmap.
- **10.5 STOP protocol (overview §4.2).** If the mechanism turns out wrong — e.g. `Slot::source` does
  not isolate own-declarations as §4.1 assumes, or a fourth `resolved`-setting dispatch site surfaces
  — STOP, log in §11, commit WIP to the track branch, escalate to a Fable-class model. Do not
  improvise a semantics change.

---

## 11. `docs/reference.md` deltas (ship in M3's commit — overview §2.2)

- **§3.4 (Member access / dispatch):** state the landed rule — an unqualified instance-method call
  dispatches on the receiver's **runtime** class (uniform for interface- and class-typed receivers);
  the compiler **devirtualizes to a direct call** whenever no subclass overrides the resolved method
  (a whole-program, §12, closed-candidate-set optimization that never changes behavior). Qualified
  `Base::m`, operators, constructors, and static/namespace functions remain statically resolved.
- **§3.4 limits block:** an overridden method that shares its (name, arity) with another overload
  cannot be dynamically disambiguated by type today → compile error (§5.4); the fix (signature-aware
  dispatch) is roadmapped. Cross-reference the §8.8 note in
  `designs/complete/techdesign-method-references.md` (now satisfied: references are override-correct
  via S3).

---

## 12. Implementation log

**2026-07-11, implementer session (Sonnet).** M1, M2, and M3 all landed clean. Design closed —
moved to `designs/complete/`.

### Probes (§6), run before touching `Checker.cpp`

All six matched the table exactly: P-1 `...` (bug confirmed), P-2 `1001000` (fast-path hot-loop
baseline), P-3 `a-int` (base overload silently wins — the §5.4 hazard, confirmed live), P-4
`base`, P-5 `box` (generic base-typed call, pre-flip), P-6 `Meow`.

### M1 — the predicate + the three-site flip

Added `overriddenBelow_`/`overrideIndexBuilt_` + `buildOverrideIndex`/`isOverriddenBelow`/
`dispatchesDynamically` to `Checker.hpp`/`Checker.cpp` exactly per §4.1 (verified `Slot::source ==
X` isolates X's own declarations by reading `Resolver::buildShape`, Resolver.cpp:4225-4229: every
own member is merged with `source = cls`, confirming the design's proof). Flipped S1
(Checker.cpp, the `base.method(...)` branch), S2 (the bare-`this`-call branch), S3
(`rewriteAsMethodRef`) to call a single shared helper (see M3 below — the helper grew to also
carry the M3 diagnostic, still one call site per S1/S2/S3).

Re-ran all six probes post-flip: P-1 `Woof`, P-4 `mid`, P-6 `Meow` (unchanged), P-5 `sub`
(generic override now correct — no §10.2 STOP needed, canonical strings matched byte-for-byte),
P-2 `1001000` (unchanged — fast path byte-identical), P-3 now hits the hazard live (see M3).

Corpus: flipped `tests/corpus/method_refs.lev` line 2 (`...` → `Woof`) and its `.expected`;
updated the `§8.8`-pin comment (was lines 63-66) to record the flip. Added
`tests/corpus/class_dispatch.lev` (+`.expected`) covering S1 (parameter), S2 (bare this-call,
the P-4 shape), the no-override fast path (Loud), and S3 (a stored+invoked method reference) —
green on oracle + IR.

### M2 — full engine coverage + fast-path identity

`class_dispatch.lev` verified green on oracle (`--run`), IR (`--ir`), emit-C++ (`--emit-cpp` +
g++), and LLVM (`--native-obj` + runtime v2 link) — all four byte-identical to `.expected`; these
are the actively-maintained engines (LLVM is the real compiled backend, portable-backend pivot —
ELF/X64Gen is frozen reference material only, not a project target, and gets no coverage here).
Full `ctest -j$(nproc)`: **131/131 passed**, including `corpus_llvm_full` and `corpus_native`
(both run the whole `tests/corpus/` dir, so `method_refs.lev` and `class_dispatch.lev` ride LLVM
and emit-C++ there too) and both churn-leak lanes. lcurl smoke (`trident build examples/curl
--leviathan build/leviathan`, run against a closed port) still compiles and runs correctly
end-to-end post-change.

### M3 — the overload hazard made loud

Implemented exactly per §5.4: `overrideDispatchAmbiguous(T, m)` checks `T`'s own shape (the
receiver's static type) for another method slot sharing `m`'s name and `params.size()` — sound
for the worked example because the collapse rule (info.md §4) guarantees any sibling present on
`T`'s shape is inherited unchanged into every subclass's shape too. Folded into one shared helper,
`resolveDispatch(T, m, span)`, used at all three sites (S1/S2/S3) — it now both decides
`dispatchesDynamically` **and** emits the diagnostic when dynamic-because-of-override (not the
interface case) is ambiguous by arity. P-3 now correctly **errors**: `method 'speak' is
overridden below 'Animal' but shares its arity with another overload 'speak' — ...`.

Added 6 `tests/test_checker.cpp` cases (clean override, the P-3 hazard via S1, the safe
different-arity case, the same hazard via S2 and via S3, and an unaffected-interfaces control) —
`checkertests`: 234 checks, 0 failures. Added the §5.2 safe-overload-set proof (`Named`/
`NamedDog`, distinct arities) to `class_dispatch.lev`/`.expected` — green on oracle/IR/emit-C++/
LLVM. Updated `docs/reference.md` new §3.4a (dispatch rule + the arity-hazard limit) and the
stale §3.4 method-reference dispatch line, in this commit.
