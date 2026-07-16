# Bug 29 — Oracle: user globals capture bare self-calls inside prelude bodies (the `at` capture)

**Status:** LANDED (2026-07-10) — M1/M2/M3 all implemented and verified; see §7
Implementation log. **Date:** 2026-07-10.
**Source:** `bug.md` #29 [P0]. **Depends on:** nothing. Runs parallel to
`techdesign-bug30-map-with-ownership.md` (disjoint files — that track owns
`runtime/` + `src/LlvmGen.cpp`; this one owns a region of `src/Eval.cpp` only).
**Owns:** `src/Eval.cpp` `evalCall` Name-callee fallback region (`:752-776`) and a
small name-only member-lookup helper next to `findMethod` (`:149-173`); new corpus
cases under `tests/`. **Does NOT touch:** the Checker, the Resolver/prelude text,
`Lower.cpp`, any backend (all already correct), `bug.md` priorities.

---

## 1. The bug is NOT what the report says it is

`bug.md` #29 frames the failure as "`return arr[i]` where the accessor value flows
into the function's `int` return evaluates to void" and points at return-type
resolution. That theory is **disproved by measurement** (2026-07-10, current tree
at `a3dd932`):

| # | program (oracle `--run`) | expected | actual | verdict |
|---|---|---|---|---|
| E1 | `int at(Array<int> a, int i) { return a[i]; }` + call | `20` | *(void)* | ✗ the original repro |
| E2 | E1 with the function renamed `idx` | `20` | `20` | ✓ **rename alone fixes it** |
| E3 | `int at(int i) { return 99; }` + top-level `g[1]`, no return anywhere | `20` | `99` | ✗ **the user global runs instead of the accessor** |
| E4 | `int at(string k) => 7` + `m["k"]` on a `Map<string,int>` holding 5 | `5` | `7` | ✗ Map capture too |
| E5 | `at(Array,int)` present in file; `int f(a) { int x = a[1]; writeln(x); return 0; }` | `20` | *(void)* | ✗ `return 0` variant ALSO fails |
| E6 | NO `at` in file; same `f` but `return x` | `20` | `20` | ✓ `return x` variant works |

E5+E6 are the report's "adjacent shapes" with the confound removed: the return
expression is irrelevant; the **presence of a user global function named `at`** is
the entire trigger. (The report's minimal repro happened to name its helper `at` —
the natural name — and every "bounding" variant inherited or dropped that name
along with the shape being varied.) E3 is the cleanest statement: a program that
never returns anything from anywhere still misroutes plain `g[1]`.

All compiled engines (`--ir`, `--build`, `--build-native`) print the correct value
on every row. Oracle-only, P0.1 stands — arguably stronger than filed, because the
trigger is *any* user global whose name collides with a prelude method that prelude
bodies call bare (`at`, `length`, ... — natural helper names), not one exotic shape.

## 2. Root cause (proven)

The prelude `Array<T>` indexes through an in-language accessor
(`src/Resolver.cpp:354`):

```
get ([])(int i) => at(i);     // Map<K,V> likewise: get ([])(K key) => at(key)  (:651)
```

The Checker **never walks prelude class bodies** (long-standing fact — see the
bug #13 note at `src/Eval.cpp:151-159`, and the Track 07 lesson: do not "fix" that
by checking the prelude; it breaks native `at`/`length` resolution on the compiled
backends). So every bare call inside a prelude body reaches the oracle with
`e->resolved == nullptr` and takes `evalCall`'s by-name fallback
(`src/Eval.cpp:752-776`), whose order is:

1. `:766` — `resolveFunction(callee)`: **global function symbols** (user program
   globals included) by name, arity-blind (`:133-147`).
2. `:769-775` — only if that misses: the receiver's own methods
   (`thisObj_` branch, and the `hasPrimThis_ && thisClass_` branch that prim/array
   receivers use) via `findMethod`.

Inside `Array.([])`'s body, `at(i)` therefore binds to any user global named `at`
before `Array.at` is ever consulted. In E1 the user's `at(Array<int>, int)` is
called with one arg (`i`); `a` binds to `vint(1)`, `i` is unbound, `a[i]` on an int
yields void, and the void propagates out — the reported symptom, three frames
removed from the cause.

Two independent authorities already encode the intended precedence, and both say
**receiver methods win**:

- **The Checker**, for checked (user) code: a bare same-class call is pre-resolved
  to the method — that is the premise of the bug-4 receiver-rebind logic at
  `src/Eval.cpp:754-763`.
- **Lower.cpp**, for the SAME unchecked prelude bodies on the compiled engines
  (`src/Lower.cpp:1137-1148`): the by-name global fallback is explicitly guarded —
  `if (fnDecl && !(curClass_ && classHasMember(curClass_, callee->text)))` — and
  the self-method CallDyn branch (`:1166-1178`) takes the call instead. This guard
  is WHY `--ir`/emit-C++/LLVM are all correct.

The oracle is the only engine missing the guard. That is the entire bug.

## 3. The fix

Port Lower's guard into `evalCall`'s fallback, so the oracle resolves unchecked
bare calls with the same precedence as every other engine:

In `src/Eval.cpp`, before accepting `resolveFunction`'s result at `:766`, skip it
when the enclosing receiver's class has ANY member of that name:

```cpp
// bug.md #29: a bare call in an UNCHECKED body (prelude accessors/methods —
// the checker never resolves those) must prefer the receiver class's own
// methods over user globals, exactly like Lower.cpp:1148's classHasMember
// guard — otherwise `int at(...)` in the user program captures the prelude
// accessor body `get ([])(int i) => at(i)` and every a[i] misroutes.
Symbol* selfCls = thisObj_ ? thisObj_->cls
                : (hasPrimThis_ ? thisClass_ : nullptr);
bool shadowedBySelf = selfCls && classHasMember(selfCls, callee->text);
if (!shadowedBySelf)
    if (const Stmt* fn = resolveFunction(callee))
        return callFunction(fn, args, nullptr, nullptr);
```

