# Research: `csv::rows(File)` — a `Seq`-producing CSV reader

**Purpose:** source material for the tech design answering
`designs/requests/request-csv-reader.md`. This document does not decide anything — it
inventories every existing mechanism the feature would be built from, with file:line
citations and code snippets, and flags the couple of places where an implementation choice
has to be made explicit. **Bottom line: every acceptance criterion in the request is
buildable today over existing prelude primitives — no compiler or runtime change is
required.** There is no blocked-feature section because nothing is blocked.

Companion reading: `designs/requests/request-csv-reader.md` (the request itself),
`docs/reference.md` §6.4.8–9 (iterator protocol / `Seq<T>`), §6.6.6 (`File`), §6.11–6.13
(JSON / DateTime / Encoding & digests — the Track 09 siblings this feature joins),
`designs/complete/techdesign-07-iteration.md` (the `Seq<T>` design this reader is built on).

---

## 1. The iterator/`Seq<T>` infrastructure this reader plugs into

Two prelude interfaces make a type usable in `for..in` and in the lazy pipeline
(`prelude/core.lev:440-446`):

```
interface IIterator<T> {
    bool hasNext();
    T next();      // past-the-end is unspecified; stdlib iterators throw (loud)
}
interface IIterable<T> {
    IIterator<T> iterator();
}
```

`Seq<T>` is a plain library class over that protocol, not a compiler-special type
(`prelude/core.lev:945-1000`):

```
class Seq<T> : IIterable<T> {
    IIterator<T> iterator() { throw RuntimeException("Seq is abstract: a subclass provides iterator()"); }
    Seq<U> map<U>((T) => U fn) => MapSeq(this, fn);
    Seq<T> where((T) => bool pred) => FilterSeq(this, pred);
    Seq<T> take(int n) => TakeSeq(this, n);
    Seq<T> takeWhile((T) => bool pred) => TakeWhileSeq(this, pred);
    Seq<T> skip(int n) => SkipSeq(this, n);
    Array<T> toArray() { ... }       // drives it.hasNext()/it.next() through the INTERFACE
    T? firstOrNone() { ... }         // pulls exactly one element — the lazy payoff
    int count() { ... }
    void forEach((T) => void fn) { ... }
    A reduce<A>(A seed, (A, T) => A fn) { ... }
}
```

Every existing concrete `Seq` (`ArraySeq`, `MapSeq`, `TakeSeq`, `StreamSeq`, …) is the same
two-class shape: an `IIterator<T>` holding real mutable cursor state, and a thin `Seq<T>`
subclass whose `iterator()` builds one. `csv::rows` is a new instance of exactly this shape,
not a new mechanism — a `CsvSeq : Seq<Array<string>>` wrapping a `File`, whose
`CsvIterator : IIterator<Array<string>>` drives `File.readln()`:

```
class ArraySeq<T> : Seq<T> {
    Array<T> items;
    new ArraySeq(Array<T> a) { items = a; }
    IIterator<T> iterator() => ArrayIterator(items);
}
```

The closest existing precedent for "a `Seq` over a live external resource, not an in-memory
array" is `StreamSeq<T>` (`prelude/rest.lev:210-219`), built over `InStream<T>`:

```
class StreamSeq<T> : Seq<T> {
    InStream<T> source;
    new StreamSeq(InStream<T> src) { source = src; }
    IIterator<T> iterator() => source.iterator();
}
```

`InStream<T>` itself is `IDisposable, IIterable<T>` (`prelude/rest.lev:158`) — the caller
owns and closes the resource (`using`), the `Seq` wrapper just borrows it. `csv::rows(File)`
should follow the same ownership shape: the caller opens and `using`s the `File`; `CsvSeq`
holds a reference to it but is not itself `IDisposable` and does not close it. Pulling a
`CsvSeq`'s terminal after the backing `File` has been closed is caveat-emptor, same as
pulling a `StreamSeq` after its `InStream` closes.

**No new compiler surface needed:** dispatch through `IIterable`/`IIterator` is ordinary
interface `CallDyn` — "protocol uniformity... costs zero checker/Eval/Lower/backend work"
(`docs/reference.md:1478-1494`, and restated for `InStream` at `prelude/rest.lev:154-157`).

## 2. `File` — what's available to build the reader over

`prelude/std.lev:1913-1953`:

```
class File : IDisposable {
    string path;
    OpenMode mode = OpenMode(0);
    int fd = 0 - 1;
    bool opened = false;
    new File(string p, OpenMode m) { path = p; mode = m; open(); }
    void open() { ... }
    void close() { ... }
    bool isOpen() => opened;
    bool exists() => std::fileExists(path);
    int size() => std::fileSize(path);
    int modified() => std::fileModified(path);
    FileInStream reader() => FileInStream(fd);
    FileOutStream writer() => FileOutStream(fd);
    void write(string s) { std::sysWrite(fd, s); }
    void writeln(string s) { std::sysWrite(fd, s + "\n"); }
    string readln() => std::sysReadLine(fd);
    string read(int max) => std::sysRead(fd, max);
}
```

