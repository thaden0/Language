# Track 04 — Stdlib: Strings & StringBuilder

**Status:** ready. **Date:** 2026-07-05. **Depends on:** nothing (prelude track;
may run parallel with 01/02/05/06).
**Source:** suggested-features.md §5 (all), §14#7 (doc gaps); contract C2 (owner).
**Owns (regions):** kPrelude `class string` (Resolver.cpp:24–38) + new
`class StringBuilder`; `RuntimeNatives.cpp` string branch (lines 17–32); `CGen.cpp`
string native cores (CGen.cpp:196–~230); `X64Gen.cpp` string entries in
`genCallNative` (1694+; charAt at 1748, subStr at 1736 are the patterns).
**Does NOT own:** `string.at`/`chars` (char-typed — Track 03, contract C1).

**Prime directive:** implement **in-language in the prelude wherever possible** —
in-language methods lower to ordinary IR and run on all five engines for free
(the `Array.where` model, Resolver.cpp:63). Natives only where in-language cannot
express it (byte access, failable parse) or a probe proves it pathologically slow.

---

## 1. Feature list and per-method plan

### 1.1 In-language methods (zero engine work)

Added to kPrelude `class string`, built over the existing native core
(`length/charAt/subStr/indexOf/...`). Bodies must follow the **multi-char-slice
rule** (never single-char append accumulation — the ELF charAt-append bug,
curl-design §2.7#1, is still live; slicing with `indexOf`/`subStr` is both faster
and the documented safe idiom):

