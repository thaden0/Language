# Tech Design LA-31 — Expression Reification `expr::Expr<F>` — 00: Overview & Rulings

**Status:** DESIGN SET — ready to implement. **Date:** 2026-07-18.
**Request:** `designs/requests/request-expr-reification.md` (LA-31, P0 — gates Atlantis
Track 06 M2, `designs/atlantis/techdesign-06-orm.md` §3).
**Research input:** `docs/research-expr-reification.md` (current as of this writing; no
compiler changes have landed since it was written — its file/line citations are live).
**This set:** 4 files in `designs/expr-reification/` — this overview + three sequential
stage documents. **On full resolution, move this entire directory to
`designs/complete/expr-reification/` and move the request to
`designs/requests/accepted/`** (see §7 Completion Protocol).

Read `known_bugs_1.md`/`known_bugs_2.md` at kickoff of every stage (quality-gates
policy) — `docs/footguns.md` was retired 2026-07-19; each bug's Workaround/Debt-sites
note now lives inline in its known_bugs entry.

---

## 1. What this is

A **lambda literal** in a position typed `expr::Expr<F>` compiles to **two co-resident
artifacts**: (leg 1) the ordinary closure every lambda already produces, and (leg 2) a
runtime, walkable `expr::Node` tree mirroring the lambda's **checked** body, plus a
`binds` array of captured values snapshotted once at construction, plus a
compile-time-unique `siteId`. Both legs are ordinary emitted code — no reflection, no
runtime parsing, `O(nodes)` construction cost, fully visible under `--expand`.

This is a **language** capability (like `regex::`, `json::`), not an ORM feature. The
ORM (Track 06) is the first consumer, not the only one. The language guarantees exactly
one thing about fidelity: **the tree mirrors the checked AST** (resolved members, folded
comptime constants, enum carriers). Consumers own their rendering fidelity via their own
differential corpora.

Engines: **oracle + IR + LLVM** full. emit-C++: existing suite must stay green (the
prelude additions must not regress reachability), but the expr corpus is not required on
it. ELF: frozen, not a target, never gates anything (standing rule).

## 2. Architecture in one diagram

```
  Lambda literal `(u) => u.active && u.name.like("A%")`
        │  in a position typed expr::Expr<(User)=>bool>       (param / bind / return /
        ▼                                                      field-init — target-typed)
  Checker target-typing hook (Stage 2)  ── reuses checkLambdaBody (types u = User)
  ┌───────────────────────────── reifier ──────────────────────────────────┐
  │ leg 1: the lambda, unchanged  →  closure (landed Lower::lowerLambda)   │
  │ leg 2: reifyNode(body)  →  expr::Bin("&&",                             │
  │            expr::Field(["active"]),                                    │
  │            expr::Call("like", expr::Field(["name"]), [expr::Lit("A%")]))│
  │        binds: []  (no captures here)      siteId: deterministic counter │
  └────────────────────────────────────────────────────────────────────────┘
        ▼  in-place Checker rewrite (the method-ref precedent, Checker.cpp:1274)
  expr::Expr<(User)=>bool>( <lambda>, <tree ctors>, [ <binds> ], <siteId> )
        ▼  ordinary construction — every engine already lowers it
  consumer calls .fn (in-memory)  |  consumer match-walks .tree + reads .binds (SQL, eval, …)
```

No new IR ops. No new Lower/Eval/IrInterp/LlvmGen code paths (verify-only). The new code
is: prelude classes + two string methods (Stage 1), and the Checker hook + reifier +
rewrite + `--expand` wiring (Stage 2).

## 3. Rulings (all decisions closed — nothing here is open at implementation time)

Decided jointly with the owner 2026-07-18, before this design was written (no-deferrals
policy). Each ruling is normative; a stage that finds a ruling unimplementable STOPS and
escalates rather than substituting its own answer.

