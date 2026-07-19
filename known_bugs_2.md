# Known bugs — part 2 of 2 (known_bugs_2.md)

Active, unresolved bugs only. This is one half of the known-bug register;
the other half lives in `known_bugs_1.md`. The two files together hold every open
entry, with high- and low-priority bugs dispersed evenly across both so
neither file skews toward one tier. Bug numbers are the stable identity —
a `#N` cross-reference may point at an entry in the companion file.

Each entry has a minimal repro, expected vs. actual behavior, and a
root-cause pointer. Fixed bugs are not tracked here — see git history
(commits prefixed `bug.md #N`) for their resolutions.

Every entry carries a priority tag (`[P0]`–`[P3]`) in its heading, assigned
by the marker checklist in **Priority system** below, plus a one-line
justification citing the exact marker(s) so the assignment is auditable.

Current standings for this file (within a tier, ordered by bug number):

| Priority | Bugs |
|----------|---------------|
| P0       | #88, #89 |
| P1       | #83, #86 |
| P2       | — |
| P3       | — |
| P3       | — |

Each entry's Workaround note (inline, above) carries its own debt sites — there is no
separate `docs/footguns.md` registry (retired 2026-07-19, merged into these two files).
Once the composition corpus lands, a fixed bug also gets a red-lane repro promoted to
green under `tests/corpus/composition/` (`designs/techdesign-composition-corpus.md`).
Fixing #N means: fix, delete the entry here, promote red→green — one commit.

---

## Priority system

Priorities are derived mechanically from the markers below so that different
agents assign the same tier to the same facts. Evaluate tiers top-down
(P0 → P3) and assign the first tier with at least one matching marker. Two
overrides, applied in this order:

1. **Explicit owner ruling wins.** If the entry records an owner ruling that
   names a priority ("low priority for now", "treat as P0", ...), use that
   priority regardless of markers, and cite the ruling.
2. **Semantics-ruling cap.** If the intended *observable behavior* is still
   undecided — the owner must choose what programs should see before any fix
   can be written — cap the priority at P2 unless a P0 marker applies. A
   pending ruling that concerns only fix *shape or ownership* (the intended
   behavior is undisputed) does not cap.

Definitions used by the markers: the **oracle** is `--run` (`Eval.cpp`), the
ground truth `.expected` files are generated from
(`designs/complete/techdesign-portable-backend.md` §0.4). **Actively-maintained engines**
are `--ir`, emit-C++ (`--build`), and LLVM (`--build-native`); LLVM is the
primary backend (portable pivot, 2026-07-05). **Frozen** means
`X64Gen.cpp`/ELF (`--emit-elf`). **Ordinary user code** means expressible in
a plain `.lev` source file without editing the compiler or the prelude.

### P0 — critical
- **P0.1** The oracle prints wrong output for ordinary user code — wrong per
  the language reference, or unanimously contradicted by the
  actively-maintained engines. (Risk: the wrong output gets baked into
  `.expected` files and the correct engines then read as regressed.)
- **P0.2** A track is blocked right now and no workaround lets it proceed.
- **P0.3** An actively-maintained engine exhibits **silent state corruption**
  for ordinary, checker-accepted code: memory corruption, data going stale
  after unrelated activity, or an operation silently dropped — any failure
  whose symptom surfaces *away from the causing site*. Distinguished from
  P1.1 (a wrong value observed at the faulting expression itself): a P0.3
  defect's blast radius is unbounded, it mis-attributes downstream debugging
  time, and a crash-later variant counts even though the exit is nonzero.
  (Owner policy 2026-07-13, stop-the-line: these head the fix queue, and no
  new consumer-track code is architected on the affected construct while one
  is open — see `designs/techdesign-composition-corpus.md` §1.)

### P1 — high
- **P1.1** An actively-maintained engine silently produces a wrong value —
  exit 0, no diagnostic — for code the checker accepts, and the entry does
  not dispute which behavior is correct.
