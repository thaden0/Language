# Tech Design Overview — the pre-framework robustness pass

**Status:** **COMPLETE** — all nine tracks landed by 2026-07-10; archived as the
implementation index. **Date:** 2026-07-05.
**Source:** `designs/suggested-features.md` (the analysis; unchanged, referenced by § below).
**Convention docs:** completed designs move to `designs/complete/` when landed.

This is the index for nine implementation tracks. Each track is one design file, one
work unit, one implementer. Tracks are grouped so **all features of a category land
together** (e.g. every new type in one track), and split so work can proceed in
parallel where file ownership allows.

---

## 1. The tracks

| # | design file | contents | src surface | depends on |
|---|---|---|---|---|
| 01 | `complete/techdesign-01-literals-operators.md` — **DONE 2026-07-06** | shifts/xor/complement (`<<` `>>` `^` `~`, all 5 engines), hex/binary literals, digit separators, `\xNN` escapes, string interpolation | compiler (lexer→backends) | — |
| 02 | `complete/techdesign-02-control-flow.md` — **DONE 2026-07-06** | `break`/`continue`, `do-while`, `using` (scope cleanup) | compiler (parser→backends) | — |
| 03 | `complete/techdesign-03-core-types.md` — **DONE 2026-07-10** | `char`, `enum`, `Block` (byte buffer) | compiler + prelude + natives | 01 (hex literals useful, not blocking) |
| 04 | `complete/techdesign-04-stdlib-strings.md` — **DONE 2026-07-06** | string toolkit, `toInt -> int?`, `byteAt`, StringBuilder | prelude + natives | — |
| 05 | `complete/techdesign-05-stdlib-collections.md` — **DONE 2026-07-06** | `map<U>`/`reduce<A>` fixes, Array completion, Map completion, `Set<T>`, key-equality protocol | prelude (+1 checker risk) | — |
| 06 | `complete/techdesign-06-stdlib-math.md` — **DONE 2026-07-06** | int/float masks, `math` namespace (top-level, not `std::math` — see its log) | prelude + natives | 01 (shifts, for toHex) |
| 07 | `complete/techdesign-07-iteration.md` — **DONE 2026-07-10** | iterator protocol, `for..in` desugar, lazy `Seq<T>` | compiler + prelude | 05 (inference fix) |
| 08 | `complete/techdesign-08-system-natives.md` — **DONE 2026-07-10** (active engines; ELF halves frozen-superseded, F6 phase 2 = roadmap) | argv/env/exit, time, random, dirs, isatty, non-blocking connect + `connectTimeout` + send-drain, DNS, IPv6, `sysSpawn`/`Process` | natives + runtime loop (interp + LLVM) | — |
| 09 | `complete/techdesign-09-web-foundations.md` — **DONE 2026-07-10** | JSON, DateTime, encoding+digests, HTTP hardening | prelude (in-language) | 01, 04, 08 |

**Bugs** stay in `/bug.md` per the workflow; tracks that brush against open bugs note
them but do not own them. Status as of Track 01 landing (2026-07-06): bug.md #1 and #2
are FIXED (unrelated to Track 01, found and fixed in the same sweep); the ELF
charAt-append miscompile this doc's tracks reference was ALSO already fixed
(commit `53f3700`, predates this whole design set) — designs 03/04 citing it as live
are stale on that point. Bug.md #10 (int `/0`/`%0` silent-zero) is filed and
**awaiting an owner ruling** — do not fix it as a side effect of unrelated work without
that ruling landing first. The `int << int` silent-void bug is fixed (Track 01 F1).

---

## 2. File-ownership matrix and merge order

Nearly every language feature touches the same compiler files, so ownership is by
**region**, with a serialization rule for the compiler-file tracks.

### 2.1 The two file families

