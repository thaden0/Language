# Track 05 — Stdlib: Arrays, Maps, `Set<T>`, and the Key-Equality Protocol

**Status:** ready. **Date:** 2026-07-05. **Depends on:** nothing to start (prelude
track); one **checker risk item** (generic inference, §2) may require a compiler fix
— it is this track's only src/-beyond-prelude work.
**Source:** suggested-features.md §6, §7; §13 items 1–2; contract C3 (owner).
**Owns (regions):** kPrelude `class Array` (Resolver.cpp:49–156), `class Map`
(161–174), `class Pair` (43–47), new `class Set`; `RuntimeNatives.cpp` Array/Map
branches (~33–100) + `keyEquals` (`RuntimeValue.hpp`, shared by the tree-walk
oracle and the bytecode interpreter); mirrored CGen core (`CGen.cpp`'s `keyEq`);
`lvrt_keyeq` (new, `runtime/lv_abi.h`/`lv_runtime.c`) + its one call site in
`LlvmGen.cpp`'s native "has" row (the LLVM path's `.at()`/`[]` already shared
`lvrt_idxget`, so wiring that one function closed both).

**Revised 2026-07-06 (corrected same day):** X64Gen's map/array native region
is **NOT** owned by this track — `X64Gen.{hpp,cpp}` was frozen by the ratified
portable-backend pivot (2026-07-05, the same day this doc was dated) as the
reference anchor Track B's new portable runtime is measured against; extending
it is exactly what got a prior Track 04 attempt reverted. `LlvmGen.cpp` and
`lv_runtime.c`, by contrast, turned out to be actively maintained and fair
game — precedent found in `corpus_strings_native_llvm` (Track 04 added new
natives there too) — so the C3 struct-recursive rule is landed there as well
(§1). Only X64Gen keeps the pre-C3 identity-only behavior, by design. See
Bug 19 (`bug.md`) for a separate, unrelated gap this surfaced.

**Prime directive** (same as Track 04): in-language bodies over the 3-intrinsic
Array core + 7-intrinsic Map core wherever expressible. New natives in this track:
**none required** for v1 (everything below is expressible; `entries()` gets an
in-language body over `keys()`+`at`).

---

## 1. The two signature fixes (land first — they are bugs in effect)

Resolver.cpp:69/74/75:

```
Array<T> map((T) => T fn)          →   Array<U> map<U>((T) => U fn)
Array<T> select((T) => T fn)       →   Array<U> select<U>((T) => U fn)
T reduce(T seed, (T, T) => T fn)   →   A reduce<A>(A seed, (A, T) => A fn)
```

Bodies are already shape-correct (accumulate via `add`); only declarations change
— plus `map`'s local `Array<T> r` becomes `Array<U> r`, `reduce`'s `T acc`
becomes `A acc`.

### The inference risk — RESOLVED 2026-07-06 (probed world 3; fix validated to world 1)

`join<U>` proves method-level generics work when `U` is inferable **from a real
argument value** (`other: Array<U>`). `map<U>` must infer `U` **from the lambda's
return type** — a capability the docs pointedly do not claim (info.md §11 notes
joins "return pairs so only U needs inference — from `other`, a real typed
value"; reference §2.5 says an opaque-lambda result falls back to the **raw
head**).

**P1 verdict (probed on the real checker): world 3.** With only the signature
changed, `Array<string> r = a.map((n) => n.toString());` hard-errors:
`cannot initialize 'r : Array<string>' with 'Array<>'`. Two independent causes,
confirmed by code reading, NOT a mere ordering problem:

