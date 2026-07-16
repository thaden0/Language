# Summary: Columnar/dense `Array<struct>` layout — the flat-memory data-processing story

`Array<T>` for a value-typed `struct` element is documented as dense, flat-memory storage
("no per-element boxing or pointer chase," info.md §9), and both native backends have
landed dense value-struct arrays as part of the memory-management work (§15: "dense
value-struct arrays" are explicitly under the ARC/COW discipline, differential-corpus
covered). What has **not** landed is the columnar transformation this was building toward —
info.md §17 names it explicitly as "the next major work" ("`Array<T>` elements generally...
the dense/columnar layout beyond arrays of plain structs is the next major work"), and
`designs/suggested-features.md` §13 named it as one of the bulk-data thrust's core bets
("Columnar/dense `Array<struct>`... the language's genuinely differentiating data
feature"). No design document exists for it yet. This ticket exists so that gap is tracked
now that the source document naming it is being retired.

## Request Details

The distinction that matters: today's "dense `Array<struct>`" is **row-major** — each
element is a flat, contiguous `struct` record, laid out one after another, which already
buys cache-friendly iteration and zero per-element boxing. **Columnar** storage is a
different layout entirely: each *field* of the struct gets its own contiguous array
(`Array<Point>` becomes, physically, one array of every `x` followed by one array of every
`y`, not an array of `{x,y}` pairs). Columnar is what real analytical/bulk-data workloads
want — `sum(points.map(p => p.x))` on a columnar layout touches only the `x` column's
memory, not the whole struct; the `struct` + dense-array + `mmap` (via `Block`) stack that
info.md §13/§17 describes as the differentiating feature is specifically betting on this.

This is a substantial compiler/runtime design, not a stdlib addition — it needs an answer
for at minimum: how a columnar `Array<struct>` is represented at the ABI level (per-backend,
across oracle/IR/emit-C++/LLVM — no ELF lane, X64Gen frozen per the portage pivot); how
field access (`arr[i].x`) compiles when the fields aren't adjacent in memory; how mutation
interacts with the existing COW-on-refcount story (info.md §11/§15) when a single mutation
now touches one column array instead of one struct's storage; and whether this is the
default `Array<struct>` representation (a behavior change for existing dense-array code) or
an opt-in alternate collection type that coexists with today's row-major dense arrays.

## Requested Specific Feature

Not prescribing an implementation — this is a request for the design work to begin, not a
spec. The open questions above (ABI representation, field-access codegen, COW interaction,
default-vs-opt-in) are exactly what a tech design for this track needs to resolve.

## Known Warnings

- This is large enough that it likely wants its own track number and STOP-and-escalate
  checkpoints (per `[[feedback_techdesign-conventions]]`), not a single-pass design.
- Interacts directly with the ARC/COW machinery that took significant work to get to
  "13/13 guarded programs green at +0 bytes" (info.md §15) — any new storage layout for
  `Array<struct>` needs its own churn-corpus coverage before it's trusted, not an assumption
  that the existing row-major discipline transfers unchanged.
- X64Gen/ELF is frozen (info.md §0/§17) — this work targets oracle/IR/emit-C++/LLVM only,
  same as every other post-pivot feature.

## Acceptance Criteria

1. A tech design document proposing the ABI representation, field-access codegen strategy,
   and COW/mutation story, reviewed before implementation starts.
2. A decision on default-vs-opt-in (does `Array<struct>` become columnar automatically, or
   is it a distinct type/annotation).
3. If implemented: a benchmark demonstrating the cache-locality win on a column-selective
   aggregate (e.g. `sum` over one field of a large struct array) versus today's row-major
   dense array, plus full churn-corpus coverage on every active engine.

## Interim Fallback

Row-major dense `Array<struct>` (already landed) for general use; hand-rolled
struct-of-arrays (separate `Array<int>`/`Array<float>`/etc. per field, manually kept in
sync by index) where columnar access patterns are needed today. No code is blocked waiting
on this — it is future-facing infrastructure work, not an unblocking dependency.