`readln()` is the one primitive this whole feature is built on: it returns one physical line
at a time and `""` at end-of-input (`docs/reference.md:2104`). Critically, it does this
**without materializing the rest of the file** — each call is one native `sysReadLine`
syscall loop, not a buffered read of the whole stream. That is exactly acceptance criterion
1 ("does not read the whole file before the first `Seq` pull returns"): a `CsvIterator`
whose `next()` calls `file.readln()` on demand, driven by `Seq`'s pull-based terminals,
satisfies it directly — no design work needed, just don't accidentally call
`string.splitLines()` on `file.read(file.size())` (which *would* defeat laziness).

**Finding — `readln()` does not strip `\r`.** The native implementation
(`src/runtime/RuntimeNatives.cpp:1350-1361`):

```cpp
if (name == "sysReadLine") {
    long long fd = args.empty() ? 0 : args[0].i;
    std::string line;
    if (fd == 0) {
        if (!std::getline(std::cin, line)) line = "";
    } else {
        char c;
        while (::read((int)fd, &c, 1) == 1 && c != '\n') line.push_back(c);
    }
    out = vstr(line);                             // "" = end of input (interim)
    return true;
}
```

splits **only** on `\n`. A CRLF file leaves a trailing `\r` on every line `readln()` returns.
The request's "Known Warnings" section says line-ending handling should reuse whatever
`string.splitLines()` already settled (Track 04) rather than re-deciding it — but
`splitLines()` operates on an in-memory string it already has in full
(`prelude/core.lev:321-331`):

```
// Splits on "\n", then trims exactly one trailing "\r" per line (handles
// both CRLF and bare LF without disturbing intentional trailing spaces).
Array<string> splitLines() {
    Array<string> lines = split("\n");
    Array<string> r = [];
    for (string line in lines) {
        if (line.endsWith("\r")) r = r.add(line.subStr(0, line.length() - 1));
        else r = r.add(line);
    }
    return r;
}
```

Since a streaming reader can't call `splitLines()` (that requires the whole file), the CSV
reader needs to replicate its **exact** per-line rule (`endsWith("\r")` → drop one trailing
byte) on each physical line `readln()` returns, rather than inventing a different CRLF
convention. This is a one-line adaptation, not new design — just don't skip it, since the
request explicitly calls out reusing the settled convention.

## 3. The RFC 4180 quoting/escaping decision — both options are buildable now

The request's one open design decision: naive `line.split(",")` (fast, wrong on quoted
fields) vs. a real state machine (correct, more work). Research finding: **the "more work"
option requires no new primitives** — it composes entirely from what's in §1/§2 plus
`StringBuilder` and byte-level string access, and there's a direct in-tree precedent for
exactly this shape of hand-rolled scanner: `JsonParser` (`prelude/web.lev:757-887`).