**Compiler files** (`Token.hpp/cpp`, `Lexer.cpp`, `Parser.cpp`, `Ast.hpp`,
`Checker.cpp`, `Eval.cpp`, `Lower.cpp`, `Ir.hpp`, `IrInterp.cpp`, `CGen.cpp`,
`LlvmGen.cpp`, `X64Gen.cpp`) — touched by tracks **01, 02, 03, 07**.

> **Merge-order rule:** compiler-file tracks land **in numeric order**:
> 01 → 02 → 03 → 07. A later track rebases on the earlier landing before starting
> its own compiler edits. Do not develop two compiler tracks against the same base
> concurrently — the token/AST/IR enums are append-points that conflict trivially
> but constantly. (Tracks may *design-read* and write prelude/test scaffolding in
> parallel while waiting.) **01 and 02 are both landed (2026-07-06)** — Track 03
> starts from the current tree as-is; read `complete/techdesign-01-literals-operators.md`
> §7 and `complete/techdesign-02-control-flow.md` §7 (their implementation logs)
> first. Things to know before touching the same files: (a) `Token.hpp`'s
> `TokenKind` now ends with `KwBreak, KwContinue, KwDo, KwUsing` — append new
> tokens after those, not before; (b) `Ast.hpp`'s `StmtKind` gained `Break,
> Continue, DoWhile` and `Stmt` gained `isUsing`/`usingClose` (F3) alongside
> Track 01's `Expr::isRawSegment` (F4) — `Rules.cpp::cloneExpr`/`cloneStmt`'s
> hand-rolled field-by-field copies (~line 1506 / ~1635) need EVERY new field
> copied through, or it silently drops through rule injection (bug found and
> fixed for `isRawSegment` in Track 01's session, and again for
> `isUsing`/`usingClose` in Track 02's — see their logs; `cloneStmt` was also
> found missing const.md's `isConst` in the same sweep, fixed as a one-liner,
> filed as bug.md #17 since it's untested); (c) `Lower.cpp` gained
> `loops_`/`usings_`/`chainRetReg_` per-function state, saved/cleared/restored
> around `lowerLambda`/`lowerPending` the same way `freshStructRegs_` already
> is — a new per-function Lower state field needs the same treatment or it
> leaks across function boundaries; (d) bug.md #16: constructing a class
> instance by direct inline `T x = T();` inside a function called repeatedly
> leaks its heap-tier fields on the ELF backend (pre-existing, found via
> Track 02's churn testing, unrelated to control flow) — route through a
> factory function instead, matching every churn corpus program's existing
> convention, until someone fixes `X64Gen.cpp`'s ARC internals.

**Prelude + native files** (`Resolver.cpp` kPrelude string, `RuntimeNatives.cpp`,
`CGen.cpp` native-core region at CGen.cpp:196, `X64Gen.cpp` genCallNative region at
X64Gen.cpp:1694) — touched by tracks **03, 04, 05, 06, 08, 09**. These divide by
**marked region** (class/namespace within the prelude):

| region | owner |
|---|---|
| `class string` + new `class StringBuilder` (kPrelude) + string natives (RuntimeNatives / CGen / X64Gen) | 04 |
| `class Array` / `class Map` / `class Pair` / new `class Set` + their natives | 05 |
| `class int` / `class float` masks + new `namespace math` + math natives | 06 |
| new `class char` / `enum` machinery / `class Block` + natives | 03 |
| `namespace std` sys-native declarations + `RuntimeNatives.cpp` sys region + `RuntimeLoop` + ELF syscall region | 08 |
| new `namespace json` / `DateTime` / `encoding` / HTTP classes (all in-language) | 09 |

Prelude tracks 04/05/06 may run **in parallel** (disjoint regions; append-only file
growth — whoever lands second rebases mechanically). 08 is independent of all
prelude tracks. 09 starts only after its dependencies land (see timeline).

**STALE as of the portable-backend pivot (found landing Track 06, 2026-07-06):**
the `X64Gen.cpp genCallNative region` clause above predates
`designs/complete/techdesign-portable-backend.md`'s owner-approved freeze ("`X64Gen.cpp` —
read-only reference material... never edit them"). Track 04 already hit this and
resolved it by never touching `X64Gen.cpp`, excluding ELF from its new natives'
corpus coverage instead (see its own implementation log). Track 06 built and
verified working ELF SSE support per this doc's original text before discovering
the freeze; the owner ruled to leave that already-complete work in place as a
one-time exception, not a precedent (see Track 06's implementation log). Track 05
landed clean under this rule too (its Set/Map ELF coverage needed zero X64Gen
edits — see its own log). **Tracks 03/08/09 (not yet landed): do not extend
`X64Gen.cpp`/`X64.hpp` for new work — treat this matrix's ELF-touching language
as void; follow Track 04's pattern (exclude ELF, isolated corpus dir) instead.**

### 2.2 Shared-file etiquette

- Additions to enums (`TokenKind`, `StmtKind`, `ExprKind`, `Op`, `VKind`) are
  **append-only at the end** of the enum — never reorder, never insert mid-enum
  (IR snapshots and switch tables elsewhere assume stability within a branch).
- `docs/reference.md` updates ship **in the same commit** as the feature (the `\r`
  gap taught us doc-lag is a real cost). Each track's design lists its reference
  sections.
- Regression tests: every feature adds a corpus program (`tests/corpus/*.ext` +
  `.expected`) exercising it on **all engines it claims**, plus checker tests in
  `tests/test_checker.cpp` for new compile errors.

---

## 3. Cross-track contracts (frozen — changes are escalation events)

- **C1 (`char` surface, owner 03):** `char` is a value primitive holding a Unicode
  scalar; `string.at(int) -> char` and `string.chars() -> Array<char>` belong to
  Track 03 (Track 04 must NOT add an `at` to string). `charAt` keeps returning
  `string` forever.
- **C2 (optional-returning natives, owner 04):** the pattern for `T?`-returning
  natives is `sysRecv -> string?` (already working on all engines incl. ELF).
  Track 04 proves it generalizes with `toInt() -> int?` and documents the recipe;
  03/08/09 reuse the recipe verbatim.
- **C3 (key equality, owner 05):** `Map`/`Set` keys compare: primitives by value,
  **structs field-wise (recursive)**, classes by identity. Engines' `keyEquals` is
  the single implementation point. Hashing is *specified* by 05 but *implemented*
  with the later Dictionary follow-up.
- **C4 (`Block` v1 API, owner 03):** `Block(int n)`, `length()`, `byteAt(int) ->
  int`, `setByte(int, int)`, `slice(int, int)` (aliasing view), `toString(int,
  int)`, `Block::fromString(string)`, `int32At/int64At` (+LE default). Tracks 08
  (random) and 09 (digests) code against exactly this; anything more is a contract
  change.
- **C5 (iteration dispatch order, owner 07):** `for..in` tries builtin fast paths
  (Range counted loop, Array/Map `IterLen`/`IterAt`) **first**, the `IIterable`
  protocol second. No track may reroute arrays through the protocol.
- **C6 (clock natives, owner 08):** `std::sysNow() -> int` (epoch ms, wall) and
  `std::sysMonotonic() -> int` (ms, monotonic). Track 09's DateTime consumes these
  names as-is.
- **C7 (unsigned-arithmetic idiom, owners 01+09):** there is no `>>>`. Digest code
  keeps all working values non-negative via mask-first (`x & 0xFFFFFFFF` before any
  `>>`); Track 01 guarantees `>>` is arithmetic shift on int64 and documents it.

---

## 4. Global protocols (apply to every track)

### 4.1 Probes before code

Each design lists **P-probes** — tiny `.ext` programs run before implementation to
verify an assumption against `build/lang` (the const.md/argv.md method: "verified
against this tree; nothing assumed"). If a probe fails, the design's
foreseeable-problems section says what to do; if none applies → STOP protocol.

### 4.2 STOP-and-escalate (mandatory)

If the design turns out wrong — an architectural choice is needed that the design
does not already make — the implementing agent must **STOP**: log findings in the
design's **Implementation log** section, commit WIP to the track branch, and
escalate to a Fable-class model for design correction. **Never improvise design
changes.** Specifically-flagged STOP points exist in each design.

### 4.3 Acceptance commands (the definition of done, per milestone)

```sh
cd build && cmake --build . -j$(nproc)          # clean build, zero warnings added
ctest                                            # 26/26 (or new count, all green)
bash tests/run_corpus.sh                         # oracle vs IR differential
bash tests/run_elf.sh                            # ELF engine corpus
bash tests/run_native.sh                         # emit-C++ corpus
python3 fuzz/churn_leak.py                       # heap discipline (13/13 + XFAIL)
# lcurl still green (the real-program smoke test):
echo '-v http://127.0.0.1:8099/' | build/lang --run --project examples/curl/project.mf
```

Engine-coverage policy follows the existing pattern: **oracle + IR are mandatory**
for everything; **emit-C++** for everything except the event-loop/system layer;
**ELF** for the whole language (deferred items need an explicit diagnostic, never
silence); **LLVM** scalar-core only (extend when trivial, else report uncovered).

### 4.4 Commit discipline

Per the repo's convention: commit per feature-milestone with the design name
prefixed (e.g. `stdlib-strings: split/replace/padStart (M1)`); no pushes without
the branch-sync ask; `src/` edits are in scope for these tracks (this is fixer-role
work, unlike the examples-agent rule).

---

## 5. Timeline (targets; gates have hard "done" criteria)

Start 2026-07-06. Estimates assume one implementer per track, tracks parallel where
§2 allows.

| phase | dates | tracks | gate criteria |
|---|---|---|---|
| **A — compiler floor** | Jul 6 – Jul 12 | 01, then 02 (serialized) | shifts/xor/`~` correct on all 5 engines; hex/underscore literals; interpolation; `break`/`continue`/`do-while`; corpus + churn green; reference.md updated |
| **B — stdlib** | Jul 8 – Jul 19 (parallel with A tail) | 04, 05, 06 (parallel) | `map<U>`/`reduce<A>` fixed; string toolkit + `toInt->int?`; Array/Map/Set complete; math live (ELF transcendentals deferred w/ diagnostic); lcurl's `ascii.ext` table-trick replaceable by `byteAt` |
| **C — types & iteration** | Jul 20 – Aug 2 | 03, then 07 | `char`/`enum`/`Block` on oracle+IR+ELF; `for..in` over user iterables; `Seq` pipeline demo green |
| **D — system floor** | Jul 20 – Aug 7 (parallel with C; disjoint files) | 08 | lcurl invocable as `lcurl <url>` with real exit codes (argv+exit landed); time/random live; spawn demo (run `echo`, capture output) on oracle+IR |
| **E — web foundations** | Aug 3 – Aug 21 | 09 | JSON round-trip corpus; digest test vectors (RFC/NIST) green; HTTP keep-alive + chunked-both-ways loopback corpus; DateTime httpDate round-trip |
| **Framework start** | ~Aug 24 | — | all gates A–E; suggested-features §15 Phase D complete |

Deferred items placed explicitly on the roadmap (not implicit): labeled
break/continue (02), generators/`yield` (07), Dictionary/O(1) maps + by-key joins
(05 follow-up), `decimal`, TLS stack (09 names the boundary), ELF DNS resolver
over an in-language UDP client (08 phase 2), typed `@Serializable` JSON via rules
(09 follow-up), type aliases (09 flags the decision).

---

## 6. Reading order for implementers

1. This file (contracts + merge order).
2. Your track's design, fully, including foreseeable problems.
3. `designs/suggested-features.md` §§ your track references (the *why*).
4. The landed exemplars: `designs/complete/const.md` and
   `designs/complete/imports.md` — the house style for probing, engine coverage,
   and regression scope. For "add a construct across all five engines," the
   `match` commit (d4bb851) is the extraction template.
