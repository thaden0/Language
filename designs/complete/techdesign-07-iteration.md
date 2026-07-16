# Track 07 — Iteration: the Iterator Protocol & Lazy `Seq<T>`

**Status:** ready. **Date:** 2026-07-05. **Depends on:** Tracks 01–03 landed
(compiler-file merge order — this is the last compiler track); Track 05's
inference decision (P1 worlds — `Seq.map<U>` shares that dependency); contract C5
(owner).
**Source:** suggested-features.md §9, §13#3; info.md §19#4 (eager/lazy — this
track answers it).
**Owns:** kPrelude interfaces + `class Seq`; `Checker.cpp` ForIn typing;
`Eval.cpp` ForIn (976–1010); `Lower.cpp` ForIn (~519+). No new IR ops (the
protocol path lowers to ordinary `CallDyn`).

---

## 1. The protocol

```
interface IIterator<T> {
    bool hasNext();
    T next();
}
interface IIterable<T> {
    IIterator<T> iterator();
}
```

`for (T x in e)` where `e`'s static type implements `IIterable<T>` desugars to:

```
var __it = e.iterator();
while (__it.hasNext()) { T x = __it.next(); <body> }
```

**Dispatch order (contract C5, frozen):** Range counted-loop → Array/Map
`IterLen`/`IterAt` fast paths → the protocol. Built-ins never reroute through the
protocol (perf is the reason arrays exist). The checker decides the path
statically from the iterable's type; there is no runtime probing.

- Element type: `T` from the `IIterable<T>` instantiation; `for (var x in e)`
  infers it. A type implementing neither builtin-iterable nor IIterable →
  compile error (today's behavior, now with a better message naming the
  protocol).
- `next()` past the end: protocol contract says "unspecified — iterators are
  driven by hasNext"; stdlib iterators throw RuntimeException (loud). Documented.
- Strings are NOT iterable in v1 (`s.chars()` returns an Array — explicit;
  avoids the bytes-vs-scalars ambiguity in a loop header).
- `InStream<T>`: **not** made IIterable in v1 (pull-on-empty throws; a for-loop
  over a live stream is a foot-gun until blocking semantics exist — noted
  deferral, revisit with the runtime-loop work).

## 2. Implementation

1. **Prelude:** the two interfaces + `Array<T>.iterator()` /
   `Map<K,V>.iterator()` (in-language index-based iterator classes — they exist
   for protocol *uniformity* (passing an Array where an IIterable is wanted);
   `for..in` over arrays still takes the fast path) + `Range.iterator()`.
   Iterator classes are ordinary prelude classes:
   ```
   class ArrayIterator<T> : IIterator<T> {
       Array<T> a; int i = 0;
       new ArrayIterator(Array<T> src) { a = src; }
       bool hasNext() => i < a.length();
       T next() { T v = a.at(i); i = i + 1; return v; }
   }
   ```
2. **Checker (ForIn case):** after the existing Range/Array/Map checks fail,
   look for `iterator()` returning `IIterator<E>` via the resolved interface
   set; bind the loop variable type to `E`; record the chosen path on the Stmt
   (a small enum field on `Stmt` — parser-invisible, set during checking) so
   Eval/Lower don't re-derive.
3. **Eval:** ForIn (976): new branch on the recorded path — call `iterator()`,
   loop `hasNext()`/`next()` through the normal method-call machinery (the same
   dynamic-dispatch helpers the rest of Eval uses); break/continue flags (Track
   02) honored identically to the array branch.
4. **Lower:** ForIn (~519): protocol branch emits `CallDyn iterator`, loop head
   `CallDyn hasNext` + `JumpIfFalse`, body, `CallDyn next` — reusing the Track-02
   loop-ctx for break/continue. Backends consume plain IR: **zero backend work.**

## 3. `Seq<T>` — the lazy pipeline (pure library)