- Lambda arguments always type as `unknown` — deliberately unmodeled
  (Checker.cpp, `ExprKind::Lambda` case: "the lambda's own type stays
  unmodeled", tied to bug.md #8) — so there is no return type to unify from,
  regardless of argument order.
- `Checker::unify` bails on any parameter whose declared type is
  `TypeKind::Function` (first line: `param->kind != TypeKind::Named` → return),
  so it never looks inside `(T) => U` at all. This also voids §2's hope that
  `flatMap`'s `Array<U>`-shaped lambda return would fare better — it hits the
  same top-level bail.
- Additionally, `substitute()`'s plain-class branch emits the malformed
  `Array<>` (empty canonical arg) for an unbound var instead of degrading to
  the raw head the way its HKT branch does — that is where the hard error
  (vs. world 2's leniency) comes from.
- The §7#1 "explicit type args" interim **does not exist**: the parser has no
  call-site type-argument syntax (`a.map<string>(...)` parses `<` as
  less-than; `parsePostfix` has no generic-args arm). Worlds 2/3 have no
  workaround surface — the checker fix is the only path to shipping `map<U>`.

**The fix (LANDED on agent3 alongside the `map<U>` signature change; diff also
kept at `designs/complete/track05-p1-checker-fix.patch` for reference — ~120 lines, all
in Checker.{hpp,cpp}):** lambda-last inference, exactly the shape sketched
below, which the checker's structure turned out NOT to resist. Five components:

1. **Defer lambda args** in `typeOfCall`: the upfront argument-typing loop
   pushes `unknown` for `ExprKind::Lambda` args (identical overload-scoring to
   today) instead of walking them. `typeOfCall` becomes a thin wrapper around
   `typeOfCallInner(e, lambdaWalked)`; after the inner call, a sweep walks any
   still-unwalked lambda arg with declared/unknown params — so every lambda
   body is walked exactly once on every path (error paths included; bug.md #8's
   "inner calls get `resolved`" invariant holds unconditionally).
2. **`checkLambdaBody(lam, paramTypes)`** (new): walks a lambda body with
   supplied parameter types (declared types win; missing → unknown, the old
   behavior) and returns the inferred return type — an expr body's type, or a
   block body's uniform `Return` type via a `lambdaReturns_` collector
   (mixed/absent returns → unknown → lenient degrade). Save/restores
   returnType_/loopDepth_ exactly as the old Lambda case did; the
   `ExprKind::Lambda` case delegates to it and still returns `unknown` (the
   lambda's own type stays unmodeled outside call position).
3. **`genericReturn(..., call, lambdaWalked)`** (extended, default-null — all
   other callers unchanged): after value arguments unify (binding T from the
   receiver, A from a seed, U from real values like `zip`'s `other`), each
   lambda argument whose parameter is `TypeKind::Function` gets its param types
   from `substitute(funcParams[i], subst)`, its body walked via
   `checkLambdaBody`, and its inferred return unified against `funcRet` —
   binding U. Value-args-first ordering is what makes `reduce<A>` (A from
   seed wins) and `sortBy<K>`-with-typed-receiver work in one pass.
4. **Ctor lambda args**: the construction path walks lambda ctor args with
   their declared function-type param types (no generic binding —
   `inferConstruction` owns class type args) and marks them walked.
5. **The world-2 safety net**: `substitute()`'s plain-class branch mirrors the
   HKT branch — any unbound type arg degrades the whole instantiation to the
   RAW head (assignable to any instantiation per §2.5) instead of emitting
   `Array<>`. Uninferable corners (e.g. mixed-return block lambdas) become
   weakly typed, never hard errors.

**Validation (all with the `map<U>` signature change applied):** P1 infers
(`Array<string>` binds; misuse `Array<string> r = a.map((n) => n * 2)` errors
correctly — the checking is real, not lenient); P1b chained maps fully typed
(`a.map((n) => n * 10).map((m) => m.toString())` — T survives into the second
lambda); block-bodied lambdas infer via the uniform-return rule; identical
output on --run, --ir, and native LLVM; **full ctest 44/44** (every engine
lane, incl. qemu-aarch64 + wine) with zero corpus fallout — notable because the
fix also strengthens ALL call-position lambdas (e.g. `where((x) => ...)` now
types `x` from the function-type parameter instead of unknown), a strict
tightening the corpus tolerates as-is.

## 2. Array additions (all in-language)

| method | signature | body notes |
|---|---|---|
| `sort` | `Array<T> sort((T, T) => int cmp)` | **merge sort** (stable — contract for orderBy later; §13#2): in-language, slice-based merge via `take`/`skip` or index loops; pure (returns new) |
| `sortBy` | `Array<T> sortBy<K>((T) => K key)` | `sort((a, b) => key(a) < key(b) ? 0-1 : (key(b) < key(a) ? 1 : 0))` — needs `<` on K at instantiation (duck-typed generic body); K without `<` → instantiation error (honest). Same U-from-lambda inference dependency (§1) |
| `find` | `T? find((T) => bool pred)` | loop, return first hit, else `None` — **probe P2**: `T?` as a generic method return (union of type-var and None) must type + lower; sysRecv precedent is concrete `string?` |
| `firstOrNone`/`lastOrNone` | `T? firstOrNone()` | length-gated |
| `flatMap` | `Array<U> flatMap<U>((T) => Array<U> fn)` | U infers from the lambda's return via the §1 fix (the "container unification may make this easier" hope was wrong — pre-fix `unify` never enters function-type params at all; post-fix the inferred `Array<int>` return unifies against `Array<U>` through the existing concrete-generic branch) |
| `forEach` | `void forEach((T) => void fn)` | trivial |
| `distinct` | ~~`Array<T> distinct()`~~ → **`Array<T> unique()`** (renamed) | O(n²) contains-scan v1; uses `==` (protocol §4). `distinct` is a **reserved member-modifier keyword** (`KwDistinct`, diamond-inheritance disambiguation — `Token.cpp`'s `{"distinct", TokenKind::KwDistinct}`), not available as a method name at class-body declaration position; using it corrupted the ENTIRE prelude parse (everything after it silently mis-parsed — caught immediately via the §7#2 rebuild-per-batch discipline). Renamed to `unique()` (LINQ/lodash naming), no grammar change |
| `groupBy` | `Map<K, Array<T>> groupBy<K>((T) => K key)` | scan; `m[k]`-rebind accumulate (bracket sugar, not `.with()` — portable to every engine incl. the two Bug 18 blocks); O(n²) scan-map v1 — correctness now, Dictionary later (§6) |
| `zip` | `Array<Pair<T, U>> zip<U>(Array<U> other)` | length = min; `U` from a real value (safe inference) |
| `takeWhile`/`skipWhile` | `Array<T> takeWhile((T) => bool)` | scans |
| `indexWhere` | `int indexWhere((T) => bool)` | -1 miss |
| `withIndex` | `Array<Pair<int, T>> withIndex()` | contract with Track 07 (§C5 consumer) |
| `insertAt`/`removeAt`/`with` | `Array<T> with(int i, T v)` | pure updates; `with` mirrors Map's vocabulary; bounds → RuntimeException |
| `slice` | `Array<T> slice(int from, int len)` | throws on OOB (arrays already throw; strings' clamp is the historical exception). **Parameter named `from`, not `start`** — `start` is `class Range`'s own field name and hijacked the tree-walk oracle's call-frame scoping (bug.md #21, found here, filed, NOT fixed — it's an `Eval.cpp` core defect, any caller-local colliding with a bare name inside ANY callee) |
| `sum`/`min`/`max`/`average` | free functions: `int std::sum(Array<int>)`, `float std::sum(Array<float>)`, `int? std::min(Array<int>)`, ... | there is no specialization mechanism on `Array<T>`, so aggregates are **overloaded free functions in std** (resolution by argument type — the language's own rule; suggested-features §6.2). `min/max` return `T?` (empty). `minBy/maxBy` DO fit as methods: `T? minBy<K>((T) => K key)` (duck-typed `<` on K) |

## 3. Map additions (in-language over the native 7)

| method | signature | body |
|---|---|---|
| `atOrNone` | `V? atOrNone(K k)` | `has(k) ? at(k) : None` (double scan v1 — fine) — same P2 generic-optional dependency |
| `atOr` | `V atOr(K k, V dflt)` | `has(k) ? at(k) : dflt` |
| `entries` | `Array<Pair<K, V>> entries()` | loop `keys()` + `at` building Pairs (or `for (Pair e in this)` — iteration already yields Pairs; probe which lowers cleaner) |
| `withAll` | `Map<K,V> withAll(Map<K,V> o)` | fold `o`'s entries via `with` |
| `mapValues` | `Map<K,U> mapValues<U>((V) => U fn)` | rebuild; U-from-lambda dependency again |
| `whereEntries` | `Map<K,V> whereEntries((K, V) => bool)` | rebuild |

## 4. `Set<T>` and the key-equality protocol (contract C3)

### 4.1 Set — in-language over `Map<T, bool>`

```
class Set<T> {
    Map<T, bool> m;
    int length() => m.length();
    bool isEmpty() => m.isEmpty();
    bool has(T v) => m.has(v);
    Set<T> with(T v) { ... m.with(v, true) ... }
    Set<T> without(T v) { ... }
    Array<T> toArray() => m.keys();
    Set<T> union(Set<T> o)     // fold o.toArray() via with
    Set<T> intersect(Set<T> o) // filter
    Set<T> except(Set<T> o)
}
```
Pure value semantics, insertion-ordered, zero natives. Constructor wrinkle: a
`Set(Array<T> items)` convenience ctor folds items. (`union` as a member name:
`union` is not a keyword — closed unions are type syntax `A | B`; P3 confirms the
name parses as a member.)

### 4.2 Key equality — settle the rule, one implementation point

**Rule (C3):** primitives by value; **structs field-wise, recursive** (a struct
IS its fields — info.md §9); classes by identity; enums by carrier (they are
structs post-desugar, so this falls out).

Today `keyEquals` (RuntimeNatives.cpp; ELF sibling in X64Gen's map natives —
string-content compare landed in commit 53f3700) covers primitives + identity.
Work: add the struct-recursive case in `keyEquals` (interpreters), CGen's mirror,
and the ELF map-native comparator (X64Gen — extend the existing keyEquals-shaped
emitted routine; the string branch added there is the pattern). Struct compare =
per-slot compare by each field's own rule (recursion terminates: structs form a
finite DAG, §15).

**Hashing:** *specified* now, *implemented* with Dictionary (§6): `hash()` will
be auto-derived field-wise for structs / value for primitives / identity for
classes, with the invariant `a == b ⇒ hash(a) == hash(b)`. No user-facing
surface in this track.

## 5. P-probes

- **P1 / P1b:** ✅ DONE 2026-07-06 (§1): world 3 as-is; world 1 with the
  validated checker fix (`designs/complete/track05-p1-checker-fix.patch`). Chaining,
  block bodies, negative case, all engines, ctest 44/44.
- **P2:** ✅ DONE 2026-07-06. `T? find(...)` on the real `Array<T>` initially
  failed the SAME way as P1 world 3 (`cannot initialize 'f : int | None' with
  'T | None'`) — `Checker::substitute` had no `TypeKind::Union` case at all, so
  a generic-optional return's `T` never got bound to the receiver's concrete
  type. Fixed: added the missing case (recursively substitutes each union
  member, mirroring `fromTypeRef`'s Union handling) — one-line-conceptually,
  ~10 lines actual, Checker.cpp only. Validated on all 4 engines (`--run`/
  `--ir`/`--build`/`--build-native`): hit + miss + empty-firstOrNone all
  narrow and print correctly; ctest 48/48.
- **P3:** ✅ DONE 2026-07-06. `union` parses fine as a member name (`Set.union`
  landed, called in `maps_set.ext`, all engines). `std::min`/`std::max`
  coexist cleanly with `int.max(int)`/`int.min(int)` — different receivers,
  resolution by argument type separates them without ambiguity, no collision
  found. (The REAL naming landmine this track hit was `distinct`, a reserved
  keyword, not `union`/`min`/`max` — see §2's `unique` rename.)
- **P4:** ✅ DONE 2026-07-06. Before-state confirmed as predicted: two
  field-equal-but-distinct `Point` instances miss on every engine (identity
  compare). Landed the struct-recursive `keyEquals` rule in `RuntimeValue.hpp`
  (tree-walk + IR interpreters), `CGen.cpp`'s `keyEq` mirror, AND
  `lv_runtime.c`'s new `lvrt_keyeq` (shared by `lvrt_idxget`'s map branch and
  LlvmGen's native "has" row) — `--run`/`--ir`/`--build`/`--build-native`
  (LLVM) all correctly `has()`/`[]`-hit on field-equal keys (verified via the
  `[]` sugar path, since `.with()`/`.at()` named-calls hit Bug 19 below on
  LLVM independent of this fix). **Only the frozen X64Gen/`--emit-elf`
  backend keeps the pre-C3 identity-only behavior** (verified: same probe
  program prints `false` there, matching the documented before-state) — a
  single-engine, permanent (by design, since X64Gen is frozen) gap, not the
  2-of-4 split originally logged. Primitive keys are unaffected everywhere.
  Byproduct: found Bug 19 (`bug.md`) — `.with()`/`.without()` are missing
  entirely from LLVM's and X64Gen's named-method native dispatch (silently
  void, not just identity-vs-structural), independent of key equality and
  pre-existing before this track touched anything.
- **P5:** ✅ DONE 2026-07-06. 10k-element `groupBy` into 100 groups: 21ms on
  `--build-native`, 109ms on `--ir` for 3k elements — no quadratic-with-large-
  constant blowup observed at this scale. Correctness ships as planned;
  Dictionary (§6) remains the eventual algorithmic fix, not an urgent one.

## 6. Explicit follow-up (on the roadmap, NOT this track): `Dictionary`

Hash-backed map + the O(n+m) by-key `join`/`groupJoin` (info.md §11's promise)
+ `hash()` derivation per §4.2. Depends on: this track's equality rule, Track 01
(shifts/xor for hash mixing). Sized ~1.5 weeks; schedule after Gate C. Recorded
here so §13's data thrust has its named next step.

## 7. Foreseeable problems & solution strategies

| # | problem | strategy |
|---|---|---|
| 1 | **P1 world 3** (inference errors) and the checker fix is nontrivial. | RESOLVED (§1): world 3 confirmed by probe, but the checker does NOT resist lambda-last ordering — the specified fix is LANDED (~120 lines, Checker.{hpp,cpp} only), validated on all engines with ctest 44/44. The "explicit type args" interim is moot — the parser has no call-site type-arg syntax at all. |
| 2 | **Prelude growth breaks the resolver's prelude parse** (kPrelude is one big string; a syntax slip breaks EVERY test at once). | Edit incrementally, rebuild + `ctest` after each method batch; the prelude parses with the same diagnostics as user code, so errors are locatable. Keep each method's body minimal — helper-free, no cleverness. |
| 3 | **Duck-typed generic bodies** (`sortBy`'s `<` on K) may error at *prelude parse* rather than instantiation if bodies are checked eagerly. | reference §2.5 says bodies check leniently at instantiation (HKT rule) — but that is documented for HKT; P1's scratch runs reveal the truth for plain methods. If eager: constrain sortBy to comparator-composition (implemented via `sort` only) and defer key-based variants; log it. |
| 4 | **`sort` in-language perf** (merge sort over pure ops allocates O(n log n) arrays). | Acceptable v1 (correctness + stability first; COW keeps constants sane). Measure in the log (10k, 100k ints on --ir/--emit-elf). If unusable (>seconds at 100k), the escape is a native `sortInPlace` core behind the same pure surface — but that is a new native: requires the §8-style deviation note, not silent addition. |
| 5 | **groupBy + struct keys before C3 lands in all engines** → engine divergence (oracle field-wise, ELF identity). | REVISED 2026-07-06, then corrected same day: X64Gen is frozen (never touched — §1's Owns revision) but `LlvmGen.cpp`/`lv_runtime.c` turned out to be fair game (Track 04 precedent) and got the fix too (`lvrt_keyeq`). So keyEquals landed on 4 of 5 surfaces (tree-walk, IR interpreter, CGen, LLVM); only the frozen ELF backend keeps identity-only struct-key compare, permanently, by design. Bug 19 (`.with()`/`.without()` missing on LLVM+ELF) is separate and unrelated — it blocks those two named methods regardless of key type, filed for its own owner, not fixed here. `set_map_keys.ext`'s acceptance is scoped to the 4 fixed engines; groupBy/distinct/Set with primitive keys are unaffected everywhere. |
| 6 | **`with(int, T)` name collision** with future Map-vocabulary reading (`Array.with` sets an index; `Map.with` adds a key) — semantically parallel, fine — but ALSO with any user classes named. | It is an added overload on Array only; no global name. No action beyond the corpus pin. |
| 7 | **Free-function aggregates in `std`** shadowing/colliding with user `sum` definitions. | Nearest-wins shadowing already governs (user decls beat std — reference §12.6/std note). Corpus case pins it. |

## 8. Milestones & acceptance

| M | deliverable | accept |
|---|---|---|
| M1 | ✅ P1–P4 done (P5 folds into M3's groupBy work; P3 into M4's Set); keyEquals struct rule on 4/5 engines (X64Gen frozen, by design) | `set_map_keys.ext`; ctest 48/48 |
| M2 | ✅ map/select/reduce signature fixes landed | `arrays_ext.ext` part 1 (map-to-string, reduce-to-int-from-strings, chained maps); every engine |
| M3 | ✅ Array additions (§2) + aggregates landed | `arrays_ext.ext` part 2 (sort stability pinned: equal-key order preserved; find/None narrowing; groupBy golden output); every engine incl. `--emit-elf` (no struct-key/`.with()` dependency) |
| M4 | ✅ Map additions (§3) + Set (§4.1) landed | `maps_set.ext`; every engine incl. `--emit-elf` (Set's `with`/`without` rebuild via bracket sugar, never `.with()`/`.without()` by name — sidesteps Bug 18 entirely); reference §6.3/§6.4.5/new §6.4.7 (Set) |

Target: **Jul 8 – Jul 19** (M1 3d, M2 2d, M3 4d, M4 3d).

## 9. Reference-doc duty — ✅ DONE 2026-07-06

`docs/reference.md` §6.3 (Array table — full rewrite, incl. the map/select/
reduce generic-inference note), §6.4.5 (Map — full method table + the C3
key-equality rule + the Bug 18 engine caveat), new §6.4.7 (`Set<T>`).
`info.md` §11: method list updated, key-equality (C3) summarized, and the
"arrays carry an index" `join`/`groupJoin` promise corrected to state its
actual current complexity (O(n·m), predicate form only) with an explicit
pointer to the `Dictionary` follow-up (§6) that will make it true.

## 10. STOP conditions

- ~~P1 world 3 + resistant checker (§7#1).~~ Cleared 2026-07-06: world 3
  confirmed but the checker is not resistant — fix validated, §1/§7#1.
- Any new native beyond keyEquals extension (the directive allows zero).
- Duck-typing probe (§7#3) reveals eager body checking AND comparator-only
  sortBy is deemed insufficient — needs a ruling on generic constraints
  (`where K: comparable`), which is §19#2 territory, firmly out of track scope.

## 11. Implementation log

**2026-07-06 — P1/P1b resolved; checker fix validated; no track code landed yet.**

- P1 probed as specified, before any signature edit: prelude `map` →
  `Array<U> map<U>((T) => U fn)` alone ⇒ **world 3** (hard error
  `cannot initialize 'r : Array<string>' with 'Array<>'`). Root causes traced
  (see §1): lambdas type as unknown by design; `unify` never enters
  `TypeKind::Function` params; `substitute`'s plain-class branch emits
  malformed `Array<>` instead of the HKT branch's raw-head degrade.
- Initial STOP was called per §1's all-at-once-unification clause; on
  re-examination the deferral is cleanly localized (typeOfCall wrapper +
  genericReturn extension), so the "resistant checker" branch does not apply.
- Fix prototyped along §1's exact shape and validated: P1 world 1; P1b chained
  maps fully typed; block-bodied lambdas infer (uniform-return rule); negative
  probe errors correctly; --run/--ir/native-LLVM agree; **ctest 44/44**, zero
  corpus fallout. Bonus tightening: every call-position lambda now gets real
  param types (previously unknown) — strictly more checking, corpus-clean.
- Parser survey: NO call-site type-argument syntax exists (`parsePostfix` has
  no `<...>` arm), so worlds 2/3 had no explicit-type-args escape hatch.
- The validated fix is **landed on agent3** (Checker.{hpp,cpp} + the prelude
  `map<U>` signature). The diff is also kept verbatim at
  `designs/complete/track05-p1-checker-fix.patch` for reference.

**2026-07-06 — M2 closed; M1's keyEquals closed on 4 of 5 engines; Bug 19 found (unfixed, filed).**

- M2: `select<U>`/`reduce<A>` signature fixes landed (Resolver.cpp), riding the
  same lambda-last inference. Validated (chained selects, string-seed reduce,
  int-seed reduce) + `tests/corpus/arrays_ext.ext` added (map/select cross-type,
  reduce cross-type, chained maps, block-bodied lambda) — passes on
  `corpus_llvm`/`corpus_core_ir` (registered corpus dirs auto-pick it up).
  Full ctest 44/44.
- M1 keyEquals (§4.2/C3): landed the struct-recursive rule in
  `RuntimeValue.hpp::keyEquals` (tree-walk + bytecode interpreters), `CGen.cpp`'s
  `keyEq` mirror, AND (after an initial, too-conservative pass — see below)
  a new `lvrt_keyeq` in `runtime/lv_runtime.c`/`lv_abi.h`, wired into
  `lvrt_idxget`'s map branch (so `.at()`/`[]` pick it up for free) and
  `LlvmGen.cpp`'s native `"has"` row (replaced its own inline tag/payload
  compare with one call to `rtKeyEq`). Validated on `--run`/`--ir`/`--build`/
  `--build-native` via the `[]` sugar path (`m[a]=100; m.has(b)` → `true`,
  `m[b]` → `100`, for field-equal-but-distinct struct keys) — all four agree.
  `--emit-elf` (X64Gen) still correctly prints the pre-C3 `false`/void,
  confirmed untouched.
- **First pass wrongly treated `LlvmGen.cpp`/`lv_runtime.c` as off-limits**,
  by over-applying the X64Gen freeze reasoning. Caught before committing to
  it: `corpus_strings_native_llvm` (already in ctest, already green) proves
  Track 04 extended `LlvmGen.cpp` for new stdlib natives, so it's actively
  maintained, not frozen — only `X64Gen.{hpp,cpp}` itself is. Redid the work
  once this was found; see the corrected §1/§7#5 text (their superseded
  "2-of-4"/"3 reachable engines" framing is gone).
- While probing P4 on native, found **Bug 19** (filed in `bug.md`, NOT fixed
  here): `.with()`/`.without()` are missing entirely from LLVM's AND X64Gen's
  named-method native dispatch (only the `[]=`/`[]` sugar path works) —
  independent of key equality, pre-existing, silently returns void rather than
  erroring or falling back. This is a much bigger and more clear-cut gap than
  keyEquals was: unlike `LlvmGen.cpp`'s "has" row (which this track owns the
  fix for, per the precedent above), fixing `.with()`/`.without()` means
  writing the missing map-upsert/map-remove native rows from scratch on two
  backends at once — real, unreviewed, ARC-sensitive new code well beyond a
  one-function key-equality swap, and not what this track's design asked for.
  Filed with a precise repro and root cause instead. It blocks Track 05's own
  `Set<T>`/`Map.withAll` bodies (both call `.with()`/`.without()` by name)
  from working under `--build-native`/`--emit-elf` regardless of the keyEquals
  fix above — noted in the M3/M4 acceptance plan below.
- **Net scope, recorded per §7#5:** keyEquals (this section) is correct on
  `--run`/`--ir`/`--build`/`--build-native`; only `--emit-elf` keeps the old
  identity behavior, permanently, by design. Bug 19 independently limits
  `--build-native`/`--emit-elf` for anything calling `.with()`/`.without()` by
  name (Set, Map.withAll) until its owner fixes it — Track 05's own corpus
  acceptance for those specific features targets `--run`/`--ir`/`--build`.

**2026-07-06 — M3 closed (Array additions §2 + aggregates); P2 done; two more
bugs found and worked around (not fixed) — bug.md #21, and a naming collision.**

- P2 (generic-optional returns, `T? find(...)`) hit the identical failure
  shape as P1 world 3 for a completely different reason: `Checker::substitute`
  had no `TypeKind::Union` branch, so `T?`'s `T` never resolved against the
  receiver. Fixed with a ~10-line addition (recurse into union members,
  mirroring `fromTypeRef`'s existing Union case) — validated on all 4 engines.
- Landed the rest of §2 in one batch, then hit a real parser/prelude
  landmine per §7#2's own predicted failure mode: naming a method `distinct`
  (as the design table specified) silently corrupted the ENTIRE prelude parse
  — `distinct` is a reserved member-modifier keyword (diamond-inheritance
  disambiguation, `Token.cpp`), not available at class-body declaration
  position. Caught immediately (rebuild-and-test-per-batch, §7#2's own
  discipline) via a trivial "hello world" program failing with `unknown type
  'T'` errors. Renamed to `unique()` — no grammar change, table above updated.
- While validating `slice`, found and pinned down **Bug 21** (filed in
  `bug.md`, NOT fixed — core `Eval.cpp` defect, not Track 05's to fix):
  `Evaluator::callFunction`/`runCtor` push a new scope onto the SAME `env_`
  stack without isolating the caller's frames beneath it (contrast
  `callClosure`, which correctly swaps to an isolated stack) — so a bare
  identifier inside ANY callee that happens to match a variable name in ANY
  calling scope up the whole call chain silently binds to that OUTER variable
  instead of the callee's own field/local. Reproduced with plain user code
  (a `class Box` ctor), confirmed **`--run`-only** (the tree-walk oracle) —
  `--ir`/`--build`/`--build-native` all correctly isolate. Triggered here
  because `slice(int start, int len)`'s parameter name collided with `class
  Range`'s own field `start`; renamed the parameter to `from` (table above
  updated) rather than leave the landmine, since fixing `Eval.cpp`'s
  call-frame model is well outside this track's scope.
- All of §2 + the aggregates validated end-to-end: `unique`, `flatMap`,
  `zip`, `takeWhile`/`skipWhile`, `indexWhere`, `withIndex`, `insertAt`/
  `removeAt`/`with`, `slice`, `sort` (stability explicitly pinned — equal
  keys keep original order), `sortBy`/`minBy`/`maxBy` (duck-typed `<` on K,
  no eager-body-check surprise — §7#3's worry didn't materialize), `find`/
  `firstOrNone`/`lastOrNone`, `groupBy` (bracket-sugar accumulate, portable
  to every engine incl. Bug-18-affected ones), and `std::sum`/`min`/`max`/
  `average` (int + float overloads, `min`/`max` return `T?`). `forEach`
  works for its real use case (side effects) — verified it does NOT support
  a "mutate an outer accumulator" pattern, but that's closures capturing by
  snapshot (a separate, pre-existing, unrelated design choice — `reduce` is
  the accumulation tool, not `forEach`), not a defect.
- `tests/corpus/arrays_ext.ext` extended with a part 2 section covering all
  of the above, including the sort-stability and groupBy golden-output cases
  the M3 acceptance criteria call for. Runs on the FULL shared corpus (every
  engine, including `--emit-elf`) — none of §2's new methods depend on
  struct-key equality or `.with()`/`.without()`, so no engine-scoping needed
  here (unlike `set_map_keys.ext`). ctest 48/48.

**2026-07-06 — M4 closed (Map additions §3 + Set §4.1); P3/P5 done. Track 05 M1–M4 all closed.**

- Map additions landed exactly as designed: `atOrNone`/`atOr` (the P2 fix
  applies directly — `V?` is the same generic-optional-return shape as
  `T?`), `entries()` via `for (Pair e in this)` (the design's own suggested
  alternative to a keys()+at() double scan — cleaner, used it), `withAll`/
  `mapValues`/`whereEntries` all rebuilding via bracket-sugar accumulate
  (`m[k] = v`), not `.with()` — a deliberate choice, not just style: it keeps
  every one of these methods working on every engine including the two
  Bug 18 blocks (LLVM, X64Gen), which `.with()`-based bodies would not.
- `Set<T>` landed over `Map<T, bool>` per §4.1's sketch, with one deviation
  from the sketch's `with`/`without` bodies: implemented via bracket-sugar
  rebuild (`mm[v] = true`, and a `for`+filter rebuild for `without`) instead
  of calling `Map.with()`/`Map.without()` by name, for the same Bug 18
  reason as the Map additions above. Verified this was the right call: the
  full probe (ctor, `with`/`without` purity, `union`/`intersect`/`except`,
  `toString`) matches byte-for-byte across ALL FIVE execution paths —
  `--run`, `--ir`, `--build`, `--build-native`, AND `--emit-elf` — meaning
  Set has zero engine-scoping needs, unlike `set_map_keys.ext`.
- P3: confirmed clean — `union` parses as a member name with no grammar
  conflict, and `std::min`/`std::max` coexist with `int.min`/`int.max`
  (different receivers, resolved by argument type, no collision). The naming
  landmine this track actually hit was `distinct` (§2, a reserved keyword),
  not anything in P3's watch list.
- P5: 10k-element `groupBy` into 100 groups runs in 21ms (`--build-native`);
  no quadratic-blowup concern at realistic sizes. Correctness-first v1 ships
  as planned; Dictionary (§6) remains the eventual algorithmic follow-up.
- `tests/corpus/maps_set.ext` added, covering every method above plus Set
  purity (`with`/`without`/`union`/`intersect`/`except` all pure — verified
  the ORIGINAL set is unchanged after each derived-set operation) and
  `toString()`. Passes on the full shared corpus, all 5 execution paths,
  ctest 48/48.
- **Track 05 M1–M4 are all closed.** Remaining from the original design: §9
  reference-doc duty (reference.md/info.md updates) and the `Dictionary`
  follow-up (§6, explicitly out of this track's scope, scheduled after Gate
  C per the design). Three bugs found and filed along the way (18, 19) plus
  one naming-keyword landmine worked around (`distinct`→`unique`) — all
  documented in `bug.md` and in this log; none required backing out any
  shipped Track 05 work.
