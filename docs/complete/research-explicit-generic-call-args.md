# Research: Explicit Generic Type Arguments on a Call/Construction Callee

**Status:** research input for a future tech design. Not a design; carries no rulings — the
options in §5 are presented for the owner to decide jointly, per the project's "no
deferrals in designs" convention (a design consuming this doc must pin one before writing).
**Trigger:** `designs/expr-reification/techdesign-02-reifier.md`, Stage 2 implementation log
(2026-07-19, landed today), ruling **R8 forced deviation** — LA-31's reifier wanted to emit
`expr::Expr<(User)=>bool>(<lambda>, <tree>, [<binds>], <siteId>)` with an explicit concrete
generic argument on the constructed callee, and discovered mid-stage that no such spelling
exists on the landed grammar. The log explicitly flags this as its own ticket: *"re-adding
the explicit spelling is a Parser change (generic-construction syntax) that belongs in its
own ticket, not S2."* This document is the research for that ticket.
**Date:** 2026-07-19. Verified against `build/leviathan` built from this tree (branch
`agent0`, forked from `master` at the LA-31 S2 landing commit).

---

## 0. One-paragraph statement of the problem

Leviathan has no syntax for supplying an explicit generic type argument at a **call or
construction site** — `Box<int>(5)`, `identity<int>(5)`, `Box::<int>(5)`. Type arguments at
a call site are **always inferred** (from argument types, then from the target/expected
type) or, failing both, the compile errors out asking the caller to add a target-typed
binding. This is distinct from — and should not be confused with — `Name<T1,...>` in **type
position** (a variable declaration, parameter type, return type, cast/`is`/`match` type
pattern), which is fully supported and is almost certainly what `docs/reference.md` §2.5
means by "Explicit `Name<T1,...>` is always available" (a claim that reads as broader than
it is once you try it at a call site). The gap is not a corner case: it is the reason LA-31
Stage 2 could not emit the literal generic argument its own design ruling (R8) called for,
and it is the only way to resolve `docs/reference.md` §3.7's "cannot infer type argument …
provide a target type or a type-bearing argument" error inline, without restructuring the
call into a target-typed binding.

---

## 1. Current behavior, verified against this tree

All four repros below were run directly against `build/leviathan --run` on this branch.

### 1.1 `Box<int>(5)` used as a construction — silently parses as a chained comparison

```lev
class Box<T> { T v; new Box(T x) { v = x; } }
Box<int> b = Box<int>(5);
```
```
error: cannot initialize 'b' with 'bool'
  Box<int> b = Box<int>(5);
               ^~~
```

The RHS type-checks to `bool`, which is conclusive: `Box<int>(5)` parsed as
`(Box < int) > (5)` — `Box < int` (an ill-typed but grammatically legal relational
comparison between two names) compared `>` against the parenthesized group `(5)`, itself a
`bool`-yielding `Binary(Gt)`. Nothing in the pipeline ever attempted to read `<int>` as a
generic-argument list.

### 1.2 `identity<int>(5)` used as a generic function call — same parse

```lev
T identity<T>(T x) => x;
int y = identity<int>(5);
```
```
error: cannot initialize 'y' with 'bool'
  int y = identity<int>(5);
          ^~~~~~~~
```

Identical failure mode. This matches the M0 note already on record in
`designs/complete/techdesign-generic-static-members.md:386-389` (LA-18's implementation
log): *"The design's explicit `make<Foo>(7)` probe spelling is stale for function calls in
this parser; function type arguments are inferred."* The gap is uniform across construction
and function/method calls — anywhere a `Call` node's callee could carry type arguments.

### 1.3 The escape hatch today — `docs/reference.md` §3.7 / `Checker.cpp:2811`

```lev
class Box<T> { T v; new Box() { } }
var b = Box();
```
surfaces (among incidental parse-recovery noise from the file's second statement; the
load-bearing line is the last one):
```
error: cannot infer type argument 'T' for 'Box'; provide a target type or a type-bearing argument
```
This is `Checker.cpp:2811-2813` verbatim. The **only** fix available today is to give the
binding a target type (`Box<int> b = Box();`) — there is no way to say "T=int" at the call
expression itself.

### 1.4 `Box::<int>(5)` (a hypothetical "turbofish") is a hard parse error today