`JsonParser` scans a string byte-by-byte via `s.byteAt(i)` (not `char`-decoding — ASCII
structural bytes like `"`, `,`, `{` can never appear as a UTF-8 continuation byte, so
byte-scanning is UTF-8-safe for delimiter/quote detection the same way `s.byteAt` is used
for JSON's structural characters), and accumulates field content into a `StringBuilder`:

```
string parseRawString() {
    i = i + 1;
    StringBuilder sb = StringBuilder();
    while (i < s.length()) {
        int c = s.byteAt(i);
        if (c == 34) { i = i + 1; return sb.toString(); }
        if (c == 92) { ... escape handling ... }
        else { sb.add(std::byteToString(c)); i = i + 1; }
    }
    this.fail();
    return "";
}
```

The CSV analogue differs from JSON in one structural way: JSON has the whole string
in memory (`s.byteAt` random-accesses it), but a streaming CSV reader only has one
`readln()`-returned physical line at a time, and a quoted field can legitimately contain an
embedded newline (spanning multiple physical lines for one logical CSV row). The standard
trick that stays entirely within `readln()` (no need for raw byte/`Block` access): **track
whether the accumulated logical line has a balanced quote count.** In a well-formed RFC 4180
record, every `"` byte is either an opening quote, a closing quote, or one half of an escaped
`""` pair inside an already-open quote — the total count of `"` bytes in a complete logical
record is always even. So:

```
string logical = file.readln();
while (logical.count("\"") % 2 == 1) {          // string.count already exists, core.lev:336-345
    string more = file.readln();
    if (more == "" ) { /* malformed: unterminated quote at EOF */ break; }
    logical = logical + "\n" + more;
}
```

then run a single-pass byte scanner over `logical` (the JsonParser shape above) tracking
`inQuotes`, splitting on the delimiter byte only when `!inQuotes`, and un-escaping `""` → `"`
inside quoted fields. This is genuinely a state machine (not `line.split(",")`), fully
correct per RFC 4180, and costs nothing beyond `readln()` + `string.count` (already exists)
+ `StringBuilder` (already exists) + `byteAt` (already exists). **Recommendation for the
tech design: there is no feasibility reason to pick the naive option** — the "correct" path
is roughly the same amount of code as a careful naive splitter, and the acceptance criteria
(§2 of the request) explicitly requires a quoted-field-with-embedded-newline corpus case, so
the design should just target RFC 4180 directly rather than staging a naive v1 first.

## 4. `char delimiter` overload — no engine blocker

The request's second signature is `csv::rows(File f, char delimiter)`. `char` is a full
value primitive with `code()` (`prelude/core.lev:73-79`, `int code()` native) — the right
way to compare a delimiter char against a scanned byte (`code() == 44` for comma), per the
documented "no arithmetic on `char`, use `code()`" rule (`docs/reference.md:174-178`).

**Finding, checked because it looked like a possible landmine:** a stray comment in
`prelude/web.lev`'s `json::utf8` helper reads *"done by hand over `std::byteToString` (not
`char.toString()`: `char` is LLVM-excluded, Track 03)"* — if still true, a `char`-typed
public parameter would silently lose LLVM coverage. It is **stale**: `docs/reference.md:178`
and `info.md` both confirm `char` shipped on LLVM as of 2026-07-10 (`LV_CHAR` ABI addendum,
`designs/deferal-char-block-abi.md`) and reference.md explicitly lists `char` as landing on
"the oracle, IR, emit-C++, **and LLVM** engines." So `char delimiter` is safe on every active
engine — but the stale comment is worth a one-line fix (or at least a gotchas.md entry) while
touching this area, since it will mislead the next person who greps for "char... LLVM."

## 5. Where this lives: Track 09's sibling namespaces are the placement precedent