- **P1.2** The only workaround is per-use: every future track touching the
  area must independently know about it and re-apply it (naming conventions,
  per-callsite guards, ...), rather than one workaround retiring the risk.

### P2 — medium
- **P2.1** Engines diverge and a semantics ruling must pick the intended
  behavior before any fix is valid (see also the cap in override 2).
- **P2.2** Performance/resource-only: output is correct on every engine, but
  asymptotic complexity or memory behavior is wrong on an
  actively-maintained engine.
- **P2.3** A documented language feature fails loud (compile-time or runtime
  error) on one actively-maintained engine while working on the others.
- **P2.4** Missing diagnostic with a correct happy path: an unsupported
  construct should error but doesn't, and no supported construct misbehaves.

### P3 — low
- **P3.1** The owner explicitly ruled it low priority (override 1).
- **P3.2** Only frozen-backend (`X64Gen`/ELF) behavior is affected.
- **P3.3** The fix already landed; only regression-test coverage is missing.
- **P3.4** Cosmetic only (formatting/spelling of output), no value or
  control-flow difference.

---

## #83 [P1] — implementing a dependency's interface requires bare (uses-imported) member types, and `uses` is package-global via source concatenation

**Markers:** P1.2 (the only workaround is per-use: every future driver/consumer
track — Track 06 ORM, `atlantis-postgres`, any package implementing a C3/atlantis
interface — must independently know and re-apply the pattern).

Two linked resolver behaviors surface when a package (B) implements an interface
declared in a dependency (A):

1. **Alias-qualified member types fail interface satisfaction.** If B spells a
   method's return/param type as `A::Data::Foo`, the checker reports
   `Promise<A::Data::Foo>` is *not assignable to* the interface's required
   `Promise<Foo>` — even for a non-generic `A::Data::Foo` vs `Foo`. The
   alias-qualified type and the interface's in-package bare name are treated as
   distinct identities. **Workaround:** `uses A::Data;` and spell member types
   **bare** (`Foo`); the interface-inheritance clause itself may stay qualified
   (`class Impl : A::Data::IThing`).

2. **`uses` behaves package-global.** Source files in a package are concatenated
   (the "file boundaries dissolve" invariant), so a bare name resolves only if
   **every** file that contributes to the namespace carries the `uses` — a single
   sibling file lacking `uses A::Data;` makes the bare names in *other* files read
   as `unknown type`. **Workaround:** put the `uses` line in *every* `.lev` file of
   the package, even files that do not themselves reference the imported types.

Minimal repro: pkgA `namespace A { namespace Data { class Foo{…} interface IThing{ Promise<Foo> make(); } } }`;
pkgB `uses A::Data; namespace B { class Impl : A::Data::IThing { Promise<A::Data::Foo> make(){…} } }`
→ "not assignable to required `Promise<Foo>`". Changing `A::Data::Foo` → `Foo`
(bare) compiles. Adding a second pkgB file that declares `namespace B { … }`
without its own `uses A::Data;` reintroduces `unknown type Foo` in the first file.

**Root cause pointer:** (1) type-identity comparison keys on the alias-qualified
symbol path rather than the canonical class; (2) `uses` scope is applied per
translation-unit-fragment but bare-name lookup runs over the merged namespace.
Both are worked around throughout `packages/atlantis-mysql/` (bare C3 types +
`uses Atlantis::Data;` in all eight src files); a fix would let drivers name C3
types either way. **Debt sites:** every file in `packages/atlantis-mysql/src/`
(8 files) + `packages/atlantis-mysql/tests/{loopback,pool}/main.lev`.

---

## #86 [P1] — LA-31 reifier's `Un("!", …)` emits the op text `"?"`, not `"!"`

**Markers:** P1.1 (an actively-maintained engine — the Checker's reifier,
exercised on oracle/IR/LLVM alike since it runs pre-lowering — silently
stores the wrong op *text* in checker-accepted code, with no diagnostic; any
downstream tree-walker that dispatches on the string, e.g. `expr::eval`'s
`if (u.op == "!") return !evalTruth(inner);`, silently takes the wrong
branch, which is an observable wrong *value*, not merely cosmetic).