```lev
Box<int> b = Box::<int>(5);
```
```
error: expected member name
  Box<int> b = Box::<int>(5);
                    ^
error: type 'Box' has no member ''
  Box<int> b = Box::<int>(5);
               ^~~
error: cannot initialize 'b' with 'bool'
  Box<int> b = Box::<int>(5);
               ^~~
```
`::` immediately followed by `<` is not legal grammar today — the parser expects a member
identifier right after `::` (`Parser.cpp:606-624`) and errors on `<`, then parse-recovers.
This is significant for §5: it means `::< ` is a genuinely free grammar slot, not an
ambiguous one.

---

## 2. Where this lives in the source — the parser has no expression-position generic-arg grammar

- **Precedence table** (`docs/reference.md` §3.1): `<` `>` `<=` `>=` sit at level 7, well
  below postfix `.`/`::`/`(args)`/`[index]` (the tightest tier). There is no special
  postfix-position handling for `<` at all — it is an ordinary binary operator.
- **`parsePostfix`** (`Parser.cpp:604-654`) handles exactly four postfix forms after a
  primary expression: `.`/`::` (→ `Member`), `(args)` (→ `Call`), `!(args)` (→ macro `Call`,
  `Parser.cpp:630-642`), `[index]` (→ `Index`). No branch consumes a leading `<`.
- **`parseExpr`/`infixBP`** (`Parser.cpp:47-78, 679ff`): `<`/`>` are parsed by the ordinary
  Pratt-parser binary-operator loop, with no lookahead for "is this actually a generic-arg
  list."
- **The only place `<...>` generic-argument lists are parsed** is `parseTypePrimary`
  (`Parser.cpp:226-277`, the `accept(TokenKind::Lt)` branch at `:270-275`), reached only
  through `parseType()` — itself called only from type-annotation contexts: var-decl type
  (`parseVarDecl`, `:816`), parameter type (`:774`), return type (`:1178`, `:1269`),
  `is`/`match` type patterns (`:482, :703`), base-class lists (`:1227`), lambda-type
  parameter/return types, and a handful of others (`Parser.cpp:242,248,272,560,703,774,
  816,850,968,1010,1178,1227,1269,1510,1902,1973`). **Never from an expression/call-callee
  position.**
