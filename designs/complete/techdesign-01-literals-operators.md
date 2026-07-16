# Track 01 — Literals & Operators

**Status:** **COMPLETE** (2026-07-06 — F1/F2/F3/F4 all landed; see §7). **Date:** 2026-07-05. **Depends on:** nothing (lands first).
**Source:** suggested-features.md §3.1–§3.3, §8 (bit ops), §14#1.
**Owns (regions):** `Token.hpp`/`Token.cpp` (token kinds), `Lexer.cpp` (whole file —
it is 182 lines), `Parser.cpp` expression region (`parseUnary` 475, `parseExpr` 498,
`parsePrimary` 348), `Checker.cpp` `typeOfBinary` (dispatch at 427) + op-name table
(lines 13–18), `Eval.cpp` `evalBinary` (836) + escape decoder (11–23), `Lower.cpp`
expression lowering + its escape decoder (~960), `IrInterp.cpp` Arith execution
(op strings at 527), `CGen.cpp` scalar binary emission (~264), `X64Gen.cpp` Arith
codegen (op table at 35–36), `LlvmGen.cpp` binary ops.

---

## 1. Features

### F1 — Integer bit operations: fix `<<` `>>`, add `^` and `~`

**Today (the bug):** `int x = 1 << 4;` compiles and runs with `x` empty — no error
anywhere (curl-design §2.8#1). `<<`/`>>` tokens exist (`LtLt`/`GtGt`, precedence
level 8) but only as *object transfer operators*; on `int` operands nothing types
or evaluates them, and the failure is silent — violating reference §3.7's own
loudness rule. `&`/`|` on int already work. `^` and `~` do not exist as tokens.

**Spec:**
- `int << int -> int` (shift left), `int >> int -> int` (**arithmetic** shift
  right — int is signed 64-bit; contract C7), `int ^ int -> int` (xor),
  `~int -> int` (complement, prefix).
- Shift counts: defined for `0..63`; counts outside that range **throw
  `RuntimeException`** ("shift count out of range") — loud, per §3.7, rather than
  inheriting x86's silent mask-to-6-bits or C's UB.
- Resolution stays by type (§1 of info.md): `<<`/`>>` on an *object* left operand
  still dispatches the `(<<)` operator method (streams unchanged); on `int` it is
  the shift. Same rule that already lets `+` be both int-add and `(+)`.
- `^` slots into precedence level 5 alongside `|` `&` (reference §3.1). `~` is a
  prefix operator alongside `!` `-`.
- **No `^` on bool** (use `!=`), no shifts on float — compile errors via the
  normal "no applicable operator" path.

**Implementation:**
1. `Token.hpp`: append `Caret`, `Tilde`. `Lexer.cpp` main switch (~line 101 area):
   add `'^'`, `'~'` cases (no compound forms; explicitly do NOT add `^=` in v1 —
   compound-assign parity can follow the pattern later if asked).
2. `Parser.cpp:475 parseUnary`: accept `Tilde` exactly as `Bang`/`Minus` are
   accepted (unary node with `op = Tilde`). Precedence table used by
   `parseExpr(minBP)` (~line 520 region): give `Caret` the same binding power as
   `Amp`/`Pipe`.