Found implementing `designs/expr-reification/techdesign-03-verification.md`
§2 row 10 (`!u.active` → expected `Un(!,Field(active))`). Found in
`Checker.cpp`'s reifier (`techdesign-02-reifier.md` §3.3): `reifyNode`'s
`ExprKind::Unary` arm (`Checker.cpp:3353-3362`) calls `reifyOp(e->op)`
(`Checker.cpp:3004-3008`), which falls through to the shared `opSymbol(k)`
(`Checker.cpp:34-47`) for anything other than `&&`/`||`. `opSymbol`'s switch
has no case for `TokenKind::Bang` (logical not) — every other reifiable
unary/binary token is covered (`Minus`, `EqEq`, `BangEq`, `Lt`, `Gt`, `Le`,
`Ge`, `Plus`, `Star`, `Slash`, `Percent`, …) — so `Bang` falls to
`default: return "?"`.

Repro:
```
class User { bool active; new User(bool a) { active = a; } }
expr::Expr<(User)=>bool> useIt(expr::Expr<(User)=>bool> e) { return e; }
expr::Expr<(User)=>bool> e0 = (u) => !u.active;
```
`--run`/`--ir`/LLVM (via `--build-native`) all print `e0.tree`'s reified
`Un` node with `.op == "?"` (verified by walking the tree with a `match`
and printing `n.op` directly) instead of the reference-spelling `"!"` the
design's dump table (doc03 §1: `Un(!,<e>)` / `Un(-,<e>)`) and the ask's own
taxonomy require. `.fn(...)` (the closure leg) is unaffected — it is
ordinary compiled negation, not text-driven — so this is purely a leg-2
(tree) defect; a differential-corpus row comparing `.fn` against
`expr::eval`'s tree-walk would show `.fn`/eval **disagreeing** on any
reified `!` body, since `expr::eval`'s `Un` arm branches on `u.op == "!"`
literally.

**Root-cause pointer:** add a `case TokenKind::Bang: return "!";` arm to
`opSymbol` (`Checker.cpp:34`), or special-case `Bang` in `reifyOp`
alongside its existing `AmpAmp`/`PipePipe` overrides (`Checker.cpp:3004`) —
either fixes it in the one register both binary and unary reification share.
**Workaround:** none at the language level (the reifier is the only path
that produces this node); LA-31 Stage 3's positive/differential corpora
exclude the `!`-unary row pending this fix (`designs/expr-reification/techdesign-03-verification.md`
§10 implementation log).

---

## #88 [P0] — `--expand` round-trip of two reified sites sharing the same `expr::Call` name (e.g. two `"contains"` sites, different receiver kinds) silently corrupts one site's tree walk

**Markers:** P0.1 (oracle and IR both — unanimously — silently produce the
wrong value for ordinary, checker-accepted code with no diagnostic; wrong per
the ask's own pinned semantics, doc 01 §3.1's Array-receiver `contains` note).

