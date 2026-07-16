# Summary: `csv::rows(File)` — a `Seq`-producing CSV reader

There is no CSV support anywhere in the stdlib. `designs/suggested-features.md` §13
proposed `csv::rows(File)` as a streaming, `Seq`-producing source, sequenced explicitly
*after* the lazy iterator work (§9) it depends on. That dependency — `Seq<T>` over the
iterator protocol — landed in full via Track 07 (info.md §11: "`Seq<T>` is the opt-in lazy
form," `docs/reference.md` §6.4.8–9). CSV itself was never picked up. This ticket carries
the ask forward now that its source document is being retired.

## Request Details

CSV is the bulk-data thrust's (info.md §13, `suggested-features.md` §13) most common
real-world ingestion format, and it is exactly the shape the language's lazy-sequence story
was built for: a large file that should stream through a pipeline (`map`/`where`/take, etc.)
rather than fully materialize as an `Array` before processing starts. Landing it now is
mechanical library work over infrastructure that already exists — `Seq<T>`, the `IIterable`/
`IIterator` protocol, `File` (`using`/`IDisposable`-conforming, info.md §19#8), and `string`
methods (`split`, already landed) are all sufficient to build a naive row-splitting reader
without any new compiler or runtime surface.

The one design decision worth making explicit before implementation: **quoting/escaping
correctness**. A reader that just calls `line.split(",")` handles the common case but is
wrong the moment a field contains a comma, a quote, or an embedded newline (RFC 4180
territory) — worth deciding up front whether v1 targets naive delimiter-split (fast, wrong
on quoted fields) or a real RFC 4180 state machine (correct, more work), rather than
discovering the gap after code depends on the naive behavior.

## Requested Specific Feature

```
Seq<Array<string>> csv::rows(File f);                 // each row as raw string fields
Seq<Array<string>> csv::rows(File f, char delimiter);  // e.g. TSV via '\t'
```

A row-and-column-agnostic reader (`Array<string>` per row) rather than a typed/schema'd
one — header-row-to-struct mapping, if wanted, is a layer on top (`rows().skip(1).map(...)`
already composes today) and shouldn't block v1.

## Known Warnings

- Decide RFC 4180 quoting/escaping scope up front (see above) — this is the one place a
  "just split on commas" implementation looks done but silently corrupts data on real-world
  input (quoted fields containing the delimiter or embedded newlines spanning physical
  lines).
- Line-ending handling (`\r\n` vs `\n`) should reuse whatever `string.splitLines()` already
  settled (Track 04, landed) rather than re-deciding it.

## Acceptance Criteria

1. `csv::rows(File)` streams rows lazily (verified: does not read the whole file before the
   first `Seq` pull returns).
2. A corpus case with a quoted field containing the delimiter and/or an embedded newline,
   with the RFC 4180 scope decision (naive vs. correct) explicitly stated in the design and
   matched by the corpus.
3. `docs/reference.md` documents the surface alongside `json`/`digest`/`encoding` (Track 09).

## Interim Fallback

Manual `File` read + `splitLines()` + per-line `split(",")`, accepting the quoted-field
limitation, wherever CSV ingestion is needed today. No code is blocked waiting on this.
