# Research: Expression Reification — `expr::Expr<F>` (LA-31)

**Status:** research input for the LA-31 tech design. Not a design; carries no rulings.
**Feeds:** `designs/requests/request-expr-reification.md` (the ask) and
`designs/atlantis/techdesign-06-orm.md` §3 (the first consumer).
**Date:** 2026-07-18. **Author target:** the owner, who builds LA-31 per R9.

This document assembles everything needed to write the LA-31 tech design: the exact
compiler touchpoints, the reusable precedents already in-tree (with file/line
citations), the reifier algorithm in enough detail to implement, a per-engine impact
map, the prelude surface to add, the testing shape, and — most importantly — the small
number of genuine architectural decisions the design must pin (§9), each stated with the
evidence that constrains it. Where the request already pins a semantic, this doc records
*how* the compiler realizes it, not whether.

---

## 0. One-paragraph statement of the problem

A **lambda literal** appearing in a position typed `expr::Expr<F>` must compile to **two
co-resident artifacts**: (leg 1) the ordinary closure the language already produces for
any lambda, and (leg 2) a **runtime, walkable data tree** (`expr::Node` prelude objects)
mirroring the lambda's checked body, plus a **binds array** holding each captured value
snapshotted once at construction, plus a **compile-time-unique `siteId`**. Both legs are
ordinary emitted code — no reflection, no runtime parsing, visible under `--expand`,
`O(nodes)` cost. The reifiable body subset is closed and small (§1.3 of the ask); any
construct outside it is a compile error *at the lambda site* naming the construct. Two
new prelude `string` methods (`like`, `ilike`) ship with pinned semantics. Nothing landed
does this: procedural macros take a `string` → opaque `Ast` (not walkable), and `comptime`
reifies *values* (not code). This is a new, scoped **language** capability, not an ORM
feature.

---

## 1. Consumer context — why the shape is what it is

The ORM (Track 06 §3) is the *first* consumer, not the *only* one; the design must not
bake ORM assumptions into the language surface. What the ORM does with the two legs
(`designs/atlantis/techdesign-06-orm.md` §3.2):

| leg | ORM consumer |
|---|---|
| `tree` + `binds` | the MySQL renderer → `SqlAndParams` → C3 driver (`col LIKE BINARY ?`, `?` params from binds) |
| `fn` (closure) | in-memory repos for tests (DI-swapped), relation stitching, any future engine that runs Leviathan natively |

The consumer surface is a `Query<E : Model>` whose combinators take `expr::Expr<(E)=>bool>`
/ `expr::Expr<(E)=>int>` (§3.5 of the ORM doc). It `match`-walks `tree`, reads `binds`
positionally, and renders one stable SQL string per call site (captured values ride as
`?` params — *user input never reaches SQL text by construction*). The `siteId` reserves
a render-memo slot (designed, not required to land).

**Design consequence:** `expr::` is a general prelude namespace (like `regex::`,
`json::`), owned by the language, consumed by libraries via `match`. The node taxonomy is
a **closed set v1** (§1.2 of the ask) that grows only by ticket amendment (§1.3's
"consistency law"). The ORM's rendering fidelity is the ORM's own differential corpus,
not the language's — the language only guarantees *tree mirrors checked AST* (ask §2.1).

---

## 2. The compiler pipeline — where every stage runs (the map the design reasons over)

