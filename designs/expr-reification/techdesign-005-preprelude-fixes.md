# Tech Design LA-31 — 005: Pre-prelude compiler fixes (qualified `match` patterns, top-level `match`, qualified-type printing)

**Status:** DESIGN — approach ruled by the owner in-session 2026-07-18; ready to
implement. **Position in the set:** stage "0.5" — inserted between doc 01's
already-landed prelude surface (Deliverables A+B: `kPreludeExpr`, `like`/`ilike`)
and doc 01's corpus deliverables (B-corpus §2.4, C §3), which **resume only after
this lands**. Doc 02 (the reifier) is unaffected in content but sequenced after.
**Owner of record for the defects:** `known_bugs_2.md` #84, #85 (both deleted by
this design's landing commit, per the register protocol).
**Window:** 2026-07-18 → 2026-07-19 (before doc 01's resumed window).

Read `docs/footguns.md` at kickoff (quality-gates policy).

---

## 1. The defects (all found implementing doc 01 Deliverable C)

- **D1 (bug #85) — a namespace-qualified type in `match`-arm position is parsed
  as a value pattern and silently never matches.** `Parser::parseMatch`
  (Parser.cpp:458-463) routes **every** arm headed `Identifier ::` to the value
  route (`arm.value = parseExpr(2)`) — the Track 03 §2 choice that makes
  enum-member arms (`Method::GET =>`) parse. `expr::Field` and `Method::GET`
  are token-identical, so the parser guesses "value" — and **no later pass
  validates the guess**: the Checker types the pattern but never compares it
  against the subject (Checker.cpp:1017-1043), and the oracle's `matchesValue`
  (Eval.cpp:144-160) compiles it to an equality test against a type-value that
  is constant-false. Net behavior: the **oracle silently takes `else`** (exit
  0); IR/emit-C++/LLVM **error loudly** ("not yet lowerable: name 'ns'") on the
  same program — the engines diverge and the oracle's output is the wrong one.
  This blocks the exact `match (n) { expr::Field => …; expr::Bin => …; }` shape
  doc 01 §3.1's `expr::eval` walker (and doc 03's differential corpus) is built
  on.
- **D2 (bug #84) — a top-level bare `match` statement requires a trailing `;`.**
  `parseStatement`'s default case special-cases a block-terminated `match`
  (Parser.cpp:1072-1075, `accept` not `expect`); `parseTopLevelItemInner`'s
  bare-expression-statement tail (Parser.cpp:1984-1988) lacks the same special
  case, so a top-level `match { … }` followed by another statement fails with a
  misleading "expected ';'" pointed at the *next* statement.
- **D3 — `AstPrinter`'s `typeStr` drops `TypeRef::path`** (AstPrinter.cpp:50-65,
  Named case prints only `t->name`). Latent today for every qualified type in a
  printed position (`catch (std::RuntimeException)` prints as
  `RuntimeException`); it becomes load-bearing once D1's fix produces qualified
  `arm.type` nodes — `--expand` output must round-trip them.

## 2. The design principle (owner ruling, 2026-07-18)

`NS::Type` and `Enum::Member` are the same token shape; the parser fundamentally
cannot classify them. So: **parse neutrally, classify semantically, fail loud on
the residue.**

1. **Parser: stop deciding** — keep the neutral value-route parse for
   qualified-head arms, except the one parse-time-provable case (`<` after the
   chain ⇒ type; enum members and consts never take generic arguments).
2. **Resolver: classify by what the name is** — in `resolveExprTypes`'s Match
   branch, the pass **all four engines share** (runs over prelude + program,
   Resolver.cpp:6680-6681, before Eval and Lower). A qualified chain rooted at
   a **namespace** whose leaf resolves to a **type** becomes a type pattern
   (`arm.value` → `arm.type`); a leaf resolving to a **value** (enum-style
   const, namespace global) stays a value pattern; both ⇒ hard error ("refuse
   to guess", reference §3.4); neither ⇒ left as a value pattern (the existing
   loud paths at check/run time apply).
3. **Checker: make the residue loud** — a value pattern that types as a
   `TypeValue` can never match anything; that is now a compile error, so any
   future misclassification in this family fails at compile time instead of
   silently taking `else`.

No new syntax. `Method::GET =>` (root is a class), `float::NaN =>` (root is a
primitive), `Box<int> =>` and every bare-head arm (routed to `parseType` today)
are all untouched by construction.

## 3. Rulings (all closed — nothing left open at implementation time)

- **R1 — parser `<`-lookahead.** In `parseMatch`'s pattern branch, when the arm
  head is `Identifier ::`: scan `j` over the chain (`while tokens[j]==Ident &&
  tokens[j+1]==ColonColon: j+=2`); if `tokens[j]==Ident && tokens[j+1]==Lt`,
  route the arm to `parseType()` (which natively handles the qualified head,
  generics, `?`, and `|` union suffixes). Otherwise the value route
  (`parseExpr(2)`) stands. Consequence, accepted: a comparison expression can
  no longer begin a qualified-head arm (`k::LIMIT < 5 =>` becomes a parse
  error). It was never meaningful surface — a pattern is compared to the
  subject by equality, so a `bool`-valued comparison pattern could only ever
  match a `bool` subject by accident; no corpus/package/example uses one
  (verified by grep).
- **R2 — resolver classification.** In `Resolver::resolveExprTypes`'s Match
  branch (Resolver.cpp:5731-5740), before the existing `arm.value` descent:
  attempt reclassification of a **pure `::`-chain** pattern (every link
  `Member` with `colon=true`, root a `Name`; any `.` link disqualifies).
  Navigate exactly as `resolveType`'s Named case does (Resolver.cpp:5673-5695):
  root segment found by walking the scope chain for a `SymbolKind::Namespace`
  (this includes `uses`-overlay names and `use NS::Sub as A` aliases — imports
  bind the same Symbol under another name); middle segments via `findLocal` on
  the namespace scope; leaf via `localLookup` on the final namespace's own
  scope. Then:
  - leaf has a **type** symbol (`isTypeKind`) and no value symbol → build a
    `TypeRef{path=segments, name=leaf}`, run `resolveType` on it (fills
    `canonical` + `resolvedSymbol`), store it in `arm.type`, clear `arm.value`;
  - leaf has a **value** symbol (Var/const/enum-member global) and no type →
    leave as a value pattern;
  - leaf has **both** → error: `ambiguous match pattern '<spelled>': names both
    a type and a value`;
  - root is not a namespace, or navigation misses → leave as a value pattern
    (untouched; the Checker backstop / runtime paths stay loud).
  The probe is diagnostic-free until the type decision is made — only the
  final `resolveType` call (guaranteed to succeed) and the ambiguity error emit
  anything.
- **R3 — `|` union patterns on the value route.** `|` binds at bp 5 > the arm
  route's bp 2 (Parser.cpp:60), so `ns::Sub | None =>` on the value route
  parses as a `Binary(Pipe)` tree. Classification: collect the tree's leaves
  (each a `::`-chain `Member` or a bare `Name`). If **any** leaf classifies as
  a type under R2 (bare `Name` leaves classify by scope-chain lookup —
  `isTypeKind` symbol or `None`), then **all** must — build a Union `TypeRef`
  of the per-leaf refs, `resolveType`, store in `arm.type`; a mixed tree is an
  error: `mixed type/value '|' match pattern`. If no leaf classifies as a type,
  the tree stays a value pattern (a bitwise-or of int consts remains legal).
- **R4 — Checker backstop.** In the Match value-arm branch (after `typeOf` at
  Checker.cpp:1023): if the pattern's type has `kind == TKind::TypeValue`,
  error — exact message:
  `match pattern is a type ('<canonical>') used as a value — this arm can never match`.
  Reachable in checked code via class-rooted static-side reads (`C::field`,
  `C::T` on a generic class); zero legitimate-value regression risk (no value
  pattern types as `TypeValue`; enum members type as the enum's value struct,
  `float::NaN` as `float`).
- **R5 — printer.** `typeStr`'s Named case (AstPrinter.cpp:50-65) emits the
  `path` prefix: for each segment, `seg + "::"` before the name. One function
  serves both the `--ast` dump and the `--expand` source printer (call sites
  at AstPrinter.cpp:137 and 509), so one fix covers both. Golden churn from
  this change may consist **only** of qualified spellings appearing where bare
  ones printed before; the `--expand`→recompile round-trip harness must stay
  green (a qualified spelling re-parses to the identical resolved arm). Any
  other diff is a STOP.
- **R6 — top-level `match` statement parity (bug #84).** `parseTopLevelItemInner`'s
  bare-`ExprStmt` tail (Parser.cpp:1984-1988) mirrors `parseStatement`:
  `if (s->expr && s->expr->kind == ExprKind::Match) accept(Semicolon); else
  expect(Semicolon)`.
- **R7 — reference update.** `docs/reference.md` §3.15 gains one normative
  sentence: *"A `::`-qualified name in arm position resolves by symbol: a
  namespace-qualified **type** is a type pattern; an enum member or
  namespace-qualified constant is a value pattern; a name that is both is a
  compile error."*
- **R8 — canonical-spelling identity is unchanged.** Exhaustiveness compares
  canonical strings; a subject union spelled with bare members (`Sub | None`
  under `uses ns;`) and an arm spelled qualified (`ns::Sub`) have distinct
  canonicals and do not satisfy each other — an `else` is then required, same
  as any other spelling mismatch today. Documented, not "fixed" (pre-existing
  identity rule, out of scope).
- **R9 — register sweep.** The landing commit deletes known_bugs #84 and #85
  (entries + standings rows) and their two `docs/footguns.md` rows. No debt
  sites exist — no workaround code was ever committed (the doc 01 corpus files
  had not been written yet; scratchpad probes don't count).
- **R10 — prelude audit.** Before landing, grep all six `kPrelude*` segments
  for qualified-head arm patterns (`::`-chain immediately before `=>` inside a
  `match`). Expected: none (prelude matches use bare/local names). If one is
  found **and** reclassification would change its behavior: STOP and escalate
  — that is a latent prelude bug needing its own ruling, not a silent ride-along.

## 4. Changes by file

| file | change | anchor |
|---|---|---|
| `src/Parser.cpp` | R1 `<`-lookahead in `parseMatch`; R6 top-level `match` semicolon parity | 454-463; 1984-1988 |
| `src/Resolver.cpp` | R2/R3 arm reclassifier in `resolveExprTypes`'s Match branch (this design's only Resolver region — doc 01's prelude string segments are untouched here) | 5731-5740 |
| `src/Checker.cpp` | R4 TypeValue backstop in the Match value-arm branch | after 1023 |
| `src/AstPrinter.cpp` | R5 `typeStr` path prefix | 50-65 |
| `docs/reference.md` | R7 §3.15 sentence | §3.15 |
| `tests/corpus/match_qualified.lev` + `.expected` | §5 positive corpus (top-level file ⇒ auto-included in `corpus_treewalk`/`corpus_ir`/`corpus_native`/`corpus_llvm_full`/verify/memverify lanes — no CMake row needed) | new |
| `tests/negative/match_type_as_value.lev`, `tests/run_match_pattern_error.sh`, `CMakeLists.txt` (one `add_test` row, name `match_pattern_error`) | §5 negative pin | new |
| `known_bugs_2.md`, `docs/footguns.md` | R9 sweep (delete #84/#85 entries + rows) | landing commit |

Sequencing note: doc 02 (S2) later owns `src/Checker.cpp` wholesale; 005 lands
first and completes before S2 starts, so file ownership stays disjoint **in
time** (the standing parallel-tracks rule is about concurrent tracks).

## 5. Tests

### 5.1 Positive corpus — `tests/corpus/match_qualified.lev`

Top-level `.lev`; rows and their pinned output lines (byte-exact `.expected`,
all lanes):

| # | construct | pins | output |
|---|---|---|---|
| 1 | `namespace k1 { class Base {} class Sub : Base { new Sub() {} } }`; `k1::Base b = k1::Sub();` `match (b) { k1::Sub => …"r1:sub"…; else => …"r1:base"…; };` | D1 core: qualified type arm dispatches | `r1:sub` |
| 2 | `k1` also holds `class Box<T> { T v; new Box(T x) { v = x; } }`; subject `k1::Box<int> \| string x = k1::Box(7);` arm `k1::Box<int> => "r2:box"; string => "r2:s";` (no `else` — pins canonical agreement + closed-union exhaustiveness over a qualified generic) | R1 `<`-lookahead; qualified generic canonical | `r2:box` |
| 3 | `namespace k2 { const int LIMIT = 7; }` `match (7) { k2::LIMIT => "r3:const"; else => "r3:no"; };` | R2 value-leaf stays a value pattern | `r3:const` |
| 4 | local `enum Method { GET, POST }`; `match (m) { Method::GET => "r4:g"; Method::POST => "r4:p"; }` — **no `else`** | class-rooted arms untouched ⇒ enumCovered bookkeeping + exhaustive-without-else regression pin | `r4:g` |
| 5 | `uses k1; Base b2 = Sub(); match (b2) { Sub => "r5:bare"; else => "r5:no"; };` | bare-head route regression | `r5:bare` |
| 6 | `k1::Base? ob = None; match (ob) { k1::Sub \| None => "r6:u1"; else => "r6:u2"; };` | R3 union-with-qualified-head on the value route reclassifies | `r6:u1` |
| 7 | top-level bare `match (1) { 1 => …"r7:m1"…; else => …"r7:mx"…; }` with **no trailing `;`**, immediately followed by `console.writeln("r8:after");` | R6 / bug #84 | `r7:m1` then `r8:after` |

`.expected`: the eight lines above in row order. All `console.writeln`, no
handshakes, no timing.

### 5.2 Negative pin — `tests/negative/match_type_as_value.lev`

```
class C { int f; new C() { } }
C c = C();
match (c) {
    C::f => console.writeln("x");
    else => console.writeln("y");
};
```

`tests/run_match_pattern_error.sh` (mirrors `run_regex_comptime_error.sh`):
compile must exit non-zero and stderr must contain the substring
`used as a value — this arm can never match`. CMake row `match_pattern_error`.
(This is the R4 backstop's reachable form: `C::f` is class-rooted, so R2 leaves
it a value pattern; `typeOfMember` types it `TypeValue` via the static-side
passthrough, Checker.cpp:1235-1239. Today it silently takes `else`.)

## 6. Acceptance

1. §5.1 corpus green on oracle + IR + emit-C++ + LLVM (the auto-lanes); §5.2
   negative green.
2. Full pre-existing ctest suite green. Golden churn, if any, consists only of
   R5 qualified spellings; every churned golden is enumerated in the
   implementation log (§8). The `--expand` round-trip harness stays green.
3. R10 prelude audit recorded in the log (expected: "none found").
4. Registers swept (R9), reference sentence landed (R7).
5. Doc 01's §3.2 hand-built-tree probe (the `expr::Field =>` / `expr::Bin =>`
   dispatch smoke from the session scratchpad) now prints the correct arms on
   the oracle — the concrete unblock proof. (The real corpus files land with
   doc 01's resumption, not here — file ownership stays doc 01's.)
6. Committed and pushed to master; doc 01 Stage 1 resumes at Deliverable
   B-corpus/C.

## 7. STOP conditions (do not improvise past any of these)

- R10's prelude audit finds a qualified-head arm whose behavior would change.
- Golden churn beyond qualified-spelling diffs.
- The classifier turns out to need more scope than R2/R3 specify (dot-chains,
  class-nested types as type patterns, alias edge cases beyond `use … as`) —
  those are explicitly **not** in scope; a program that needs them keeps
  today's behavior (value pattern + backstop error). File a request instead.
- Any engine disagrees with the oracle on the §5.1 corpus after the fix.

## 8. Implementation log

- 2026-07-18: all file/line anchors re-verified against master after rebasing
  over 42bf540 (block-scoped-use substrate — Scope now also carries a
  type-keyed `binds` table; `findLocal`/`localLookup` navigation used by R2 is
  unchanged). Checker/Resolver anchors shifted and were updated; Parser and
  AstPrinter anchors unmoved.

(Append findings, golden-churn enumeration, the R10 audit result, and the
completion note here.)
