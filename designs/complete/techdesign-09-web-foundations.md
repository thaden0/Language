# Track 09 — Web Foundations: JSON, DateTime, Encoding & Digests, HTTP Hardening

**Status:** ready. **Date:** 2026-07-05. **Depends on:** Track 01 (shifts/xor/hex —
digests), Track 04 (`byteAt`/`fromByte`, split/toolkit — everything here),
Track 08 M3 (`sysNow` — DateTime); Track 03 `Block` improves digests/binary
bodies but is NOT blocking (mask-idiom over ints + byteAt suffices, contract C7).
**Source:** suggested-features.md §12 (all), §13#5 partially; proposal-web-framework.md
is the *consumer* of this track — read it for shape, not scope.
**Owns:** new prelude regions only (in-language): `namespace json`, `DateTime`/
`Duration`, `namespace encoding`, `namespace digest`, HTTP classes
(HttpRequest/HttpResponse/HttpServer/HttpClient rewrite + new HeaderMap) —
Resolver.cpp:330–520 region. **Zero new natives, zero compiler changes** — this
entire track is language code; every engine gets it via lowering. That is the
design's central claim and its main de-risking.

**Prelude-scale decision (made now):** kPrelude is one string and this track
roughly doubles it. Before writing feature code, split kPrelude into several
adjacent raw-string segments concatenated at gather (`kPreludeCore`,
`kPreludeStd`, `kPreludeNet`, `kPreludeWeb`, ...) — a mechanical, zero-semantic
refactor (probe: corpus byte-identical), committed alone. Moving the stdlib to
shipped `.ext` files is the *better* long-term answer but changes the compiler's
distribution story → explicitly out of scope; flagged on the roadmap for an
owner ruling.

---

## 1. F1 — JSON (`namespace json`)

### 1.1 The value model (the one design decision that matters)

There are no type aliases, and a recursive named union (`JsonValue = ... |
Array<JsonValue>`) cannot be declared. **v1: a class.**

```
class JsonValue {
    int kind;                          // 0 null, 1 bool, 2 number, 3 string, 4 array, 5 object
    bool b; float num; string str;
    Array<JsonValue> items;            // arrays
    Map<string, JsonValue> fields;     // objects (insertion-ordered — round-trip friendly)

    bool isNull(); bool isBool(); ... bool isObject();
    bool? asBool(); float? asNum(); string? asStr();        // None on kind mismatch
    Array<JsonValue>? asArray(); Map<string, JsonValue>? asObject();
    JsonValue at(int i);               // array index; throws on kind/bounds (loud)
    JsonValue at(string key);          // object field; throws on missing (Map.at rule)
    JsonValue? atOrNone(string key);
    string render();                   // compact; renderPretty(int indent) too
    static ctors: JsonValue::ofNull(), ofBool(b), ofNum(f), ofStr(s), ofArray(a), ofObject(m)
}
namespace json {
    JsonValue? parse(string s);        // None on malformed (v1; see problem #1)
    string render(JsonValue v) => v.render();
}
```

`kind` as int (not enum) **until Track 03 lands**, then mechanically upgraded to
`enum JsonKind` — noted cross-track rider, non-blocking either direction.

### 1.2 Parser