Driver: [main.cpp:446-557](../src/main.cpp#L446-L557). The order is load-bearing for
LA-31 because of the `--expand` timing question (§9.1).

```
  Lexer → Parser
    → Resolver pass 1            (Resolver::run; scopes, enum desugar, struct-eq synth)
    → RuleEngine (if hasMeta)    (comptime fold, macro expand, rule inject)  [Rules.cpp]
    → Resolver pass 2            (resolver2; re-resolves folded/injected code)
    ├─ mode==Expand  → printProgramSource(program)     ← NO checker has run  ★
    ├─ mode==ExpandAst → printProgram(program)         ← NO checker has run
    └─ mode∈{Run,Ir,EmitCpp,EmitLlvm,...}:
         Checker::run(program)   [Checker.cpp]         ← types, overload res, rewrites
         → Evaluator (oracle)   | Lowerer → IrModule → { IrInterp | LlvmGen | X64Gen }
```

Key facts, each verified in-tree:

- **`--expand` and `--ast-after-rules` run *before* the Checker.**
  [main.cpp:508-526](../src/main.cpp#L508-L526) dumps the program in the `Expand`/`ExpandAst`
  arms; the `Checker checker(...)` construction is only reached in the
  `Full|Run|Ir|EmitCpp|...` arm at [main.cpp:528-534](../src/main.cpp#L528-L534). **Any AST
  rewrite the Checker performs is invisible to `--expand`.** This is the crux of §9.1.
- The **rule stage is gated on `program.hasMeta`** ([main.cpp:456](../src/main.cpp#L456));
  a program with no metaprogramming surface never runs it. LA-31 must *not* depend on
  `hasMeta` (a plain query program uses no attributes/comptime/rules), so reification
  cannot live inside the RuleEngine as currently gated — unless the gate is widened or the
  reifier is a new pass. (§9.1.)
- **`comptime` folding happens in the RuleEngine, before both the checker and `--expand`.**
  So "folded comptime constants" (ask §2.1/§2.3) are already literals by the time any later
  stage sees them: `RuleEngine::foldExpr` → `reify` at
  [Rules.cpp:524-547](../src/Rules.cpp#L524-L547).
- **Enum members desugar in the Resolver** to per-member const globals + a carrier value,
  recorded in `Program::enumDesugars` ([Ast.hpp:483-492](../src/Ast.hpp#L483-L492)); this
  survives the pass-2 re-resolve because it lives on the `Program`, not the `Sema`.

---

## 3. The four reusable precedents (this is most of the implementation)

LA-31 is largely *composition of landed mechanisms*. The design should lean on these
rather than invent.

### 3.1 Closure lowering + capture analysis — **leg 1 is already done, and it hands us the binds set**

Every engine already turns a lambda literal into a closure with a captured-value set:

- IR shape: `MakeClosure` / `CaptureVar` / `LoadCapture`
  ([Ir.hpp:78-81](../src/Ir.hpp#L78-L81)).
- Lowerer: `Lowerer::lowerLambda` at
  [Lower.cpp:1723-1819](../src/Lower.cpp#L1723-L1819). The capture set is *exactly the
  visible locals the body references* (`lwrCollectExprNames`, snapshot semantics), plus
  `this` when the body touches a member ([Lower.cpp:1740-1753](../src/Lower.cpp#L1740-L1753)).
- Oracle: `Closure { const Expr* lambda; env; thisObj; thisClass; }`
  ([RuntimeValue.hpp:80-85](../src/RuntimeValue.hpp#L80-L85)); free-name capture mirrors
  Lower's ([Eval.cpp:756-808](../src/Eval.cpp#L756-L808)).

**Why this matters for leg 2:** the binds array is the *same capture set*, snapshotted at
the *same moment* — that is precisely what makes the two legs agree (ask §2.2). A captured
local/param/`this.x` read that becomes `CaptureVar` in leg 1 becomes a `Bind(slot)` node +
a `binds[slot]` value in leg 2. The reifier should reuse the closure's capture-ordering
discipline so slot numbers and closure-capture order are one analysis, not two. (Snapshot
timing: leg 1 captures by reference into the closure `env`; leg 2 evaluates the value once
into `binds`. For value-typed captures these agree trivially; for reference captures they
agree because the *tree only ever consults `binds`*, never the live closure — the corpus
row in ask §2.2 pins this.)

### 3.2 `RuleEngine::reify` — value → AST literal (the *building-AST-from-nothing* precedent)

[Rules.cpp:549-649](../src/Rules.cpp#L549-L649) constructs fresh `Expr` nodes (IntLit,
StringLit, BoolLit, `None` as a Name, Array, and **value-struct construction as a
`ClassName(field₁,…)` Call** at [Rules.cpp:609-644](../src/Rules.cpp#L609-L644)). LA-31's
reifier does the structurally identical thing, but its *input* is the lambda body AST (not
a runtime `Value`) and its *output* is a tree of `expr::Node`-subclass constructor calls.
The node-construction idiom — `make_unique<Expr>(ExprKind::Call)`, set callee to a
namespaced name, push args — is copy-pasteable. Note especially the value-struct case:
it already emits a **constructor Call whose callee is a bare class Name**; LA-31 emits
`expr::Field(...)` etc. (namespaced), which the bug-32 path handles (§3.5).

### 3.3 Method-reference eta-expansion — **the killer precedent: an in-place Checker rewrite into ordinary AST**

`Checker::rewriteAsMethodRef` at
[Checker.cpp:1274-1363](../src/Checker.cpp#L1274-L1363) rewrites a value-position `C::m`
into a synthesized `Lambda` node *in place* — "an ordinary Lambda every engine already
executes … so this is the whole implementation: no new IR, no Lower/Eval code"
([Checker.cpp:1274-1277](../src/Checker.cpp#L1274-L1277)). It mutates `e->kind`,
`e->params`, `e->a`, etc. and returns a synthesized `FuncRef` type.

This is the template for LA-31's recommended shape: **the Checker rewrites a Lambda
literal that lands in an `expr::Expr<F>` position into a construction Call**
`expr::Expr<F>(<the lambda>, <tree ctors>, <binds array>, <siteId>)`, which every engine
already lowers as ordinary construction. No new IR op, no new Eval/Lower/LlvmGen path for
the *carrier* — only the reifier that builds the argument sub-trees, plus the prelude
class definitions. The `--expand` visibility caveat (§9.1) is the one wrinkle vs.
method-refs (which are also invisible to `--expand`, and the ask does not demand
otherwise for them — but *does* for LA-31 in §2.4).

The method-ref code also demonstrates the two **hook points** a lambda can arrive through:
- **call-argument position** — deferred and walked in `genericReturn`/overload resolution
  ([Checker.cpp:2790-2813](../src/Checker.cpp#L2790-L2813), lambda-last inference);
- **value position with an expected type** (bind/return/assignment) — `tryResolveMethodRef`
  is driven from `typeOf` with an `expected` type
  ([Checker.cpp:3050-3052](../src/Checker.cpp#L3050-L3052)).
  LA-31 must fire in **both** (ask §1.1: "parameter, bind, return — ordinary
  target-typing").

### 3.4 Enum member → const global + carrier int — the `Lit(carrier int)` path

`Enum::Member` reads resolve, in `Checker::typeOfMember`
([Checker.cpp:1153-1167](../src/Checker.cpp#L1153-L1167)), to the mangled const global
recorded in `EnumDesugar::Member { name; carrier; global; }`
([Ast.hpp:483-492](../src/Ast.hpp#L483-L492)). Ask §1.3 wants `RelationType::Friend` →
`Lit(carrier int)`. The reifier, seeing a `Member` node with `colon==true` whose resolved
target is an enum-member global, reads `m.carrier` from `program_->enumDesugars` and emits
`expr::Lit(<carrier int literal>)`. (This is the "folded comptime constant / resolved
member" clause of ask §2.1 made concrete — and it needs *post-resolve* info at minimum,
*post-check* info to be robust; see §9.1.)

### 3.5 Namespaced construction & closed-set `match` (consumer plumbing that already works)

- **`expr::Field(...)` construction** — qualified `ns::Class(...)` construction dispatches
  in prelude/unchecked and user code (memory: *Bug 32 namespace-ctor landed*). The reifier
  emits `Call` nodes with a `Member(colon, path=["expr"], name="Field")` callee; the
  Checker's construction path stamps `resolvedClass` ([Lower.cpp:1204-1333](../src/Lower.cpp#L1204-L1333)).
- **`match` over the closed Node set** — consumers dispatch with `match (n) { expr::Field
  => …; expr::Bin => …; else => … }`. Type-pattern arms narrow the subject
  ([Checker.cpp:955-1006](../src/Checker.cpp#L955-L1006)); the MI-dispatch family that made
  container/leaf method resolution correct is landed (memory: *Container-leaf mixin
  footgun FIXED*, MI-dispatch family `054e159`). Reference classes in the taxonomy are
  explicitly fine (ask §1.2): the tree is runtime data, immutable by convention.
- **The `string|int|float|bool|None` union field** (`Lit.v`, `Expr.binds` element) is the
  same "DbValue" union the MySQL driver and C3 already move end-to-end (memory: *Atlantis
  Track 05 MySQL landed*), so union storage/`match` needs no new capability.

### 3.6 Generics are erased — `Expr<F>` costs nothing structurally

Leviathan generics are **type-erased, compiled once, dynamically dispatched** (memory:
*Generics are erased*). `Expr<F>` and `Expr<(U)=>bool>` are the same runtime class; `F fn`
is just a closure-typed field. No monomorphization, no `T::member` (that would be LA-18
territory). The generic parameter exists only for the checker to (a) target-type the
lambda against `F`'s signature and (b) give the consumer a typed `.fn`.

---

## 4. Mapping the ask's surface to concrete compiler work

### 4.1 The carrier type + node taxonomy → **prelude classes** (Resolver string literals)

The prelude is C++ raw-string source parsed at startup:
`kPreludeCore` + `kPreludeStd` + `kPreludeRest` + … concatenated in
`Resolver` ([Resolver.cpp:22](../src/Resolver.cpp#L22),
[:963](../src/Resolver.cpp#L963), [:2597](../src/Resolver.cpp#L2597),
[:5303-5308](../src/Resolver.cpp#L5303-L5308)). LA-31 adds a `namespace expr { … }` block
(likely appended to `kPreludeRest` or a new `kPreludeExpr` segment, wired into
`preludeFile_.text` at [Resolver.cpp:5304-5305](../src/Resolver.cpp#L5304)).

Concrete prelude to add (fields per ask §1.1/§1.2, **plus constructors the reifier calls**
— the reify precedent §3.2 requires a ctor whose params match the emitted call shape):

```
namespace expr {
    class Node { }                                            // base; empty
    class Field  : Node { Array<string> path;  new(Array<string> path) { this.path = path; } }
    class Lit    : Node { string | int | float | bool | None v;  new(string|int|float|bool|None v) { this.v = v; } }
    class Bind   : Node { int slot;            new(int slot) { this.slot = slot; } }
    class Bin    : Node { string op; Node l; Node r;  new(string op, Node l, Node r) { … } }
    class Un     : Node { string op; Node e;   new(string op, Node e) { … } }
    class Call   : Node { string name; Node recv; Array<Node> args;  new(…) { … } }
    class Assign : Node { Field target; Node value;  new(Field target, Node value) { … } }

    class Expr<F> {
        F fn;
        expr::Node tree;
        Array<string | int | float | bool | None> binds;
        int siteId;
        new(F fn, expr::Node tree, Array<string|int|float|bool|None> binds, int siteId) { … }
    }
}
```

Prelude authoring gotchas that *will* bite (memory: *Prelude backend gotchas*,
*Prelude not checked*, *emit-C++ by-name reachability*):
- The **Checker never walks the prelude** (memory: *Prelude not checked*) — these bodies
  are resolved but not type-checked; keep them trivial (field stores), no flow-narrowing
  reliance (memory: *Prelude no narrowing*).
- **Same-namespace construction/calls must be qualified or bare-per-rule** — inside
  `namespace expr`, a ctor calling a sibling class uses the bare/`this.` discipline from
  memory *this-receiver lambda bug* and *Prelude backend gotchas*.
- **emit-C++ eager-global-instance** forces every method of a class that has a global
  instance — avoid module-level `expr::*` globals in the prelude.
- Adding a common-named method (none here) can drag natives into emit-C++ by-name
  reachability — the `expr::` names are distinctive, low risk.

### 4.2 The conversion rule (ask §1.1) → **Checker target-typing hook**

Today a Lambda argument is only accepted against a `TKind::FuncRef` parameter
([Checker.cpp:2455-2460](../src/Checker.cpp#L2455-L2460)). `expr::Expr<F>` is a
`TKind::Class`, so the current path rejects the lambda ("applicable=false"). The design
adds: **when the target type is `expr::Expr<Fn>` and the argument is a *lambda literal*,
extract `Fn`'s function signature from the class's single generic argument, run
`checkLambdaBody` against those param types
([Checker.cpp:2739-2773](../src/Checker.cpp#L2739-L2773)), then reify.** A non-literal
lambda *value* in that position is the compile error "only a lambda literal can be
reified" (ask §1.1) — detectable because the argument's `ExprKind != Lambda`.

Both arrival paths (§3.3) must be covered: call-argument (`typeOfCall`/`genericReturn`
lambda-last) and expected-type value position (`typeOf` with `expected`, the
`tryResolveMethodRef` driver site [Checker.cpp:3050](../src/Checker.cpp#L3050)).

### 4.3 The reifiable subset + diagnostics (ask §1.3) → **the reifier walk** (§5)

### 4.4 `string.like` / `string.ilike` (ask §1.4) → **two prelude methods** (§7)

### 4.5 `--expand` (ask §2.4), `--expand` roundtrip acceptance → **printer + timing** (§6, §9.1)

---

## 5. The reifier algorithm (implementable detail)

A single recursive walk `reifyNode(const Expr* e) -> ExprPtr /* a construction Call */`,
run over the (checked) lambda body, plus a slot allocator shared with capture analysis.
Modeled on `RuleEngine::reify` (§3.2) but type-directed.

**Inputs / state:**
- `param0Name` (and any further lambda params) — the reifiable subjects.
- `slotOf : (value-expr identity) -> int`, and `binds : vector<ExprPtr>` — the bind
  table. First reference to a given captured value allocates the next slot and pushes its
  reifying expression (a *copy of the original capture expression*, so leg-2 evaluates the
  live value once at construction) into `binds`.
- The reifier emits, at the site, the construction:
  `expr::Expr<F>( <original lambda>, reifyNode(body), [ binds… ], IntLit(siteId) )`.

**Per-node rules (mirror ask §1.3):**

| body construct | test | emit |
|---|---|---|
| `u.f`, `u.f.g` (base is a lambda param) | `Member` chain rooted at a param `Name` | `expr::Field([ "f", "g" ])` |
| enum member `E::M` | `Member`, `colon`, resolved to an enum-member global (§3.4) | `expr::Lit(<carrier int>)` |
| int/float/bool/string/`None` literal | `*Lit` / `Name "None"` | `expr::Lit(<literal>)` |
| captured local / param / `this.x` | `Name`/`Member(this)` not rooted at a lambda param | `expr::Bind(slotOf(e))` + push value into `binds` |
| `== != < <= > >= && \|\| + - * / %` | `Binary` with a whitelisted `op` | `expr::Bin("<op>", reify(l), reify(r))` |
| `!`, unary `-` | `Unary` | `expr::Un("<op>", reify(e))` |
| `x == None` / `x != None` on `T?` | `Binary(EqEq/BangEq)` w/ a `None` operand, field side `T?` | `expr::Bin("==", Field, Lit(None))` shape |
| whitelisted call | `Call` whose callee method name ∈ {`like,ilike,startsWith,endsWith,contains`} (string) or `contains` (Array); recv+args reifiable | `expr::Call("<name>", reify(recv), [ reify(args)… ])` |
| single `u.field = <reifiable>` | `Binary(Assign)`, LHS a param field, only in `Expr<(E)=>int>` positions | `expr::Assign(Field, reify(value))` |
| **anything else** | — | **compile error at `e->span`** naming the construct (see below) |

**Reject set (each a distinct, named diagnostic — ask §1.3, acceptance #1):** non-whitelisted
call, `await` (`ExprKind::Await`), nested lambda (`ExprKind::Lambda` inside the body),
statements/blocks (a block-bodied lambda, or any `Stmt` other than a single `return`/expr),
mutation of a capture, `is`/`match` (`ExprKind::Is`/`Match`), string interpolation over a
field (`StringLit` with interpolation segments touching a `Field`), index/range/ternary
(unless the design chooses to admit ternary → it is *not* in the whitelist, so reject).
Diagnostic text points at "the LA-31 whitelist (request-expr-reification.md §1.3)".

**Slot ordering & determinism.** Allocate slots in **first-reference source order** during
the single body walk. `siteId` must be "compile-time-unique per lambda site" and "stable
across runs of the same build" (ask §2.3) — a deterministic source-order counter (e.g. a
monotonic `int` bumped per reified site during the checker pass) or a hash of the site's
`SourceSpan.offset`. Do **not** use anything nondeterministic; the compiler already bans
`Date::now`/random in metaprogramming for exactly this reason. (§9.4 records the choice.)

**`Field` path construction.** A chain `u.a.b` parses as `Member(Member(Name u,"a"),"b")`;
the reifier walks down to the param root, collecting `["a","b"]` (ask §1.2's example). A
chain *not* rooted at a lambda param but at `this` → `Bind` (it's `this.x`); rooted at a
captured local → `Bind`; a chain rooted at a captured object with a further `.field`
(`capturedObj.field`) is **not** in the whitelist and must be decided (§9.3).

**Whitelist enforcement needs types.** To distinguish `s.like(p)` (accept) from
`u.age.like(p)` (reject as an ordinary type error) and to reject unknown methods with a
good message, the reifier benefits from the receiver's resolved type — an argument for
running *after* the checker has typed the body (§9.1). The *closure leg* independently
type-checks (it is real code), so a purely syntactic reifier can lean on that for
type-correctness and only own *reifiability* errors — a viable but weaker split (§9.1
option B).

---

## 6. `--expand`, source re-emit, and the round-trip acceptance

- The source printer `printProgramSource`
  ([AstPrinter.cpp:801](../src/AstPrinter.cpp#L801)) already renders `Call`, `Array`,
  `Member`, `Lambda` ([AstPrinter.cpp:479-495](../src/AstPrinter.cpp#L479-L495)), so a
  synthesized `expr::Expr<F>(...)` construction re-emits as compilable source **iff it is
  in the tree at print time**.
- The **round-trip acceptance** (`tests/run_expand_roundtrip.sh`) does `--expand` → `--run`
  the output → assert byte-identical run output. Crucially: this passes **whether or not
  the construction is visible**, because `--run` re-reifies the raw lambda. So the
  *functional* round-trip is satisfied by a checker-stage rewrite; only the **visibility**
  pin (ask §2.4 "shows everything") is not. The design must decide which it honors (§9.1).
- Negative-error acceptance uses the substring-grep harness style of
  `tests/run_regex_comptime_error.sh` (run, assert non-zero exit, assert the diagnostic
  substring). Each ask-§1.3 reject form gets one such fixture in `tests/negative/`.

---

## 7. `string.like` / `string.ilike` — concrete prelude implementation

These are **ordinary in-language prelude methods** (no natives), added to `class string`
in `kPreludeStd` alongside `contains`/`startsWith`/`indexOf`
([Resolver.cpp:176-334](../src/Resolver.cpp#L176-L334)). Semantics pinned by ask §1.4 and
ORM §3.4:

- `like(pattern)` — SQL-wildcard: `%` = any run (incl. empty), `_` = exactly one char,
  `\%`/`\_` = literals; **byte-exact, case-sensitive**.
- `ilike(pattern)` — same matcher, ASCII-case-insensitive (A–Z folded); beyond ASCII,
  bytes compare exact (documented bound).

A single backtracking matcher over `byteAt`/`length` (avoid `char` decode — the semantics
are byte-level; UTF-8 multibyte is compared byte-for-byte, matching the `LIKE BINARY`
rendering). `ilike` folds ASCII on both text and pattern before matching. Author them as
**leaf** helpers (no bare self-calls — memory *this-receiver lambda bug*; the string class
already documents this discipline at [Resolver.cpp:204-213](../src/Resolver.cpp#L204)).
Sketch:

```
bool like(string pattern) {
    // classic %/_ glob with \-escape, iterative backtracking over bytes.
    int n = length(); int m = pattern.length();
    int i = 0; int j = 0; int star = -1; int mark = 0;
    while (i < n) {
        if (j < m && pattern.byteAt(j) == 92) {          // backslash: next is literal
            if (j + 1 < m && pattern.byteAt(j+1) == byteAt(i)) { i = i+1; j = j+2; }
            else if (star >= 0) { j = star + 1; mark = mark + 1; i = mark; }
            else return false;
        } else if (j < m && pattern.byteAt(j) == 95) {   // '_'
            i = i+1; j = j+1;
        } else if (j < m && pattern.byteAt(j) == 37) {   // '%'
            star = j; mark = i; j = j+1;
        } else if (j < m && pattern.byteAt(j) == byteAt(i)) {
            i = i+1; j = j+1;
        } else if (star >= 0) { j = star + 1; mark = mark + 1; i = mark; }
        else return false;
    }
    while (j < m && pattern.byteAt(j) == 37) j = j+1;      // trailing '%'
    return j == m;
}
```

`ilike` = same body over an ASCII-folded copy of `this` and `pattern` (fold via a small
byte helper, not `toLower()` which is UTF-8-aware and would exceed the ASCII contract).
Ship their own unit rows (wildcards, escapes, ASCII fold, empty pattern, trailing `%`) —
ask §3 acceptance #3.

---

## 8. Per-engine impact

Because the recommended architecture rewrites to an **ordinary construction of an ordinary
class holding an ordinary closure**, the engine burden is *near zero* beyond the prelude:

| engine | what it must do | new work? |
|---|---|---|
| Oracle (`Eval.cpp`) | construct `expr::Expr`, store closure + tree object graph + binds array | none — ordinary `NewObject`/field stores + closure ([Eval.cpp:867](../src/Eval.cpp#L867)) |
| IR interp (`IrInterp.cpp`) | same, via `NewObject`/`MakeClosure`/`NewArray` | none |
| Lowerer (`Lower.cpp`) | lower the synthesized construction Call + the reused `lowerLambda` | none new; the reifier produces AST Lower already handles ([Lower.cpp:1204-1333](../src/Lower.cpp#L1204-L1333), [:1723](../src/Lower.cpp#L1723)) |
| LLVM (`LlvmGen.cpp`) | construct object, union-typed array, closure | none — all landed (closures, union arrays, ref classes) |
| emit-C++ / ELF | **standard deferral posture** (ask §2.5); ELF is frozen (memory *X64Gen frozen*) — do **not** gate landing on ELF | n/a |

The single caveat: whichever stage builds the tree (reifier) is new code, but it emits AST
the engines already consume. Confirm the union element type `string|int|float|bool|None`
round-trips through the LLVM leg (it does — MySQL DbValue, memory *Atlantis Track 05*).

---

## 9. The architectural decisions the design must pin (with the evidence)

The ask leaves these open; the design (per memory *No deferrals in designs*) must decide
each jointly with the owner *before* writing, with a fixed end-state.

### 9.1 ★ WHERE reification runs — the `--expand` timing tension (the big one)

**Facts.** (a) The reifiable-subset validation and the `tree-mirrors-checked-AST` guarantee
want *type info* (enum carriers, `T?` for `== None`, whitelist receiver types, resolved
members) — i.e., *after* the Checker. (b) `--expand` runs *before* the Checker
([main.cpp:508-534](../src/main.cpp#L508)), and ask §2.4 requires `--expand` to **show the
emitted construction**. (c) The method-ref precedent (§3.3) rewrites in the Checker and is
*not* shown by `--expand` — an accepted asymmetry there, but ask §2.4 forbids it here.

**Options:**
- **A — Checker rewrite + teach `--expand` to run it.** Reify during the Checker (full type
  info, best diagnostics, matches the method-ref precedent). Then make the `Expand` arm run
  the reification pass (or a display-only reifier) so the construction is visible. Cost: a
  small `--expand` wiring change; the printer already handles the node shapes (§6).
  *Best diagnostics; honors §2.4; most code reuse.* Recommended for evaluation.
- **B — Dedicated post-resolve, pre-check reifier pass (syntactic + resolver info).** Runs
  after resolver pass 2, before the Checker, so `--expand` sees it natively. Uses
  `enumDesugars` for carriers and scope info for capture/param distinction; leans on the
  ordinary Checker (closure leg) for type-correctness of whitelisted receivers. Cost: `T?`
  `== None` detection and whitelist receiver typing are weaker pre-check; some diagnostics
  degrade to ordinary type errors. Must run unconditionally (not `hasMeta`-gated).
- **C — RuleEngine integration.** Rejected on its face: the RuleEngine is `hasMeta`-gated
  ([main.cpp:456](../src/main.cpp#L456)) and a plain query program has no meta surface; and
  it runs before types exist. Documented only to close it.

**Recommendation to weigh:** A (Checker rewrite) for diagnostic quality and precedent
alignment, with the minimal `--expand` wiring to satisfy §2.4 — unless the owner values
`--expand` seeing the *un-reified* lambda and treats §2.4 as "the construction is
inspectable somewhere" (then B). This is the single most important thing to settle first.

### 9.2 The `Expr<F>` target-typing hook — call-arg vs. value-position, and overload interaction

The lambda-in-`Expr<F>` acceptance must slot into overload resolution
([Checker.cpp:2455-2460](../src/Checker.cpp#L2455-L2460)) without regressing the existing
"lambda only matches a FuncRef param" rule (bug #34). Decision: does a lambda literal
match *both* a `(U)=>bool` param and an `Expr<(U)=>bool>` param when both overloads exist
(ambiguity), or is `Expr<F>` strictly preferred / disjoint? Query APIs (ORM §3.5) only ever
declare the `Expr<F>` form, so a simple rule ("lambda literal → `Expr<F>` when the param is
`expr::Expr<_>`, else the existing FuncRef rule") likely suffices — pin it.

### 9.3 Capture-object member access (`capturedObj.field`)

`this.x` is explicitly a `Bind` (ask §1.3). A general `capturedLocal.field` read is *not*
listed. Decide: reject it (whole `capturedObj.field` is not `this` and not a param →
non-reifiable), or admit it as a single `Bind` of the whole `capturedObj.field` value
(snapshot). Simpler/safer: **the value is snapshotted as one `Bind`** exactly like any
other captured runtime value — the tree never needs to see inside it. Recommend admitting
it as a `Bind` (it *is* a captured runtime value), and pin that only *parameter*-rooted
field chains become `Field`. Confirm with the owner.

### 9.4 `siteId` generation

Deterministic source-order counter vs. `SourceSpan` hash (§5). A counter is simplest and
"stable across runs of the same build"; a span hash is stable under unrelated edits
elsewhere. Pin one. (Note: metaprogramming forbids `Date`/random — either choice complies.)

### 9.5 `binds` snapshot vs. closure capture — the exact agreement contract

Leg 1 captures references into `env`; leg 2 evaluates values into `binds`. Pin: `binds[k]`
is the value of the k-th distinct captured expression, evaluated **once, at construction,
in first-reference order**, and the tree consults *only* `binds` (never the live closure).
This is the corpus row in ask §2.2; state it as the normative contract so the differential
corpus can assert it (mutate a captured original after construction → both legs unchanged).

### 9.6 Whitelist growth mechanism

Ask §1.3 says the whitelist grows "only by ticket amendment" carrying both legs + a
corpus row (the §3.4 consistency law). The design should encode the whitelist as a single
enumerated table in one place (the reifier) so an amendment is a one-site change, and cite
that this is the "consistency law enforced at the register."

---

## 10. Testing strategy (maps 1:1 to ask §3 acceptance)

1. **Reify corpus (acceptance #1).** For each §1.3 form: a `.lev` that builds an `Expr` and
   prints a canonical dump of `tree`/`binds` (via a tiny in-language walker), with a
   `.expected`, run on oracle + IR + LLVM (the `run_corpus.sh` / `run_native.sh` matrix,
   CMake `corpus_*` targets). Plus **hand-written-twin byte-equivalence** for the emitted
   construction — compare `--expand` (or the reifier's output) of the reified site against
   a hand-authored `expr::Expr<F>(...)` twin (the `rule_orm` discipline; mechanism = the
   expand round-trip harness, §6).
2. **Negative corpus (acceptance #1).** One `tests/negative/*.lev` per reject form
   (§5 reject set), each asserted via a `run_*_error.sh`-style substring check that names
   the construct.
3. **Differential rows (acceptance #2).** Per whitelisted op: closure leg vs. a **~60-line
   in-language `expr::eval` reference tree-interpreter** (shipped with the corpus) over
   fixture inputs — must agree. Include the `like`/`ilike` pins and the capture-snapshot
   rule (§9.5). `expr::eval` is itself a canonical *consumer* proving the taxonomy is
   walkable (§11 sketch).
4. **`like`/`ilike` unit rows (acceptance #3).** Wildcards, `\%`/`\_` escapes, ASCII fold,
   empty pattern, leading/trailing `%`, non-ASCII byte-exactness.
5. **`--expand` visibility (ask §2.4).** Assert the construction appears in `--expand`
   output (only meaningful under §9.1 option A/B) and that the round-trip harness stays
   green.
6. **Track 06 M2 activation smoke (acceptance #4).** A minimal `Query.where/orderBy/set`
   compiling against `expr::Expr<F>` with zero ORM M1 changes.

---

## 11. Appendix A — the `expr::eval` reference interpreter (shape, for acceptance #2)

A ~60-line in-language walker proving the tree is consumable and pinning the in-memory
semantics the SQL renderer must match. It is *the* differential oracle for §10.3.

```
namespace expr {
    // Evaluate a reified tree against a record accessor + the binds table.
    // `get(path)` yields the record's field value; returns DbValue.
    string|int|float|bool|None eval(Node n,
            (Array<string>) => (string|int|float|bool|None) get,
            Array<string|int|float|bool|None> binds) {
        match (n) {
            expr::Field  => return get((n as expr::Field).path);
            expr::Lit    => return (n as expr::Lit).v;
            expr::Bind   => return binds[(n as expr::Bind).slot];
            expr::Un     => { … "!"/"-" over eval(e) … }
            expr::Bin    => { … dispatch on op over eval(l), eval(r);
                                "==" with a None operand => IS NULL semantics … }
            expr::Call   => { … like/ilike/startsWith/endsWith/contains/Array.contains … }
            else         => throw RuntimeException("expr::eval: unhandled node");
        }
    }
}
```

(The exact body is the design's to write; the point here is that `match` + `as`-downcast +
the union return type are all landed capabilities — §3.5.)

## 12. Appendix B — class/authority diagram

```
  Lambda literal `(u) => u.active && u.name.like("A%")`
        │  in a position typed expr::Expr<(User)=>bool>
        ▼  Checker target-typing hook (§4.2)  ── reuses checkLambdaBody (types u=User)
  ┌───────────────────────────── reifier (§5) ─────────────────────────────┐
  │  leg 1: the lambda (unchanged)  →  closure  (Lower::lowerLambda, §3.1)  │
  │  leg 2: reifyNode(body)  →  expr::Bin("&&",                             │
  │             expr::Field(["active"]),                                    │
  │             expr::Call("like", expr::Field(["name"]), [expr::Lit("A%")]))│
  │         binds: []   (no captures here)      siteId: <det. counter §9.4> │
  └────────────────────────────────────────────────────────────────────────┘
        ▼  rewritten to construction Call (§3.3 precedent)
  expr::Expr<(User)=>bool>( <lambda>, <tree>, [ ], <siteId> )
        ▼  ordinary construction — every engine already lowers it (§8)
  ┌──────────────┐        ┌───────────────────────────────┐
  │ consumer .fn │        │ consumer match-walks .tree     │
  │ in-memory    │        │ + reads .binds → SQL + ? params│
  │ repo / tests │        │ (ORM MySQL renderer, §1)       │
  └──────────────┘        └───────────────────────────────┘
```

## 13. Appendix C — file-by-file change inventory (for the design's milestones)

| file | change |
|---|---|
| `src/Resolver.cpp` | add `namespace expr { Node/Field/Lit/Bind/Bin/Un/Call/Assign/Expr<F> }` to the prelude segments ([:2597](../src/Resolver.cpp#L2597)/[:5304](../src/Resolver.cpp#L5304)); add `string.like`/`ilike` to `class string` ([:176](../src/Resolver.cpp#L176)) |
| `src/Checker.cpp` | target-typing hook for lambda→`Expr<F>` in call-arg ([:2455](../src/Checker.cpp#L2455)) and value/return position ([:3050](../src/Checker.cpp#L3050)); the reifier walk + slot allocator + siteId; whitelist enforcement + per-construct diagnostics; the in-place rewrite (model on `rewriteAsMethodRef` [:1274](../src/Checker.cpp#L1274)) |
| `src/main.cpp` | if §9.1 option A: wire the `Expand` arm ([:511](../src/main.cpp#L511)) to run reification for display; if option B: insert the new reifier pass after resolver pass 2 ([:495](../src/main.cpp#L495)) |
| `src/AstPrinter.cpp` | none expected (Call/Array/Member/Lambda already render §6) — verify |
| `src/Lower.cpp` / `Eval.cpp` / `IrInterp.cpp` / `LlvmGen.cpp` | none expected (ordinary construction §8) — verify only |
| `tests/corpus/expr_reify_*` , `tests/negative/expr_reify_*` , new `run_*_error.sh`, CMake `corpus_expr_reify_*` targets | the corpora of §10 |
| `docs/reference.md` | document `expr::` namespace, the reifiable subset, `like`/`ilike` |
| `designs/complete/` + `designs/requests/accepted/` | on landing, move the design in and the request to accepted (memory *Design workflow file moves*) |

---

## 14. Cross-references

- Ask: `designs/requests/request-expr-reification.md`
- First consumer: `designs/atlantis/techdesign-06-orm.md` §3, §11 (P-probes), §15
- Precedents in-tree: method-ref rewrite ([Checker.cpp:1274](../src/Checker.cpp#L1274)),
  comptime reify ([Rules.cpp:549](../src/Rules.cpp#L549)), enum desugar
  ([Ast.hpp:483](../src/Ast.hpp#L483)), closure lowering
  ([Lower.cpp:1723](../src/Lower.cpp#L1723)), prelude string methods
  ([Resolver.cpp:176](../src/Resolver.cpp#L176)).
- Footguns to read at kickoff: `docs/footguns.md`; memory entries *Prelude backend
  gotchas*, *this-receiver lambda bug*, *emit-C++ by-name reachability*, *Generics are
  erased*, *X64Gen frozen*, *No deferrals in designs*.