- **R1 — Where reification runs: Checker rewrite, and `--expand` runs the Checker.**
  Reification happens **inside the Checker**, modeled exactly on the method-reference
  in-place rewrite (`Checker::rewriteAsMethodRef`, Checker.cpp:1274). Rationale: the
  feature's quality lives in its diagnostics and in the tree-mirrors-checked-AST
  guarantee, and both need types (enum carriers, `T?` for `== None`, whitelist receiver
  typing); this also matches the one battle-tested precedent for in-place AST rewriting.
  To satisfy the ask's §2.4 (`--expand` shows everything): **the `Expand` arm in
  `main.cpp` now runs the full `Checker::run` before printing.** A reification-only
  sub-pass is not an option — knowing a lambda argument targets an `Expr<F>` parameter
  requires overload resolution, which *is* checking. Consequences, all accepted:
  - An ill-typed program now **fails `--expand`** with the same diagnostics as a normal
    compile (non-zero exit). `--ast-after-rules` (`ExpandAst`) **stays pre-Checker** and
    is the sanctioned hatch for debugging expansions of ill-typed programs.
  - `--expand` output now shows **all** Checker rewrites (method-refs as their
    eta-expansion lambdas, named-arg/default normalization, LA-31 constructions).
    Existing `--expand` goldens that change are updated as part of Stage 2 — the new
    output is correct by this ruling. The `--expand`→`--run` round-trip harness must
    stay green (rewritten AST is ordinary printable source).
  - Recompiling `--expand` output does **not** double-reify: the printed construction's
    first argument is a lambda targeting the ctor's `F fn` parameter (a FuncRef target),
    so the reifier does not fire on it.
- **R2 — Overload interaction: disjoint-by-target, no preference rule.** A lambda
  literal matches a FuncRef param (existing rule) and matches `expr::Expr<_>` (new
  rule), at the **same applicability tier**. If overload resolution ends in a tie where
  the same lambda-literal argument matched FuncRef in one candidate and `expr::Expr` in
  another, that is a **compile error (E3, ambiguity)** — it overrides any
  declaration-order tiebreak. Rationale: no landed or planned API declares both forms;
  a preference rule is speculative complexity in exactly the territory (lambda-vs-
  overload scoring) where bug #34 bred a silent wrong-pick. An error is cheap to relax
  later; a preference is a compatibility commitment.
- **R3 — `capturedObj.field` is admitted, as a single `Bind`.** Any maximal member
  chain **not rooted at a lambda parameter** (a bare captured local, `this.x`,
  `capturedObj.field`, `this.x.y`, …) reifies as **one `Bind`** of the whole chain's
  value, snapshotted at construction. Only **parameter-rooted** chains become `Field`.
  The tree never looks inside a captured value.
- **R4 — `siteId`: deterministic per-compilation counter, source order.** A monotonic
  `int` on the Checker, starting at **0**, bumped once per successful reification in
  Checker walk order (deterministic for a fixed build). Compile-time-unique and stable
  across runs of the same build; trivially debuggable (`siteId: 3` = fourth reified
  site). No span hash, no time, no randomness.
- **R5 — Snapshot contract, pinned exactly.** `binds[k]` is the value of the k-th
  **distinct** captured expression, evaluated **once, at `Expr` construction, in
  first-reference order**; the tree consults **only** `binds`, never the live closure
  environment. Distinctness is by **canonical chain spelling** (root symbol + member
  path — two occurrences of `limit` share a slot; `this.x` and `this.x` share a slot).
  Snapshot is of the **value** for value types and of the **reference** for reference
  types — post-construction mutation of a captured *variable* affects neither leg;
  post-construction mutation of a captured **reference-typed value's contents** (e.g.
  a captured `Array` used via `contains`) is visible to **both** legs equally. Both
  facts are pinned by corpus rows (Stage 3). Mechanically, the snapshot falls out of
  ordinary left-to-right construction-argument evaluation: closure, tree ctors (no user
  code — `Bind` holds only a slot int), then the binds array literal (the only place
  captured expressions are evaluated — including any `get`-view they run, once).
- **R6 — Whitelist: one static table in the reifier; diagnostics generated from it.**
  A single table `{method name, receiver type, arity} → node emission` drives **both**
  the accept path and the "non-whitelisted call" diagnostic's here's-what-IS-allowed
  list. v1 rows: `string.like/1`, `string.ilike/1`, `string.startsWith/1`,
  `string.endsWith/1`, `string.contains/1`, `Array<T>.contains/1`. Growth is **only by
  ticket amendment** carrying both legs' semantics + a differential-corpus row (the
  consistency law, enforced at this one register).

### 3.1 Subsidiary rulings (micro-decisions closed now so no stage decides them)

- **R7 — Tree mirrors the checked AST literally; no normalization.** No operand
  reordering: `None != u.x` reifies with `Lit(None)` on the left, exactly as written.
  `None` reifies as `Lit(None)` uniformly wherever it appears; the ask's
  `Bin("==", Field, Lit(None))` describes the common spelling, not a canonical form.
  Consumers handle either side (`expr::eval` does; the ORM renderer's handling is the
  ORM's own corpus's problem). A None-on-left corpus row pins this.