Found implementing `designs/expr-reification/techdesign-03-verification.md`
§5 (the `--expand`→`--run` round-trip check) while building the §4
differential corpus. A file with a **string**-receiver `contains`
(`u.name.contains("d")`, whitelist row `string.contains/1`) and an
**Array**-receiver `contains` (`tags.contains(u.tag)`, whitelist row
`Array.contains/1`) — both reify to `expr::Call("contains", …)`, differing
only in `recv`'s node kind (`Field` vs `Bind`) — round-trips fine on the
**first** compile (ordinary reification via the lambda-literal path) but
**breaks the Array-receiver site's tree walk** after one `--expand`+recompile
cycle. Minimal repro:
```
class Rec { string name; new Rec(string n) { name = n; } }
class Tag { string tag; new Tag(string t) { tag = t; } }
// … expr::eval walker (doc 01 §3.1) pasted in verbatim …
Rec ada = Rec("Ada");
expr::Expr<(Rec)=>bool> eCont = (u) => u.name.contains("d");
console.writeln("strcont:" + eCont.fn(ada).toString() + ":" +
    evalTruth(evalNode(eCont.tree, recOfRec(ada), eCont.binds, noArr)).toString());

Array<string> tags = [];
tags = tags.add("red");
Array<string | int | float | bool | None> tagArr = [];
tagArr = tagArr.add("red");
expr::Expr<(Tag)=>bool> eArrCont = (u) => tags.contains(u.tag);
Tag tRed = Tag("red");
console.writeln("arrcont:" + eArrCont.fn(tRed).toString() + ":" +
    evalTruth(evalNode(eArrCont.tree, recOfTag(tRed), eArrCont.binds, tagArr)).toString());
```
First compile (`--run`): `strcont:true:true` / `arrcont:true:true` — correct.
After `--expand > rt.lev` then `--run rt.lev` (or `--ir rt.lev`):
`strcont:true:true` / **`arrcont:true:false`** — the Array-receiver site's
`.fn` (closure leg, unaffected — still `true`) and `evalNode` tree-walk now
disagree; `evalNode`'s own `match (c.recv) { expr::Bind => { … } }` special
case (evalNode.cpp-equivalent in the corpus's own `expr::eval`, doc 01 §3.1)
silently falls through to the generic string-receiver path instead of
matching, even though a **direct** `match (eArrCont.tree.recv) { expr::Bind
=> … }` right next to the call (outside `evalNode`) correctly reports "IS
Bind" — ruling out a node-identity/runtime-tag corruption at the object
level. The defect requires BOTH: (a) round-tripping through `--expand` (the
same construct reified directly, never printed and reparsed, does not
reproduce it — see `tests/corpus/expr_reify/expr_reify_r15_arraycapture.lev`,
which is round-trip-clean in isolation), AND (b) **two** `expr::Call(...)`
construction sites in the same file sharing the identical `name` string
literal (`"contains"`) with different receiver-node kinds — a single
Array-receiver site round-trips fine alone (`expr_reify/expr_reify_twin_r15.lev`
and a synthetic single-site repro both round-trip clean); a two-site
same-string-different-shape file is what triggers it.

**Root cause pointer:** not isolated to a file/line (out of this stage's
`src/`-touching scope). Given the trigger requires two *reparsed* (not
directly-reified) `expr::Call` construction sites sharing a literal `Call`
name/text, the likely area is a checker- or printer-side cache/dedup keyed on
call-site text or argument shape (`Program::synthNames` interning, per the
LA-31 Stage 2 implementation log's note that reified text is interned into
the program's arena — `designs/complete/techdesign-02-reifier.md` §10) rather
than the reifier itself (unaffected, since this only reproduces via
`--expand` reparse of two already-reified constructions, not via reification
proper). **Workaround:** none identified; avoid relying on `--expand` output
of files with two or more reified `Array<T>.contains` (or any two calls
sharing a whitelisted name with different receiver kinds) sites for anything
beyond visibility. LA-31 Stage 3 does not wire an `--expand` round-trip test
over `tests/corpus/expr_diff/` for this reason (only the required
`tests/corpus/expr_reify/` round-trip, which has no two-different-receiver
same-name pairing, is wired) — logged in
`designs/expr-reification/techdesign-03-verification.md` §10.

---

## #89 [P0] — a whitelisted `Call` (e.g. `.like`) inside a reified lambda whose parameter type is a generic type parameter (not a concrete class) is rejected as "non-whitelisted", even though the SAME body reifies fine with a concrete parameter type — blocks Track 06 M2's headline example verbatim

**Markers:** P0.2 (a track is blocked right now with no workaround: Track 06
ORM's own headline example, `designs/atlantis/techdesign-06-orm.md:337`
— `.where((u) => u.active && u.name.like("A%"))` through
`Query<E> { Query<E> where(expr::Expr<(E) => bool> p); }` — fails to
compile verbatim; there is no way to spell this ORM API shape and have
`.like`/`.ilike`/`.startsWith`/`.endsWith`/`Array<T>.contains` reify inside it).

Found implementing `designs/expr-reification/techdesign-03-verification.md`
§6 (the Track 06 M2 activation smoke: "a minimal in-file `Query<E>`-shaped
consumer... driven with the ask's own example shapes"). Minimal repro:
```
class User { int age; string name; new User(int a, string n) { age = a; name = n; } }
class Query<E> {
    Array<expr::Node> wheres;
    new Query() { wheres = []; }
    Query<E> where(expr::Expr<(E)=>bool> e) { wheres = wheres.add(e.tree); return this; }
}
Query<User> q = Query();
q = q.where((u) => u.age > 18);              // OK — reifies fine
q = q.where((u) => u.name.like("A%"));       // FAILS — see below
```
The second call fails with:
```
error: cannot reify non-whitelisted call 'like': outside the LA-31 reifiable subset
note: reifiable calls: string.like/1, string.ilike/1, string.startsWith/1, string.endsWith/1, string.contains/1, Array.contains/1
```
even though `like` **is** in the printed allow-list and the *identical* lambda
body (`(u) => u.name.like("A%")` against a **non-generic** `expr::Expr<(User)=>bool>`
parameter — `designs/expr-reification/tests/corpus/expr_reify/expr_reify_r12_like.lev`)
reifies correctly. The plain-comparison call one line above, through the
exact same generic `Query<E>.where`, reifies fine — isolating the defect to
the whitelisted-`Call` path specifically, not generic lambda-parameter typing
in general (`Bin`/`Un` reification never queries the receiver's type at all,
so it can't observe this).

**Root cause pointer (likely, not confirmed — this stage does not touch
`src/`):** `Checker.cpp:3384-3386`, the reifier's whitelist-applicability
check —
```cpp
Type rt = typeOf(recv);
bool isStr = rt.canonical == "string";
bool isArr = rt.kind == TKind::Class && rt.sym && rt.sym->name == "Array";
```
— re-derives `recv`'s type via a fresh `typeOf(recv)` call from *within the
reifier's own walk*, separate from whatever substitution context
`checkLambdaBody` used when it originally checked the lambda body against the
generic method's substituted parameter type (`E → User`). Ordinary field
access through the substituted `E` clearly resolves correctly elsewhere (the
plain-comparison sibling call works, and `u.name`'s field type is fixed by
`User` regardless of how `u` is reached), so the substitution context is
available *somewhere* — but the reifier's own `typeOf(recv)` re-query,
running after/outside that original checking pass, most plausibly loses it,
yielding a `Type` whose `.canonical` is not literally `"string"` (an erased
placeholder or the unsubstituted `E`, not isolated further). This is
`Checker.cpp`-internal, in the same file/function region Stage 2 already
owns (`reifyNode`'s `ExprKind::Call` arm, `designs/complete/techdesign-02-reifier.md`
§3.3) — out of this stage's scope to fix.

**Workaround:** none inside the reified body — a whitelisted method call
survives reification only when the lambda's parameter is a **concrete**
class type, not a class generic-substituted through the consuming method's
own type parameter. Track 06 M2 cannot adopt the `Query<E>.where(expr::Expr<(E)=>bool>)`
shape from `techdesign-06-orm.md` §3/§11 as designed until this is fixed;
LA-31 Stage 3's ORM activation smoke (`tests/corpus/expr_reify/expr_orm_smoke_1.lev`)
therefore demonstrates only the plain-comparison/arithmetic shapes (which
reify correctly through `Query<E>`) and does **not** include a whitelisted
call, with this bug cited inline — logged in
`designs/expr-reification/techdesign-03-verification.md` §10.
