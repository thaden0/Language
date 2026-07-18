# Track 03 — Core Types: `char`, `enum`, `Block`

**Status:** ready. **Date:** 2026-07-05. **Depends on:** Tracks 01, 02 landed
(compiler-file merge order); Track 01's hex literals are a soft dependency for
pleasant Block/char test-writing.
**Source:** suggested-features.md §2.1, §2.2, §2.3, §10; contracts C1, C4 (overview §3).
**Owns:** new prelude classes (`char`, `Block`) + enum machinery; `Token.hpp/cpp`,
`Lexer.cpp` (char-literal path), `Parser.cpp` (enum declaration), `Resolver.cpp`
(desugar + prelude), `Checker.cpp` (literal target-typing, enum exhaustiveness),
`RuntimeValue.hpp` (`VKind` append), `RuntimeNatives.cpp` (char/Block/string natives),
`Eval.cpp`/`IrInterp.cpp` (value plumbing), `CGen.cpp` (native cores + value model),
`X64Gen.cpp` (new tag, Block allocator/ARC, natives). LLVM: uncovered-report policy.

**Currency check (2026-07-06 — quick pass against the post-Track-01 / post-LLVM-parity
tree; body left as written, read these deltas first):**

- **Track 01 landed 2026-07-06** (hex/binary literals, `\xNN` escapes, interpolation) —
  the soft dependency is met. Track 02 has NOT landed; the hard dependency stands, this
  design stays queued behind it.
- **bug.md #2 is FIXED** (2026-07-06 sweep, see overview §"status" and bug.md history).
  P2's fallback contingency (problem #3, "budget for fixing the underlying bug") is
  likely already paid — run the P2 probe anyway, but expect it to pass.
- **STALE: the "LLVM: uncovered-report policy" line in the Owns block.** It predates
  the portable-backend pivot reaching LLVM parity (doc-1 A-M5..A-M7, all landed by
  2026-07-06). The LLVM lane is now the primary backend with mandatory-green ctest
  lanes (`corpus_llvm`, `corpus_llvm_full`, churn-llvm), and new corpus files
  (chars.ext / enums.ext / blocks.ext) will run there. Read: LLVM is full-coverage for
  this track, not uncovered-report. Enum costs nothing extra (desugars to struct+int).