with `classHasMember(Symbol*, name)` as a small name-only scan of
`cls->shape.slots` (the flattened, inheritance-inclusive slot list `findMethod`
already walks — `findMethod(cls, name, -1) != nullptr` is an acceptable spelling;
a dedicated helper reads better and matches Lower's name). The existing
`thisObj_`/`hasPrimThis_` method-dispatch branches at `:769-775` then take the
call unchanged, including `findMethod`'s bug-13 arity preference.

Decision points, settled here so the implementer doesn't re-derive them:

- **Guard, not reorder.** Moving the self-method branches above `resolveFunction`
  is behaviorally equivalent for name matches, but the guard form is textually
  parallel to `Lower.cpp:1148`, which makes the cross-engine parity reviewable
  line-against-line. Decision: guard.
- **Name-only, not arity-aware.** Lower's `classHasMember` ignores arity; the
  oracle must match Lower, not improve on it, or the engines diverge on the
  (pathological) "class method `foo(int)`, bare call `foo()`, global `foo()`
  exists" shape. Both engines send that to the method path and let dispatch
  fail loud. Decision: name-only.
- **Do NOT check the prelude** to give it `e->resolved` — ruled out by the
  Track 07 finding (prelude checking resolves native `at`/`length` calls and
  breaks the compiled backends). The fallback-order fix retires the whole class
  without touching that hazard.
- **No prelude renames.** Renaming `at(i)` to `this.at(i)`-style spellings in the
  prelude would fix instances, not the class, and is exactly the per-use
  workaround P1.2 exists to reject.

## 4. Blast radius audit (M1, mechanical)

Any prelude method name that appears as a BARE call inside a prelude body is
capturable today: `at` (`Array.([])`, `Map.([])`, `first`, `last`), `length`
(`isEmpty`, `last`), `isEmpty` (`firstOrNone`/`lastOrNone`), `add` (`where`,
`map`, `filter`, ...), `iterator`, `hasNext`/`next` (iterator protocol), string
helpers, etc. As part of landing:

1. `grep` the kPrelude text in `src/Resolver.cpp` for bare-call sites inside
   class bodies; confirm each intends a self-call (spot-check says yes: the
   prelude's global calls are namespaced `std::`/`sys*` natives, whose names no
   prelude class reuses as a method — so the guard flips nothing it shouldn't).
2. Record the audited list in the implementation log, so the next prelude author
   knows bare self-calls are safe again.

## 5. Milestones & acceptance

- **M1** — audit (§4). Output: one log entry, no code.
- **M2** — the guard + helper in `src/Eval.cpp` (§3). Acceptance:
  E1–E6 all print the expected column under `--run`, byte-identical to
  `--ir`/`--build`/`--build-native` on the same file.
- **M3** — regression corpus: land E1/E3/E4/E5 as corpus programs (name them for
  the capture, e.g. `oracle_prelude_name_capture_*.lev`). **Ordering rule:**
  generate their `.expected` files only AFTER M2 is green — the oracle is the
  wrong engine today, and pinning pre-fix output is precisely the P0.1
  poisoning risk this bug was tagged for. Also re-run the full existing corpus:
  expected zero diffs (the guard only fires on the collision shape, which the
  corpus — by virtue of having been green on all engines — cannot currently
  contain).
- Priorities: unchanged. This doc does not re-tier the bug.

**Timeline:** half a day, one agent, one file. M1 ~1h, M2 ~2h, M3 ~2h.

## 6. STOP conditions

1. The M1 audit finds a prelude bare call that genuinely targets a global whose
   name IS also a method of the enclosing class — the guard would flip it. STOP
   and escalate with the site (expected: none exist; Lower already imposes this
   precedence on the compiled engines, so such a site would already be broken
   there — i.e., a distinct, pre-existing bug worth its own entry).
2. M3's corpus re-run shows ANY diff outside the new cases. STOP with the
   differential matrix (engine × case) — that means user-visible code depended
   on global-over-self capture under `--run`, and the ruling on which behavior
   wins belongs to the owner.
3. Anything tempts you to run the Checker over the prelude. STOP; re-read §3.

## 7. Implementation log

- **2026-07-10 — M1:** Audited bare-call sites across all four `kPrelude*`
  segments. Calls whose names are members of the enclosing class are intended
  self-calls: primitive/string helpers (`code`, `length`, `subStr`, `charAt`,
  `indexOf`, `indexOfFrom`, `split`, `trim`, `startsWith`, `endsWith`,
  `toLower`); Array/Map helpers (`at`, `length`, `isEmpty`, `first`, `take`,
  `skip`, `sort`, `has`); Seq iterator helpers (`iterator`, `prime`, `doSkip`);
  stream/socket/file lifecycle helpers (`isEmpty`, `send`, `stopDrain`, `close`,
  `settle`, `open`); and DateTime formatting accessors (`hour`, `minute`,
  `second`). Actual global/native calls are namespace-qualified or do not share
  a member name. No STOP condition found.
- **2026-07-10 — M2:** Added the name-only `Evaluator::classHasMember` scan and
  guarded the unchecked global-function fallback in `evalCall`. Build and
  evaluator tests pass.
- **2026-07-10 — M3:** Added E1/E3/E4/E5 as
  `oracle_prelude_name_capture_*` corpus cases, with expected files generated
  after the fix. Each case is byte-identical under `--run`, `--ir`, `--build`,
  and `--build-native`; the full shared corpus has zero diffs on all four lanes.