- **R8 — The emitted construction carries the explicit concrete generic argument**:
  `expr::Expr<(User)=>bool>(<lambda>, <tree>, [<binds>], <siteId>)`. Function types as
  generic arguments are landed surface (`Array<(A,B)=>R>`, info.md §6.8). Stage 2
  verifies the printer renders it and the parser re-accepts it (round-trip).
- **R9 — Block-bodied lambdas are rejected outright**, including a single-`return`
  block. The reifiable body is exactly **one expression** (`(u) => <expr>`). Diagnostic
  names the construct ("block body").
- **R10 — `Assign` is admitted iff** the assignment is the **entire body** and the
  target `F` returns `int` (the `Expr<(E)=>int>` set-lambda shape; assignment is an
  expression yielding `int` per the ask). An assignment anywhere else in a body is a
  named reject ("assignment outside a set-shaped lambda"). The assignment target must
  be a **parameter-rooted** field chain (→ `Field`); assigning to a captured chain is a
  named reject ("mutation of a capture").
- **R11 — Multi-parameter lambdas reify**: every lambda parameter is a legal `Field`
  root. (`F`'s arity fixes the lambda's arity as usual.)
- **R12 — Accepted positions** are the target-typed set: call argument, local/global
  bind with declared type, `return` against a declared `Expr<F>` return, and **class
  field initializer** with declared `Expr<F>` field type (same expected-type path).
  Each gets a corpus row. Parameter **defaults** are compile-time constants and cannot
  hold lambdas today — no new rule, out of surface.