- **NEW cross-track constraint (the one real plan change):** `char`/`Block` on the
  LLVM lane mean new value tags in **runtime v2's ABI** — `runtime/lv_abi.h`'s LV_*
  enum is a closed 0–9 set, and Block additionally needs retain/release/recursive-free
  and to_string rows in `runtime/lv_runtime.c`. Both files are Track B-owned and §2
  ABI changes are STOP-gated (techdesign-portable-backend-2.md §0.2: "contract changes
  are always STOP events"). A Fable-ratified ABI addendum (LV_CHAR immediate tag;
  LV_BLOCK heap layout with the slice-retains-parent edge) must land before M1's LLVM
  leg and M3/M4 — do not add tags unilaterally. The ELF tag additions in §1.2/§3.2 are
  unaffected (X64Gen edits by language tracks follow Track 01's landed precedent).
- **RULING (2026-07-06, ABI addendum — shape ratified, implementation DEFERRED):**
  the addendum's *shape* is decided (below) so Track B can implement it, but
  char/Block get **LLVM-excluded-lane treatment for v1** (like ELF under the X64Gen
  freeze) rather than extending the Track-B contract in this pass — extending a
  closed 0–9 set that this track does not own and cannot fully land+verify in
  `lv_runtime.c` this session would ship a half-implemented contract, which is worse
  than a logged deferral.
    - **LV_CHAR** = tag 10, a pure immediate: `payload` = Unicode scalar; no heap,
      no ARC; retain/release no-op; `lv_runtime.c` to_string row = UTF-8 encode.
      Trivial, but still extends the closed contract → left for a Track-B co-landed
      pass.
    - **LV_BLOCK** = tag 11, heap type: header `{rc, meta}` + body `{parentPtr,
      off, len, dataPtr}`; a slice sets `parentPtr` to the root and retains it;
      recursiveFree releases `parentPtr`; to_string row = `Block(len=N)`. Touches
      the allocator prefix contract (§8 STOP condition 3) → genuinely Track-B-sized.
    - **Consequence for this track:** **enum is full-coverage** (no tag). **char and
      Block ship on oracle + IR + emit-C++ (CGen) ONLY** — their corpus dirs get NO
      ELF lane (X64Gen frozen) and NO LLVM lane until the addendum lands. Deliberate,
      logged deferral — not an implicit gap. M1's LLVM leg and M3/M4's native legs
      defer with it. The stale "LLVM is full-coverage for this track" line above
      holds ONLY for enum.
- Minor: `Lexer.cpp` is now 227 lines, not 182 (Track 01 grew it); the "either is
  fine" choice in §1.2 item 3 stands. Spot-checked anchors still valid:
  `Resolver.cpp:15` (`class int`), CGen op-switch region, Parser value-pattern site.

The three features are one track because they are all "teach every layer a new
kind of value/type" work — the same person doing all three amortizes the layer
tour (contract: overview §1, "add all the types while working the section").

---

## 1. `char` — Unicode scalar value primitive

### 1.1 Spec

- `char` is a value primitive (object mask, §9 of info.md) holding one Unicode
  scalar (`0..0x10FFFF` minus surrogates), unboxed, default `'\0'` (bare
  declaration auto-constructs to scalar 0).
- Methods (prelude `class char`):
  `int code()` (native), `string toString()` (native: UTF-8 encode),
  `bool isDigit()/isAlpha()/isSpace()/isUpper()/isLower()` (in-language over
  `code()` for ASCII ranges; non-ASCII returns false in v1 — documented),
  `char toUpper()/toLower()` (in-language, ASCII-only v1).
  **Factory — RULING (2026-07-06): homed as the `std::` free function
  `std::charFromCode(int) -> char`** (native; out-of-range/surrogate →
  RuntimeException), NOT a static method: class static-side fields/functions do not
  exist (§2.2 ruling), and `std::` free functions are the documented house
  precedent for static-like factories absent a `static` keyword (reference
  §557-559, `std::byteToString`). A labeled ctor `char::fromCode` was rejected —
  char is a primitive-mask class whose ctor story is "auto-construct to scalar 0",
  and one factory does not justify the primitive-mask-ctor risk. char is otherwise
  all instance methods + literal target-typing + comparisons; it needs no static
  side.
- Operators: `== != < <= > >=` by scalar value. **No `+`/arithmetic** (use
  `code()`; avoids C promotion mess — suggested-features §2.1). Comparisons make
  `'a'..'z'` ranges work via existing Range machinery **only if** Range is
  int-backed — v1: ranges stay int (`'a'.code()..'z'.code()`); char-ranges are a
  noted deferral.
- **Literals:** single-quoted, single-scalar literals are **target-typed**:
  `char c = 'a';` yields a char; `var s = 'a';` stays `string` (back-compat: both
  quote styles are string literals today, reference §1.4). Exact rule: a
  single-quoted literal whose decoded content is exactly one scalar acquires type
  `char` when (and only when) the expected type in context is `char` — the same
  resolution-by-type shape as constructor-argument inference. Double-quoted
  literals are never char. Escapes work (`'\n'`, `'\x41'` after Track 01 F3).
- Strings (contract C1): `char string.at(int byteOffset)` — decodes the scalar
  **starting at byte offset i** (O(1), pairs with the byte-counted `length()`/
  `indexOf` world; mid-sequence offset → RuntimeException "not a scalar
  boundary"); `Array<char> string.chars()` — full UTF-8 decode. `charAt`
  unchanged forever.

### 1.2 Implementation

1. **Value model:** `RuntimeValue.hpp` — append `VKind::Char`; payload reuses the
   int field. `vchar(int)` helper. Printing: `valueToString` renders the UTF-8
   bytes. ELF: new tag number (append to the tag list in `X64.hpp` — grep the
   tag constants; values are 16-byte `[tag][payload]`, char payload = scalar in
   the payload word, **no heap, no ARC** — it is a pure immediate like int/bool).
2. **Prelude:** `class char { ... }` beside `class int` (Resolver.cpp:15). The
   primitive-mask machinery keys off class name — find where `int`/`string`
   literals bind to their mask classes (grep `"int"` in Resolver/Checker for the
   literal-typing site) and add `char`.
3. **Literal target-typing (the one compiler-subtle piece):** the checker types
   literals today by token kind. Add: where a `StringLiteral` expression meets an
   expected type of `char` (declaration init with declared type, parameter
   binding, comparison against char, return into char), re-type to char iff
   single-quoted + one scalar; else normal string typing + ordinary mismatch
   error. Requires the token to record its quote style — add a bit to the token
   or re-inspect source text via the span (span re-inspection avoids a Token
   field; Lexer is only 182 lines — either is fine, pick one, log it).
4. **Natives:** `nativeCall` gains `cls == "char"` branch (code/toString/
   fromCode); `cls == "string"` gains `at`/`chars`. CGen mirrors (CGen.cpp:196
   region). X64Gen: `genCallNative` additions — UTF-8 decode/encode as emitted
   code (bounded, table-free bit logic; the charAt/subStr emitters at
   X64Gen.cpp:1736–1762 are the pattern).
5. **Checker/engines for comparisons:** char-char compare = int compare on the
   scalar (Arith path with the existing comparison tks; type rule added beside
   the int rules in `typeOfBinary`).

---

## 2. `enum`

### 2.1 Spec

```
enum Method { GET, HEAD, POST }              // carrier: int, auto 0..n-1
enum Status : int { OK = 200, NotFound = 404 }   // explicit carrier values

Method m = Method::GET;                       // members on the static side (::)
m.toString()      // "GET"
m.code()          // 0  (carrier value)
Method::fromCode(200) -> Method?              // None if no member matches
match (m) { Method::GET => ...; Method::HEAD => ...; Method::POST => ...; }
                                              // closed set: exhaustive, no else needed
```

- An enum is a **value type** (copied, no identity, final) with a closed member
  set. Equality/ordering by carrier (`==`, `<` allowed).
- Members live on the enum's **static side**, reached by `::` — consistent with
  "the non-instantiated side" rule (reference §3.4).
- Default value (bare declaration): the **first-declared member** (the same
  first-member rule unions use for defaults, reference §2.3).
- Duplicate carrier values: compile error. `: int` is the only carrier in v1
  (string carriers deferred; note on roadmap).
- Map/Set keys: enums compare by value (they are value types — contract C3).

### 2.2 Implementation — desugar in the Resolver, one new checker fact

**RULING (2026-07-06, Opus-class — resolves the §8 STOP; supersedes the original
"(b) static-side consts" mechanism):** class static-side const *fields do not
exist* in the language — no storage, no `::` lookup path, no diagnostics (re-probed;
see §9). The design's §5 problem #3 STOP fired. Of the three §5 options, **Option B
(enum-specific resolution to compiler-mangled globals) is adopted.** Option A (build
real class statics) is a multi-week language-grammar change out of this track's
scope; Option C (namespace fallback) is **dead** — probed `namespace Method` +
`struct Method` coexisting: the type shadows the namespace and `Method::GET`
silently reads void (the collision is not a wart, it is broken, §9 probe H). Every
mechanism fact below is probe-verified against master `0172ca8`.

**Mechanism (Option B, verified viable — §9 probes E2/F2):** an enum declaration
desugars during resolution into:

(a) a value `struct` named `Method` with one `int` field `$code`, a
    **class-name-labeled** constructor `new Method(int c) { $code = c; }` — the
    ONLY form that carries a positional value (plain `Method(2)` on a default-only
    struct silently no-ops; `new(int)` is rejected "expected constructor label";
    both probed) — an instance method `int code() => $code;`, and an instance
    method `string toString()` = a `match ($code)` over the members' carriers
    returning the member-name string literal (with an `else` arm — int `match`
    requires exhaustive-or-else).

(b) one **compiler-mangled top-level const global per member**, `Method$GET =
    Method(0)`, `Method$HEAD = Method(1)`, `Method$POST = Method(2)` (carriers per
    the declaration; `: int` explicit values honored). The `$` is excluded from
    user identifiers by `isIdentStart`/`isIdentCont` (`Lexer.cpp:3-7`), so these
    globals are unreachable from user source and cannot collide — this is exactly
    how B sidesteps C's collision. They initialize on the ordinary global-init path
    (namespace/top-level globals of struct type init correctly today — probe F2; the
    bug.md #2 family is fixed).

(c) a resolver-side **closed-member table** `enumMembers["Method"] =
    [(GET,0),(HEAD,1),(POST,2)]` that the checker's `match` exhaustiveness reads.

**The one narrow resolution rule (added in two places, each gated on
`enumMembers.count(class)` so it NEVER fires for ordinary user classes):**

- **Value position** `Method::GET`: in `Checker::typeOfMember`, BEFORE the
  `bt.kind == TypeValue` blanket passthrough (`Checker.cpp:652`), insert: `bt` is a
  TypeValue whose class is enum-registered AND `e->text` is a member → type = the
  `Method` struct. In each engine's `ExprKind::Member` path (`Eval.cpp:849` and the
  IR/CGen/LLVM siblings), the same predicate → read the mangled global
  `globals_["Method$GET"]` (every engine already reads globals by name —
  `Eval.cpp:842-848` is the namespace-global precedent). ADDITIVE: unknown members
  still fall through to the passthrough — the silent-void loudness bug (filed as
  bug.md #28) is NOT fixed here; tightening `:652` is a separate delicate change.

- **Call position** `Method::fromCode(200) -> Method?`: cannot be a constructor
  (ctors return the class, not `Method?`). Desugar synthesizes a mangled free
  function `Method$fromCode(int) -> Method?` (body: `match` over carriers →
  `Method(c)` or `None`). The call path (`typeOfCallInner`/`evalCall`, where
  call-position `Type::name(args)` is already routed — confirm bare-Member call
  nodes land there) gains the sibling rule: base TypeValue enum-registered + callee
  `fromCode` → resolve to `Method$fromCode`.

**`match (m) { Method::GET => ...; ... }`**: arms of shape `Type::Member` are
constant patterns; they lower to the carrier comparison `m.$code == 0` — the same
value-pattern path `match` already has for ints (d4bb851). Exhaustiveness consults
`enumMembers[class]`; the missing-member diagnostic names them.

This buys: **zero engine value-model work** — structs + globals + ints + `std::`
free functions already run on all lanes, so enum needs NO new `VKind` and NO ABI
tag and is **FULL-COVERAGE including LLVM** (unlike char/Block, §"currency check")
— real exhaustiveness, and `::` access via one gated rule instead of a whole
statics feature.

1. Token `KwEnum` (P1 grep passed — no user collisions); `Parser.cpp` declaration
   position (beside class/struct parsing — from `parseStatement`'s declaration
   dispatch); AST: **`StmtKind::Enum` + resolver rewrite** (keeps the parser dumb;
   honest `--ast` dump).
2. Resolver: synthesize the struct + mangled member globals + `fromCode` free
   function + closed table (Rules.cpp's declaration-cloning/node-building helpers
   are the synthesis precedent; reuse if exportable).
3. Checker: the two narrow `::` rules above; match exhaustiveness consults the
   closed table; missing-member diagnostics name them.
4. `toString()`/`code()`: synthesized instance methods (in-language at desugar —
   cost-identical to hand-written).

---

## 3. `Block` — the byte buffer (contract C4)

### 3.1 Spec (frozen v1 API)

```
Block b = Block(4096);              // zeroed, fixed length; length() == 4096
int  v = b.byteAt(i);               // 0..255; OOB -> RuntimeException
b.setByte(i, v);                    // v masked to low 8 bits? NO: 0..255 or throw (loud)
Block s = b.slice(off, len);        // ALIASING view (shared bytes); OOB -> throw
string t = b.toString(off, len);    // copy bytes -> string
Block  c = Block::fromString(str);  // copy string bytes -> new Block
int u = b.int32At(i);  b.setInt32(i, u);   // little-endian; sign-extended read
int w = b.int64At(i);  b.setInt64(i, w);   // little-endian
```

- `Block` is a **class** (reference semantics, honestly mutable — it *is* the
  gated mutable buffer of info.md §16's table; the gate is the type itself:
  nothing implicit converts to/from it).
- Views (`slice`) share storage; mutation through a view is visible to the
  parent. Documented aliasing — that is the point (zero-copy).
- Bounds: every access checked, throws `RuntimeException` (§3.7 loudness).
- I/O overloads (same commit series): `std::sysRead(fd, Block, max) -> int`,
  `sysWrite(fd, Block, off, len) -> int`, `sysRecv(fd, Block, max) -> int?`,
  `sysSend(fd, Block, off, len) -> int`; `File.read/write` Block overloads.
  Overload resolution by argument type lets string forms coexist untouched.

### 3.2 Implementation

1. **Interpreters:** `VKind::Block`; payload `struct BlockData { std::shared_ptr
   <std::vector<uint8_t>> bytes; size_t off, len; }` — a slice shares `bytes`
   with different off/len. Natives in `nativeCall` (`cls == "Block"`); prelude
   `class Block` with empty-bodied natives + `new Block(int n)`.
2. **CGen:** mirror BlockData in the embedded runtime (CGen.cpp:196 region).
3. **ELF (the real work):** new heap tag. Layout: heap record `[refcount prefix]
   [parentPtr][off][len][dataPtr]` where a root block's data is inline after the
   header and a slice's `parentPtr` retains the root (ARC: slice retains parent —
   release on free; the free-list allocator + retain/release machinery from §15
   handles it once the tag is registered in genRetain/genRelease and
   recursiveFree). byteAt/setByte/int32At...: emitted bounds-check + load/store
   (the genCallNative pattern; masks/shifts are plain x86).
4. **ARC/churn:** add `block_churn.ext` to the churn corpus — alloc/slice/drop in
   a loop must hold +0B; a slice outliving its parent must keep bytes alive
   (differential vs oracle).

---

## 4. P-probes

- **P1:** `grep -rn '\benum\b\|\bchar\b\|\bBlock\b' tests/ examples/ src/Resolver.cpp`
  — name collisions (`char`/`Block` as identifiers or class names in user code).
- **P2 (statics probe):** do static-side consts of struct type initialize
  correctly today? `struct S { int v; } class E { public: const S A = ...; }` —
  actually statics syntax: probe whatever the current static-side declaration
  form is (grep reference §6.6 / corpus for static usage). If static const
  struct values don't initialize (bug.md #2's bare-global cousin), the enum
  desugar targets **namespace-scoped consts** instead (`namespace Method { const
  Method GET = ...; }`) — but that collides name-wise with the type; see
  problem #3.
- **P3 (target-typing probe):** how does the checker flow expected types into
  initializers today (`Promise<int> p = Promise(n*n)` works — reference §2.5's
  "target type" inference)? Read that code path; char literal typing rides it.
- **P4:** `string?`-returning native round-trip on ELF (`sysRecv` corpus) —
  reconfirm the optional-native recipe before `fromCode -> Method?` relies on it
  (contract C2 says Track 04 documents it; if 03 starts first, do the probe here).

## 5. Foreseeable problems & solution strategies

| # | problem | strategy |
|---|---|---|
| 1 | **Char-literal back-compat**: corpus/lcurl compare `charAt` results against single-quoted strings (`ch == " "` — cli.ext does exactly this). Target-typing must never re-type a literal compared against a *string*. | The rule is expected-type-driven only; `==` against string keeps string typing. The P1 grep + full corpus run is the regression net. Riskiest site: overloads where both `f(char)` and `f(string)` exist — most-specific-wins must prefer... **decide now: `string` wins** (back-compat trumps; reaching the char overload requires a char-typed expression, not a literal). Encode as a checker test. |
| 2 | **UTF-8 `at()` boundary semantics** (mid-sequence byte offset) may surprise; and `chars()` on invalid UTF-8 (lone bytes from sysRecv) must not crash. | `at` throws on non-boundary (loud). `chars()`/`toString` policy for invalid sequences: **replacement scalar U+FFFD** (never throw on data — data is not a programming error). State both in reference §6.1b. |
| 3 | **Enum-namespace name collision** (P2 fallback): a namespace and a struct sharing the name `Method` — `::` reaches both; resolution may conflict. | Primary design avoids it (statics on the struct). If P2 fails AND statics can't be fixed cheaply, STOP — do not ship the namespace fallback without a ruling (it bakes a naming wart into every enum). Fixing static-const-struct init is likely the same family as bug.md #2 (global init) — budget for fixing the underlying bug instead; that is the preferred world. |
| 4 | **Match-arm syntax `Method::GET =>`**: match value-patterns parse expressions (`parseExpr(2)` at Parser.cpp:309) — a qualified name parses fine, but the *checker* must know it is a constant pattern, not a type pattern. | Enum members are values (consts), so they route to the value-pattern path by kind; add an explicit checker test (`match` with qualified enum arms + exhaustiveness error message naming missing members). |
| 5 | **Block slice ARC on ELF**: slice-retains-parent must interact correctly with hfree's poisoning (0xFE fill on free) — a bug here corrupts live views. | The retain edge makes parent-free-while-viewed impossible *if* genRelease knows the tag. Churn test (block_churn.ext) with view-outlives-binding shapes; run under `--mem-verify` and the ELF heap meter. |
| 6 | **Value-model churn in five places** (VKind in interpreters, CGen runtime, ELF tag) drifting apart on edge semantics (e.g. what does `console.write(block)` print?). | Spec the print form here: `Block(len=N)` — one line in each engine, corpus-pinned (`blocks.ext` prints one). Differential corpus is the drift detector. |
| 7 | **`setByte` value range**: masking (silent) vs throwing (loud) — design says throw, but digest code (Track 09) computes `x & 0xFF` anyway. | Keep throw (loudness rule); Track 09's idiom already masks (contract C7). Revisit only if profiling shows the check hot — then it needs an owner ruling (gate pattern), not a quiet change. |
| 8 | **`char` in unions / `char?`** — new primitive must slot into union tagging + narrowing + `match` type patterns. | It is "just another value kind" on the existing union machinery; add union+narrowing+match cases to the chars.ext corpus explicitly rather than assuming. |

## 6. Milestones & acceptance

| M | deliverable | accept |
|---|---|---|
| M1 | `char` end-to-end (oracle+IR first, then CGen+ELF); `string.at`/`chars` | chars.ext corpus (literals, escapes, compare, union/match, at/chars incl. multi-byte + boundary-throw); checker tests (problem #1 overload rule) |
| M2 | `enum` (desugar; exhaustive match) | enums.ext corpus on all engines; checker tests (dup carrier, non-exhaustive match error text) |
| M3 | `Block` on oracle+IR+CGen | blocks.ext (bounds throws, slices/aliasing, int32/64 LE, fromString/toString round-trip) |
| M4 | `Block` on ELF + ARC + I/O overloads | blocks.ext on run_elf; block_churn.ext +0B; sysRead/sysRecv Block overload probe program |

Target: **Jul 20 – Aug 2** (M1 3d, M2 3d, M3 2d, M4 4d).

## 7. Reference-doc duty

reference §1.4 (char literals), §2.2 (char primitive), new §4.2c (enum), §6.1
(char methods; string.at/chars), new §6.10 (Block), §6.6.5 (sys Block overloads);
info.md §9 (char in the primitives story), §19 #9 gets its partial answer recorded.

## 8. STOP conditions

- P2 statics probe fails and fixing static-const init is not a contained fix
  (problem #3 — no namespace-fallback without a ruling).
- Char literal typing requires touching more than the expected-type flow (i.e.
  a second literal-typing mechanism starts growing).
- Block's ELF layout wants changes to the allocator prefix contract ([P-16]
  refcount / [P-8] size — §15); that contract is load-bearing for every heap
  kind and must not fork.

## 9. Implementation log

- **2026-07-06 — STOP (§8 condition 1): P2 statics probe FAILED; no code written.**
  Class static-side const fields do not exist as a mechanism at all — `E::A` on a
  class silently typechecks as TypeValue (`Checker.cpp:652` blanket passthrough,
  never looks up the member) and evaluates to void (`Eval.cpp:851`); `const` class
  fields are ordinary instance slots. Deeper than problem #3's anticipated
  init-order failure; namespace fallback remains forbidden without a ruling. Full
  trace, probe results (P1 passed; lowercase ctor labels verified working, so
  `Block::fromString` is unblocked as a labeled ctor), ruling options, and a
  ready-to-file bug.md draft for the silent-void loudness bug are in
  **`/this_bug.mg`** (repo root). Escalated to a Fable-class model per §4.2 of the
  overview; §2.2's mechanism (and §1.1 `fromCode`'s home) must be amended before
  implementation resumes.

- **2026-07-07 — STOP RESOLVED; design is implementation-ready (still unimplemented).**
  The three amendments the STOP demanded are now in the design body (a prior
  session authored them; this entry records that they clear the STOP and were
  previously left uncommitted):
    1. **enum mechanism** — §2.2 adopts **Option B**: enum desugars to a value
       struct + compiler-mangled per-member const globals (`Method$GET`, …) + two
       narrow `::` resolution rules gated on `enumMembers.count(class)`. No general
       class statics are built. enum is therefore **full-coverage including LLVM**
       (no new `VKind`, no ABI tag).
    2. **`char` factory** — §1.1 homes it as the `std::charFromCode(int)` free
       function, not a static method (class static-side fields do not exist).
    3. **char/Block native lanes** — the currency-check RULING ratifies the
       LV_CHAR/LV_BLOCK ABI *shape* but **defers implementation**: char/Block ship
       on oracle + IR + emit-C++ ONLY for v1 (no ELF under the X64Gen freeze; no
       LLVM until Track B lands the addendum — resolution tracked in
       `designs/deferal-char-block-abi.md`).
  The hard **Track 02 dependency is now met** — Track 02 landed
  (`designs/complete/techdesign-02-control-flow.md`). **No design blocker remains.**
  What keeps this doc out of `designs/complete/` is simply that **Track 03 is not
  yet implemented** — zero code as of 2026-07-07 (no `KwEnum`, no `VKind::Char`/
  `Block`, no `char`/`Block` prelude, no `charFromCode`, no `enumMembers`;
  verified by grep). Implementation is milestones M1–M4 (§6); the char/Block
  full-coverage legs (M1's LLVM leg, M4) stay gated on the deferred ABI addendum,
  everything else (enum full-coverage, char/Block on oracle+IR+CGen) is unblocked.
  The secondary silent-void loudness bug (`Checker.cpp:651-652` TypeValue
  passthrough) is filed as **bug.md #28**.

- **2026-07-08 — M1/M2/M3 IMPLEMENTED and landed on master** (commits: M2 enum
  `fc01c21`, M1 char `4a1e1db`, M3 Block `7951745`).
    - **M2 enum — FULL-COVERAGE (oracle/IR/emit-C++/LLVM).** The §2.2 Option B
      desugar landed: the Resolver lowers each `enum` to a value struct + mangled
      `Name$Member` const globals + a `Name$fromCode` free function (generated as
      source text, parsed, then renamed to interned `$`-names). Metadata lives on
      the Program (survives the metaprogramming re-resolve). Checker does the two
      gated `::` rules + enum-closed `match` exhaustiveness; Eval/Lower read the
      resolved global. `::`-qualified match-arm heads parse as value patterns. A
      pre-existing CGen `obj ==/!= None` crash (dispatched into the struct's own
      `==`) was fixed as a dependency (short-circuit None like Eval::combine).
      Corpus `tests/corpus/enums/enums.lev` on all four lanes.
    - **M1 char — oracle+IR+emit-C++.** `VKind::Char` (scalar in the `i` field).
      Single-quoted single-scalar literals target-type to char (declared type /
      `char?` / `==` / match arm; call-arg position deferred per §5 problem #1).
      `code()`/`toString()` native (UTF-8); classification/case in-language;
      `std::charFromCode`; `string.at`/`chars`. Parser records `Expr::singleQuoted`
      (quote style is lost to the interpolation raw-segment); checker sets
      `Expr::charLit`. LLVM-excluded: it errors loudly on the char natives, and
      LlvmGen's bug.md #27/#28 by-name reachability is taught to skip the char
      class so string programs don't drag char methods in. Corpus
      `tests/corpus/chars/chars.lev` on run/ir/cpp.
    - **M3 Block — oracle+IR+emit-C++.** `VKind::Block` +
      `BlockData{shared_ptr<vector<uint8_t>> bytes; off,len}`; slices are aliasing
      views (no COW). Full C4 API with bounds-checked throws; construction
      special-cased in Eval + two IR ops (`NewBlock`/`NewBlockStr`) mirrored in
      IrInterp + CGen (kind 11). LLVM errors loudly (the gate). Corpus
      `tests/corpus/blocks/blocks.lev` on run/ir/cpp.
    - **bug.md #28** (silent-void on unknown `Type::member`) was fixed
      concurrently by another agent; the enum `::` rule composes over it.
    - **Remaining:** M4 (Block on ELF + ARC + sysRead/sysWrite Block I/O
      overloads) stays gated on the deferred LV_CHAR/LV_BLOCK ABI addendum; the
      char-range / string-carrier-enum deferrals are tracked in
      `techdesign-track03-type-surface.md`. Full ctest 84/84 green.

- **2026-07-08 (agent3) — §7 reference-doc duty discharged + deferral docs created.**
  Verified M1/M2/M3 landed in this branch and green (the built binary was stale —
  predated the char/Block commits; rebuilt, then `corpus_enums/chars/blocks` ×
  treewalk/ir/cpp + `corpus_enums_llvm` + the 6 unit suites all pass, 16/16). No
  code touched (M4 remains ABI-gated + X64Gen frozen — nothing implementable left
  in this track's own scope). Completed the documentation deliverables the design
  required but that never landed with the code:
    - **reference.md** (§7 duty): §1.4 char-literal target-typing; §2.2 `char`
      primitive; new **§4.2c** enum; §6.1 `char` methods table + `string.at`/`chars`
      row + char engine-coverage/back-compat note (and refreshed the stale
      `reverse()`/`chars()` note now that `chars()` shipped); new **§6.10** `Block`;
      §6.6.5 `Block` I/O overloads marked **[planned]** (they are M4, not present).
    - **info.md**: §9 `char` in the primitives-story; §19 #9 (string encoding) gets
      its Track-03 partial answer (bytes stay byte-counted; `at`/`chars` are the
      opt-in scalar path).
    - **Deferral docs** the log referenced but which did not exist:
      `designs/deferal-char-block-abi.md` (LV_CHAR/LV_BLOCK ABI shape + M4/LLVM gate)
      and `designs/techdesign-track03-type-surface.md` (char ranges, string-carrier
      enums, char-literal call-arg target-typing).
  This doc stays out of `designs/complete/` only because **M4** is a genuinely
  unimplemented milestone (deferred to Track B's ABI pass), not because of any
  remaining Track-03-owned work.

- **2026-07-10 — M1 char + M3 Block LLVM legs LANDED (LV_CHAR/LV_BLOCK ABI
  addendum shipped).** The two deferred LLVM legs are now green; only M4 (ELF)
  stays deferred behind the X64Gen freeze. Implementing `designs/deferal-char-
  block-abi.md`'s ratified shape:
    - **`runtime/lv_abi.h`:** the closed 0–9 tag set is extended by `LV_CHAR = 10`
      (pure immediate) and `LV_BLOCK = 11` (heap, body `{parentPtr, off, len,
      dataPtr}`), with the Block heap-layout note in §2.4 and prototypes for the
      char UTF-8 (`lvrt_char_to_string`/`lvrt_str_at`/`lvrt_str_chars`) and
      `lvrt_block_*` natives. A contract change shipped whole (doc-2 §0.2).
    - **`runtime/lv_runtime.c`:** `lv_is_counted`/`lv_recursive_free` gain the
      `LV_BLOCK` branch (slice releases `parentPtr`=root; root frees `dataPtr`);
      `lvrt_to_string` renders `char` (UTF-8) and `Block(len=N)`; the char/block
      helper bodies mirror `RuntimeNatives.cpp` byte-for-byte (incl. the
      lvrt_raise_oob OOB wording and the `0..255` range throw).
    - **`src/LlvmGen.cpp`:** char `LoadConst`/`charFromCode`/`code()` retag
      inline; `effClassId`/`IsType` learn tags 10/11; the `char`-class
      reachability skip is removed; `NewBlock`/`NewBlockStr` ops + tag-10/11
      `emitNativeRows` rows + `kCovered` entries added.
    - **Tests:** `corpus_chars_llvm` + `corpus_blocks_llvm` ctest lanes (byte-
      identical vs oracle). Full LLVM suite (22 lanes incl. `corpus_llvm_full`,
      `corpus_churn_leak_llvm`, `corpus_mem_verify`) green; a block-churn probe
      holds live-at-exit flat (288 B) from N=100 to 10000; slice-outlives-parent
      verified against the oracle.
    - **Remaining:** M4 only (`Block` on ELF + ARC + `sys*`/`File` `Block` I/O
      overloads) — untouched, X64Gen is the frozen bootstrap anchor.
