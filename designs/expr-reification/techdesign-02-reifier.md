# Tech Design LA-31 — 02: The Reifier (Checker hook, rewrite, `--expand` wiring)

**Stage:** 2 of 3. **Difficulty: Fable** — this stage operates inside overload
resolution (the historically silent-wrong-pick surface, bug #34's family), performs
in-place AST rewrites, and moves `--expand`'s pipeline position. Residual
micro-decisions (exact scoring seam placement, printer coverage gaps) are expected to
surface here; this stage is authorized to close them **within the rulings of doc 00** —
it may not alter a ruling. **Depends on:** Stage 1 landed (the `expr::` classes exist).
**Feeds:** Stage 3.
**Owns:** `src/Checker.cpp`, `src/Checker.hpp`, `src/main.cpp`; `src/AstPrinter.cpp`
only if hazard H5 fires; `tests/corpus/expr_reify_smoke_*`,
`tests/negative/expr_reify_smoke_*`; updates to existing `--expand` goldens forced by
R1. **Window:** 2026-07-20 → 2026-07-23.

Read `docs/footguns.md` before starting. The four precedents to have open while
implementing: `Checker::rewriteAsMethodRef` (Checker.cpp:1274–1363, the in-place
rewrite template), `RuleEngine::reify` (Rules.cpp:549–649, the build-AST-from-nothing
idiom), the lambda-applicability seam (Checker.cpp:2455–2460), and `checkLambdaBody`
(Checker.cpp:2739–2773).

---

## 1. Overview of the change

Three pieces, all in the Checker + driver:

1. **Target-typing hook** — a lambda **literal** whose target type is the prelude class
   `expr::Expr` with one generic argument `Fn` is accepted (both arrival paths), its
   body checked against `Fn`'s signature, then handed to the reifier.
2. **The reifier** — a single recursive walk over the checked body producing the tree
   construction AST, the binds array, the slot table, and per-construct diagnostics;
   followed by an in-place rewrite of the Lambda node into the
   `expr::Expr<F>(<lambda>, <tree>, [<binds>], <siteId>)` construction Call.
3. **`--expand` wiring** — the `Expand` arm in `main.cpp` runs `Checker::run` before
   `printProgramSource` (ruling R1). `ExpandAst` unchanged.

No changes to Lower/Eval/IrInterp/LlvmGen (verify-only — §7). No `hasMeta` interaction:
the Checker always runs, so a plain query program with no metaprogramming surface
reifies fine.

## 2. The target-typing hook

### 2.1 Recognizing the target

A type is an **Expr-target** iff it is `TKind::Class`, its resolved class is the
prelude's `expr::Expr`, and it carries exactly one generic argument `Fn` whose shape is
a function type (`TKind::FuncRef`). Extract `Fn`'s parameter types and return type; the
lambda literal's arity must equal `Fn`'s arity (else: not applicable in the overload
path / ordinary type error in the value path).

### 2.2 Arrival path 1 — call argument (overload resolution)

At the applicability seam (Checker.cpp:2455–2460, "a Lambda argument is only accepted
against a FuncRef parameter") add: **if the parameter type is an Expr-target and the
argument's `ExprKind == Lambda`, the argument is applicable**, at the **same score tier
as lambda→FuncRef** (ruling R2 — no preference). Follow the existing deferred/
lambda-last discipline (Checker.cpp:2790–2813): the body is checked only after the
candidate's parameter types are known, via `checkLambdaBody` with `Fn`'s param types.

**Ambiguity (E3):** after resolution, if the winning tier contains ≥2 candidates where
the same lambda-literal argument matched a FuncRef parameter in one and an Expr-target
parameter in another, emit E3 (text in §6) **instead of** applying any declaration-order
tiebreak. Implement this as a post-scoring check over the tied set — do not touch the
scoring numbers themselves (bug-#34 adjacency: any change to relative scores is out of
bounds for this design).

If the argument in an Expr-target position is a lambda-typed **value** (not a literal):
the candidate is **not applicable** via this rule. If no other candidate accepts it and
the call would otherwise resolve to the Expr-target candidate, emit **E1** (not a
generic no-overload error) — detect by re-scanning failed candidates for an Expr-target
param position holding a FuncRef-typed non-literal argument. This is what makes the
ask's "only a lambda literal can be reified" a *named* error rather than a resolution
mystery.

### 2.3 Arrival path 2 — expected-type value position

At the `typeOf`-with-`expected` driver seam (the `tryResolveMethodRef` call site,
Checker.cpp:3050–3052): **if `expected` is an Expr-target and `e->kind == Lambda`**,
run `checkLambdaBody` against `Fn` and reify (same routine as path 1). This covers all
of ruling R12's positions: local/global bind with declared type, `return` against a
declared `Expr<F>` return type, and class **field initializers**. If `expected` is an
Expr-target and `e` is a non-literal lambda-typed value → **E1**.

Order the new test **before** `tryResolveMethodRef`'s handling so a `C::m` reference in
an Expr-target position falls through to E1 naturally (it is not a lambda literal).

### 2.4 Recursion / idempotence guards

- The reifier runs on the **original** lambda body before rewrite; the rewrite replaces
  the Lambda node in the *enclosing* position. The lambda that becomes the
  construction's first argument targets the ctor's `F fn` parameter (a plain generic →
  FuncRef target), so re-checking it — including on a re-parse of `--expand` output —
  never re-enters the hook (ruling R1's no-double-reify property). Add an assert-level
  guard anyway: a Lambda whose immediate parent is an `expr::Expr` construction Call
  synthesized this compile (track via a small `std::unordered_set<const Expr*>`) is
  skipped by the hook.
- Nested lambdas inside a reified body are a **reject** (E2/"nested lambda") before
  any inner hook could fire — the reject fires first because the reifier walks the body
  before typeOf descends further. Ensure the reifier's rejection prevents the inner
  lambda from separately reaching the hook (reject → the whole site errors; no rewrite).

## 3. The reifier walk

### 3.1 State

```cpp
struct ReifyCtx {
    const Expr* lambda;                       // the literal being reified
    std::vector<std::string> paramNames;       // Fn-arity lambda params — Field roots
    // Bind table: canonical spelling -> slot; insertion order = slot order (R5).
    std::vector<std::pair<std::string, const Expr*>> binds; // (canonKey, capture expr)
    int findOrAddSlot(const std::string& canonKey, const Expr* captureExpr);
    bool setShaped;                            // Fn returns int (ruling R10)
    // diagnostics sink + span of the lambda site
};
```

`canonKey` for a chain is the root spelling + '.'-joined member path: `"limit"`,
`"this.x"`, `"cfg.minAge"`. Two textually identical chains share a slot; the capture
expression stored is a **deep clone** of the first occurrence (it will be spliced into
the binds array literal and evaluated once at construction — R5).

### 3.2 Entry

`reifyLambda(Expr* lambdaExpr, const Type& fnType) -> bool`:

1. Body must be a single expression (`Lambda` with expression body). A block body →
   reject R9 ("block body"). (`checkLambdaBody` has already typed the body against
   `Fn` — every subexpression carries its checked type when the walk runs.)
2. If `setShaped` and the body's top node is an assignment → the `Assign` arm (§3.3
   last row). Otherwise walk `reifyNode(body)`.
3. On any reject: emit the E2 diagnostic **at the offending subexpression's span**,
   with the lambda site named in a note line, and bail (no rewrite; the site is a hard
   compile error).
4. On success: perform the rewrite (§4), bump the Checker's `exprSiteCounter_` (R4 —
   member of `Checker`, initialized 0 in the ctor, so ids are per-compilation,
   source-walk-ordered, stable).

### 3.3 Per-node rules (normative table; mirrors ask §1.3 + rulings R3/R7/R9/R10/R11)

`reifyNode(const Expr* e) -> ExprPtr` (a construction Call, built with the
`RuleEngine::reify` idiom: `make_unique<Expr>(ExprKind::Call)`, callee =
`Member(colon, ["expr"], "<Class>")`, push args — the bug-32 qualified-construction
path stamps `resolvedClass` when the rewritten AST is re-typed).

| body construct | recognizer (on the checked AST) | emit |
|---|---|---|
| `u.f`, `u.f.g` (param-rooted chain) | `Member` chain whose root is `Name` ∈ `paramNames`, no `colon` links | `expr::Field(["f","g"])` — an `Array` literal of `StringLit`s |
| enum member `E::M` | `Member` with `colon`, resolved target is an enum-member const global (`program_->enumDesugars`, the Checker.cpp:1153–1167 path) | `expr::Lit(<IntLit carrier>)` |
| int / float / bool / string literal | `*Lit`, **no interpolation segments** | `expr::Lit(<clone of the literal>)` |
| `None` | `Name "None"` (checked type `None`) | `expr::Lit(None)` — argument is `Name "None"` (the Rules.cpp:549 idiom) |
| captured chain (not param-rooted): bare local/param-of-enclosing-fn, `this.x`, `capturedObj.field`, `this.x.y` | maximal `Member`/`Name` chain whose root is NOT a lambda param (R3) | `expr::Bind(<slot>)` via `findOrAddSlot(canonKey, clone(chain))` |
| `l <op> r`, op ∈ `== != < <= > >= && \|\| + - * / %` | `Binary` with whitelisted op | `expr::Bin("<op>", reify(l), reify(r))` — **no operand reordering** (R7) |
| `!e`, `-e` | `Unary` | `expr::Un("<op>", reify(e))` |
| whitelisted call | `Call`, callee a `Member` (dot) whose **checked receiver type** + method name + arity matches a row of the static whitelist table (§3.4); receiver and every argument themselves reifiable | `expr::Call("<name>", reify(recv), [reify(args)…])` |
| `u.field = <expr>` as the **entire body**, `setShaped` | `Binary(Assign)`, LHS a param-rooted chain | `expr::Assign(<Field from LHS>, reify(rhs))` |
| **anything else** | — | reject: E2 naming the construct (catalog §6) |

**Array-typed captures** (pinned in doc 01 §3.1): when a captured chain's checked type
is `Array<T>`, the slot is allocated normally (`Bind(slot)` in the tree) but the value
pushed into the binds array literal is **`None`** (the union cannot carry an Array).
The closure leg still sees the real array (it captures normally); consumers that need
the contents keep their own handle. This applies to *any* capture whose checked type is
outside `string|int|float|bool|T?-of-those` — Array is the only whitelist-reachable
case (via `Array.contains`), but the rule is type-driven, not name-driven: **a captured
value of a non-DbValue type is only legal as the receiver of a whitelisted
`Array<T>.contains`; anywhere else it is reject E2 "capture of a non-value type"**
(e.g. a captured object used as `Bin` operand). `T?`-typed captures of DbValue base
types are legal anywhere (their `None` case is the union's `None`).

**`T?` and `== None`:** no special node — `x == None` on a `T?` field/capture reifies
by the ordinary `Bin`/`Lit(None)` rows above. The checked type of the non-None side
being `T?` is what made the comparison check; the reifier needs no extra logic (this is
why running post-check matters — pre-check this shape is untypable).

### 3.4 The whitelist table (ruling R6 — the single register)

```cpp
struct ExprCallRow { const char* name; enum { Str, ArrayT } recv; int arity; };
static constexpr ExprCallRow kExprWhitelist[] = {
    {"like",       ExprCallRow::Str,    1},
    {"ilike",      ExprCallRow::Str,    1},
    {"startsWith", ExprCallRow::Str,    1},
    {"endsWith",   ExprCallRow::Str,    1},
    {"contains",   ExprCallRow::Str,    1},
    {"contains",   ExprCallRow::ArrayT, 1},
};
```

Receiver matching uses the **checked type** of the receiver expression (`string` /
`Array<T>` any T). `u.age.like(p)` therefore fails **before** the whitelist is
consulted — the ordinary checker already rejected `like` on `int` when it typed the
body (the closure leg is real code); the reifier never sees an ill-typed body. The
whitelist's own reject (E2 "non-whitelisted call") fires for calls that *type-check*
but are not rows — e.g. `u.name.toUpper()`. The E2 message's "allowed calls" list is
**generated by iterating this table** (never hand-maintained), so a ticket amendment
is a one-site change and the diagnostic can never drift (R6).

## 4. The in-place rewrite

Modeled line-for-line on `rewriteAsMethodRef` mechanics (mutate the node the enclosing
AST already points at):

1. Allocate a fresh `Expr` node; **move** the Lambda's fields into it (kind, params,
   body, spans). This fresh node becomes construction argument 1.
2. Build argument 2 (the tree) = `reifyNode(body)` result (its sub-nodes reference
   clones, never the moved body — the moved body stays the closure's).
3. Argument 3 = an `ExprKind::Array` literal of the binds capture-expression clones,
   in slot order (possibly empty). The array's element type is target-typed by the
   ctor's declared param — no annotation emitted.
4. Argument 4 = `IntLit(siteId)`.
5. Rewrite the original node in place into `ExprKind::Call` whose callee is a
   `Member(colon)` spelling `expr::Expr` **with the explicit generic argument** `<Fn>`
   (ruling R8) — reuse however the AST encodes generic args on a callee (the same
   encoding `Array<(A,B)=>R>` construction uses); set `resolvedClass` if the
   construction path expects it stamped post-check.
6. Return the checked type `expr::Expr<Fn>` for the site.

The rewritten call must be **re-typeable from scratch** (that is what the `--expand`
round-trip does): construction of a prelude generic class with 4 arguments, one of them
a lambda literal targeting `F` — all landed surface.

## 5. `--expand` wiring (`src/main.cpp`)

In the driver (main.cpp:446–557), change the `Expand` arm (main.cpp:508–526) to:

```cpp
case Mode::Expand: {
    Checker checker(program, /* existing ctor args */);
    if (!checker.run()) { /* print diagnostics exactly as the Full arm does */ return 1; }
    printProgramSource(program);
    return 0;
}
```

- `ExpandAst` (`--ast-after-rules`) is **not** touched — it stays the pre-Checker
  debugging view (ruling R1).
- Every `--expand` golden that now differs gets regenerated **after eyeballing the
  diff is exclusively checker-rewrite churn** (method-ref eta-lambdas, named-arg/
  default normalization, LA-31 constructions). Any *other* category of diff is a
  finding — stop and investigate before regenerating.
- The `--expand`→`--run` round-trip harness (`tests/run_expand_roundtrip.sh`) must be
  green afterward. Known pre-existing limit: bug #69 (enum `$` re-lex) already blocks
  whole-program round-trips for enum-using programs — unchanged by this design, do not
  chase it here.

## 6. Diagnostic catalog (exact strings; Stage 3's negative corpus greps these)

All errors point at the offending span; E2 adds a note naming the lambda site.

- **E1** — `only a lambda literal can be reified to expr::Expr<F>` — non-literal
  lambda value in an Expr-target position (both paths, §2.2/§2.3).
- **E2** — `cannot reify <construct>: outside the LA-31 reifiable subset` followed by
  the generated allow-list line
  `reifiable calls: string.like/1, string.ilike/1, string.startsWith/1, string.endsWith/1, string.contains/1, Array.contains/1`
  (iterate `kExprWhitelist` — R6). `<construct>` is one of the fixed construct names:
  `non-whitelisted call '<name>'` · `await` · `nested lambda` · `block body` ·
  `assignment outside a set-shaped lambda` · `mutation of a capture` ·
  `'is' expression` · `'match' expression` · `string interpolation` · `indexing` ·
  `range` · `conditional expression` · `capture of a non-value type` ·
  `spawn` (any remaining ExprKind falls into a final `unsupported construct '<kind>'`
  arm so no shape can slip through unnamed).
- **E3** — `ambiguous lambda argument: matches both a function parameter and an
  expr::Expr parameter; extract a typed local to select` — the R2 tie.

## 7. Verify-only assertions (no code expected; check, don't change)

- `AstPrinter.cpp` renders the rewritten construction as compilable source: Call with
  `Member(colon)` callee carrying generic args, Array literal, Lambda, IntLit — the
  research expects zero printer work, **but generic arguments on a `Member` callee are
  the likeliest gap (hazard H5)**. If the printer drops `<Fn>`, fixing the printer to
  render callee generic args IS in scope for this stage (ownership note in the header).
- Lower/Eval/IrInterp/LlvmGen: the rewritten AST is ordinary construction + closure +
  union-array — all landed (MySQL DbValue proved the union array end-to-end on LLVM).
  Run the smoke corpus (§8) on all three engines to confirm; touching these files is
  **out of scope** — a miscompile found here is a STOP-and-file (it would be a
  pre-existing engine bug this feature merely exposes).

## 8. Stage-2 acceptance (all green before Stage 3 starts)

1. **Positive smoke** `tests/corpus/expr_reify_smoke_1.lev`: the ORM shape
   `(u) => u.active && u.name.like("A%")` passed to a function taking
   `expr::Expr<(User)=>bool>`; program prints a canonical dump of `.tree`/`.binds`/
   `.siteId` (hand-rolled dump in the test — Stage 3 owns the full dump spec) **and**
   calls `.fn` on a fixture. Byte-identical oracle+IR+LLVM.
2. **Positions smoke** `expr_reify_smoke_2.lev`: the same lambda arriving via local
   bind, `return`, and a class field initializer (R12); plus a capture row
   `(u) => u.age >= lo` printing `binds=[18]` and `Bind(0)` in the dump; plus two
   sites in one file printing `siteId=0` then `siteId=1` (R4).
3. **Negative smoke** (3 files in `tests/negative/`): a block body (E2/`block body`),
   a non-whitelisted call (E2 with the generated allow-list line), a lambda **value**
   in an Expr position (E1). Substring-checked.
4. **E3 smoke**: a two-overload fixture (FuncRef + Expr forms) called with a lambda
   literal → E3 substring.
5. `--expand` on smoke 1 shows the `expr::Expr<(User)=>bool>(…)` construction;
   recompiling the emitted output runs byte-identically (no double reification).
6. Full pre-existing ctest suite green, including regenerated `--expand` goldens and
   the round-trip harness; emit-C++ lanes green.
7. Everything committed and pushed to master.

## 9. Hazard register (why this stage is Fable — read before starting)

- **H1 — bug-#34 adjacency.** Do not alter existing scores; the Expr-target rule adds
  applicability, never re-ranks. The E3 check is post-hoc over ties. Regression watch:
  the regex library's declaration-order workaround for #34 must keep passing.
- **H2 — `--expand` golden churn.** Bounded by eyeball-then-regenerate discipline
  (§5). The churn is a one-time cost accepted by R1.
- **H3 — lambda-last inference ordering.** Expr-target lambdas must ride the same
  deferral as FuncRef lambdas (body checked only once the candidate's param types are
  known); checking eagerly double-reports or mis-types multi-candidate calls.
- **H4 — generic-arg plumbing on the target type.** `Expr<F>`'s single generic arg
  must survive to the hook through `TKind::Class` (erased generics carry checker-side
  args; runtime carries none — that is fine, `.fn` typing is checker-side only).
- **H5 — printer generic args on a qualified callee** (§7) — in scope if it fires.
- **H6 — clone discipline.** Binds capture clones and tree literal clones must be deep
  and span-carrying (diagnostics and printer both read spans). The moved-lambda node
  must leave the original node reusable for the in-place Call rewrite (the
  method-ref rewrite shows the exact move order).

## 10. Implementation log

(Append micro-decisions closed under §authorization, findings, golden-churn inventory,
and completion note here.)