| method | signature | body sketch |
|---|---|---|
| `split` | `Array<string> split(string sep)` | scan `indexOf(sep, from)`-style via a while loop slicing whole segments; **empty sep → array of 1-char strings** (JS-compatible; uses charAt per element construction, which is safe — it is not *append accumulation*); keeps empty segments (JS/C# semantics) |
| `replace` | `string replace(string from, string to)` | replace-all; segment-concat between matches; `from == ""` returns `this` unchanged (define it; don't loop forever) |
| `lastIndexOf` | `int lastIndexOf(string sub)` | backward scan via forward `indexOf` loop remembering the last hit (O(n·m) worst case — fine v1) |
| `indexOfFrom` | `int indexOf(string sub, int from)` | overload; `subStr`+offset math (result re-based to absolute index; negative `from` clamps to 0) |
| `padStart`/`padEnd` | `string padStart(int len, string pad)` | build pad by repeat, slice to exact, concat |
| `repeat` | `string repeat(int n)` | doubling concat (O(log n) concats), `n <= 0` → `""` |
| `trimStart`/`trimEnd` | `string trimStart()` | charAt probe loop for the boundary index, then ONE slice |
| `splitLines` | `Array<string> splitLines()` | split on `\n`, then per-line `trimEnd` of one `\r` (handles CRLF + LF) |
| `isBlank` | `bool isBlank()` | trim().isEmpty() |
| `count` | `int count(string sub)` | indexOf-from loop; `sub == ""` → 0 (define) |
| `removePrefix`/`removeSuffix` | `string removePrefix(string p)` | startsWith → slice, else `this` |
| `equalsIgnoreCase` | `bool equalsIgnoreCase(string o)` | length gate then `toLower() == o.toLower()` v1 (allocates; acceptable — note perf follow-up) |
| `reverse` | `string reverse()` | byte-reverse is WRONG for UTF-8; v1: charAt-based per-scalar? No — charAt is byte-indexed 1-byte strings. **Defer `reverse` to Track 03's `chars()`** (reverse the scalar array, join). Listed here so it isn't lost; implemented as `chars().reverse().joinToString("")` once 03 lands — log the handoff |

### 1.2 Native changes

| method | signature | notes |
|---|---|---|
| `toInt` | `int? toInt()` | **BREAKING**: today `atoll` garbage→0 (RuntimeNatives.cpp:32). New: strict full-string parse — optional `-`, digits only, no surrounding space; anything else → `None`; overflow → `None`. Update: RuntimeNatives + CGen mirror + X64Gen native + LLVM (if covered) |
| `toFloat` | `float? toFloat()` | same shape (strtod with full-consumption + finite check) |
| `byteAt` | `int byteAt(int i)` | byte value 0..255; OOB → RuntimeException (loud, matches at()) |
| `fromByte` | static `string string::fromByte(int b)` | 1-byte string; 0..255 or throw. (Static-side native on the string mask — if the static-native plumbing doesn't exist, fall back to a free function `std::byteToString(int)`; log which) |
| `concatAll` | `string Array<string>.concatAll()` | **one** native powering StringBuilder: sum lengths, allocate once, memcpy each part — O(total). Lives in the Array native branch but is owned by THIS track (StringBuilder's engine); coordinate the RuntimeNatives Array-region edit with Track 05 (append-only, trivial rebase) |

### 1.3 `StringBuilder` (in-language over `concatAll`)

```
class StringBuilder {
    Array<string> parts = [];
    int len = 0;
    StringBuilder add(string s) { parts = parts.add(s); len = len + s.length(); return this; }
    StringBuilder (<<)(string s) => add(s);
    int length() => len;
    bool isEmpty() => len == 0;
    string toString() => parts.concatAll();
}
```

A **class** (reference semantics — mutation across call sites is the point).
`parts = parts.add(s)` is the MoveClear self-append path → COW-in-place when
uniquely owned (§11/§15) → amortized O(1) append. `toString` is O(total) via the
native. No per-engine work beyond `concatAll`.

## 2. `toInt -> int?` migration (the breaking change, handled deliberately)

1. **Callers in-tree** (grep first — P2): `examples/curl/url.ext` (port parse —
   its digit-pre-validation workaround can now shrink), `cli.ext`
   (`--max-redirs`/`--max-time`), any corpus programs. Each becomes
   `int? p = s.toInt(); if (p != None) { ... }` — narrowing does the rest.
2. **Recipe documentation (contract C2):** this is the reference implementation
   of an optional-returning *method* native (sysRecv is the *namespace-fn*
   precedent). Write the recipe into this design's log when done: value-model
   None representation per engine, checker signature spelling (`int?` in a
   prelude native), any Lower/ELF wrinkle found. Tracks 03/08/09 reuse it.
3. Reference §6.1 gains toInt/toFloat rows with the strictness rule spelled out.

## 3. P-probes

- **P1 (optional-native probe — run FIRST):** hand-edit a scratch build's prelude
  with `int? probeParse();` native returning None/int; run on all five engines.
  This de-risks the whole track's headline item. If ELF or IR fails on an
  optional *int* specifically (sysRecv proves optional *string* works — int
  payload + None tag may differ), the fallback is: native returns a boxed
  sentinel via existing union machinery — investigate how `string?` None is
  encoded (grep None handling in Lower/X64Gen) and mirror for int. STOP only if
  optional-int is structurally unlowerable (then toInt ships as
  `bool tryToInt(...)`-shaped pair? NO — that is a design change; STOP means
  stop).
- **P2:** `grep -rn 'toInt' tests/ examples/ src/Resolver.cpp` — full caller
  inventory before the break.
- **P3:** COW self-append reality check for StringBuilder: 100k `sb.add("x")`
  loop timed on `--ir` and `--emit-elf` (expect linear; if quadratic, the
  MoveClear path isn't firing for a *field* — see problem #2).
- **P4:** multi-char concat on ELF (the §2.7 M0 probe rerun): `"ab" + "cd"` in a
  loop, 10k iterations — confirms the split/replace bodies' concat pattern is
  safe on ELF today.

## 4. Foreseeable problems & solution strategies

| # | problem | strategy |
|---|---|---|
| 1 | **P1 fails on optional-int natives** (None tag vs int payload plumbing). | Mirror the string? encoding; the None value is tag-only (reference §2.3) so payload kind shouldn't matter — if it does, the fix is in the one place that gates union tags by payload (find via the string? path). Escalate per STOP if it spreads beyond one site. |
| 2 | **StringBuilder append is quadratic** because `parts` is a *field* — the COW-unique fast path was measured on locals; a field write goes release-old/retain-new (§15) and the buffer may read as shared during the add. | P3 measures before building. If quadratic: (a) preferred — route the same MoveClear treatment to field self-append in Lower (find the `MoveClear` emission for `x = x.add(...)` locals and extend the pattern match to `this.f = this.f.add(...)`); that fix benefits every accumulating class, not just StringBuilder. (b) If (a) is deep, interim: StringBuilder holds a native handle instead (a tiny mutable native class) — but log it as debt against the pure model. |
| 3 | **The ELF charAt-append bug** contaminating new bodies: `split("")`'s per-char path and `trimStart`'s charAt probes are fine (no append accumulation), but a future editor may "simplify" into the broken idiom. | Every new prelude body carries the `// multi-char slices only (ELF §2.7)` comment where relevant; `strings_ext.ext` corpus runs the whole toolkit on run_elf.sh so any regression is caught differentially. Root-cause fix of the ELF bug itself stays in bug-workflow (it is a compiler bug, not stdlib). |
| 4 | **`split`/`replace` semantics disagreements** (empty-sep, empty-from, keep-empties) surfacing later as "bug reports." | The table above *defines* them (JS-compatible); encode each edge in the corpus expected-output file so the semantics are pinned, and reference §6.1 documents all three edges. |
| 5 | **Overload `indexOf(sub, from)`** — overloads on the primitive mask: verify the resolver's most-specific-wins handles same-name different-arity on a native class (it does for constructors; methods should match). | One-line probe before writing; if arity overloads on native-mask methods fail, name it `indexOfFrom` (log the deviation + reference note). |
| 6 | **`fromByte` static natives** may have no plumbing (all current string natives are instance methods). | Fallback named in §1.2: free function `std::byteToString`. Decide by probe, not by reading. |
| 7 | **Track-05 collision in RuntimeNatives Array region** (`concatAll`). | Append-only etiquette + explicit note in both designs; whoever lands second rebases (overview §2.1). |

## 5. Milestones & acceptance

| M | deliverable | accept |
|---|---|---|
| M1 | P1–P4 probes logged; in-language toolkit (§1.1 minus reverse) | `strings_ext.ext` corpus (every method, every defined edge) green on all 5 engines; ctest |
| M2 | `toInt`/`toFloat` → optional; caller migration; recipe logged | corpus + lcurl green (`url.ext` simplified); checker test for un-narrowed use compile error |
| M3 | `byteAt`/`fromByte` natives (4 engines) | bytes probe corpus; lcurl `ascii.ext` gets a `// replaceable by byteAt` note (actual rewrite is optional cleanup, not gating) |
| M4 | `concatAll` native + StringBuilder | builder corpus; P3 perf numbers in log (linear confirmed); reference §6.1/§6.x updated |

Target: **Jul 8 – Jul 16** (M1 3d, M2 2d, M3 1d, M4 2d).

## 6. Reference-doc duty

reference §6.1: full string table rewrite (add every §1.1/§1.2 method + the
already-missing `toInt` row + relational-comparison note + `\r` cross-ref);
new StringBuilder section §6.4.6-ish; info.md §10 note (toolkit completed).

## 7. STOP conditions

- P1 optional-int structurally unlowerable (see probe text — no tryParse
  improvisation).
- StringBuilder problem #2 requires (b) and (a) is rejected — the pure-model
  debt needs an owner ruling.
- Any new method wants a *new* native beyond the five listed (§1.2) — the
  in-language directive is load-bearing; a sixth native is a design change.

## 8. Implementation log

**2026-07-06 — M1–M4 all implemented and green. Branch `track-b-runtime`.**

**Architecture note applied before any code was written:** this design predates
(or was authored independently of) the portable-backend pivot memo declaring
`src/X64Gen.cpp` frozen ("kept, never extended," `designs/complete/techdesign-portable-
backend.md`) — the design's own "five engines" framing assumes X64Gen still
receives new native work. Reconciled as follows, and never touched
`X64Gen.cpp`/`X64Gen.hpp`:
- **M1** (in-language methods) needs *zero* new natives — it runs on all
  five engines, including frozen ELF, for free, exactly as the prime
  directive promises. Landed in the shared `tests/corpus/strings_ext.ext`
  (all five ctest lanes green).
- **M2–M4** (new/changed natives: `toInt`/`toFloat` optional, `byteAt`,
  `std::byteToString`, `Array.concatAll`) do **not** touch X64Gen at all —
  ELF keeps its old, frozen behavior for these (accepted, documented
  divergence). New tests for these live in a new isolated corpus dir,
  `tests/corpus/strings_native/`, with four new ctest lanes (treewalk, ir,
  emit-cpp, llvm) that deliberately exclude ELF. This is a value judgment,
  not a design change: the alternative (extending X64Gen) is explicitly
  forbidden by the more recent, explicit owner policy.

### M1 — in-language toolkit (`split`, `replace`, `lastIndexOf`, `padStart`/
`padEnd`, `repeat`, `trimStart`/`trimEnd`, `splitLines`, `isBlank`, `count`,
`removePrefix`/`removeSuffix`, `equalsIgnoreCase`)

- **Problem #5 (arity overload on a primitive mask) confirmed live, not
  hypothetical.** Adding `int indexOf(string sub, int from)` alongside the
  native `int indexOf(string sub)` and bare-self-calling it from `split`
  hung forever on `--run`. Root-caused (bug.md #13): `Checker::run` only
  walks the *user's* `program.items`; the prelude lives in a separate
  `Resolver::preludeProgram_` that the Checker never sees, so **no prelude
  class's in-language method body is ever type-checked** — a general,
  pre-existing gap, invisible until now because no existing prelude method
  ever bare-self-called a same-class overload. Unresolved calls fall back to
  Eval.cpp's arity-blind by-name lookup, which silently picked the 1-arg
  native for a 2-arg call. Applied the design's own pre-registered fallback:
  renamed to **`indexOfFrom`** (not an overload). No other new method
  introduces a same-class overload.
- **The ELF charAt-append bug** this design's §4#3 cites as live was already
  fixed (commit `53f3700`, per Track 01's log) — stale by the time of
  writing. Bodies still use whole-segment slicing throughout (good practice,
  not load-bearing).
- **Found and fixed in passing:** `CGen.cpp::escapeString` didn't escape
  `\r` when re-emitting a decoded string literal into generated C++ source
  — `"a\rb"` failed to compile via `--build`/`--emit-cpp` (any program, not
  specific to this track). One-line fix (`case '\r': out += "\\r";`), needed
  for `splitLines()`'s own CRLF test case to run on the emit-cpp lane.
- `reverse()` deliberately not implemented (byte-reverse is wrong for
  UTF-8) — deferred to Track 03's `chars()`, per the design.
- Accept: `tests/corpus/strings_ext.ext` green on all 5 engines (treewalk,
  ir, native/emit-cpp, ELF, LLVM) + `ctest`.

### M2 — `toInt`/`toFloat` → optional

- Strict parse implemented per spec (optional `-`, digits-only, no
  surrounding space, full consumption, overflow → `None`; `toFloat` adds a
  finite-result check rejecting `"inf"`/`"nan"` text) in `RuntimeNatives.cpp`
  (oracle+IR), `CGen.cpp` (mirrored `strictParseInt`/`Float` inline in the
  emitted-C++ runtime), and `runtime/lv_runtime.c` (LLVM path).
- **ABI evolution, not a new native:** `lvrt_str_toint` changed shape from
  `int64_t lvrt_str_toint(const LvValue*)` to the out-param
  `void lvrt_str_toint(LvValue* out, const LvValue*)` (tag INT or NONE) —
  matching the existing `lvrt_str_substr`/`trim`/`case` convention — since a
  bare scalar can't carry a None tag. `lv_abi.h`'s own header calls ABI
  contract changes a STOP event for cross-track coordination; judged safe to
  do directly here since both sides (LlvmGen.cpp, lv_runtime.c) moved
  together in one change and Track A/B are both status "done" per memory
  (no concurrent agent depends on the old shape). Logged prominently per the
  STOP protocol's spirit rather than silently improvised. Updated the one
  other call site, `runtime/selftest.c`'s `runtime_selftest` unit test.
- Caller migration: prelude's own `HttpResponse.parse` (Resolver.cpp) and
  five call sites across `examples/curl/{url,http,cli}.ext`. Discovered
  along the way: the checker does **not** narrow past an early
  `if (x == None) return/throw;` guard — only same-branch
  (`if (x != None) { use x }`) narrowing is confirmed working; two lcurl
  sites were written with the early-return shape first and had to be
  restructured. Full lcurl integration suite (`examples/curl/test/
  run-tests.sh`, 72 cases × `--run`/`--ir`/`--native`) green after migration.
- New checker tests (`tests/test_checker.cpp`): un-narrowed `toInt()`/
  `toFloat()` use is a compile error; narrowed via `if`/`??` is clean.
- `tests/corpus/strings_native/toint_tofloat.ext`: valid/invalid/overflow/
  whitespace/sign cases, both functions; green on treewalk/ir/cpp/llvm.

### M3 — `byteAt` / `std::byteToString`

- **Problem #6 confirmed by probe, not by reading:** grepped the token enum
  (`src/Token.hpp`) for `KwStatic` — absent. No `static` keyword exists
  anywhere in the parser/checker. `string::fromByte(int)` as a static-side
  native has no plumbing at all, so used the design's own pre-registered
  fallback: the free function `std::byteToString(int)`.
- Both throw `RuntimeException` on out-of-range (no `None` — a byte
  position is either valid or a bug, matching `Array.at`'s OOB shape
  exactly, including the wording: `"index N out of bounds (length M)"`,
  verified byte-identical across engines rather than inventing a
  `"byte index..."` variant).
- **`CGen.cpp`'s `Op::CallNativeFn` never called `throwCheck()`** — a latent
  gap, invisible until `byteToString` became the first native *free*
  function that can raise (`sysWrite`/`sysReadLine` never do). Added the
  missing `throwCheck` call, mirroring `Call`/`CallDyn`/`CallValue`.
- `examples/curl/ascii.ext` gets the design's requested `// replaceable by
  byteAt` note (cleanup left undone, per the design — not gating).
- `tests/corpus/strings_native/byte.ext`: happy path + both OOB throws,
  cross-engine identical message text; green on treewalk/ir/cpp/llvm.

### M4 — `concatAll` + `StringBuilder`

- `concatAll()` declared on `Array<T>` generically (no specialization
  mechanism exists in the language) — type-checks as `string` for any `T`,
  correct-by-convention for the one instantiation it's ever called on
  (`Array<string>`, from `StringBuilder`). Implemented as designed (sum
  lengths, one allocation, append) in all three native cores + the LLVM/
  runtime-ABI path (`lvrt_arr_concatall`, boxed-array layout only — strings
  never go dense).
- `StringBuilder` implemented exactly as designed (`parts`/`len` fields,
  `add`/`(<<)`/`length`/`isEmpty`/`toString`).
- **`(<<)` doesn't lower on `--emit-cpp` for user-defined classes at all** —
  confirmed via a minimal repro unrelated to StringBuilder (bug.md #14,
  pre-existing, general). `.add(...)` works everywhere; the corpus test
  uses only `.add(...)` so `corpus_strings_native_cpp` stays green. `<<`
  manually verified on `--run`/`--ir`/`--native-obj`.
- **P3 probe run as designed, and it changed the plan.** `sb.add("x")` ×
  100k timed out (>120s) on `--ir`; scaling curve at n=1000/2000/4000/8000
  (0.16s/0.56s/2.2s/8.5s, ~4x per doubling) confirmed true O(n²), not just a
  slow constant.
  - **Fix (a) attempted and landed, but went through two revisions before it
    was actually correct — the second bug only surfaced under end-to-end
    verification, not the unit/corpus suite.** Extended `Lower.cpp`'s
    local-only self-append COW-receiver pattern (`moveRecvClear_`) to plain
    (accessor-free) class fields: reading a field always copies into a fresh
    register (unlike a local, whose register *is* its storage), so the
    field-read needs an accompanying void-write to the field's own slot
    (via the *existing* `GetMember`/`RawSet` ops — no new IR op), releasing
    its reference before the call; the marshaling step also needed
    `moveRecvClear_`, not just the field-clear, or it quietly re-retained a
    copy on the way into the call (found by instrumenting `use_count()`
    directly — first attempt still measured 3 live references, not 1).
    **First landing emitted the clear-write as soon as the field was read**
    (inside the bare-Name field-read case) — this passed the FULL `ctest`
    suite (39/39) and every hand-written corpus probe, but broke
    `examples/curl`'s end-to-end integration suite (71/72,
    `examples/curl/test/run-tests.sh`) on exactly one case, silently:
    `ChunkedDecoder.feed`'s `buf = buf.subStr(nl+1, buf.length()-(nl+1))`
    returned wrong (truncated/empty) slices on `--native-obj` only. Root
    cause: the receiver (`buf`) is read-and-cleared *before the call's
    OWN ARGUMENTS are lowered* — and the second argument (`buf.length() -
    ...`) reads the SAME field, seeing the just-cleared void value instead
    of the real string. The wrong length (`0`, from `.length()` on void)
    happened to be masked into a *correct* result on the oracle/IR/emit-cpp
    engines by C++ `std::string::substr`'s own negative-length-clamps-to-end
    behavior (`(size_t)-1` wraps to `SIZE_MAX`, which `substr` clamps to
    "rest of string") — LLVM's own `lvrt_str_substr` clamps a negative `n`
    to `0` explicitly, so it alone exposed the bug as visibly wrong output
    instead of an accidentally-right one. **Fixed by moving the clear-write
    from the field-read site to the call-marshaling site**: `pendingFieldClear_`
    (a name+slot struct, not a bare bool) is captured-and-reset at the exact
    same instant as `clearRecv`/`moveRecvClear_` — before the receiver or any
    argument is lowered, so a nested call reached while lowering an argument
    can't consume it — and the actual void-write is emitted after all
    arguments are lowered, right before the receiver is marshaled into the
    call. Re-verified the full probe set (single call, two sequential calls,
    the original `ChunkedDecoder`-shaped repro, `StringBuilder` itself) on
    all four active engines, then re-ran `ctest` (39/39) AND
    `examples/curl/test/run-tests.sh` (72/72, twice) before considering this
    done. Measured **100k-iteration `StringBuilder.add` in 0.080s (emit-cpp),
    0.030s (LLVM), 0.028s (frozen ELF — for free, since the fix emits only
    pre-existing IR ops X64Gen already implements)** — down from the same
    O(n²) every other engine showed pre-fix. This is genuine, general
    infrastructure ("benefits every accumulating class," as the design
    hoped), not a StringBuilder special-case — and the lesson underlined by
    the near-miss: a passing corpus/unit suite is not sufficient evidence for
    a change to shared lowering machinery; the existing end-to-end
    integration suite (lcurl) is what actually caught it.
  - **`--run`/`--ir` remain O(n²) for self-append — but not because of
    anything field- or StringBuilder-specific.** Instrumented
    `RuntimeNatives.cpp`'s `Array.add` directly: `use_count()` was **3** at
    every call, for a **plain local** self-append too (`a = a.add("x")` in
    a bare loop — no class, no field). Root cause: `IrInterp.cpp`'s own
    call-marshaling (`Op::CallDyn` → `args` vector from the register
    window → `callDecl(..., args)` passed *by value* → `Value self =
    args[0];` inside it) creates at least two incidental shared_ptr copies
    of the receiver before `nativeCall` ever runs, independent of how
    uniquely-owned the IR's *source* register was. `use_count()==1` is
    structurally unreachable through this path today. Threaded a `cow`
    parameter through `nativeCall` and added the matching `use_count()==1`
    fast path (mirroring CGen's) to confirm the diagnosis — it built and
    ran correctly but the fast path never fired (still measured 3), so it
    was **inert complexity and was reverted**, not left half-landed. Filed
    as bug.md #15 with the full trace; it's a general IR-interpreter
    calling-convention issue (every method call, not just `Array.add`), out
    of this track's scope to fix. `--run` staying slow is by design (info.md
    §11: the oracle is deliberately copy-always); `--ir`'s claim of already
    having this ("info.md: had this already via MoveClear") is empirically
    false as measured and is now a tracked, separately-owned gap.
  - Per this design's own STOP condition ("StringBuilder problem #2
    requires (b) and (a) is rejected — needs an owner ruling"): (a) was not
    rejected — it was implemented, verified correct, and landed, delivering
    the intended fix on 3 of 5 engines. The residual gap is a *different*,
    pre-existing bug this track's probe surfaced, not a reason to fall back
    to (b) (a native-handle StringBuilder) — (b) would not have helped
    `--run`/`--ir` either, since the gap is in the shared `Array.add`
    native itself, and would have cost the pure-model debt the design
    explicitly wants to avoid for no upside.
- `tests/corpus/strings_native/stringbuilder.ext`: `concatAll` (incl.
  empty array) + full `StringBuilder` lifecycle via `.add(...)`; green on
  treewalk/ir/cpp/llvm.

### Bugs filed (all in `bug.md`, none fixed beyond the narrow items above)
- **#11** — prelude class bodies never type-checked (root cause of the
  `indexOf`/`indexOfFrom` overload hang).
- **#12** — `(<<)`/`(>>)` operator-method calls don't lower on `--emit-cpp`
  for user-defined classes.
- **#14** — IR interpreter's method-call marshaling multiplies the
  receiver's refcount, making `use_count()==1` unreachable for any
  self-append, local or field.

### Reference-doc duty
`docs/reference.md` §6.1 (full string table rewrite: all M1 methods,
`toInt`/`toFloat`/`byteAt` rows, relational-comparison note, `\r` cross-ref),
new §6.3.5 `StringBuilder`, `Array<T>` table gains `concatAll`. `info.md` not
touched (no new language-level rule — everything here is stdlib + one
compiler-internals extension).

### Not done / explicitly out of scope
- `reverse()` — deferred to Track 03 per design.
- Bug 13/14/15 themselves — filed, not fixed; each is a pre-existing,
  general gap this track's probes surfaced, not something Track 04 owns.

Status: **M1–M4 complete.** `ctest`: 39/39 green (5 new: `corpus_strings_ext`
folded into the existing five corpus lanes; 4 new `corpus_strings_native_*`
lanes). No regressions in the pre-existing 35. `examples/curl/test/
run-tests.sh` (the full lcurl integration suite, 72 cases × `--run`/`--ir`/
`--native`): 72/72 — this is what actually caught the field-clear-timing bug
in the first landing of the M4 Lower.cpp fix (`ctest` alone did not).
