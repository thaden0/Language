# Deferral resolution — UTF-8 `chars()` & the string ops blocked on it

**Status:** COMPLETE — landed 2026-07-19 (see §9 log); promoted out of deferral 2026-07-17 (was
`deferal-utf8-chars-string-ops.md`); **every gate is now open.** Track 03 `char` landed in
full on all four active engines (oracle/IR/emit-C++/LLVM) including the LV_CHAR ABI addendum —
so the §3.4 lane plan's "LLVM gated on the addendum" is satisfied and D3 collapses into D1/D2
(ELF stays excluded — X64Gen frozen). Re-verified against master 2026-07-17: `chars()` is
*declared* in the prelude (`Resolver.cpp:194`) — reconcile any existing body/native against
§3.1/§3.2 at pickup rather than assuming absence — while `reverse()` is still the literal
deferral comment (`Resolver.cpp:331-333`), i.e. §4.1 is unbuilt. Problem #8 (corpus `.lev`
glob) is resolved — `.lev` corpus files are auto-globbed today. `file:line` refs are
Jul-6-era; re-ground at start. Note this is also on the self-host/interpreter critical path
(a Leviathan-written lexer needs scalar iteration), which raises its priority beyond the
original string-ops framing. **Date:** 2026-07-06 (design); 2026-07-17 (promoted).
**Depends on:** ~~Track 03 M1~~ (landed); Track 04 landed (byteAt/`std::byteToString`/
concatAll — all landed, `designs/complete/techdesign-04-stdlib-strings.md` §8).
**Source:** the logged deferrals in `src/Resolver.cpp:264-266`,
`designs/complete/techdesign-04-stdlib-strings.md:43` (+ impl log :195-196,
:370-372), `designs/complete/techdesign-07-iteration.md:44-48`, `docs/reference.md:563-565`;
contract C1 (overview §3, `designs/complete/techdesign-00-overview.md:127-130`).
**Owns (spec only):** the `chars()` API + UTF-8 decode semantics + the dependent
string ops (`reverse()`, scalar iteration/indexing idioms). **Code ownership is
unchanged:** `string.at`/`chars` remain Track 03 property (contract C1 — Track 04
"Does NOT own" them, techdesign-04 header :10); the prelude `class string` region
and `RuntimeNatives.cpp`/`CGen.cpp` string branches are Track 03's to edit when M1
runs. This doc exists so the deferral has a complete, ratified landing plan instead
of a comment.

**Backend policy (restating the frozen facts):** LLVM is the primary backend;
`X64Gen.cpp`/`X64.hpp` are FROZEN — nothing here touches them, ever, and the
ELF lane is excluded for all char-typed work (Track 03 currency-check RULING,
techdesign-03 :38-58). The LLVM lane for anything carrying a `char` value is
gated on the ratified-but-deferred **LV_CHAR ABI addendum** (tag 10,
`runtime/lv_abi.h`'s LV_* enum is a closed 0–9 set, :26-27; additions are Track-B
STOP-gated). Until that addendum lands, char-typed corpus runs
**oracle + IR + emit-C++ only** — a deliberate, logged lane exclusion, exactly the
Track 04 `strings_native` pattern.

---

## 1. Summary — the deferrals being resolved

Several string operations are logged-deferred because they need a UTF-8 scalar
decomposition that does not exist yet. The dependency has a name and an owner
(`string.chars() -> Array<char>`, Track 03, contract C1) but no design detail
beyond one sentence — so every dependent op is parked on a comment. The inventory:

| # | deferred op | where logged | exact text |
|---|---|---|---|
| 1 | `string.reverse()` | `src/Resolver.cpp:264-266` (prelude `class string`, end of the Track 04 toolkit) | "`reverse()` is deliberately NOT implemented here: a byte-reverse is wrong for UTF-8. Deferred to Track 03's `chars()` — once it lands, this becomes `chars().reverse().joinToString("")`." |
| 2 | `string.reverse()` (design-side) | `designs/complete/techdesign-04-stdlib-strings.md:43` (§1.1 table) + impl log :195-196 + "Not done" :370-372 | "byte-reverse is WRONG for UTF-8 … **Defer `reverse` to Track 03's `chars()`** … implemented as `chars().reverse().joinToString("")` once 03 lands — log the handoff" |
| 3 | `string.reverse()` (user-facing doc) | `docs/reference.md:563-565` (§6.1) | "`reverse()` is deliberately not offered yet … it lands with Track 03's `chars()`" |
| 4 | scalar iteration over strings | `designs/complete/techdesign-07-iteration.md:44-48` | "Strings are NOT iterable in v1 (`s.chars()` returns an Array — explicit; avoids the bytes-vs-scalars ambiguity in a loop header)" — the *documented* iteration idiom is `for (char c in s.chars())`, which cannot be written today |
| 5 | scalar-correct counting/indexing | implied by C1 (`string.at(int) -> char`, `chars() -> Array<char>`, overview :127-130) + techdesign-03 §1.1 | byte-counted `length()`/`indexOf` are the only index world today; there is no way to ask "how many scalars" or "the i-th scalar" |