3. `Checker.cpp typeOfBinary` (dispatch at 427; the comment "comparisons/logical ->
   bool, arithmetic/concat -> same type" marks the rule site): add
   int-op-int → int for `LtLt`, `GtGt`, `Caret`; keep the object path (operator
   method lookup) when the left operand is a class type. Unary: `Tilde` on int →
   int, else error. Extend the op-name table at lines 13–18 with `"^"`, `"~"`.
4. `Eval.cpp:836 evalBinary`: int cases for shl/sar/xor with the range check;
   unary `Tilde` beside `Not`/`Neg` (~line 830 unary region).
5. **IR: no new ops needed.** `Op::Arith` carries `tk` (Ir.hpp:31); lower
   `Caret` through Arith like `Amp`/`Pipe`. Lower `~x` as
   `Arith(Caret, x, constMinus1)` — one less op for four backends to learn.
   Shift range check: lower emits the check? No — the check lives in each engine's
   Arith implementation for shift tks (see F1-P3).
6. `IrInterp.cpp` (~527 op-string region + arith exec): int shl/sar/xor + range
   throw. `CGen.cpp` (~264 — the note "LtLt/GtGt (object transfer operators) stay
   unmapped" is exactly the spot): emit `(a << b)` etc. for int operands with a
   preceding range-check-throw. `X64Gen.cpp` (op table 35–36 already numbers
   LtLt=13/GtGt=14): emit `mov cl, bl; shl/sar rax, cl` after a bounds check
   branch to the throw helper; xor is `xor rax, rbx`. `LlvmGen.cpp`:
   `CreateShl`/`CreateAShr`/`CreateXor`; range check as a compare+branch to the
   existing throw path (if LlvmGen lacks a throw path, report uncovered — its
   scalar-core policy).

### F2 — Numeric literal forms: `0xFF`, `0b1010`, `1_000_000`

`Lexer.cpp:39 lexNumber` currently scans decimal with an `isFloat` flag. Extend:
- `0x`/`0X` prefix → hex digits `[0-9a-fA-F_]+`; `0b`/`0B` → `[01_]+`. Result is
  an ordinary `IntLiteral` token (value parsed at the same place decimal is —
  find where the token text is converted; if conversion happens downstream in
  Eval/Lower, put one shared `parseIntLiteral(text)` helper in `Token.cpp` and
  call it from both, mirroring the escape-decoder consolidation in F3).
- `_` allowed **between digits** in all three bases; leading/trailing/adjacent-to-
  prefix underscores are a compile error with the literal's span ("misplaced digit
  separator").
- Malformed forms error loudly: `0x` with no digits, `0b2`, hex float (`0x1.5`).
- No octal. Decimal floats unchanged.

### F3 — String escapes: `\xNN`, document `\r`, consolidate the decoder

The escape decoder exists **twice**: `Eval.cpp:11–23` (oracle decodes at eval) and
`Lower.cpp:~960` (decodes into IR constants — which is what CGen/LLVM/X64
consume). That duplication is how `\r` ended up implemented-but-undocumented.

1. Extract one `std::string decodeEscapes(std::string_view raw, ...)` into
   `Token.cpp` (or a small `Escape.hpp`); call from both sites. Diff-test: corpus
   must be byte-identical before/after (pure refactor commit, separate from the
   feature commit).
2. Add `\xNN` (exactly two hex digits → that byte; strings are byte-clean through
   the floor, curl-design §2.4) and `\0`. Reject unknown escapes loudly? **No** —
   today unknown escapes pass through; keep that (compat) but document it.
3. `docs/reference.md` §1.4: document `\r`, `\xNN`, `\0`, `'` -vs-`"` equivalence,
   and pass-through behavior.

### F4 — String interpolation: `"...${expr}..."`

**Spec:** inside an ordinary string literal (either quote style), `${expr}` embeds
a full expression. Desugars **in the parser** to concatenation with `toString()`:

```
"code=${resp.status}!"   ≡   "code=" + (resp.status).toString() + "!"
```

- `\${` escapes a literal `${`. A bare `$` (no `{`) stays literal — no `$name`
  shorthand (avoids ambiguity; quasiquote holes `$name` exist only inside
  backticks and are untouched).
- Empty hole `${}` → compile error. Types lacking `toString()` → the normal
  "no such member" error at the hole's span (honest, no special case).
- Works in any string context; comptime strings interpolate fine (the desugar is
  pre-comptime, it is just concat).

**Implementation shape** (the layering decision, made here so the implementer does
not have to): the **lexer** keeps producing ONE `StringLiteral` token (raw text
preserved); the **parser**, when building a string-literal primary
(`parsePrimary`, 348), scans the decoded-escape-aware raw text for `${` holes.
For each hole it:
1. finds the matching `}` by tracking brace depth AND string-quote state inside
   the hole (so `"${m.at("k")}"` and `"${f(a, {})}"`-shaped nesting work);
2. sub-lexes + sub-parses the hole's source slice with a fresh
   `Lexer`/`Parser::parseExpr(0)` over that range, **offset so spans point into
   the real file** (diagnostics inside holes must carry true positions —
   span-attribution is a known soft spot; this sidesteps it because the text IS
   in the source);
3. builds the `+`-chain with `(hole).toString()` calls (a synthesized `Member` +
   `Call` around each hole expression).

Escape decoding order: hole scanning happens on the **raw** text (before escape
decode) so `\${` can suppress a hole; the literal segments then decode escapes as
usual through the F3 shared decoder.

---

## 2. P-probes (run before coding)

- **P1:** `int x = 1 << 4; console.writeln(x);` on all five engines — confirm the
  silent-void baseline (and that no corpus program depends on it).
- **P2:** `console.writeln("a" + (5).toString());` — confirms the desugar target
  shape types and runs everywhere (it does — this is the documented §9 idiom).
- **P3:** grep corpus + examples + prelude for `^`, `~`, `0x`, `_` in numbers, and
  `${` inside string literals: `grep -rn '\${' tests/ examples/ src/Resolver.cpp`.
  Expected: no hits (no back-compat exposure). Any hit → assess, note in log.
- **P4:** `writer << value` stream corpus (`tests/corpus/`, stream programs) still
  green after F1 — the object-path preservation check.

---

## 3. Foreseeable problems & solution strategies

| # | problem | strategy |
|---|---|---|
| 1 | **`<<`-on-int vs transfer-operator ambiguity** in generic/duck-typed contexts (a `T` operand): checker can't pick shift-vs-method until `T` binds. | Resolution is by *static* type at the use site, same as `+` today. Inside a generic body, `T << int` resolves at instantiation (duck-typed, HKT rule). No new machinery; add a checker test with both instantiations. |
| 2 | **x86 variable shifts require the count in `CL`**, and the IR's stack-slot model may have the count in any slot; clobbering `rcx` may collide with existing codegen temporaries. | Follow X64Gen's existing Arith pattern for div/mod (which already juggles fixed registers `rax`/`rdx` — find it by grepping `idiv` in X64Gen.cpp) and mirror its save/restore discipline for `rcx`. |
| 3 | **Where does the shift-range check live** — lowering (one place, all backends) or each engine? Lowering-emitted compare+throw bloats IR for every shift. | Engines implement it inside their Arith handling (4 small copies), matching how div-by-zero is presumably handled (verify: probe `1/0` behavior first; mirror whatever discipline exists — if div-by-zero is currently silent/UB, STOP: that is a pre-existing loudness gap worth an owner ruling, fix both the same way). |
| 4 | **Interpolation hole scanning vs escapes**: scanning decoded text loses `\${`; scanning raw text must not mis-count braces inside escaped quotes (`"${s.contains("\"}")}"`). | Scan raw text with a tiny state machine: in-hole tracks (brace depth, in-string quote char, escape-pending). It is ~30 lines; unit-test it directly in `tests/test_parser.cpp` with the pathological cases before wiring. |
| 5 | **Sub-parse span offsets**: `Lexer` may assume it starts at offset 0 of a Source; hole slices start mid-file. | Check `Source.hpp`/`Lexer` for an offset base; if absent, add a `baseOffset` to the Lexer constructor (additive change) so all token spans shift — this is also useful to Rules/quasiquotes later. If Lexer is structurally offset-0-bound, fallback: lex the whole file once and *re-slice the token stream* by hole span instead of re-lexing (parser-level splice). |
| 6 | **`(expr).toString()` on a value that is already `string`** — wasteful copy per hole. | `string.toString()` is `=> this` (Resolver.cpp:37) — semantically free; let the optimizer question wait. No special case in v1. |
| 7 | **Interpolation inside comptime/quasiquote contexts** — backtick templates must NOT interpolate `${` (their `$name` holes are a different system). | Quasiquotes lex via the dedicated backtick path (Lexer.cpp:62 region) and never reach the string-literal primary — verify with a rules-corpus run (`tests/` meta tests) and add one guard test. |
| 8 | **Corpus churn from new tokens**: `tests/test_lexer.cpp` golden expectations may enumerate token kinds. | Append-only enum rule (overview §2.2); update lexer tests in the same commit. |

---

## 4. Milestones & acceptance

| M | deliverable | accept |
|---|---|---|
| M1 | F1 shifts/xor/`~` on oracle+IR; checker errors for float/bool misuse; range-throw corpus test | build + ctest + `run_corpus.sh`; new `tests/corpus/bitops.ext` (shl/sar/xor/`~`, boundary counts 0/63, catch of count=64) |
| M2 | F1 on CGen/ELF/LLVM (or LLVM uncovered-report) | `run_native.sh`, `run_elf.sh` incl. bitops.ext |
| M3 | F2 literals + F3 escapes (refactor commit, then feature commit) | lexer unit tests; corpus `literals.ext`; reference §1.4 updated |
| M4 | F4 interpolation, all engines (desugar = free coverage) | `interp.ext` corpus (nesting, escapes, member holes, match-expr hole); lcurl still green; reference §1.4 + §3.2 updated |

Target: **Jul 6 – Jul 9** (M1–M2), **Jul 10 – Jul 12** (M3–M4). Gate A depends on
this track finishing on time; interpolation (M4) may slip behind Track 02's start
without blocking (different Parser regions — statement vs expression — but flag it
in the log so 02 rebases carefully).

---

## 5. Reference-doc duty

reference.md §1.4 (literals incl. hex/binary/underscore/escapes/interpolation),
§1.5 (add `^ ~`), §3.1 (precedence rows), §6.1 (int gains nothing here — bit ops
are operators, not methods; but document shift semantics + range rule in §3.5 or a
new §3.5b "integer operators").

## 6. STOP conditions

- Probe P3 finds real `${`/`^`/`~` usage in the wild.
- Div-by-zero probe (problem #3) reveals a silent-UB baseline.
- Lexer cannot carry a base offset and token-stream re-slicing (problem #5
  fallback) also fights the parser structure.
- Any need to add a new IR op (design says none is needed — needing one means the
  design missed something).

## 7. Implementation log

**2026-07-06 (later session) — F1/F2/F3/F4 ALL LANDED. Track complete.**
Commits: `e917a91` (F1, all 5 engines), `48f4752` (F2), `f5873d8` (F3),
`77f524d` (F4). Every milestone's acceptance suite green at each step
(ctest 26/26, corpus 35/35 on ELF, native, churn 13/13, 19/19 projects,
lcurl unregressed at every checkpoint). Deviations from the design, found
during implementation:

- **F1 shift-range-check placement (problem #3):** landed exactly as
  designed (each engine's own Arith handling), independent of the div/mod
  question — see the STOP entry below, it turned out not to block this.
- **F1 CGen gate:** the design didn't anticipate that enabling the LtLt/GtGt
  opcodes unconditionally in CGen's `ar()` dispatcher would also un-gate the
  OBJECT-dispatch path for those same tokens (stream `(<<)`/`(>>)`), which
  has a real latent bug on this backend (confirmed: `streams.ext` hangs).
  Fix: gate on `in.decl` (null only for the primitive path — the checker
  already computes this distinction) so object dispatch keeps its original
  "unmapped -> coverage skip" behavior untouched.
- **F1 ELF `int_to_str` INT64_MIN:** not in the design at all — found via the
  corpus test's own `1 << 63` case. `neg(rax)` can't fix INT64_MIN (two's
  complement asymmetry); fixed as an immortal data-segment literal, the same
  shape `float_to_str` already uses for nan/inf.
- **F2 float-literal parsing:** the design says "decimal floats unchanged,"
  but the lexer change (digit separators in ALL three bases, including a
  float's fractional part) meant `atof` — which stops at the first `_` —
  would silently truncate `1_000.000_5` to `1.0`. Extended the shared-parser
  fix (`parseFloatLiteral`) to match, since "lexer accepts it, value-parser
  doesn't" is exactly the silent-corruption shape this whole session was
  about eliminating.
- **F2 CGen INT64_MIN warning:** a new g++ warning (not an error, but the
  track's own acceptance bar is zero new warnings) from spelling the literal
  the C++-illegal way; fixed with the `<climits>` idiom.
- **F3 emit-C++ escapeString:** `\0`/`\r` (now reachable via literal escapes
  for the first time) broke the GENERATED C++ source outright (unescaped
  control bytes inside a string literal). Generalized to hex-escape any
  non-printable byte, split into a fresh literal each time (`\x` is greedy in
  C++ and would otherwise swallow a following hex-digit character into the
  same escape). Once that compiled, the VALUE was still wrong — `vs(const
  char*)` is strlen-based and truncated at the embedded NUL; switched string
  constants to `vstr(std::string(ptr, len))` with the length already known
  at codegen time.
- **F3 LLVM, deliberately NOT fixed:** the identical class of bug exists in
  NativeRuntime.cpp, but systemically (`LV.s` is a bare `const char*` with no
  length field anywhere in the runtime; concatenation itself runs through
  strlen/strcpy/strcat) — not a one-line fix like CGen's. LLVM's curated
  corpus (`tests/corpus/core`) doesn't exercise embedded NULs and still
  passes unchanged. Left as a known, scoped-out gap; would need a length
  field added to `LV` and every string op rewritten, its own effort.
- **F4 lifetime bug (caught before it shipped):** the first implementation
  draft synthesized `quote + content + quote` text for each literal segment
  and pointed `Expr::text` at it — a dangling-view bug waiting to happen
  (worse for a hole containing its own nested interpolated string, where a
  SUB-parser's local string pool would need to outlive the sub-parser
  itself). Redesigned to store bare content as a real view into the shared
  source buffer (`Expr::isRawSegment` tells Eval/Lower to skip quote-
  stripping) — no synthesis, no pool, no lifetime question at all.
- **F4 Rules.cpp `cloneExpr` bug (found via the full ctest, not by design):**
  the rule-injection clone path copies Expr's bool flags by hand, one at a
  time, and had no line for the new `isRawSegment` — a cloned raw segment
  silently lost the flag and got quote-stripped a second time, eating its
  first/last byte. Caught because `tests/corpus/meta/` (a directory outside
  the general `tests/corpus/` this session had been diff-testing against)
  is part of `ctest` and failed on the first post-F4 run. One missing line,
  fixed; the general lesson — corpus byte-identical checks against ONE
  directory don't substitute for the full `ctest` run — is why every
  milestone after this one ran the complete suite, not just the general
  corpus, before committing.
- **F4 `.toString()`-missing-member (design vs. reality):** the design's
  prose assumed this would be a checker-time "no such member" error; it's
  actually a RUNTIME `RuntimeException` — verified this is pre-existing
  behavior for any hand-written `.toString()` call on a class without one,
  not something this feature regressed or should independently fix.

Every deviation above was implemented and verified in-session (none needed
owner escalation) except the two items explicitly logged as deferred
(LLVM's NUL-string gap; bug.md #10's div/mod ruling, unchanged from before).

---

**2026-07-06 (earlier session) — probe phase run; feature work STOPPED by
owner instruction ("fix all the bugs then stop work"). No Track 01 features
were implemented at that point (see the entry above for what came after).**

Probe results (all against `build/lang` of Jul 6, branch agent3):

- **P1 (shift silent-void):** confirmed on oracle/IR — `int x = 1 << 4;`
  printed nothing, no error. No corpus program depended on it. **Superseded:**
  the loudness half of F1 landed as a bug fix (commit `3e42988`) — undefined
  primitive operators (incl. `<<`/`>>` on int) are now COMPILE ERRORS. When F1
  proper (real shifts) is implemented, replace the whitelist rejection for
  int `<<`/`>>`/(new) `^` with the int-op-int→int type rule per §1 — the
  whitelist lambda `primOpOk` in `Checker.cpp::typeOfBinary` is the one place
  to touch.
- **P2 (concat/toString desugar target):** works everywhere, as documented.
- **P3 (back-compat grep):** no `^`/`~`/hex/underscore-numeric/`${` usage in
  corpus, examples, or prelude. lcurl's ascii.ext contains `^`/`~`/`_` only
  inside string literals (the ASCII table) — no exposure.
- **P4 / problem #3 (div-by-zero discipline):** probe revealed int `/ 0` and
  `% 0` silently return 0 on ALL engines (deliberate, hand-mirrored guards) —
  the design's STOP condition. Owner ruling 2026-07-06: keep div/mod as-is;
  filed as **bug.md #10** for a separate decision. Consequence for F1: the
  shift-range check (counts outside 0..63 throw) has no existing discipline to
  mirror — implement it as designed (engines' Arith handling), and note the
  deliberate asymmetry with div/mod in the commit.
- **Float-arithmetic discovery (out of Track-01 scope, fixed immediately per
  owner instruction):** float `+ - * /`, comparisons, and unary minus were
  broken on ALL FIVE engines (computed on the int payload). Fixed in commits
  `be3ec6c` (arithPrim + Eval/IrInterp/CGen/NativeRuntime) and `d5ac03e` (ELF:
  SSE2 encoders in X64.hpp, genAr float branch, float_to_str fixup -72,
  ts_build/toString/Neg). **Track-01 consequence:** X64.hpp now HAS an SSE
  substrate; `tests/corpus/floats.ext` pins float behavior the F1 work must
  not disturb. The design's problem-#2 register note (CL for shifts) stands.
- **Stale-claim correction:** the ELF charAt-append bug this design references
  (§2 "the ELF charAt-append miscompile") was FIXED by commit `53f3700`
  (Jul 4) — verified by probe on this tree (`abc`, correct). Designs 01/03/04
  citing it as live are stale on that point; the multi-char-slice idiom in the
  prelude remains good practice but is no longer load-bearing.
- **Related bug fixes landed in the same sweep** (session context for whoever
  picks Track 01 up): bug.md #1 (block-scoped namespace imports in lowering,
  `d51366a`) and bug.md #2 (bare top-level globals as real globals, `511ec59`).

Status: F1 loudness-half done (as a bug fix); F1 shifts/xor/`~`, F2, F3, F4
**not started**.