- **AST**: `Expr` (`Ast.hpp:144ff`) has no `typeArgs`/`generics` field on `Call` or `Member`
  nodes. `struct Expr` carries `text`/`op`/`colon`/`optChain`/`a`/`b`/`c`/`list` and a grab
  bag of feature-specific flags, but nothing for an explicit type-argument list. Only
  `TypeRef::generics` (`Ast.hpp:52`, type position) and `Stmt::generics` (`Ast.hpp:349`,
  declaring a callable's own type *parameters* — `R f<R>(R x)`) exist. There is nowhere to
  even *put* an explicit call-site type argument today, independent of the parser gap.

### 2.1 Why the existing declaration-vs-expression lookahead can't be reused as-is

The parser already resolves an analogous ambiguity at **statement/declaration** level —
`Box<int> x` (a declaration) vs. `Box < int` (a comparison expression statement, unusual but
legal) — via `looksLikeVarDecl` (`Parser.cpp:792-807`) and the top-level decl/func scan
(`Parser.cpp:1949-1970`), both built on the shared lookahead helpers `skipAngles` /
`skipTypeSuffix` / `skipTypeFull` / `expectGt` (`Parser.cpp:80-138, 183-196`). Both of these
disambiguate by scanning **past** the closing `>` and checking for a **trailing declaration
marker**: an identifier immediately followed by `=`/`;`/`(` — i.e., they exploit the fact
that a declaration always has a variable/function *name* right after the type. **A
construction/call in ordinary expression position has no such trailing name to scan for** —
`f(Box<int>(5))`, `return Box<int>(5);`, `[Box<int>(5), Box<int>(6)]` all place the
ambiguous span with nothing distinguishing after it. This is why the existing machinery,
while directly relevant as a precedent for *how* generic-arg lists are lexically closed
(`expectGt`'s `>>`-splitting, `skipAngles`'s depth tracking), cannot simply be extended to
expression position — the disambiguation problem is structurally harder there, not just
unimplemented.

---

## 3. Where this lives in the checker — 100% inference, by construction

- **`inferConstruction`** (`Checker.cpp:2753-2839`) builds its `map` of bound type-parameter
  names from exactly two sources: (1) `unify()` against each constructor argument's declared
  parameter type vs. its actual argument type (`:2769-2771`), and (2) `unify()`/direct
  assignment against the expected/target type, including the "constructor result used as a
  declared base/interface" shape (`:2780-2788`). Inside a generic scope, an unresolved
  parameter degrades leniently to the raw head; outside one, it is the hard error from
  §1.3 above (`:2810-2813`). **No third source reads an explicit call-site type-argument
  list**, because the parser never produces one to read.
- **`genericReturn`** (`Checker.cpp:3615-3663`), the parallel machinery for generic
  function/method calls, is likewise built entirely on `unify(fn->params[i].type.get(),
  args[i], subst)` (`:3622-3623`) plus the lambda-last/method-ref deferral loop
  (`:3624-3659`, itself a LA-25/LA-31 precedent, not a type-argument source). Zero code
  path reads an explicit type argument here either.
- **`unify`/`substitute`** (`Checker.cpp:2843-2864, 2866ff`) are the shared machinery both
  of the above call into; neither has any notion of a pre-seeded/explicit binding — `map`/
  `subst` always start empty and are filled purely from structural unification.

**Consequence:** everywhere in the checker that would need to *consume* an explicit
argument, the natural seam is "seed `map`/`subst` from the explicit list before running the
existing `unify()` passes" — a small, well-contained change *if* the AST can hand the
checker such a list. The parser/AST gap in §2 is the entire blocker; the checker-side change
is comparatively mechanical.

---

## 4. Generics are erased — this is a checker-only feature, zero engine work

Per the standing project understanding (Leviathan generics are type-erased, compiled once,
dynamically dispatched — not C++-style monomorphized) and as re-confirmed by
`designs/complete/techdesign-generic-static-members.md` §3 (LA-18) and
`docs/research-expr-reification.md` §3.6: an explicit call-site type argument, once parsed,
carries **no runtime value on any of the five engines** (oracle, IR, emit-C++, LLVM, the
frozen ELF backend). It is purely an additional (or overriding — see §5.3) **inference
source** for the checker, exactly like the target-type source `inferConstruction` already
has. This mirrors LA-18's and LA-25's "above-the-IR" character: the feature lives entirely
in `Parser.cpp` (parse it), `Ast.hpp` (carry it), `Checker.cpp` (consume it), and
`AstPrinter.cpp` (render it back out for `--expand`/round-tripping) — no `Lower.cpp`,
`Eval.cpp`, `IrInterp.cpp`, or `LlvmGen.cpp` change is expected; those files are
verify-only, the same posture LA-18/LA-25 landed with.

### 4.1 The printer gap is real, not hypothetical

`AstPrinter.cpp` currently has **zero** support for rendering generic arguments on a `Call`
or `Member` callee — confirmed both by grep (no `generics`-adjacent logic near any Call/
Member print path) and by `techdesign-02-reifier.md`'s own Stage 2 log (§7, "H5 — printer
generic args on a qualified callee… did not fire, because the emitted construction has no
generic args on the callee to render"). Whatever design lands this must add that rendering
path, or `--expand`/the round-trip harness (`tests/run_expand_roundtrip.sh`) will not
survive a checker-side rewrite that introduces explicit call-site generics — the same
class of hazard R1/H2 tracked for LA-31.

---

## 5. The core design decision: what syntax, and how it avoids the C++ `<`-ambiguity trap

`Box<int>(5)` in expression position is **lexically identical**, up to the point of parsing,
to the legal (if usually ill-typed) expression `(Box < int) > (5)` — §1.1/§1.2 prove this is
not theoretical, it is exactly what happens today. This is structurally the same ambiguity
that plagued pre-C++11 template parsing (`a<b>(c)`), which C++ resolves only because its
parser has a symbol table available during parsing (an identifier known to be a template
name changes how `<` is lexed/parsed). **Leviathan's parser has no such table** — name
resolution is a separate `Resolver` pass that runs *after* the full `Parser` pass
completes — so reusing C++'s solution directly would be a nontrivial architecture change,
not a local grammar tweak. Three shapes are worth weighing; presented here, not decided,
per the project's "surface every choice to the owner and decide jointly" convention.

### 5.1 Option A — an unambiguous new spelling: `Name::<T1,...>(args)` ("turbofish")

`::` immediately followed by `<` is **verified to be a hard parse error today** (§1.4) — a
genuinely free grammar slot, with an exact in-tree precedent for claiming one: the
expression-macro-call feature repurposed a previously-illegal `Bang` token position with the
explicit rationale *"`Bang` here was a parse error before this feature… so the grammar slot
is free with no adjacency check needed"* (`Parser.cpp:630-636`). Concretely: teach the
`ColonColon` branch of `parsePostfix` (`Parser.cpp:606-624`) that if the token right after
`::` is `Lt` rather than an identifier, parse a generic-argument list (reusing
`parseType()`/`expectGt()` — the exact machinery `parseTypePrimary` already uses at
`:270-275`) instead of a member name, and stash it on the `Member`/`Call` node's new
`typeArgs` field.
- **Zero parsing ambiguity, no lookahead/backtracking needed** — the parser decides
  correctly at the `::` token, LL(1)-style, exactly like every other postfix form.
- **Cheapest to implement**: one new branch in `parsePostfix`, one new AST field, no changes
  to `infixBP`/`parseExpr`/the declaration-lookahead machinery at all.
- **Cost:** a spelling users must learn that differs from type-position `Name<T1,...>` (two
  different generic-argument spellings depending on position — type vs. call). This is a
  real ergonomic cost, not free, and should be weighed explicitly, not waved away.

### 5.2 Option B — the "natural" bare spelling: `Name<T1,...>(args)`, disambiguated by lookahead or a parser-side symbol table

Highest continuity with type-position syntax (one spelling everywhere), but the highest
risk/cost route. Two sub-approaches, both nontrivial:
- **Speculative parse-and-backtrack**: attempt the generic-arg-list parse when a `<`
  follows a primary `Name`; if it doesn't resolve into a syntactically valid closer
  followed by `(`, roll back and re-parse as a comparison chain. Workable but adds a new
  class of backtracking to a parser that is currently backtracking-free everywhere else
  (the existing `looksLikeVarDecl`-style helpers are pure lookahead with no rollback — they
  decide, then commit). A genuinely ambiguous case remains: `f(a < b, c > (d))` — is this
  one call-arg `a < b` and one call-arg `c > (d)`, or (with a comma inside the angle
  brackets reinterpreted) a single generic call `f(a<b,c>(d))`? Real languages that allow
  bare `<...>` generics resolve exactly this class of case with symbol-table lookups, not
  syntax alone.
- **Parser-side symbol awareness**: give the `Parser` enough forward knowledge of declared
  generic type/function names to decide `<` vs. "start of a comparison" the way C++ does.
  This is a genuine architecture change (the Parser is currently a single, symbol-free
  pass — `Resolver` runs strictly after) with unclear blast radius on every other grammar
  production that shares this ambiguity shape (e.g., the statement-level heuristics in §2.1
  would become partially redundant, or would need reconciling with the new symbol
  awareness).
- Recommend closing this option only if the ergonomic cost of Option A's differing spelling
  is judged unacceptable — it is the "wants it most" option and the most expensive/riskiest
  one simultaneously.

### 5.3 Option C — parse permissively, rewrite in the checker (closed off, not recommended)

Mirroring the `rewriteAsMethodRef` (`Checker.cpp:1274-1363`) / LA-31 reifier in-place-rewrite
precedent: parse `Box<int>(5)` exactly as today (`(Box < int) > (5)`), then have the checker
pattern-match the specific shape — a `Binary(Gt)` whose LHS is `Binary(Lt)` whose LHS
resolves to a generic type/function symbol and whose RHS is type-shaped, with a parenthesized
argument-list RHS — and rewrite it in place into a `Call` carrying recovered type args.
**This is unsound in general, not merely awkward**: a *bona fide* chained comparison
`Box < int > (5)` where `Box` and `int` happen to be local variables that shadow (or simply
coincidentally share names with) a generic type and a type name would be silently
reinterpreted as a generic call, or the reverse — the checker cannot tell "the user meant
comparison" from "the user meant a generic call" from the same parse tree without either a
syntactic marker (which is exactly what Options A/B are debating) or accepting an
unsound heuristic. Recorded here explicitly so it is not re-proposed and re-litigated later,
matching the practice `designs/complete/techdesign-generic-static-members.md` §4.4 and
`docs/research-expr-reification.md` follow for closed-off alternatives.

### 5.4 A recommendation to weigh, not a ruling

Option A is the only one of the three with zero parsing ambiguity and a direct, successful
in-tree precedent for the exact move (claiming a previously-illegal token adjacency). Option
B is what surface-level ergonomics wants but is the most architecturally invasive and is the
one place this research surfaces a genuine unresolved ambiguity (`f(a < b, c > (d))`) rather
than "merely unimplemented." Option C should likely be closed outright on soundness grounds
rather than carried forward as live. This is the single most important thing for the
consuming design to settle first — everything in §6-§8 below is written spelling-agnostic
except where noted.

---

## 6. The checker-side question §5 doesn't answer: does explicit override or merely seed?

Once a `typeArgs` list reaches `inferConstruction`/`genericReturn`, a second, smaller
decision remains, independent of §5's syntax choice: when an explicit argument is present
**and** a type-bearing constructor/call argument or target type is *also* present and they
disagree (`Box<int>::(5) ` — wait, spelling aside — `Box::<string>(5)` where the ctor param
is `T x` and `5` is an `int`), does the explicit argument win outright (the constructor
argument is then checked *against* the pinned `T=string`, producing an ordinary "argument
type mismatch" error), or is disagreement itself the error ("explicit type argument
conflicts with inferred type")? The natural, precedent-aligned answer is **the former** —
seed `map`/`subst` from the explicit list first (`Checker.cpp:2769` becomes "seed from
`typeArgs`, then unify remaining/all params against ctor args using the pinned substitution
already in `map`"), so an explicit argument behaves exactly like a target type does today
(`:2780-2788`) — an authoritative binding that downstream argument checks are then
type-checked *against*, not merely compared to. This keeps the change additive to
`inferConstruction`/`genericReturn` rather than introducing a new conflict-detection error
class, and mirrors how a target-typed binding already takes priority as an inference source.
Flagging this as a decision the design should state explicitly (in its rulings section, the
project's established practice), not leave implicit in the implementation.

---

## 7. Relationship to nearby work (what this is, and is not)

- **`designs/complete/techdesign-generic-static-members.md` (LA-18)** and its extension
  request **`designs/requests/accepted/request-generic-overload-monomorphization.md`**: both
  are about resolving `T::member`/overloaded calls **inside a generic body**, per concrete
  instantiation, via demand-driven monomorphization. Related in that both confirm the
  erasure model (§4 above), but solving a different problem — neither touches call-site
  syntax, and this ticket's feature needs no monomorphization machinery (the whole point is
  the checker already knows the concrete type; there's nothing to specialize).
- **`designs/complete/techdesign-method-references.md` (LA-25)**: the `rewriteAsMethodRef`
  in-place-rewrite mechanism is cited above (§5.3) as the precedent for — and the reason to
  reject — Option C. Not otherwise related.
- **`designs/expr-reification/techdesign-02-reifier.md` (LA-31 S2)**: the direct trigger
  (§0). Landing this ticket lets a follow-up LA-31 patch re-add the literal `<Fn>` spelling
  to the reifier's emitted construction, satisfying R8's exact wording — but the S2 log is
  explicit that R8's *intent* is already met without it (the site's checked type is already
  the precise `expr::Expr<Fn>`; only the round-tripped **source text** differs). This ticket
  is therefore a nice-to-have completion for LA-31, not a blocker on anything currently
  in flight.
- **`docs/footguns.md` / `known_bugs_1.md` / `known_bugs_2.md`**: grepped for any existing
  row or bug entry on this gap — none exists. This is a fresh, previously-undocumented gap,
  not a known footgun with a workaround; the only "workaround" on record is
  `docs/reference.md` §3.7's target-typed-binding escape hatch (§1.3 above), which the
  reifier's own generated code cannot use (a construction argument position, not a
  local-variable declaration).
- **`docs/reference.md` §2.5**: the line "Explicit `Name<T1,...>` is always available" should
  be revisited once a design lands here — either clarified now to explicitly scope it to
  type position (a documentation-only fix, doable independent of any code change), or left
  as-is if the landed design makes it literally true by adding call-site support. Flagging
  the ambiguity rather than picking a fix, since which one applies depends on §5's outcome.

---

## 8. Sketch of a testing/corpus strategy (for whichever design consumes this)

Mirroring `docs/research-expr-reification.md` §10's shape:

1. **Positive corpus**: explicit generic construction and explicit generic function/method
   call, oracle/IR/LLVM differential — including the case §1.3 currently can't express
   (`var b = Box<int>();` — no ctor argument, no target type, previously a hard error).
2. **`--expand` round-trip**: the chosen spelling must re-print and re-parse identically
   (closes the printer gap, §4.1) — this is the one hazard most likely to bite, per the
   reifier's own H5 flag.
3. **Negative corpus**: arity mismatch (wrong number of explicit type arguments for the
   target's declared parameter count) as a new, explicit diagnostic; and, if §6 is decided
   as "explicit wins," a corpus row proving a disagreeing constructor-argument type surfaces
   as an ordinary argument-type-mismatch error at the argument, not a new conflict error.
4. **LA-31 regression tie-back**: a corpus case re-landing the reifier's dropped `<Fn>`
   spelling as an explicit acceptance check, tying this ticket back to its trigger (§0).
5. **Whatever §5 does NOT choose** should get one negative-corpus row proving it's still
   correctly rejected/unaffected (e.g., if Option A ships, confirm bare `Box<int>(5)` still
   parses as the comparison chain it does today — no accidental widening of the bare form).

---

## 9. File-by-file change inventory (spelling-agnostic where possible; Option A assumed for concreteness)

| file | change |
|---|---|
| `src/Parser.cpp` | new branch in `parsePostfix`'s `ColonColon` arm (`:606-624`) — if the token after `::` is `Lt` (not an identifier), parse a generic-arg list via the existing `parseType()`/`expectGt()` pair (reusing `:270-275`'s loop) instead of a member name; attach to a new `typeArgs` field on the resulting `Member`/`Call` node. If Option B is chosen instead, this row is replaced by lookahead/backtracking work in `parseUnary`/`parseExpr` and the shared helpers in `:80-138`. |
| `src/Ast.hpp` | new field on `Expr`, e.g. `std::vector<TypeRefPtr> typeArgs`, populated on the `Call` (and/or `Member`, depending on where the chosen grammar attaches the list — Option A attaches to the `Member` callee, before the `Call`'s own `(args)`). |
| `src/Checker.cpp` | `inferConstruction` (`:2753`) and `genericReturn` (`:3615`) — seed `map`/`subst` from `typeArgs` before/instead of the existing `unify()` passes, per §6's ruling; new arity-mismatch diagnostic. |
| `src/AstPrinter.cpp` | render `typeArgs` on a `Call`/`Member` callee — currently absent everywhere (§4.1); needed for `--expand` round-tripping. |
| `src/Lower.cpp` / `Eval.cpp` / `IrInterp.cpp` / `LlvmGen.cpp` | none expected (§4) — verify-only, per the LA-18/LA-25 "above-the-IR" precedent. |
| `docs/reference.md` | §2.5 clarify/complete the "Explicit `Name<T1,...>`" bullet once §5 is decided (§7); §3.7 update the "no inference source" escape-hatch bullet to mention the new inline spelling; §3.1 precedence-table footnote only if Option B is chosen. |
| `designs/expr-reification/techdesign-02-reifier.md` | optional follow-up note (not required) once this lands, closing R8's literal-spelling residual. |

---

## 10. Cross-references

- Trigger: `designs/expr-reification/techdesign-02-reifier.md` §"R8 forced deviation" (Stage
  2 implementation log, 2026-07-19); the original ruling: `designs/expr-reification/
  techdesign-00-overview.md:135-138`.
- Erasure model / "above-the-IR" precedent: `designs/complete/
  techdesign-generic-static-members.md` (LA-18), especially §3, §4.4, and the M0 log
  (`:386-389`) independently confirming the same gap for generic function calls.
- Rejected-alternative precedent format: `designs/complete/
  techdesign-generic-static-members.md` §4.4; `docs/research-expr-reification.md` (overall
  structure this doc mirrors).
- In-place-rewrite precedent (cited and rejected as Option C):
  `designs/complete/techdesign-method-references.md`, `Checker.cpp:1274-1363`
  (`rewriteAsMethodRef`).
- Free-grammar-slot precedent for Option A: `Parser.cpp:630-636` (`name!(args)` macro-call
  comment).
- Related but distinct: `designs/requests/accepted/
  request-generic-overload-monomorphization.md` (per-instantiation overload resolution
  inside a generic body — not call-site syntax).
- `docs/reference.md` §2.5 (Generics), §3.1 (precedence table), §3.7 (Strictness — the
  current "no inference source" error).
- Confirmed absent: `docs/footguns.md`, `known_bugs_1.md`, `known_bugs_2.md` (no existing
  row/entry for this gap as of 2026-07-19).