**The missing dependency, precisely:** `Array<char> string.chars()` — a full
UTF-8 decode of the string into an array of Unicode scalar values, element type
`char` (the Track 03 value primitive, techdesign-03 §1.1). This doc designs (a)
`chars()` itself — API, decode semantics, homes, engine story — and (b) the
dependent ops that unblock the moment it lands.

## 2. Why these were deferred (bytes vs scalars)

The string native core is **byte-indexed by design**: `length()` is the byte
count, `charAt(i)`/`subStr(a,n)`/`indexOf` address bytes
(`RuntimeNatives.cpp:55-62` — all of them index `std::string` positions;
`charAt` returns a **1-byte** string). That world is fast, O(1), and correct for
every op Track 04 shipped, because those ops only ever *slice at match
boundaries* (a substring match in UTF-8 always falls on scalar boundaries — UTF-8
is self-synchronizing).

`reverse()` is the op where that stops being true: reversing *bytes* shreds every
multi-byte sequence (`"é"` = `C3 A9` byte-reversed is `A9 C3` — ill-formed
garbage). Correct reversal needs the string decomposed into scalars first, the
scalar sequence reversed, then re-encoded. The same decomposition is what scalar
iteration (deferral #4) and scalar counting (#5) need. Rather than ship a wrong
`reverse()` or invent a private half-decoder inside one method body, Track 04
correctly parked all of it on the one shared primitive — `chars()` — which is
Track 03's to build because its element type is Track 03's `char`. Track 03 then
STOPped on an unrelated probe failure (enum statics — handoff in `/this_bug.mg`;
the char portion itself was *not* the blocker and is unblocked by the 2026-07-06
rulings recorded in techdesign-03), so the dependency chain has sat unresolved.
This doc closes the design half of that chain.

## 3. Resolution — the `chars()` design

### 3.1 API

```
class string {
    ...
    // Full UTF-8 decode: every Unicode scalar in order. Ill-formed bytes decode
    // to U+FFFD (never throws on data). Empty string -> []. O(n) time and
    // allocation; byte-indexed ops (length/indexOf/subStr) remain the primary
    // index world — chars() is the explicit opt-in to the scalar world.
    Array<char> chars();
}
```

- **Element type is `char`** (Track 03 §1.1: value primitive, one Unicode scalar
  `0..0x10FFFF` minus surrogates, unboxed immediate). Contract C1 fixes this
  signature; this doc does not get to change it and doesn't want to.
- **Sibling, for orientation (not designed here):** `char string.at(int
  byteOffset)` — the O(1) single-scalar read at a byte offset, throwing on a
  non-boundary offset (techdesign-03 §1.1, contract C1). `chars()` is the O(n)
  whole-string form; `at()` is the O(1) point form. `charAt` keeps returning
  a 1-byte `string` forever (C1).
- Scalar count = `s.chars().length()`; byte count = `s.length()`. Both real,
  both documented, never conflated (`"héllo".length() == 6`,
  `"héllo".chars().length() == 5` — corpus-pinned).

### 3.2 Decode semantics (the normative part — every engine matches this exactly)

Well-formedness is RFC 3629 / Unicode: 1–4 byte sequences, **overlong encodings
rejected, surrogate code points (U+D800–U+DFFF) rejected, values above U+10FFFF
rejected, truncated sequences rejected**. The lead-byte table (this is the whole
algorithm — small enough to mirror without drift):

| lead byte | seq len | valid 2nd byte | notes |
|---|---|---|---|
| `00..7F` | 1 | — | ASCII fast path |
| `C2..DF` | 2 | `80..BF` | `C0`/`C1` are never-valid leads (overlong) |
| `E0` | 3 | `A0..BF` | tighter low bound kills overlongs |
| `E1..EC`, `EE..EF` | 3 | `80..BF` | |
| `ED` | 3 | `80..9F` | tighter high bound kills surrogates |
| `F0` | 4 | `90..BF` | tighter low bound kills overlongs |
| `F1..F3` | 4 | `80..BF` | |
| `F4` | 4 | `80..8F` | tighter high bound kills > U+10FFFF |
| `80..BF`, `C0..C1`, `F5..FF` | — | — | never a valid lead |

Third/fourth bytes are always `80..BF`. Scalar assembly is the usual
mask-and-shift (`(b0 & 0x1F)<<6 | (b1 & 0x3F)` etc.).

**Ill-formed input → U+FFFD, never a throw.** This is already ruled: techdesign-03
§5 problem #2 — "replacement scalar U+FFFD (never throw on data — data is not a
programming error)"; strings can legitimately hold arbitrary bytes (sysRecv,
`\xNN` escapes). The exact replacement policy is the **maximal-subpart** rule
(Unicode-recommended, WHATWG-decoder behavior): consume the longest prefix of a
sequence that is still valid per the table; on the first byte that breaks it,
emit ONE U+FFFD and **resume at that byte** (it may itself be a valid lead).
Pinned consequences, encoded verbatim in the corpus so all engines are held to
them differentially:

| input bytes | decodes to |
|---|---|
| `41` | `[U+0041]` |
| `C3 A9` (`é`) | `[U+00E9]` |
| `E2 82 AC` (`€`) | `[U+20AC]` |
| `F0 9D 84 9E` (𝄞) | `[U+1D11E]` — one scalar, 4 bytes (astral pin) |
| `80` (lone continuation) | `[U+FFFD]` |
| `E2 82` at end of string (truncated) | `[U+FFFD]` — one, not two |
| `C0 AF` (overlong `/`) | `[U+FFFD, U+FFFD]` — `C0` never-valid lead, then `AF` lone continuation |
| `ED A0 80` (surrogate encoding) | `[U+FFFD, U+FFFD, U+FFFD]` — `ED` breaks at `A0` (out of `80..9F`), then `A0`, `80` are each lone continuations |
| `F5 41` | `[U+FFFD, U+0041]` — resume-at-breaking-byte keeps valid data |
| `""` | `[]` |

Round-trip law (corpus-pinned): for any **well-formed** `s`,
`s.chars().joinToString("") == s` (encode is the exact inverse of decode; a
`char` can never hold a surrogate or out-of-range value because `chars()` never
produces one and `std::charFromCode` throws on them, techdesign-03 §1.1). For
ill-formed input the round trip is lossy by design (bad bytes normalize to
U+FFFD's encoding `EF BF BD`) — documented, and the answer for byte-faithful
work is Block (contract C4), not string.

Test authoring note: Track 01's `\xNN` escapes (landed 2026-07-06) let corpus
files write ill-formed bytes in string literals directly; `std::byteToString(int)`
(Track 04 M3, `docs/reference.md:556-559`) is the fallback constructor if the
lexer rejects any of them. Probe P3 (§6) settles which.

### 3.3 Where it lives — primary: in-language over `byteAt`

The Track 04 prime directive applies unchanged (techdesign-04 §"Prime directive":
in-language in the prelude wherever possible; natives only where in-language
cannot express it or a probe proves it pathologically slow). UTF-8 decode **is
expressible in-language today**, because Track 04 M3 landed exactly the two
bricks it needs: `int byteAt(int i)` (byte access, `Resolver.cpp:127`) and — from
the Track 03 ruling — `std::charFromCode(int) -> char` (the char factory,
techdesign-03 §1.1 RULING). So the primary design is:

- **Home:** kPrelude `class string` (the Track 04 toolkit region,
  `Resolver.cpp:107-267`), an ordinary in-language body placed where the
  :264-266 deferral comment sits today (the comment is *replaced by the thing it
  promised*). Body shape: a `while` loop over byte positions; classify the lead
  via `byteAt(i)` per the §3.2 table; compute the scalar or `0xFFFD`; accumulate
  `r = r.add(std::charFromCode(cp))`; advance by the consumed length. The
  accumulator is the standard self-append shape — amortized O(1) on the compiled
  lanes since Track 04 M4's Lower.cpp COW-receiver fix (techdesign-04 impl log,
  measured linear).