```
class Seq<T> : IIterable<T> {
    IIterator<T> iterator();                 // each Seq subclass/closure provides
    Seq<U>  map<U>((T) => U fn);
    Seq<T>  where((T) => bool pred);
    Seq<T>  take(int n);
    Seq<T>  takeWhile((T) => bool pred);
    Seq<T>  skip(int n);
    Array<T> toArray();
    T? firstOrNone();                         // short-circuits (the lazy payoff)
    int count();
    void forEach((T) => void fn);
    A reduce<A>(A seed, (A, T) => A fn);
}
Seq<T> Array<T>.asSeq();                      // the bridge in
```

Implementation strategy: **iterator-composition classes**, not closures-holding-
closures (`MapSeq<T,U> : Seq<U>` wrapping the source Seq + fn; its iterator wraps
the source's). Closures capture by snapshot (reference §7.1 — "capture-by-
snapshot closure conversion"), which is exactly wrong for a *stateful* iterator
chain — classes hold mutable cursor state honestly. This is the design's central
implementation decision; see problem #1.

Laziness contract (documented): nothing runs until a terminal (`toArray`,
`firstOrNone`, `count`, `forEach`, `reduce`) pulls; `map`/`where` fns run at most
once per pulled element; `firstOrNone` on an infinite-ish source terminates when
the predicate hits.

Answering §19#4 in the docs: **arrays are eager; `Seq` is the opt-in lazy form**
(one sentence into info.md §11 + §19#4 marked resolved-by).

## 4. P-probes

- **P1 (generic interfaces):** the load-bearing probe. A generic interface with a
  generic-returning method (`IIterable<T>.iterator() -> IIterator<T>`),
  implemented by a generic class, consumed through an interface-typed variable —
  on all five engines. Nothing in the corpus obviously exercises
  interface-with-type-params (`InStream` is a class). Three sub-probes:
  declaration checks; dynamic dispatch through the interface type; `is`/match
  narrowing to it.
- **P2:** method-level generic on a generic class returning the *other* generic
  (`Seq<U> map<U>` on `Seq<T>`) — Track 05's P1 verdict transfers; re-run its
  probe shaped as Seq if 05 landed world 2 (raw-head results may break Seq
  chaining harder than Array chaining — chained lazy ops are the whole point).
- **P3:** mutual/recursive prelude class references (`Seq` methods returning
  `MapSeq` which references `Seq`) — prelude declaration order sensitivity.
- **P4:** `for (var x in userIterable)` inference of the loop var through the
  protocol.

## 5. Foreseeable problems & solution strategies

| # | problem | strategy |
|---|---|---|
| 1 | **P1 fails** — generic interfaces are unimplemented/broken somewhere (most likely: dynamic dispatch of a generic method through an interface on IR/ELF). | This becomes the track's real work: fix the generic-interface path (it is required language surface regardless — the framework's `IController`-style contracts will hit it). Budget: the timeline reserves 4 days for exactly this. If the fix demands type-system architecture (e.g. interface method tables don't exist at all on a backend), STOP with the probe matrix. |
| 2 | **Iterator invalidation semantics** — arrays are pure values, so `ArrayIterator` snapshots a value and can NEVER be invalidated (nice); but a protocol iterator over a mutable user collection can self-invalidate. | Document the contract as caveat-emptor for user iterables (same stance as any user code); stdlib iterators are over pure values — immune by construction. One doc paragraph, no machinery. |
| 3 | **`Seq` over closures vs classes** — if implemented with lambdas, snapshot-capture silently freezes cursor state (the §15 Timer-cycle corpus note is the precedent for capture surprises). | Already decided: composition classes with honest mutable fields (§3). Add a corpus program that would fail under snapshot semantics (two pulls advancing shared state) to pin it. |
| 4 | **Infinite sequences** (a user `IIterable` that never ends) + `count()`/`toArray()` — runaway. | Documented terminal-op contract ("terminals require finite sources"); no runtime guard (same stance as `while(true)`). `take(n)` is the user's bound. |
| 5 | **Break/continue inside protocol-desugared loops** — the desugar's `while` must interact with Track 02's loop ctx exactly like a hand-written while. | It IS a lowering to the same StmtKind::While path in Lower (or the same emitted shape) — add loops-in-protocol cases to loops.ext. In Eval, the new ForIn branch must honor `breaking_`/`continuing_` — explicit checklist item, corpus-pinned. |
| 6 | **Cost of protocol loops** (three CallDyn per element vs IterAt) tempting someone to reroute arrays "for uniformity." | Contract C5 forbids it; the design records the reason (measured 2.8× shape-lookup vs offset precedent, §17 — indirection costs are real). |
| 7 | **`Map` iteration duality** — `for (Pair e in m)` (builtin) vs `m.iterator()` (protocol) must yield identical Pair sequences. | One corpus program compares both traversals element-wise. |

## 6. Milestones & acceptance

| M | deliverable | accept |
|---|---|---|
| M1 | P1–P4 logged; generic-interface fixes if needed | probe programs graduate into `tests/corpus/generic_iface.ext` |
| M2 | Protocol: interfaces, checker path-record, Eval + Lower branches; Array/Map/Range iterator classes | `iterator.ext` (user LinkedList-style iterable; var-inference; break/continue inside; Map duality) on all five engines |
| M3 | `Seq` + `asSeq` + terminals | `seq.ext` (laziness pinned: side-effect counter proves at-most-once-per-pull + firstOrNone short-circuit; take-of-many) |
| M4 | Docs + §19#4 resolution note | reference new §6.4.8 (protocol), §6.4.9 (Seq); info.md §11/§19 updates |

Target: **Jul 27 – Aug 2** (M1 2–4d depending on P1, M2 2d, M3 2d, M4 0.5d).
The M1 range is why this track runs last in the compiler chain — its risk is
discovery, not volume.

## 7. STOP conditions

- P1's failure mode is architectural (§5#1).
- The desugar wants a new IR op or a runtime type-probe (design says static
  path-record; runtime probing violates the resolution-by-type rule).
- Seq requires inference the 05-world doesn't provide AND explicit type args
  make chains unusable — escalate with examples rather than shipping a
  compromised surface.

## 8. Implementation log

### 2026-07-06 — prerequisite: merged Track 05 (agent3) into master

`Seq<U> map<U>` needs the exact lambda-last generic-inference checker fix
Track 05 landed (world 1) — that work only existed on `agent3`, unmerged.
Merged into master (conflicts: `CMakeLists.txt` additive test-lane blocks;
`bug.md` — both sessions independently filed unrelated findings as "Bug 18"
same day, renumbered Track 05's to Bug 19). Verified: ctest 52/52 green
post-merge, then pushed (two more concurrent-session commits landed on
`origin/master` mid-session — Trident P2.1b/P2.1c — merged those too,
ctest green throughout, all pushed).

### 2026-07-06 — P1 (generic interfaces): PASSED on all five engines, after
three targeted checker/resolver fixes

Probe: `interface IIterator<T>`/`IIterable<T>`, a generic class implementing
`IIterator<T>`, dynamic dispatch through an interface-typed variable, and
`is`-narrowing to the interface. Found and fixed, all in-scope (mechanical,
not architectural — the established pattern in this file for "found a gap
generic-composition needs, fixed it, logged it"):

1. **`Checker::inferConstruction`** had no graceful fallback when a bound
   type argument resolves to `unknown()` (constructing `Foo<T>` from inside
   a generic body using the enclosing class's own, still-abstract `T` —
   `fromTypeRef` has no representation for a bare type-parameter reference).
   It built a malformed `Foo<>` (empty brackets) instead of degrading to the
   raw head the sibling function `substitute()` already falls back to for
   this exact shape. Fixed: mirror `substitute()`'s leniency.
2. Same function's step 2 (fill missing type args from the target/expected
   type) required `expected`'s resolved symbol to be `cls` itself — failing
   the common protocol shape `IIterator<T> iterator() => ArrayIterator(a);`
   where the constructed class's result is used AS one of its own declared
   bases/interfaces. Fixed: added `directBaseTypeRef` — when `expected` is a
   direct base/interface of `cls`, unify against that base clause's generics
   (written in `cls`'s own parameter names) instead of requiring an exact
   symbol match.
3. **`Resolver::buildShape`**'s interface-satisfaction check (and the
   ordinary class-base slot merge) compared inherited-slot canonical text
   verbatim — `interface IIterator<T> { T next(); }`'s requirement is
   literally `next : () -> T`, so a class satisfying `IIterator<U>` (a
   differently-named parameter) via `U next()` spuriously failed/didn't
   collapse. Fixed: substitute the base's own declared generic names with
   the actual base-clause type-argument text before comparing, for both the
   interface-requirement list and the ordinary class-base slot merge.

All three verified: `p1_probe` (declaration + dynamic dispatch through the
interface + `is`-narrowing) passes byte-identical on `--run`, `--ir`,
`--build` (emit-C++), `--build-native` (LLVM), and `--emit-elf` (frozen,
informational only). Full `ctest` still green after each fix. `unify()`'s
step-1 argument-based inference also needed the same generalization (see P2
below) before P1's probe compiled at all — logged together since both
landed in the same edit pass.

### 2026-07-06 — P2 (`Seq<U> map<U>` via composition classes): checker
fixes landed; STOPPED on an unrelated, pre-existing oracle bug (Bug 21)

Probed the design's §3 composition-class shape (`MapSeq<T,U> : Seq<U>`
wrapping a source `Seq<T>` + `fn`) directly, per its own P2 warning that
"raw-head results may break Seq chaining harder than Array chaining."

**Two more in-scope checker/resolver fixes, same pattern as P1:**
4. `inferConstruction`'s step 1 (bind type args from constructor ARGUMENTS)
   only ever matched a parameter typed as a BARE class type-parameter
   (`T v`) — never a compound generic shape (`IIterator<T> s`, `Array<T>
   src`). Replaced with a per-parameter `unify()` call (the same function
   Track 05 built for lambda-last inference), which already handles both
   shapes correctly.
5. A non-generic-specific design finding: this language's dynamic dispatch
   is genuinely interface-only by design (`Checker.cpp:824`'s own comment:
   "A call through an INTERFACE-typed receiver must dispatch on the runtime
   object's class... Leave `resolved` unset"; a CLASS-typed receiver bakes
   in the statically-resolved declaration, a deliberate perf choice, not a
   bug). `Seq<T>`'s own terminal/combinator methods and any field holding
   "a Seq" (e.g. `MapSeq.source`) must be typed by the INTERFACE
   (`IIterable<T>`), not the class (`Seq<T>`), wherever the actual concrete
   subclass's override needs to run — confirmed this resolves cleanly once
   applied consistently. Not a code change (no general-dispatch-rule change
   attempted); a documented implementation-detail note for how `Seq`'s
   prelude classes must be written before P3/M2.

**STOP finding (Bug 21, filed in `bug.md`, NOT fixed here):** isolating a
remaining "cannot resolve call target" failure down to its minimal shape
(a completely non-generic, two-class repro with a plain `int` field — see
`bug.md` Bug 21) found a real, pre-existing, **oracle-only** (`--run`;
`--ir`/`--build`/`--build-native` all correct) bug: a bare-name field
write/read inside a method/constructor body falls through to a same-named
local in a still-active CALLER's stack frame instead of the object's field,
whenever the collision exists (confirmed via a leak-detection tracer: the
caller's own local visibly changes to the value the "field" write computed).
This is core `Eval.cpp` call/scope machinery (`localLookup`, `evalAssign`,
`callFunction`, `runCtor`) — outside every track's owned region, and the
fix (a per-call frame-boundary marker `localLookup` must not search past)
is a real design decision about the oracle's scoping model, not a one-line
mechanical patch. It DIRECTLY blocks `Seq<T>`'s own design as sketched: the
composition classes' natural field names (`fn`, `source`) collide with
identically-natural parameter names on the combinator methods that construct
them (`map<U>((T) => U fn) => MapSeq(this, fn)` — MapSeq's OWN `fn` field
write, executed while `map`'s frame holding ITS OWN `fn` parameter is still
on the stack, silently no-ops on the object and corrupts `map`'s parameter
instead). Escalating per §7/the global STOP-and-escalate protocol rather
than patching core interpreter scoping unilaterally mid-track — logged here
per that protocol's instruction, and the implementer is pausing for a
ruling: fix Bug 21 first (blocks this track cleanly and permanently), or
proceed by threading every Seq/MapSeq/iterator prelude name to avoid any
collision with any caller's parameter names transitively (workable for the
prelude's OWN internal wiring, but not a fix for user code hitting the same
collision independently).

### 2026-07-06 — Bug 21 FIXED upstream; track unblocked

Ruling landed: fixed, not routed around. `callFunction`/`runCtor`/
`callPrimMethod` (`Eval.cpp`) now isolate `env_` per call (save the caller's
whole stack, run the body on a fresh stack holding only its own params
frame, restore after) — the same save/swap/restore `callClosure` already
did. `localLookup` can no longer walk past a call boundary into a
still-live caller's frame, so a bare-name field write/read inside a
method/constructor body reaches the object's own field first, regardless of
what the caller happens to have declared under the same name. Verified
against the Bug 21 minimal repro, a forwarding-call variant, and a
synthetic `Seq`-shaped class with `fn`/`source` fields colliding with
same-named ctor/method parameters — all four non-frozen engines
(`--run`/`--ir`/`--build`/`--build-native`) agree. Full `ctest` 56/56.
`bug.md` Bug 21 updated to FIXED.

This track is **unblocked**: `MapSeq<T,U>`/`FilterSeq<T>`/etc. can use their
natural field names (`fn`, `source`) exactly as sketched in §3 — no
defensive renaming needed, in the prelude or in user code that hits the
same shape. P3 (`Seq`/M3) can proceed without the workaround previously
under consideration.

### 2026-07-08 — M2 + M3 + M4 landed; track COMPLETE, all four non-frozen engines green

**M2 — the protocol.** Wired the desugar end to end:
- `Stmt::forInProtocol` (a single bool path-record on the AST, `Ast.hpp`) — the
  checker's ForIn decides the path statically and stamps it; Eval/Lower read it,
  never re-derive (contract C5).
- **Checker** (`ForIn` case): after the Range/Array/Map fast-path checks, look up
  `IIterable` and, if the iterable's type implements it, bind the loop var to `E`
  from `iterator()`'s `IIterator<E>` return (via `genericReturn`) and set
  `forInProtocol`. A concrete type that is neither built-in-iterable nor
  `IIterable` is now a compile error naming the protocol (interfaces/unknowns stay
  lenient, so no corpus regressions).
- **Eval** (`ForIn`): a protocol branch driving `iterator()`/`hasNext()`/`next()`
  through the ordinary dynamic-dispatch helpers; break/continue/return/throw
  honored identically to the array branch.
- **Lower** (`ForIn`): a protocol branch emitting `CallDyn iterator` then a
  `hasNext`+`JumpIfFalse` / body / `next` loop head, `decl` left null so dispatch
  resolves on the runtime object; reuses the Track-02 loop-ctx (continue → head).
  No new IR op → **zero backend work** for the loop shape itself.
- **Prelude**: `interface IIterator<T>` / `IIterable<T>`; `Array<T>`/`Map<K,V>`/
  `Range` now implement `IIterable` with `ArrayIterator`/`MapIterator`/
  `RangeIterator` — purely for §2.1 *uniformity* (a built-in usable where an
  `IIterable` is wanted); `for..in` over them keeps its fast path.

  *One real gap surfaced and was fixed (design problem #1's "fix the
  generic-interface path — required surface regardless"):* passing a **built-in**
  where an `IIterable` is wanted and iterating it dispatches `Array.iterator()` /
  `Range.iterator()` by name on the compiled backends. Their reachability seed
  only marked in-language methods of *NewObject-instantiated* classes; arrays/
  ranges arrive via `NewArray`/`MakeRange`, so the iterator classes those methods
  construct never got their `hasNext`/`next` emitted. Fixed in `CGen.cpp`
  (unified the collection-seed drain with the main drain — the old one was an
  incomplete copy that skipped the instClasses member-expansion — and added
  `Range` to the seed) and `LlvmGen.cpp` (added `Range` to its seed; its drain
  already reused the full `scan`). ELF (`X64Gen`, frozen) untouched.

  `tests/corpus/iterator.lev` (user `LinkedList` iterable, `var` inference,
  break/continue inside, Map duality, Array/Range uniformity) — green on
  `--run`/`--ir`/`--emit-cpp`/`--build-native`.

**M1 graduate.** `tests/corpus/generic_iface.lev` — a generic interface with a
generic-returning method, generic-class implementers, dynamic dispatch through an
interface-typed variable, and `is`/`match` narrowing **to** the generic interface
(via a `Box<int> | string` union). Green on all four engines.

**M3 — `Seq<T>`.** Composition classes exactly as §3 sketched: `ArraySeq`,
`MapSeq`/`FilterSeq`/`TakeSeq`/`TakeWhileSeq`/`SkipSeq` each wrapping their source
by the **interface** type (`IIterable<T>`) + a paired `*Iterator` holding honest
mutable cursor fields. `Array<T>.asSeq()` is the bridge. Two implementation
decisions, both forced by the language's dynamic-dispatch model (P2 finding #5):
1. **The base `Seq` is abstract** with a throwing `iterator()` (a bodyless method
   is treated as a native by the IR; a throwing body is never reached because a
   `Seq` value is always a concrete subclass).
2. **The terminals drive the iterator through an interface-typed local**
   (`IIterable<T> self = this; var it = self.iterator(); while (it.hasNext()) …`),
   NOT `for..in`. Reason: the prelude's own method bodies are **not** run through
   the Checker (main.cpp checks only the user program), so `forInProtocol` is
   never stamped on prelude loops — and running the Checker over the whole prelude
   is not viable (it *resolves* the prelude's native `at`/`length`/`add` calls,
   which breaks the compiled backends' by-name native dispatch). A bare
   `this.iterator()` would statically bind the abstract base; the interface upcast
   makes the concrete subclass's override run. Closure-typed fields are called via
   a copy-to-local (`var f = fn; …`), the existing `StreamBuffer` idiom (the IR
   can't call a closure field by bare name).

  `tests/corpus/seq.lev` pins the laziness contract with a side-effect counter:
  building a pipeline runs nothing; `firstOrNone` maps only the 3 elements it must
  pull; `take(2)`/`takeWhile` pull only what they need; a stateful iterator's
  cursor advances across pulls (the composition-class-vs-snapshot pin, problem
  #3). Green on all four engines.

**M4 — docs.** `docs/reference.md` §6.4.8 (protocol: interfaces, desugar, dispatch
order C5, uniformity, past-the-end/invalidation, strings/`InStream` deferrals) and
§6.4.9 (`Seq` surface + laziness contract + composition-class rationale); Array/
Map/Range rows updated. `info.md` §11 + §19 #4 marked **resolved**: arrays eager,
`Seq` the opt-in lazy form.

**Status:** full `ctest` **70/70**. Corpus `.lev` files run on
`--run`/`--ir`/`--emit-cpp`/`--build-native`; the frozen ELF and the
`ir-verify`/`mem-verify` lanes glob only `.ext`, so they cleanly skip the new
files (ELF stays informational-only per §4/§7). Track 07 **done**.
