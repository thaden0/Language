# Summary: Small stdlib method gaps — `String.indexOfAny`, ranged `subStr`, `Array.orderBy`/`thenBy`

Tracks 04/05 landed essentially the entire string/array/map method wishlist from
`designs/suggested-features.md` §5/§6/§7 (`split`, `replace`, `toInt() -> int?`, `sort`,
`sortBy`, `flatMap`, `groupBy`, `zip`, `withIndex`, `distinct`/`unique`, `atOrNone`,
`mapValues`, and the rest — see `designs/complete/techdesign-04-stdlib-strings.md` and
`techdesign-05-stdlib-collections.md`). Three small, independent method-surface asks from
those sections were not picked up and have no tracking doc anywhere else. This ticket
exists to carry them forward now that the source document is being retired, grouped
together because each is a small, self-contained addition over already-landed
infrastructure — none blocks the others or is blocked on anything new.

## Request Details

1. **`string.indexOfAny(Array<string>)`** — find the earliest index at which *any* of a
   set of substrings occurs (and, implicitly, which one). Useful for scanners/parsers that
   need to find the next of several delimiters without a manual loop over `indexOf` calls.
2. **Ranged `subStr`** — `s.subStr(2..5)` as a `Range`-taking overload of the existing
   `subStr(start, len)` (info.md §11 already documents `Range` as a first-class, spreadable
   value; this is that same value used as a slice argument). `suggested-features.md` §5.3
   named this explicitly as "once slices land" — `Array.slice(start, len)` has since landed
   (Track 05), so the array side of "slices" exists; the string side (a range-taking
   overload rather than the two-int form) is what's still open.
3. **`Array.orderBy` / `thenBy`** — a named, chainable stable multi-key sort
   (`arr.orderBy(k => k.lastName).thenBy(k => k.firstName)`), the LINQ-shaped ergonomic
   layer over the already-landed `sort`/`sortBy` (Resolver.cpp; both stable per
   `techdesign-05-stdlib-collections.md`). A code comment beside the existing `sortBy`
   implementation already anticipates this ("a contract `orderBy` (later)") but no design
   doc exists — this ticket is that doc.

## Requested Specific Feature

```
int    indexOfAny(Array<string> needles)         // -1 if none found
string subStr(Range r)                            // overload of subStr(start, len)
Array<T> orderBy<K>((T) => K key)                 // first sort key, ascending
Array<T> thenBy<K>((T) => K key)                   // chained on OrderedArray<T>-shaped result
```

`orderBy`/`thenBy` need a decision on representation: either `orderBy` returns a thin
wrapper type that `thenBy` composes on top of (so `arr.sortBy(k)` and `arr.orderBy(k)` stay
distinguishable at the type level, and `thenBy` cannot be called standalone), or `orderBy`
is defined as a `sortBy` alias and `thenBy` takes an already-sorted array plus a tie-break
predicate applied only within equal-key runs of the primary key (the actually-nontrivial
part — a naive second `sortBy` call would re-sort the whole array and break stability
across the first key).

## Known Warnings

- `thenBy`'s correctness hinges on "stable, tie-break-only-within-runs" semantics, not
  "sort again" — a naive implementation is an easy-to-miss correctness bug (looks right on
  small/random test data, breaks on inputs with many equal primary keys).
- `indexOfAny` should have a defined answer for the empty-needles-array case (recommend:
  -1, consistent with `indexOf` on a not-found substring).

## Acceptance Criteria

1. All three methods present with the signatures above (or an owner-approved variant),
   corpus-covered on every active engine.
2. `thenBy` demonstrably stable within equal-primary-key runs (a corpus case with several
   equal keys, not just monotonically distinct ones).
3. `docs/reference.md` §6.1 (string) and the Array section gain entries for all three.

## Interim Fallback

`indexOfAny` via a manual loop over `indexOf` per candidate substring; ranged `subStr` via
the existing `subStr(start, len)` two-int form; multi-key sort via a single `sortBy` with a
composite comparator key (e.g. a tuple-shaped string/struct combining both fields). No code
is blocked waiting on this.