`json`, `datetime`, `digest`, and `encoding` — the other "self-contained data-format engine"
namespaces (Track 09 F1/F2/F3) — all live in one file, `prelude/web.lev`
(`namespace encoding` at line 8, `namespace datetime` at 190, `namespace digest` at 427,
`namespace json` at 925), and are documented as siblings in `docs/reference.md` §6.11–6.13
(JSON / DateTime & Duration / Encoding & digests), each a short subsection with a fenced
code block showing the surface. `csv` is the same shape of thing (a self-contained,
boot-checked data-format engine, request's own framing) and the request's acceptance
criterion 3 explicitly asks for `docs/reference.md` documentation "alongside
`json`/`digest`/`encoding`" — so the natural placement is a new `namespace csv { ... }` block
appended to `prelude/web.lev`, with a new `## 6.13b CSV (namespace csv)`-shaped subsection in
`docs/reference.md` immediately after §6.13.

This requires **no build-system change**: `web.lev` is already one of the compiled-in
prelude segments (`CMakeLists.txt:33`, `set(LV_PRELUDE_SEGMENTS core std rest regex_core
regex_api web wasm expr)`), and namespaces are declaration-based, not file-based
(`info.md:1183-1187`) — `csv::rows(...)` resolves via `::`-qualification with no `uses`
needed, same as `json::parse(...)` today (confirmed: `tests/corpus/json/json.lev` calls
`json::parse` with no `uses json;` anywhere in the file). The alternative — a new
`prelude/csv.lev` file — is also viable but requires adding `csv` to
`LV_PRELUDE_SEGMENTS` in `CMakeLists.txt:33` for it to be picked up by
`GenPreludeEmbedded.cmake`; there's no technical reason to prefer it over appending to
`web.lev`, only a file-size/organization judgment call for the tech design to make (`web.lev`
is already ~44K).

(Aside, not load-bearing: the stdlib-ship-as-files decision — `.lev` source files, not
per-target in-binary segments, `info.md §19#18`, shipped 2026-07-19 — governs how the
*already-compiled-in* prelude gets distributed to a running program, not whether a new
namespace goes in an existing segment file or a new one. It doesn't bear on this choice.)

## 6. Testing convention — a precedent tension worth flagging explicitly

`docs/policies.md` § Testing says libraries "including the standard library" need Sonar unit
tests, with ctest reserved for "the Language code itself." But the direct siblings this
feature is joining — JSON, DateTime, Encoding, Digest — are **all** tested via ctest corpus
files, not Sonar, despite living in the prelude and being "standard library" in every
practical sense:

```
tests/corpus/json/json.lev + json.expected
tests/corpus/digest/digest.lev + digest.expected
tests/corpus/encoding/encoding.lev + encoding.expected
```

(e.g. `tests/corpus/json/json.lev:1-5`: *"Track 09 F1 (M4): JSON — parse/render
round-trips... LLVM lane excluded: bug.md #30... runs on --run/--ir/emit-C++."*) These are
plain `.lev` scripts whose stdout is diffed against a `.expected` file across the active
engines — the same corpus mechanism the compiler's own conformance suite uses, not a Sonar
unit test. Practically, this makes sense for prelude-native features: they need
cross-**engine** agreement (oracle/IR/LLVM must agree, `docs/policies.md` § Testing) which is
exactly what the corpus harness checks and Sonar doesn't. **Recommendation: follow the
direct precedent** — `tests/corpus/csv/csv.lev` + `csv.expected`, matching
`json`/`digest`/`encoding`'s folder shape — rather than the more generic policy line, and
have the tech design say so explicitly rather than silently picking one, since it's a real
discrepancy between the written policy and the actual precedent for this exact class of
feature.

Two corpus patterns to reuse directly:

- **File fixtures inline, not on-disk test data** — `tests/corpus/di_capabilities.lev:29-40`
  writes a temp file, then reads it back in the same test function, all in `/tmp`:
  ```
  void roundTripFile(IFileSystem fs) {
      string path = "/tmp/di_capabilities_test.tmp";
      File w = fs.open(path, std::write);
      w.write("hello capability");
      w.close();
      File r = fs.open(path, std::read);
      console.writeln("content: " + r.read(64));
      r.close();
  }
  ```
  A CSV corpus test should do the same: write a CSV fixture (including the quoted/embedded-
  newline row acceptance criterion 2 asks for) to a temp path, then `using (File f = ...)
  csv::rows(f)...` it back.

- **Proving laziness with a call-count witness** — `tests/corpus/seq.lev`'s whole design is
  proving the laziness contract via a side-effect `Counter` (`tests/corpus/seq.lev:8-15,
  20-23`) rather than timing. For criterion 1 ("does not read the whole file before the
  first pull"), the file-based analogue: after `csv::rows(f).firstOrNone()` (or
  `.take(1).toArray()`) on a multi-row fixture, call `f.readln()` directly and assert it
  returns the **second** data row's raw text — proving the `Seq` pull consumed exactly one
  logical row's worth of physical lines from the file, not the rest of it.

## 7. A gotcha that applies if any internal helper gets overloaded

`docs/gotchas.md:10,13`: *"Overloaded functions called from an UNCHECKED prelude body...
resolve by ARITY only, not by argument type... Keep same-named prelude overload families
arity-unique."* The two public signatures the request asks for are already arity-unique (1
vs. 2 params: `rows(File)` / `rows(File, char)`), so this is not a problem for the public
surface. It matters only if the implementation introduces an internal helper family (e.g. a
private field-splitter) with multiple same-arity overloads distinguished solely by argument
type — keep any such helpers arity-distinct, per the existing landmine.

## 8. Complexity rating

Per `docs/policies.md` § Tech Designs: *"Any code designs that work within a Library,
including the Standard Library... mark the design complexity as sonnet."* This feature is
pure `.lev` prelude code (new classes + a namespace in `prelude/web.lev`, or a new prelude
segment file) plus a `docs/reference.md` subsection and a ctest corpus fixture — no compiler,
checker, IR, or backend change anywhere. **Recommendation: `techdesign-csv-reader-sonnet.md`.**

## 9. Summary — nothing is blocked

| Acceptance criterion (request) | Buildable today? | Basis |
|---|---|---|
| 1. Streams lazily, no full-file read before first pull | Yes | `File.readln()` is already one-line-at-a-time; `Seq`/`IIterator` pull model (§1, §2) |
| 2. Quoted field w/ delimiter and/or embedded newline, RFC 4180 scope stated & matched by corpus | Yes | Quote-parity line-joining + byte-scan state machine, entirely over existing `readln`/`byteAt`/`StringBuilder`/`string.count` (§3) |
| 3. `docs/reference.md` documents it alongside `json`/`digest`/`encoding` | Yes | Same file/section pattern already established (§5) |
| `char delimiter` overload | Yes | `char.code()`, full engine coverage confirmed (§4) |

No requested feature is blocked by missing compiler/runtime surface. The only two things the
tech design needs to decide explicitly (not because they're infeasible, but because they're
judgment calls) are: (a) new namespace in `prelude/web.lev` vs. a new `prelude/csv.lev`
segment (§5), and (b) ctest corpus vs. Sonar for the test suite, where this document
recommends following the direct `json`/`digest`/`encoding` precedent (§6) over the more
generic policy wording.