- **R13 — `like`/`ilike` matcher pins** (normative detail in doc 01 §2.2–2.3):
  anchored full-string match; `%` = any run incl. empty; `_` = exactly one byte; `\`
  escapes **any** next byte to a literal; a **lone trailing `\` matches a literal `\`**
  (doc 01 §2.3's refinement, pinned by corpus rows 16–18); byte-exact comparison
  (UTF-8 multibyte compares byte-for-byte); `ilike` folds A–Z↔a–z **per byte at each
  comparison site** (no folded string copies), escaped literals included; beyond
  ASCII, exact bytes. Empty pattern matches only the empty string.
- **R14 — `expr::eval` ships in the corpus** (checked user code), not the prelude —
  one self-contained copy inside each differential test file (no cross-file imports in
  corpus tests). It is the canonical proof the taxonomy is walkable and the
  differential oracle for leg agreement.
- **R15 — Prelude placement**: a new `kPreludeExpr` raw-string segment appended last in
  the `preludeFile_.text` concatenation (Resolver.cpp:5304 region); `like`/`ilike` go
  inside `class string` in `kPreludeStd` next to `contains`/`startsWith`
  (Resolver.cpp:176–334), both delegating to one `__likeMatch(pattern, fold)` body with
  explicit `this.` receivers.
- **R16 — Non-literal lambda values in an `Expr<F>` position are error E1** ("only a
  lambda literal can be reified"), detected in **both** arrival paths (call-argument
  and expected-type value position). No runtime reification exists, v1 and always
  (growth would be a new ticket).
- **R17 — Non-DbValue captures.** The binds element union is DbValue-shaped by the ask
  and cannot widen without amendment. A captured value whose checked type is outside
  `string|int|float|bool` (+ `T?` of those) is legal **only** as the receiver of a
  whitelisted `Array<T>.contains`; its `Bind` slot is allocated normally but the binds
  array carries **`None`** in that slot (consumers keep their own reference — the
  closure leg captures normally). Any other use of a non-DbValue capture is reject E2
  "capture of a non-value type". Normative detail: doc 01 §3.1, doc 02 §3.3.

## 4. Stage map, difficulty, ownership, timeline

Stages are **sequential** (each depends on the previous), with **disjoint file
ownership** — no file is touched by two stages.

| stage | doc | difficulty | owns (src) | owns (tests/docs) | window |
|---|---|---|---|---|---|
| S1 prelude surface | `techdesign-01-prelude.md` | **Sonnet** (stop-and-escalate) | `src/Resolver.cpp` (prelude string segments only) | `tests/corpus/expr_prelude_*`, `tests/corpus/expr_like_*`, `tests/corpus/expr_eval_*` + CMake rows | 2026-07-19 → 07-20 |
| S2 reifier | `techdesign-02-reifier.md` | **Fable** | `src/Checker.cpp`, `src/Checker.hpp`, `src/main.cpp` (+ `src/AstPrinter.cpp` only if H5 fires) | `tests/corpus/expr_reify_smoke_*`, `tests/negative/expr_reify_smoke_*`, updated `--expand` goldens | 2026-07-20 → 07-23 |
| S3 verification & landing | `techdesign-03-verification.md` | **Sonnet** (stop-and-escalate) | **nothing in src/** (hard rule — findings are filed, never patched) | full `tests/corpus/expr_reify_*`, `tests/negative/expr_reify_*`, `run_expr_reify_error.sh`, differential rows, `docs/reference.md` §expr, `known_bugs_1.md`/`known_bugs_2.md` entries, M2 smoke | 2026-07-23 → 07-25 |

**Difficulty rationale.** S2 is **Fable**: it operates inside overload resolution (the
historically bug-breeding surface — #34's silent wrong-pick class), performs in-place
AST rewrites, and moves `--expand`'s pipeline position (global golden churn); residual
micro-decisions (exact scoring seam, printer coverage) may surface there and S2 is
authorized to close them within the rulings. S1 and S3 are **Sonnet**: every artifact
is fully specified in their docs (complete code for the matcher, the walker, the
dump format, every corpus row and diagnostic string); their protocol on hitting
*anything* requiring a design change is **STOP — do not improvise — write up the
finding and escalate** (each doc restates this with its specific stop conditions).

**Timeline.** Target land ≤ **2026-07-25** (same-week precedent: threads, TLS,
readonly, `target::`). Hard escalation per the ask's §4 if not landed by
**2026-09-01** (Track 06 M2 gate is 2026-09-25). Each stage commits and pushes to
master on completion — never leave work uncommitted.

## 5. Non-goals (explicitly out; each has a home, none is a dangling deferral)

- **No runtime reification** of lambda *values* — E1 by ruling R16; any future need is
  a new request ticket.
- **No SQL/rendering semantics** — the ORM's renderer fidelity is Track 06's own
  differential corpus (`designs/atlantis/techdesign-06-orm.md` §3.4).
- **No whitelist growth path in code** — growth is ticket-amendment-only (R6).
- **No emit-C++/ELF expr lanes** — posture pinned in §1; emit-C++ regression-green is
  an acceptance row, ELF untouched (X64Gen frozen, standing rule).
- **No `siteId` memo machinery** — the field lands; consumer-side memoization is the
  ORM's designed-not-required feature, entirely on their side.
- **No new `.ext` files anywhere** — all new test sources are `.lev` (standing rule).

## 6. Acceptance (whole-feature; maps to the ask §3 — details live in stage docs)

1. Every ask-§1.3 form reifies (positive corpus, oracle+IR+LLVM, canonical tree dumps);
   every reject form errors **at the lambda site naming the construct** (negative
   corpus, substring-checked); hand-written-twin `--expand` byte-equivalence.
2. Differential rows per whitelisted op: closure leg vs `expr::eval` leg agree,
   including the `like`/`ilike` pins and both R5 snapshot rows.
3. `like`/`ilike` land with their own unit rows (wildcards, escapes, ASCII fold,
   empty/trailing-`%`/lone-`\` edges, non-ASCII byte-exactness).
4. `--expand` shows the emitted construction; the round-trip harness stays green; the
   full pre-existing ctest suite stays green (including emit-C++ lanes).
5. Track 06 M2 activation smoke: `Query.where/orderBy/set` shapes compile against
   `expr::Expr<F>` with zero changes to ORM M1 code.

## 7. Completion protocol (run at the end of Stage 3)

1. Confirm §6 all green on a clean rebuild (`ctest` full matrix).
2. `git mv designs/expr-reification designs/complete/expr-reification` (whole set).
3. `git mv designs/requests/request-expr-reification.md designs/requests/accepted/`.
4. `docs/reference.md` §expr section landed; any new bugs found en route filed as
   entries in `known_bugs_1.md`/`known_bugs_2.md`.
5. Note in the commit message that Atlantis Track 06 M2 is unblocked.
6. Commit and push to master (`git push origin <branch>:master` from your own
   worktree). Nothing left untracked.

## 8. Cross-references

- Ask: `designs/requests/request-expr-reification.md` (LA-31).
- Research (all file/line citations current): `docs/research-expr-reification.md`.
- First consumer: `designs/atlantis/techdesign-06-orm.md` §3, §11, §15.
- Precedents: method-ref rewrite (Checker.cpp:1274), comptime reify (Rules.cpp:549),
  enum desugar (Ast.hpp:483), closure lowering (Lower.cpp:1723), prelude string methods
  (Resolver.cpp:176).
- Standing rules that bind every stage: prelude-not-checked, prelude-no-narrowing,
  prelude backend gotchas, this-receiver-lambda (explicit `this.`), emit-C++ by-name
  reachability, X64Gen frozen, `.lev` only, commit-and-push discipline.
