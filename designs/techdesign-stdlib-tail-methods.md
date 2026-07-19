# Stdlib tail methods — `indexOfAny`, ranged `subStr`, `Array.orderBy`/`.thenBy`

**Status:** ready. **Date:** 2026-07-19. **Depends on:** nothing unlanded — rides
entirely on already-shipped infrastructure (Track 04's string toolkit, incl. the
`indexOfFrom` naming precedent this design reuses; Track 05's `Array`/`Pair`/
`sort`/`sortBy`/`select` and its lambda-last generic-inference checker fix;
`docs/reference.md`'s bytes-vs-scalars rule from the recently-landed UTF-8
`chars()` track).
**Source:** `designs/requests/request-stdlib-tail-methods.md` (carries forward
three items from the now-deleted `designs/suggested-features.md` §5.3/§7 that
Tracks 04/05 did not pick up).
**Owns (regions):** kPrelude `class string` (`src/Resolver.cpp`, `indexOfAny`
lands beside `indexOfFrom`/`lastIndexOf` around line 207; `subStrRange` lands
beside `subStr`'s declaration around line 180); kPrelude `class Array<T>`
(`orderBy` lands beside `sortBy`, `src/Resolver.cpp:635-638`); two brand-new
prelude classes, `OrderedArray<T>` and `OrderedArrayIterator<T>` (placed near
`Pair`/`ArrayIterator`, `src/Resolver.cpp:397-401`/`685-691`). `docs/reference.md`
§6.1 and §6.3. New/extended corpus files (§6 below).
**Does NOT own / touches nothing in:** `RuntimeNatives.cpp`, `CGen.cpp`,
`LlvmGen.cpp`, `runtime/lv_runtime.c`, `runtime/lv_abi.h`, `X64Gen.cpp` — see
Prime directive.

**Prime directive:** all three features are expressible **in-language, over
already-landed natives**, with **zero new natives and zero backend edits** —
including on the frozen `X64Gen.cpp`/`--emit-elf` backend, which per the
portable-backend pivot is never extended. This is not just an aspiration here:
§2 below documents a real, verified hazard where a naive implementation of one
of these three features (a same-named `subStr` overload) would silently
*corrupt* several already-shipped ELF behaviors through a frozen-backend
dispatch mechanism, without touching that backend at all. Avoiding that is this
design's central engineering decision.

---

## 1. `string.indexOfAny(Array<string> needles)`

Plain in-language scan over the existing native `indexOf`. New method name —
no collision with `indexOf`/`indexOfFrom`, no overload-arity risk anywhere.

```cpp
// Earliest index at which ANY needle occurs, or -1 if none do (incl. an empty
// needles array — recommended by the request ticket, consistent with indexOf
// on a not-found substring). Does not report WHICH needle matched; a caller
// that needs the identity can re-check each needle's indexOf against the
// returned position (documented in reference.md, not a second return value —
// the request's signature is a bare int).
int indexOfAny(Array<string> needles) {
    int best = -1;
    for (string needle in needles) {
        int p = indexOf(needle);
        if (p >= 0 && (best < 0 || p < best)) best = p;
    }
    return best;
}
```

Placement: `class string`, immediately after `indexOfFrom` (`src/Resolver.cpp`
~line 219).

**Edge cases (pinned, matching the request's Known Warnings):**
- empty `needles` → loop body never runs → `-1`.
- `needles` contains `""` → `indexOf("")` is `0` on every engine (native `find`
  with an empty pattern matches position 0), so `indexOfAny` returns `0`
  regardless of the other needles or their order. Documented as a defined edge
  case, not a bug — callers that don't want this filter empty strings out of
  `needles` themselves.
- Order-independence: the *position* found is what's minimized, not needle
  array order — a needle listed last that happens to occur earliest still wins.

No natives, no engine-specific work; runs on all five engines for free (the
established "Array.where model", `designs/complete/techdesign-04-stdlib-strings.md`
line 14).

## 2. Ranged `subStr` — **not** implemented as a `subStr` overload

### 2.1 The hazard (why this deviates from the request's literal ask)

The request asks for `s.subStr(2..5)` as "a `Range`-taking overload of the
existing `subStr(start, len)`." Adding a same-named, body-bearing `subStr`
method to `class string` is unsafe on the frozen `--emit-elf` backend and would
silently corrupt multiple already-shipped features. This was verified by
reading the actual dispatcher, not inferred:

`X64Gen::genCallM` (`src/X64Gen.cpp:2180-2235`) is the dynamic-dispatch routine
every prelude **bare self-call** compiles to (`src/Lower.cpp:1439-1450`: any
call whose callee resolves against `curClass_`'s own members lowers to a
name-carrying `Op::CallDyn` with a **null** static `decl`, because prelude
class bodies are never walked by `Checker::run` — it only walks the user's
`program.items`). For each class, `genCallM` loops over that class's
**body-bearing** methods (`X64Gen.cpp:2200-2210`, gated by
`mod_.byDecl.count(m)`) and, for each one, compares the *runtime* method-name
pointer passed at the call site (`RDX`, loaded from stack slot `-24`) against
that candidate's own interned name (`X64Gen.cpp:2215-2216`). **It never inspects
the caller's actual argument count** (`RCX`, stored at `-32` on entry but never
read again inside the loop) — once a name match is found, it unconditionally
forwards `np - 1` arguments, where `np` is the *matched candidate's own*
declared parameter count (`X64Gen.cpp:2214`, `2217-2220`), not however many the
call site actually passed.

This is the exact mechanism that already forced Track 04 to rename an
`indexOf(string, int)` overload to `indexOfFrom` (`src/Resolver.cpp:207-211`'s
own comment: *"a same-name bare self-call inside this class would resolve
through Eval/Lower's arity-blind by-name fallback and silently pick the wrong
arity. Distinct name sidesteps the ambiguity entirely."*). That specific
fallback was later **hardened to be arity-aware on four of five engines** —
`Eval.cpp:278-302`, `IrInterp.cpp:27-47`, `CGen.cpp:1400-1428`,
`LlvmGen.cpp:1577-1610` all now prefer the uniquely-arity-matching candidate —
but `X64Gen::genCallM` was **never patched**, consistent with the frozen-backend
policy (`docs/reference.md:975-977`).

`subStr(int start, int len)` (`src/Resolver.cpp:180`) is declared **native,
with no body** today, so it currently has **zero** entries in `genCallM`'s
per-class method table (only body-bearing decls are collected,
`X64Gen.cpp:2205`). Every existing bare self-call to `subStr(a, b)` inside
`class string` — `indexOfFrom` (`:217`), `split` (`:242,246`), `replace`
(`:258,262`), `padStart`/`padEnd` (`:270,276`), `trimStart`/`trimEnd`
(`:297,303`), `splitLines` (`:312`), `removePrefix`/`removeSuffix`
(`:332-333`) — currently bypasses `genCallM`'s loop entirely and falls straight
to the arity-correct native-core fallback (`X64Gen.cpp:2230-2234`, the same
hand-rolled `genStrSubStr`, `X64Gen.cpp:2990`).

**The moment any body-bearing `subStr`-named method is added to `class string`
— regardless of what that new method's own body does — it becomes the sole
body-bearing `subStr` candidate in `genCallM`'s table.** Every one of the nine
existing call sites above still emits `rdx = "subStr"`, `rcx = 2` at its call
site; on `--emit-elf` only, that dynamic dispatch now matches the new
one-parameter candidate by name (arity is never checked), forwards a single
argument, and reinterprets the caller's first `(int)` argument's tag/payload as
if it were a `Range` object pointer — silently corrupting `split`, `replace`,
`padStart`, `padEnd`, `trimStart`, `trimEnd`, `splitLines`, `removePrefix`,
`removeSuffix`, and `indexOfFrom` on the ELF backend, all already-shipped and
corpus-covered, the instant the overload lands. Every other engine is
unaffected (their dispatch layers got the bug.md #13 arity-aware fix); only the
frozen backend carries this exposure, and it carries it silently — no compile
error, no test failure until something specifically exercises `--emit-elf`.

### 2.2 Decision: distinct name, exactly the `indexOfFrom` precedent

Per Acceptance Criteria #1's "owner-approved variant" allowance, this ships as
a **distinctly-named** method, not an overload:

```cpp
// Byte-indexed range slice — matches subStr(start, len) exactly (Range has no
// unit of its own; this is "the same value used as a slice argument" the
// request cites, applied to the byte-indexed core per docs/reference.md's
// Bytes-vs-scalars rule, not a new rune-indexed slice). Range is
// inclusive-inclusive (class Range's own ctor comment, Resolver.cpp ~2650/
// 2660), so len = end - start + 1.
//
// Deliberately NOT named `subStr` (not an overload) -- see the X64Gen
// genCallM hazard in this track's design doc §2.1: a same-named body-bearing
// overload would hijack every existing bare self-call to subStr(int,int)
// inside this class on the frozen --emit-elf backend (name-only, arity-blind
// dynamic dispatch). Same shape of fix Track 04 used for indexOf/indexOfFrom.
string subStrRange(Range r) {
    int len = r.end - r.start + 1;
    return len <= 0 ? "" : subStr(r.start, len);
}
```

Placement: `class string`, immediately after `subStr`'s native declaration
(`src/Resolver.cpp` ~line 181).

**The `len <= 0` guard is deliberate, not defensive filler:** passing a
negative `len` straight into the native `subStr` disagrees across engines
*today*, latent and never previously observable because no in-language caller
ever passed one: `RuntimeNatives.cpp:633`/`CGen.cpp:495`'s `std::string::substr`
with a huge size_t-cast negative count clamps to "rest of string", while
`lv_runtime.c`'s `lvrt_str_substr` comment states it "clamps a negative n to 0
explicitly." `subStrRange` sidesteps that disagreement entirely by making
empty/backwards ranges (`r.end < r.start`) a well-defined `""` on every engine,
uniformly, before the native is ever called.

**Out-of-bounds `r.end` (`>= length()`)** is inherited as-is from `subStr`'s
existing clamp-not-throw behavior (`RuntimeNatives.cpp:633`:
`a >= 0 && a <= s.size() ? s.substr(a, n) : ""`, with `n` itself silently
clamped by `std::string::substr`) — the same "historical clamp" `Array.slice`'s
own comment contrasts itself against (`Resolver.cpp:608`: *"Throws on OOB
(unlike string.subStr's historical clamp)"*). Worth stating explicitly so it
isn't mistaken for an oversight later: `subStrRange` clamps (matches its
sibling `subStr`), `Array.slice` throws (matches its own sibling `Array.at`) —
different by design, each consistent with its own class's existing convention.

No natives, no backend edits (X64Gen included, precisely because this avoids
being a `subStr`-named candidate at all). Runs on all five engines.

## 3. `Array.orderBy` / `OrderedArray<T>.thenBy`

### 3.1 Representation decision

The request poses an explicit fork: a wrapper type (`orderBy` returns
something `thenBy` composes on, `thenBy` uncallable standalone) vs. a
`sortBy` alias plus a tie-break-within-runs `thenBy` on plain `Array<T>`. This
design takes the wrapper path, using the exact name the request itself
proposes, `OrderedArray<T>`:

1. It's the request's own stated preference (type-level distinguishability).
2. It makes the request's own flagged failure mode — "a naive `thenBy`
   re-sorts the whole array and breaks stability across the first key" —
   **structurally unreachable**, not merely avoided by careful implementation:
   `thenBy` does not exist on plain `Array<T>` at all, so there is no method a
   future caller (or editor) could invoke on an unsorted array expecting
   `sortBy`-like standalone behavior.
3. Direct mechanical precedent already in the prelude: `Set<T>`
   (`Resolver.cpp:779+`) is exactly this shape — a pure, in-language,
   zero-native wrapper exposing a narrow read surface over a native collection,
   every "mutating" method returning a new value. `OrderedArray<T>` follows the
   identical mold.
4. `Array<T> : IIterable<T>` (`Resolver.cpp:416`) is the established way a
   wrapper stays directly `for`-loopable without forcing an explicit unwrap at
   every call site; `OrderedArray<T>` does the same via a dedicated
   `OrderedArrayIterator<T>`, mirroring `ArrayIterator<T>`
   (`Resolver.cpp:685-691`) line for line.

This is a deliberate deviation from the request's literal code-block
signatures (`Array<T> orderBy(...)`, `Array<T> thenBy(...)`) — but matches the
very next paragraph of the same request precisely, and is exactly the kind of
"owner-approved variant" Acceptance Criteria #1 pre-authorizes.

### 3.2 Design: one algorithm, `orderBy` is `thenBy`'s degenerate case

`OrderedArray<T>` carries the sorted items plus a **run id** per element — a
plain `int`, monotonically assigned in the array's current order, where two
adjacent elements share a run id iff they tied on every key applied so far.
Representing the run boundary as a type-erased `int` (rather than trying to
carry each key's own type `K` forward) is what makes chaining `thenBy` an
arbitrary number of times possible without the wrapper's type growing a new
generic parameter per call.

```cpp
class OrderedArray<T> {
    Array<T> items;
    Array<int> runId;   // runId[i] == runId[i-1]  <=>  items[i-1]/items[i]
                         // tie on every key applied so far
    new OrderedArray(Array<T> its, Array<int> ids) { items = its; runId = ids; }

    // Reuses the already-proven-stable Array.sort rather than a hand-rolled
    // segmented sort: tag each element with its EXISTING run id, sort tagged
    // pairs comparing run id first -- key() is never even evaluated for a
    // cross-run pair, so cross-run order literally cannot be touched, not
    // just "usually preserved" -- then recompute a finer run partition.
    // `orderBy` (Array<T>, below) is the one-run degenerate case of this same
    // method: there is exactly one implementation of "tie-break only within
    // runs," so it cannot drift out of sync with itself.
    OrderedArray<T> thenBy<K>((T) => K key) {
        Array<Pair<int, T>> tagged = [];
        for (int i in 0 .. items.length() - 1) {
            tagged = tagged.add(Pair::Of(runId.at(i), items.at(i)));
        }
        Array<Pair<int, T>> sorted = tagged.sort((p, q) => {
            if (p.first < q.first) return 0 - 1;
            if (q.first < p.first) return 1;
            K ka = key(p.second);
            K kb = key(q.second);
            return ka < kb ? 0 - 1 : (kb < ka ? 1 : 0);
        });
        Array<T> newItems = sorted.select(pr => pr.second);
        Array<int> newRunId = [];
        int counter = 0;
        for (int i in 0 .. sorted.length() - 1) {
            if (i > 0) {
                Pair<int, T> prev = sorted.at(i - 1);
                Pair<int, T> cur = sorted.at(i);
                bool sameOldRun = prev.first == cur.first;
                K kprev = key(prev.second);
                K kcur = key(cur.second);
                bool tiedOnKey = !(kprev < kcur) && !(kcur < kprev);
                if (!(sameOldRun && tiedOnKey)) counter = counter + 1;
            }
            newRunId = newRunId.add(counter);
        }
        return OrderedArray(newItems, newRunId);
    }

    Array<T> toArray() => items;
    int length() => items.length();
    bool isEmpty() => items.isEmpty();
    IIterator<T> iterator() => OrderedArrayIterator(this);
}

class OrderedArrayIterator<T> : IIterator<T> {
    Array<T> a;
    int i = 0;
    new OrderedArrayIterator(OrderedArray<T> src) { a = src.items; }
    bool hasNext() => i < a.length();
    T next() { T v = a.at(i); i = i + 1; return v; }
}
```

`Array<T>.orderBy<K>` — placed beside `sortBy` (`Resolver.cpp:635-638`, whose
own comment already anticipates this: *"a contract `orderBy` (later) depends
on"*):

```cpp
    // orderBy is the K=1-run degenerate case of thenBy: everything starts in
    // one run (no key applied yet), so thenBy's own run-refinement collapses
    // to exactly a stable sort by `key` -- same algorithm, same correctness
    // guarantee as multi-key chains, zero duplicated sort logic.
    OrderedArray<T> orderBy<K>((T) => K key) {
        Array<int> allRun0 = [];
        for (int i in 0 .. length() - 1) allRun0 = allRun0.add(0);
        return OrderedArray(this, allRun0).thenBy(key);
    }
```

`arr.orderBy(k => k.lastName).thenBy(k => k.firstName)` — the request's own
example — chains exactly as written: `orderBy` returns `OrderedArray<T>`,
`.thenBy` is defined there and returns `OrderedArray<T>` again, so a third
`.thenBy` composes with no special-casing. Callers get a plain `Array<T>` back
via `.toArray()`, or iterate the wrapper directly
(`for (T x in arr.orderBy(...).thenBy(...))`) without unwrapping at all.

**Correctness argument for "tie-break only within runs, not sort again"** (the
request's own named risk): the composite comparator inside `thenBy` compares
`p.first`/`q.first` (the OLD run id) *first*, and only evaluates `key(...)` at
all once those compare equal. A cross-run pair therefore never has `key`
called on it and is ordered purely by (already order-preserving) run id — the
new key cannot move it. This is a structural guarantee from the comparator's
short-circuit shape, not an emergent property of "the sort happens to be
stable" — stability additionally guarantees that pairs tied on **both** the
old run id and the new key keep their prior relative order, which is what
makes chaining a third or fourth `.thenBy` correct too.

No natives; zero backend edits. No X64Gen hazard analogous to §2's: `orderBy`/
`thenBy`/`OrderedArray`/`OrderedArrayIterator` are all brand-new names with no
existing body-bearing candidates anywhere to collide with.

## 4. P-probes

Per this codebase's own established practice (Track 05 §1/§5: never assume a
generic-inference or dynamic-dispatch mechanism generalizes without hand-
verifying on the real checker/backend first), run these before or immediately
upon starting implementation:

- **P1 (generic-inference-on-a-new-class probe — run first for §3):**
  Track 05 §1's lambda-last `genericReturn` extension was built and validated
  against `Array<T>`'s and `Map<K,V>`'s *own* methods; the mechanism is
  documented as keying off the receiver's type substitution generically, but
  `OrderedArray<T>.thenBy<K>` is the first time it would need to bind a method
  type-var on a **user-defined, non-collection generic class**. Before writing
  the real feature, hand-write a minimal scratch reproduction exercising the
  same three shapes at once (matching Track 05's own "probe with the real
  shape, not a toy" discipline): (a) a toy one-field generic class with a
  method taking `(T) => K` and returning something built from `K`; (b) that
  method's receiver constructed via a plain value-arg constructor (mirrors
  `OrderedArray(this, allRun0)` — ordinary generic-ctor inference, same shape
  `Set(Array<T> items)` already uses, lower risk); (c) a nested-generic
  `Array<Pair<int, T>>.sort(...)` call (an already-generic method,
  `Array<T>.sort`, instantiated at a nested-generic `T = Pair<int, T>` — no new
  mechanism, but not previously exercised in exactly this shape either, since
  today's nested-generic precedent, `zip`'s `Array<Pair<T,U>>` return, is never
  itself the receiver of a further generic call). Run on `--run` first (fast
  iteration), then all four non-frozen engines.
- **P2 (X64Gen `genCallM` hazard, confirmatory — run before or alongside §2):**
  §2.1's finding is from direct source reading of `X64Gen.cpp:2180-2235`, not
  a probe — but per this codebase's own culture ("probed on the real checker,"
  not just read), confirm it end-to-end once: in a scratch branch, add a
  temporary one-parameter body-bearing method literally named `subStr` to
  `class string` (any body), leave every existing `subStr(a,b)` call site
  untouched, and run `tests/corpus/strings_ext.ext` on `--emit-elf`. Expect
  `split`/`replace`/`padStart`/`trimStart`/`indexOfFrom` etc. to start failing
  or producing wrong output — this validates the decision to use
  `subStrRange` instead of an overload. If it does **not** fail, the source
  reading in §2.1 is wrong somewhere and the overload path should be
  reconsidered — but do not skip this probe on the assumption that reading the
  assembly-emission code was sufficient; that assumption is exactly what P2 is
  for.
- **P3 (`indexOfAny` edge cases, cheap — run alongside implementation):**
  confirm the empty-`needles` (`-1`) and `""`-in-`needles` (`0`) cases end to
  end on all five engines, not just reasoned about — matches §1's own pinned
  behavior.
- **P4 (`thenBy` stability, design-time — do this on paper/scratch before the
  real corpus file):** construct the "several people share the same
  `lastName`, and some of those also share `firstName`" case described in §5's
  M3 acceptance row *before* considering the implementation done, per the
  request's own Known Warning that a naive implementation "looks right on
  small/random test data, breaks on inputs with many equal primary keys" — the
  point of this probe is specifically to catch that failure mode on a
  many-ties input, not a monotonically-distinct one.

## 5. Foreseeable problems & solution strategies

| # | problem | strategy |
|---|---|---|
| 1 | **A `subStr(Range)` overload silently corrupts ELF-backend `split`/`replace`/etc.** (§2.1) — the headline risk of this whole design. | Resolved by construction: ship `subStrRange`, a distinct name, never entering `genCallM`'s per-class table under the `subStr` name at all. P2 confirms. If a future editor "simplifies" this back into a same-named overload, P2's regression guard (§6's corpus note) is the tripwire. |
| 2 | **`orderBy`/`thenBy`'s lambda-`K` inference doesn't generalize to a new user-defined generic class** (P1 fails). | Not unprecedented — Track 05 §1 landed an analogous ~120-line `Checker.{hpp,cpp}` fix for exactly this class of gap, scoped around `Array<T>`/`Map<K,V>`'s own methods. If `genericReturn`'s extension turns out to be receiver-class-specific rather general, the resolution is the same shape of fix, generalized to any receiver — not a redesign of `orderBy`/`thenBy` itself. Escalate per STOP (§8) rather than improvising a checker patch inline if the gap is nontrivial. |
| 3 | **Negative/backwards `Range` fed to `subStrRange`** hits the pre-existing cross-engine `subStr`-negative-`len` disagreement (RuntimeNatives/CGen clamp to "rest of string"; `lv_runtime.c` clamps to 0). | Sidestepped entirely: `subStrRange`'s own `len <= 0` guard returns `""` before the native is ever called, on every engine uniformly. Not a fix to the underlying native disagreement (out of scope — no in-language caller other than this one is known to hit it), just a documented avoidance. |
| 4 | **`thenBy`'s composite-comparator body is non-trivial in-language code** (nested lambda capturing `K`, tagged pairs, a second linear scan) — more surface for a subtle bug than the request's Known Warning anticipates from a "naive resort" alone. | §3.2's correctness argument is structural (short-circuit on run id, no cross-run `key` evaluation) rather than "tested and seemed right" — but P4 (a many-ties corpus case, not just monotonically distinct keys) is the acceptance gate precisely because the request itself flags that naive-vs-correct is invisible on easy inputs. |
| 5 | **`Pair::Of` / nested `Array<Pair<int,T>>` allocation overhead** in `thenBy` — one intermediate `Array<Pair<int,T>>` per call, on top of `sort`'s own O(n log n) allocation churn (§13#4 of Track 05's own design, "acceptable v1"). | Same complexity class as `groupBy`/`sortBy` already ship with; Track 05 §5 P5 measured 10k-element `groupBy` at 21ms on `--build-native` with no quadratic blowup at realistic sizes — `thenBy` is strictly cheaper (one extra O(n) tag/untag pass around an existing O(n log n) sort). Correctness-first v1, no perf gate for this design. |
| 6 | **`OrderedArray<T>`'s constructor is technically public** (no `private`/module-visibility keyword exists in this language — confirmed absent, same gap Track 04/05 already documented for `static`), so a caller could hand-construct a bogus `(items, runId)` pair with a mismatched or malformed `runId` array. | Same norm as `Set`/`StringBuilder`/`Pair`/`Range` — none of the prelude's existing wrapper classes hide their internals either. Not a blocking issue; document `OrderedArray`'s constructor as "produced by `orderBy`, not meant to be hand-built" the same way `RangeIterator`/`ArrayIterator` are documented as protocol-internal despite being technically constructible. |

## 6. Milestones & acceptance

Three independent milestones, matching the request's own framing ("none
blocks the others or is blocked on anything new") — any can land first.

| M | deliverable | accept |
|---|---|---|
| M1 | `string.indexOfAny` (§1) | extend `tests/corpus/strings_ext.ext`: multiple needles at distinct positions (earliest wins, independent of needle-array order), no match (`-1`), empty `needles` (`-1`), `""` present in `needles` (`0`). Green on all 5 engines. |
| M2 | `string.subStrRange` (§2) | new `tests/corpus/strings_subrange.ext`: mid-string range, range touching start/end, single-char range (`start==end`), backwards/empty range (`end<start` → `""`), range extending past `length()` (clamps, doesn't throw). Plus an explicit **regression section** re-exercising `split`/`replace`/`indexOfFrom`/`trimStart` after `subStrRange` lands, as a standing tripwire against problem #1 (§5) ever silently reappearing. Green on all 5 engines, including `--emit-elf` (this method never touches the `subStr` name, so it's not excluded the way `strings_native/` is). P2 run and logged. |
| M3 | `Array.orderBy` / `OrderedArray<T>.thenBy` (§3) | new `tests/corpus/arrays_orderby.ext`: (a) single-key `orderBy` vs. existing `sortBy` on distinct keys, same output; (b) the request's own `lastName`/`thenBy(firstName)` example, with at least one shared `lastName` in non-firstName-sorted input order, proving `thenBy` reorders within the tied group; (c) **the stability-proof case** (Acceptance Criteria #2) — several records sharing the primary key, some of those *also* sharing the secondary key, each record carrying a hidden original-order id, asserting (i) different-primary-key groups never interleave regardless of secondary key and (ii) records tied on *both* keys keep their original input order; (d) empty array; (e) single-element array; (f) direct `for`-loop iteration over an `OrderedArray` without `.toArray()`; (g) a third chained `.thenBy` call. Green on all 5 engines. P1/P4 run and logged. |

## 7. Reference-doc duty

`docs/reference.md`:
- §6.1 (`string`, currently `:958-965`'s `category | members` table): add
  `indexOfAny` under the `search` row (beside `indexOfFrom`/`lastIndexOf`); add
  a dedicated callout paragraph for `subStrRange` (mirroring the existing
  `byteAt`/`reverse()` paragraphs, `:983-997`) spelling out: byte-indexed
  (cross-reference the Bytes-vs-scalars section, `:999-1017`), `Range` is
  inclusive-inclusive, backwards/empty range → `""`, out-of-range `end` →
  clamps (matches `subStr`, unlike `Array.slice`).
- §6.3 (`Array<T>`, `:1072-1105`): add `orderBy<K>(fn)`/`thenBy<K>(fn)` to the
  `sorting` row, cross-referencing a new subsection for `OrderedArray<T>`
  placed alongside §6.3.5 `StringBuilder`/§6.4.7 `Set<T>` (`:1110`/`:1309`) —
  same template: purpose, minimal method surface (`thenBy`, `toArray`,
  `length`, `isEmpty`, iteration), and the "not meant to be hand-constructed"
  note from problem #6 (§5).

## 8. STOP conditions

- P2 (§4) does **not** confirm the X64Gen hazard as read in §2.1 — i.e., the
  source-reading turns out to be wrong in a way that changes the safe/unsafe
  call. Re-derive the actual hazard boundary before choosing between an
  overload and a distinct name; do not ship an overload on the strength of "P2
  didn't reproduce it" alone without understanding why not.
- P1 (§4) reveals the lambda-last generic-inference mechanism is deeply
  receiver-class-specific (not a bounded, Track-05-§1-shaped extension) —
  needs an owner ruling on checker scope before `orderBy`/`thenBy` proceeds;
  M1/M2 are unaffected and can land independently regardless.
- Any variant of any of the three features is found to need a **new native**
  beyond the ones already landed (`indexOf`, `subStr`, `sort`) — the
  zero-new-natives prime directive is load-bearing for all three; a new
  native is a design change, not an implementation detail.

## 9. Implementation log

*(empty — fill in at landing time, per this repo's own convention: date,
engines validated, any deviation from this design and why, bugs found/filed.)*