Recursive-descent over the string with an index cursor (class `JsonParser`
holding `s`, `i` — the incremental-parse shape lcurl's chunked decoder proved).
Numbers: parse into `float` (doc: JSON numbers are IEEE doubles in v1;
big-int-preserving mode is a roadmap note). Strings: full escape set incl.
`\uXXXX` → UTF-8 encode (in-language: hex-parse the 4 digits — Track 04
`byteAt`/Track 01 hex make this clean; surrogate pairs handled: a
`\uD800-\uDBFF` must pair with a following `\uDC00-\uDFFF`, else replacement
char per the Track-03 invalid-data rule). Depth cap (128) → parse failure, not
stack death. Trailing garbage after the value → failure (strict).

**No `try` needed internally:** the parser returns `JsonValue?` propagating
`None` upward (unions doing Result work — the language's own §12.6 stance for
expected outcomes).

### 1.3 render

Compact by default: escape `" \\ \n \r \t` + control bytes as `\u00XX`;
non-ASCII bytes pass through raw (UTF-8 in, UTF-8 out). Floats: integral values
render without `.0`? **Decide: render what `toString()` gives, pinned by corpus**
— revisit float formatting globally later (Track 06 P4 pinned the shape); JSON
consumers tolerate both.

Typed `@Serializable` derive via the rules engine (§16.5) is the flagship
**follow-up**, not v1 — recorded on the roadmap.

## 2. F2 — DateTime & Duration

```
struct Duration { int ms; }           // + statics ofSeconds/ofMinutes/...; plus/minus; toString "1h02m03s"
struct DateTime {
    int epochMs;                       // UTC only in v1 (documented)
    static DateTime now();             // sysNow (contract C6)
    static DateTime ofEpochMs(int);
    int year(); int month(); int day(); int hour(); int minute(); int second(); int weekday();
    DateTime plus(Duration d); Duration minus(DateTime o);
    string httpDate();                 // "Sun, 06 Nov 1994 08:49:37 GMT" (RFC 7231 IMF-fixdate)
    static DateTime? parseHttpDate(string s);   // IMF-fixdate required; obsolete forms -> None v1
    string iso8601();                  // "2026-07-05T12:00:00Z" (second precision; ms if nonzero)
    static DateTime? parseIso8601(string s);    // Z or ±hh:mm offset accepted, normalized to UTC
}
```

Civil-time conversion: the standard days-from-civil / civil-from-days integer
algorithms (Howard Hinnant's public-domain formulation) — pure int64 arithmetic,
in-language, no tables. Leap seconds ignored (Unix convention). Both structs are
value types (data, no identity — exactly what struct is for). Weekday from epoch
days mod 7 (anchor: 1970-01-01 = Thursday = 4).

## 3. F3 — Encoding & digests

### 3.1 `namespace encoding` (generalizing lcurl's ascii.ext, now byte-true)

- `string base64Encode(string bytes)` / `string? base64Decode(string b64)` —
  over `byteAt`/`fromByte` (arbitrary bytes now — the printable-ASCII limit
  dies); StringBuilder accumulates. Padded, standard alphabet; a `base64Url`
  pair too (JWT-adjacent, 4 lines).
- `string percentEncode(string s)` / `string? percentDecode(string s)` —
  RFC 3986 unreserved passthrough; encode UTF-8 bytes as `%XX` uppercase.
- `string hexEncode(string bytes)` / `string? hexDecode(string s)`.
- Decoders return `None` on malformed input (data errors are values).

### 3.2 `namespace digest`

`string md5(string bytes)`, `sha1`, `sha256`, `string hmacSha256(string key,
string msg)` — returning **raw bytes** (composable: `encoding::hexEncode(
digest::sha256(x))`).

In-language implementation notes (the C7 idiom, spelled out for the implementer):
- All words held in int64, **masked to 32 bits after every add/xor/or**
  (`& 0xFFFFFFFF` — hex literals from Track 01).
- rotl32(x, n) = `((x << n) | (x >> (32 - n))) & 0xFFFFFFFF` — safe because x is
  pre-masked non-negative, so `>>` (arithmetic) equals logical here. This
  invariant is THE correctness linchpin: **never right-shift an unmasked value.**
- Message scheduling over `byteAt`; length in bits fits int64.
- Verification: RFC 1321 (md5), RFC 3174 (sha1), NIST FIPS 180-4 + RFC 4231
  (hmac) test vectors as the corpus expected-values — not hand-derived.

Perf note: in-language digests will be slow-ish on interpreters (fine for
cookies/etags; a native fast path is a *later* opt-in — log measurements at M3;
ELF-compiled speed should be respectable already).

## 4. F4 — HTTP hardening

Rewrites the demo-grade prelude HTTP (Resolver.cpp:330–520; curl-design §2.2's
indictment) into framework-grade pieces. All in-language; lcurl's transfer
engine is the proven source material — **port its logic, don't reinvent**
(chunked decoder §5.6, sendAll §5.5, header rules §5.4).

1. **`HeaderMap`** — ordered multimap, case-insensitive lookup:
   `add(name, value)`, `set(name, value)` (replace-all-of-name),
   `string? first(name)`, `Array<string> all(name)`, `remove(name)`,
   `Array<Header> entries()` (order + duplicates preserved — Set-Cookie).
   Backed by `Array<Header>` + `toLower` compares (Header struct from lcurl).
2. **`HttpRequest`/`HttpResponse`** re-typed over HeaderMap; both grow
   incremental `parse(feed)` state machines (status/request line → headers →
   body by mode) lifted from transfer.ext; bodies stream via callbacks
   (`onBodyChunk`) with a buffered-string convenience for small bodies.
3. **Chunked both directions:** decoder ported from lcurl (fragmentation-proof,
   already corpus-hardened); encoder trivial (`size.toHex()\r\n` + chunk +
   trailer).
4. **Keep-alive v1 (server side):** after a response with neither side sending
   `Connection: close`, reset the per-connection parser and re-arm — bounded by
   `maxRequestsPerConn = 100` + an idle timer (`std::after`) honoring the §2.3
   lifetime invariant (every path unwatches/closes: the corpus asserts loop
   exit). Pipelining: buffered-but-serial (parse next request only after the
   response completes — correct, simple).
5. **`HttpClient` (real):** `request(method, url, HeaderMap, body,
   (HttpResponse) => void)` + `get/post` sugar; redirects (lcurl's rewrite
   rules), `Duration` timeout (max-time shape), Content-Length/chunked
   handling; connect via Track 08's `connectTimeout` when present. Client
   keep-alive/pooling: **deferred** (framework-era; one-connection-per-request
   is honest v1).
6. **HttpServer** rides HeaderMap + keep-alive + a 500-path: an uncaught
   exception in a handler → `500`, `Connection: close`, loop survives (the
   per-callback try/catch pattern institutionalized — this is the seed of the
   framework's error-page story).

**Back-compat:** existing corpus (http.ext, sockets.ext, churn timer/http
programs) must stay green; old `HttpRequest.headers` was `Map<string,string>` —
the type changes to HeaderMap. Grep consumers first (P3); the corpus http
programs are few and in-tree; adjust them in the same commit (breaking prelude
surface is fine pre-1.0 when done deliberately + logged).

## 5. P-probes

- **P1:** prelude-split refactor probe (byte-identical corpus before feature
  work — §0 decision).
- **P2:** struct static methods (`DateTime::now()`) — statics on structs
  declaration+call shape (Track 03's P2 cousin; if statics-on-struct are
  awkward, `namespace time { DateTime now(); }` fallback, logged).
- **P3:** `grep -rn 'HttpRequest\|HttpClient\|\.headers' tests/ examples/` —
  consumer inventory for the F4 break.
- **P4:** deep-recursion headroom for JSON depth-128 on each engine (IR/ELF
  frame growth) — a 128-deep nested-array parse probe; lower the cap if any
  engine strains.
- **P5:** digest inner-loop timing spike (sha256 of 1KB × 100 on --ir) —
  expectation-setting number for the log.

## 6. Foreseeable problems & solution strategies

| # | problem | strategy |
|---|---|---|
| 1 | **`JsonValue?` loses error detail** (position/reason) — frameworks want "line 3: unexpected `,`". | v1 accepts it (contract: parse is total, failure is None). Add `json::parseVerbose -> JsonParseResult` (class w/ value?/error string/offset) as an M-stretch if time allows; else roadmap. Do NOT switch to exceptions for malformed data (expected-outcome rule, §12.6). |
| 2 | **Mutual recursion in prelude** (`JsonValue` ↔ `json::parse`; JsonValue methods returning JsonValue) — declaration-order sensitivity in the gather. | Whole-program resolution should make order irrelevant (§12); P1's split-refactor run plus one targeted probe (two mutually-referencing prelude classes) confirms. If order bites, it is a resolver bug → bug.md, with reordering as interim. |
| 3 | **`Map<string, JsonValue>` with class values** — Map values are references; JsonValue trees share nodes on copy (Map is pure but the VALUES alias). A user mutating a parsed tree node sees spooky sharing. | v1: JsonValue is **read-mostly**; document the aliasing honestly (objects are references — language rule, not a JSON quirk). Builders use the static ofX ctors. If it bites hard in practice, a `deepCopy()` method is 15 lines. |
| 4 | **Keep-alive lifetime leaks** — a watch/timer left armed = server never exits; the churn corpus's timer-cycle asterisk (§15 Timer→handler cycle) is adjacent: per-connection closures capturing the connection risk cycles. | Follow the corpus's own discipline (bind callbacks before constructing holders where cycles threaten — the §15 note documents the pattern); every connection teardown path through one `closeConn()`; an http_churn.ext program (N sequential keep-alive requests, then close) asserting +0B and clean loop exit. |
| 5 | **CRLF/whitespace edge divergence from real clients** (curl, browsers) once the framework serves them. | Fixture-based: port lcurl's test server approach; add a raw-bytes request corpus (folded headers rejected loud, bare-LF tolerated? decide: **tolerate bare LF, reject folds** — RFC 7230 guidance, pinned in tests). |
| 6 | **Digest wrongness that tests pass** (vectors green but an edge like length ≡ 55/56 mod 64 broken). | Vector sets deliberately include the padding boundary lengths (0, 55, 56, 63, 64, 65 bytes) — the classic trap set, encoded in the corpus. |
| 7 | **DateTime month/day arithmetic off-by-ones** (the perennial). | Hinnant algorithms verbatim + corpus spanning epoch, leap years (2000/1900/2024), month ends, and a round-trip property loop (10k random epochs: ofEpochMs(x).civil→epoch == x). |
| 8 | **Prelude doubling slows every compile** (gather+resolve cost on all tests). | Measure ctest wall-time before/after (log). If regression > ~20%, escalate the ship-stdlib-as-files question (already on the roadmap) rather than trimming features. |

## 7. Milestones & acceptance

| M | deliverable | accept |
|---|---|---|
| M0 | prelude split refactor (P1) | corpus byte-identical, committed solo |
| M1 | encoding namespace | RFC vectors corpus (base64 incl. padding trio, percent round-trips, hex) all engines |
| M2 | DateTime/Duration | date corpus (§6#7 set); httpDate/iso round-trips |
| M3 | digests | vector corpus (§6#6 lengths); P5 timing logged |
| M4 | JSON | json.ext (parse/render round-trip suite incl. \uXXXX + surrogates, deep nesting, malformed→None table); reference section |
| M5 | HeaderMap + parser hardening + chunked-both-ways | ported-decoder corpus at every fragmentation granularity (lcurl's method) |
| M6 | keep-alive + HttpClient + 500-path | loopback corpus: 3 requests one connection; handler-throw → 500 + survival; http_churn +0B; lcurl unregressed |

Target: **Aug 3 – Aug 21** (M0 1d, M1 1d, M2 2d, M3 2d, M4 3d, M5 3d, M6 4d).

## 8. Reference-doc duty

reference: new §6.11 (json), §6.12 (DateTime/Duration), §6.13
(encoding/digest), §6.6.57 rewrite (HTTP — HeaderMap, keep-alive, client);
info.md §13 HTTP paragraph refresh + §19 additions (parseVerbose, client
pooling, stdlib-as-files ruling wanted).

## 9. STOP conditions

- Recursion/aliasing probes (P2, #2, #3) reveal resolver/value-model behavior
  that makes the class-based JsonValue untenable → the type-alias language
  question must be ruled on (it is a language-surface decision, firmly above
  this track's pay grade).
- Keep-alive cannot satisfy the lifetime invariant without loop changes
  (RuntimeLoop edits belong to Track 08's owner — coordinate, don't trespass).
- Any native creep (this track's zero-native claim is the design).

## 10. Implementation log

**2026-07-09 — landed (agent3), all milestones M0–M6.** Zero new natives except the
documented Track 08 cross-track dependency `std::sysNow()` (wall-clock epoch ms), added
across the four active engines (oracle/IR via `nativeFreeCall`, emit-C++ `rt_sysnow`, LLVM
`lvrt_sysnow` over a new `lv_plat_now_realtime_ms` on posix + win32). Everything else is
in-language prelude code.

- **M0** — `kPrelude` split into `kPreludeCore/Std/Rest/Web` adjacent raw-string segments
  concatenated at gather; corpus byte-identical.
- **M1** `namespace encoding` — base64(+url)/percent/hex; RFC 4648 vectors + malformed→None.
- **M2** `DateTime`/`Duration` structs — Hinnant civil↔days; http/iso emit+parse round-trips;
  ~10k-sample bijection property. "static" split: infallible → labeled ctors, fallible parsers
  → `datetime::` free functions returning `DateTime?`.
- **M3** `namespace digest` — md5/sha1/sha256/hmacSha256, mask idiom, RFC/FIPS vectors incl. the
  55/56/63/64/65 padding trap set. P5: sha256 1KB×100 on `--ir` ≈ 2.4s.
- **M4** JSON — class `JsonValue` (int `kind`), failed-flag recursive-descent parser, `\uXXXX`
  + surrogate combining, depth cap 128, compact + pretty render.
- **M5/M6** HTTP — `Header`/`HeaderMap`, `ChunkedDecoder` (ported from lcurl) + encoder,
  incremental request/response parsers over HeaderMap, server keep-alive, 500 path, real
  `HttpClient`. Existing http.ext/async.ext stay green on all four engines incl. frozen ELF.

**Engine coverage:** encoding/datetime/digest — oracle+IR+emit-C+++LLVM (not ELF: post-freeze
`byteAt`/`byteToString`). JSON — oracle+IR+emit-C++ (not LLVM: **bug.md #30**). HTTP parse/
chunked (`http_parse`) — oracle+IR+LLVM (emit-C++ cleanly skips: coarser reachability pulls in
socket natives). Socket tests (`http_500`, `http_keepalive`) — oracle+IR.

**Findings recorded for successors:**
1. **bug.md #30 (P1)** — a `Map<K, recursive-class>` built in a class method is corrupted on
   LLVM (garbage length → segfault); the JSON object model triggers it, so JSON's LLVM lane is
   excluded pending the backend fix. Correct on the other three engines.
2. **Prelude may not rely on `T?`/union flow-narrowing** — the prelude is unchecked, so the
   static backends (LLVM) misread a narrowed union crossing a call boundary (an ISO parser using
   six narrowed `int?` locals returned `None` only on LLVM). Fix pattern: plain-value helpers
   with a sentinel (`datetime::parseUInt`, `HeaderMap.firstOr`, `std::bodyLenOf`) instead of
   `.toInt()`/`??` in prelude bodies. Memory: `leviathan-prelude-no-narrowing`.
3. **`StringBuilder` is post-freeze** — `.toString()` lowers to the `concatAll` native, absent
   on the frozen ELF backend. The HTTP classes (which still target ELF via corpus/elf) use plain
   `+` concatenation in `render()`/`request()` instead.

**STOP conditions:** none tripped. The class-based `JsonValue` model is sound on three engines
(bug #30 is a backend refcount bug, not a model problem), so problem #9's type-alias question
was not forced. Keep-alive needed no RuntimeLoop edits (the existing watch/close surface
sufficed), so Track 08's owner was not trespassed.

**Deferred (roadmap, see reference §6.6.57 / info.md §19 #16–18):** `json::parseVerbose` +
global float formatting; HTTP client redirects / URL-string parse / timeout / pipelining /
client-side chunk-send / connection pooling; an http_churn +0B mem-verify program (clean loop
exit is demonstrated by `http_keepalive`); the enum-`JsonKind` upgrade; ship-stdlib-as-files
owner ruling.