- **Why in-language wins here:** ONE decoder, one place, instead of three
  mirrored native copies (RuntimeNatives.cpp + CGen's embedded runtime +
  lv_runtime.c) that can drift on exactly the edge semantics §3.2 pins —
  drift-in-N-places is techdesign-03's own problem #6, and this dodges it
  structurally. In-language bodies lower to plain IR and run on every lane that
  can hold a `char` value at all, for free — including LLVM the day the ABI
  addendum lands, with zero additional work in this doc's scope.
- **Native deps consumed (all Track 03 M1 or already landed):** `byteAt`
  (landed), `std::charFromCode` (M1), `char.toString()` (M1, UTF-8 *encode* — the
  §3.2 table's inverse; needed by `joinToString`), `Array.add`/`length` (landed).

**Fallback (only if probe P1 fires — pathologically slow):** a native
`chars()`. Homes, for the record, so the fallback is a decision and not a
scramble: `RuntimeNatives.cpp` string branch (:51-78 — constructs the array the
way `Map.keys/values` already does at :139-142, a native building
`VKind::Array` directly); `CGen.cpp` `callnative` string branch (:258-272
mirror); and for the LLVM lane `runtime/lv_runtime.c` gains
`void lvrt_str_chars(LvValue* out, const LvValue* s)` following the existing
out-param family convention (`lvrt_str_substr`/`trim`/`case`, lv_runtime.c:563+)
— reads the LV_STR payload `{len; bytes}`, decodes per §3.2, writes a **boxed**
LV_ARR of LV_CHAR immediates (chars are pure immediates per the ratified addendum
shape — no per-element ARC; strings-never-go-dense already set the
boxed-layout precedent in `lvrt_arr_concatall`). Describe-only here; nobody
implements this unless P1 says so.

### 3.4 The Track 03 dependency, stated exactly

`chars()` cannot exist one day before `char` does: its return type *is*
`Array<char>`, its factory is `std::charFromCode`, and its round-trip partner is
`char.toString()`. All three are Track 03 M1 deliverables (techdesign-03 §6, M1
row: "`char` end-to-end (oracle+IR first, then CGen…); `string.at`/`chars`").
This doc is therefore **the detailed spec for the `chars()` slice of Track 03
M1**, plus the immediately-following dependent-op wave (§4) that Track 04's
handoff assigned to "once 03 lands." Lane availability inherits Track 03's
ruling: oracle + IR + emit-C++ at M1; + LLVM when the LV_CHAR addendum is
co-landed by Track B; ELF never (frozen).

## 4. The dependent ops that unblock

### 4.1 `string.reverse()` — the headline handoff

Exactly as promised in all three deferral logs (§1 rows 1–3):

```
// UTF-8-correct: reverses scalars, not bytes ("désert" -> "trésed", the é
// intact). Ill-formed bytes normalize to U+FFFD (chars() policy, §6.1b).
string reverse() => chars().reverse().joinToString("");
```

Every piece now exists or lands with M1: `Array<T>.reverse()` is in-language and
landed (`Resolver.cpp:337-342`), `joinToString(sep)` is in-language and landed
(`Resolver.cpp:514-523`, calls `x.toString()` per element →
`char.toString()` = UTF-8 encode). **Handoff log duty (from techdesign-04:43
"log the handoff"):** when this lands, (a) the `Resolver.cpp:264-266` deferral
comment is replaced by the method; (b) `docs/reference.md:563-565`'s "not offered
yet" paragraph becomes the live `reverse()` row in the §6.1 table; (c) the
closure is recorded in THIS doc's §9 log —
`designs/complete/techdesign-04-stdlib-strings.md` §8's "Not done" list is not
edited retroactively (completed designs stay historical).

Perf note, pre-logged: `joinToString` accumulates by string concat — O(total²)
worst case. Fine for v1 `reverse()` (strings being reversed are human-scale; the
op was absent entirely yesterday). If profiling ever objects, the O(total)
rewrite is `chars().reverse().map((char c) => c.toString()).concatAll()` over the
Track 04 M4 native — a body swap, no API change. Not done preemptively.

### 4.2 Scalar iteration — Track 07's documented idiom becomes real

`for (char c in s.chars())` now works: `chars()` returns a real `Array`, so the
loop takes the existing Array fast path (contract C5 — no protocol work, no
iterator classes, nothing owed to Track 07). This *discharges* techdesign-07
:44-48's forward reference: strings stay non-iterable in v1 exactly as that
design froze it, and the explicit-array escape hatch it named now exists. No
change to Track 07's text or plan.

### 4.3 Scalar counting / indexing idioms (documentation, not new API)

- scalar count: `s.chars().length()` (vs byte `length()`);
- i-th scalar: `s.chars().at(i)` — O(n); the O(1) byte-offset form is Track 03's
  `s.at(byteOffset)` and out of this doc's scope;
- scalar-wise transforms (e.g. classify via `c.isDigit()`, map, filter) — the
  whole Array toolkit applies for free.

Reference §6.1 gains a short "bytes vs scalars" paragraph pinning all of this
(one table row per idiom). No further ops are added in this pass —
`scalarLength()`/`scalarAt()` conveniences were considered and rejected as
premature API surface; the compositions above are one call longer and honest
about their cost.

## 5. Foreseeable problems & solution strategies

| # | problem | strategy |
|---|---|---|
| 1 | **Ill-formed UTF-8** (lone continuations, truncations, overlongs, surrogates, `F5+` leads) — a decoder that throws bricks sysRecv'd data; a decoder that's sloppy admits overlong-encoding smuggling (a classic security wart). | §3.2 is normative: never throw, reject all ill-formed classes via the tightened-second-byte table, U+FFFD per maximal subpart. The pinned-cases table is encoded byte-for-byte in the corpus; differential lanes hold every engine to it. This also re-ratifies techdesign-03 §5#2's ruling rather than re-litigating it. |
| 2 | **Allocation cost** — `chars()` materializes N values per call (oracle `Value`s; 16-byte boxed elements on LLVM later). `if (s.chars().length() > 0)` in a hot loop is a trap. | Documented as O(n) time *and* allocation in the prelude comment + reference §6.1; byte ops stay the primary index world; the O(1) point form is `at(byteOffset)` (Track 03); a lazy scalar `Seq<char>` is a **noted deferral** to post-Track-07 (`chars().asSeq()` composes for free once Seq lands; a streaming decoder iterator would be the real fix and is not v1). No premature caching — strings are immutable but `chars()` results are ordinary arrays; callers hoist. |
| 3 | **`char` vs single-scalar-string representation** — the tempting interim is `Array<string>` of 1-scalar substrings: it would run on ALL lanes *today* with zero new value machinery. | **Rejected, and this is the doc's firmest ruling:** it forks contract C1's frozen signature, permanently types the documented loop idiom's variable as `string` (retyping the element later is a silent breaking change at every call site — comparisons, overloads, `match`), and buys only schedule, not capability. `chars()` waits for `char`. If Track 03 slips materially, that is a STOP (§7), not a license to ship the fork. |
| 4 | **`joinToString` availability & correctness for char** — `reverse()`'s pipeline needs element `toString()`; a wrong/missing `char.toString()` encode makes round-trip fail invisibly. | `joinToString` is landed and element-type-agnostic (`Resolver.cpp:514`); `char.toString()` is Track 03 M1 native with §3.2's table as its inverse spec. The corpus round-trip law (`chars().joinToString("") == s`, incl. an astral-plane pin) is the tripwire — it fails loudly if encode and decode ever disagree. |
| 5 | **Ordering with Track 03 / the LLVM gate** — Track 03 is queued behind Track 02 and just came off a STOP (enum statics, `/this_bug.mg`); the char slice is ruled-unblocked but LLVM's LV_CHAR tag is Track-B-gated. Risk: this design goes stale or someone "helpfully" adds tag 10. | Explicit gates in §7; the LV_CHAR addendum shape is already ratified (techdesign-03 currency check) so Track B can co-land it without a second ruling; this doc's lane plan (oracle/IR/emit-cpp first, +LLVM at addendum, ELF never) matches Track 03's and the Track 04 `strings_native` precedent exactly, so there is no new lane policy to invent. |
| 6 | **Prelude bodies are not type-checked** (bug.md #11 — `Checker` never walks the prelude program), so a type error in the in-language `chars()`/`reverse()` bodies surfaces only at runtime. | Same mitigation Track 04 shipped under: no same-class overloads (the indexOf/indexOfFrom lesson), corpus exercises every path (incl. each §3.2 table row, which collectively covers every branch of the decoder body) on all active lanes. |
| 7 | **Decoder drift across engines** if the P1 fallback (native `chars()`) fires — three copies of §3.2. | Primary design has this problem structurally solved (one in-language body). If the fallback fires: §3.2's table IS the implementation (each copy is a transliteration, not a reinterpretation), and the pinned corpus is the differential detector — precedent: `toInt` strictness landed identically in three runtimes this way (techdesign-04 M2 log). |
| 8 | **Corpus tooling gap:** `tests/run_corpus.sh:5` globs `*.ext` only, but new source files must be `.lev` (hard rule). | Flagged, not improvised (same flag the Track 03 handoff carries): the new corpus dir needs either a one-line second glob in the runner or an owner call — raise it at D1, one line, before writing test files. |

## 6. P-probes (before any code, per overview §4.1)

- **P1 (decode perf):** the in-language decoder over `byteAt` on a ~100 KB
  mixed-plane string (ASCII/Latin/CJK/emoji), timed on `--ir` and emit-C++.
  Pathological = worse than ~10× a native-loop estimate on the *compiled* lanes
  (the oracle is deliberately slow, info.md §11 — it does not vote). Fires →
  §3.3's native fallback. Expectation: passes; the same shape (`trimStart`'s
  charAt probe loop, split("")'s per-char build) is already shipped practice.
- **P2 (Array<char> plumbing):** once M1's `char` value exists — an
  `Array<char>` through `add`/`at`/`reverse`/`for..in`/`==` on oracle+IR+emit-cpp.
  chars() assumes "char is just another value kind in an array"; verify, don't
  assume (techdesign-03 problem #8's union cousin).
- **P3 (ill-formed literals):** can `\xNN` escapes place arbitrary ill-formed
  bytes in a string literal (Track 01 F3), or does some layer reject/normalize
  them? Decides corpus authoring style (literals vs `std::byteToString` build-up).

## 7. Milestones, timeline, STOP conditions

| M | deliverable | accept |
|---|---|---|
| D0 | Probes P1–P3 logged | results in this doc's §9 log |
| D1 | `chars()` in-language body + corpus (`tests/corpus/` char dir shared with Track 03 M1; `.lev`, runner glob resolved per problem #8) | every §3.2 pinned row + round-trip law green on oracle/IR/emit-cpp lanes; ctest |
| D2 | `reverse()` + scalar-idiom docs + handoff closure (§4.1 a–c) | reverse corpus (multi-byte, astral, ill-formed, empty, palindrome) green; reference §6.1 updated; Resolver comment replaced |
| D3 | LLVM lane (**gated**: LV_CHAR addendum, Track B co-land) | same corpus green on `corpus_llvm`; if P1's fallback fired, `lvrt_str_chars` lands here too |

Timeline: D0–D2 ride **inside/immediately after Track 03 M1** (M1 is 3d in
Track 03's Jul 20 – Aug 2 window; D1–D2 add ~1.5d to that window — target
**Jul 20 – Jul 25**). D3 is unscheduled by design — it moves when Track B moves,
and nothing in D1/D2 blocks on it.

**STOP conditions:**

1. **Track 03's `char` does not land or slips past its window** — do NOT ship any
   interim decomposition (`Array<string>`, `Array<int>` of code points, a
   `scalars()` side-API). Problem #3's ruling holds; escalate for a re-plan
   instead. The deferral has waited since Track 04; it can wait for a ruling.
2. **LV_CHAR tag work is never unilateral** — `runtime/lv_abi.h`/`lv_runtime.c`
   additions are Track-B STOP-gated contract changes (techdesign-portable-
   backend-2 §0.2). D3 waits for the co-land; a green D1/D2 with no LLVM lane is
   the correct intermediate state, not a gap to "fix."
3. **P1 fires AND the native fallback wants more than the three described homes**
   (e.g. a new IR op, checker special-casing for a native returning
   `Array<char>`) — that's a design change, stop.
4. **P2 fails structurally** (char values don't survive Array machinery) — that's
   a Track 03 value-model bug to fix there, not something to paper over in
   `chars()`.
5. Any pressure toward an ELF lane or `X64Gen.cpp`/`X64.hpp` edits — automatic
   stop; the freeze is owner policy.

## 8. Reference-doc duty

reference §6.1: `chars()` row (signature, O(n) note, U+FFFD policy pointer) +
`reverse()` row replacing the :563-565 deferral paragraph + the §4.3 "bytes vs
scalars" idiom table; §6.1b (or the section Track 03 creates for char) carries
the normative §3.2 decode table and the round-trip law. info.md: one sentence in
the strings story (byte world primary, `chars()` the explicit scalar door).

## 9. Implementation log

- 2026-07-19 — LANDED (D0–D2; D3 rides along because LV_CHAR already landed).

  **D0 probes.** P3 first (it decides the rest): `\xNN` escapes DO place raw
  ill-formed bytes into a string literal — no lexer/parser normalization — so the
  corpus authors §3.2 cases as plain literals (no `std::byteToString` build-up).
  P1 is moot: `chars()`/`at()` were found **already implemented as natives** on
  all four lanes (RuntimeNatives.cpp, CGen.cpp, lv_runtime.c), so there is no
  in-language decoder to time. P2 is subsumed — the pre-existing `Array<char>`
  plumbing (add/at/reverse/for-in/==) is what `reverse()` composes over and it is
  green.

  **The reconcile call (design header's "reconcile any existing native against
  §3.1/§3.2").** The landed natives were a *lenient* decoder — it admitted
  overlongs (`C0 AF`→U+002F), **surrogates** (`ED A0 80`→U+D800), and
  **> U+10FFFF** (`F4 90 80 80`→U+110000), and split a truncated tail into two
  U+FFFD. That is exactly the "overlong-encoding smuggling security wart" of §5#1
  and violates the normative §3.2 table. Decision: **keep the native homes but
  make all three decoders strict**, rather than rewrite to the §3.3-primary
  in-language body. Rationale: the natives already run byte-identically on all
  four lanes (including the LLVM lane the in-language route was partly meant to
  reach, now that LV_CHAR has landed); the header explicitly sanctions reconcile
  over assume-absence; and §3.2 says each native copy is "a transliteration, not
  a reinterpretation" of one table — which is exactly what the three now are.
  `utf8DecodeAt` (RuntimeValue.hpp — shared by oracle/IR *and* the comptime char
  literal decode in Eval/Lower), `u8dec` (CGen), and `lv_utf8_decode_at`
  (lv_runtime.c) were rewritten to the tightened-second-byte table with
  WHATWG maximal-subpart replacement (`len` advances by the maximal valid
  subpart → exactly one U+FFFD per ill-formed sequence, resume at the breaking
  byte).

  **D1 — `chars()`.** Now strict per §3.2 on all four lanes. New corpus
  `tests/corpus/chars/chars_utf8.lev` pins every §3.2 row by scalar *code*
  (unambiguous ASCII `.expected`): ASCII/é/€/astral 𝄞, lone-continuation,
  truncated-3-byte (one FFFD not two), overlong `C0 AF`, surrogate `ED A0 80`,
  `F5 41` resume, `F4 90 80 80` out-of-range, empty; plus scalar-vs-byte counts
  (`"héllo"` → 6 bytes / 5 scalars), the round-trip law across all four plane
  widths, and the `for (char c in s.chars())` iteration idiom.

  **D2 — `reverse()`.** `string reverse() => chars().reverse().joinToString("")`
  replaces the `Resolver.cpp` deferral comment (handoff duty a). Corpus
  `tests/corpus/chars/reverse.lev` (ASCII, empty, single, palindrome, multi-byte
  `désert`→`treséd` with é intact, astral `𝄞x`→`x𝄞`, double-reverse identity).
  reference.md §6.1 updated (handoff duty b): `reverse()` row added, the
  "not offered yet" paragraph replaced by the live method + a bytes-vs-scalars
  idiom table + the normative decode/round-trip pins; the char row notes the
  strict-RFC-3629 rejection; info.md gains the one-sentence scalar-door note.

  **D3 — LLVM.** Not gated after all: the LV_CHAR ABI addendum landed 2026-07-10,
  so `corpus_chars_llvm` was already live and now runs the strict decoder too —
  green, byte-identical to oracle/IR/emit-C++. The P1 native-fallback `lvrt_str_chars`
  described in §3.3 was already present; it just needed its shared decoder tightened.

  **Verification:** all four `corpus_chars_*` lanes green (treewalk/ir/cpp/llvm);
  every §3.2 pinned row and the round-trip law hold identically across engines.
  No behavior drift in the wider string/regex/json corpus (the decoder is only
  reached by `chars`/`at`/char-literal decode; regex is byte-oriented).
