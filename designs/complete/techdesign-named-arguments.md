# Tech Design — Named Arguments & Default Parameter Values ("parameter ergonomics")

**Status:** implemented, verified, and archived. **Date:** 2026-07-10.
**Track:** single track, ordered milestones M1 → M4 (one implementer). This is **not** a fan-out:
defaults, named args, and overload resolution are one resolver story that must land together
(`designs/suggested-features.md:181-182`: *"named args + overloads + defaults must have one
story"*). Splitting them would fracture the very interaction the design exists to unify.
**Source (the *why*):** `designs/suggested-features.md` §3.4 (`:172-182`).
**Unblocks:** metaprogramming item M (attribute named args) — see
`designs/deferal-metaprogramming-tail.md` §4 (`:126-155`), which is explicit that this is *not*
metaprogramming work and must exist as its own language design first. M4 discharges that debt.
**Convention docs:** `designs/complete/techdesign-00-overview.md` (contracts, engine-coverage policy,
STOP-and-escalate, commit discipline) governs this track too; read it first.

---

## 1. The one rule (philosophy fit)

A call is a **set of bindings from arguments to parameters.** Today the language builds exactly
one kind of binding — *by position* — and resolves overloads over it (info.md §9, "Overload
resolution — resolution by type"). This design generalises the binding step to three ways of
naming the same parameter, with **one resolution story** over all of them:

| binding | written | reads as |
|---|---|---|
| positional | `f(x)` | arg *i* → param *i* (unchanged) |
| **named** | `f(label: x)` | arg → the param named `label` |
| **default** | param `= expr` fills an omitted param | the signature's own fallback |
| injected (existing) | a `bind` in scope fills an omitted param | §12.5, unchanged |

This is **not** new syntax bolted onto calls; it is the "explicit over implicit" rule (info.md §1)
applied to the argument list, plus the existing "resolution is by type" overload machinery
extended to score a *mapped* arg→param set instead of a purely positional one. Crucially, the
runtime is **untouched**: a named/defaulted call is normalised at check time into the identical
positional call it is equivalent to, so every engine (oracle, IR, emit-C++, ELF, LLVM) sees only
positional calls it already handles (§4). Named arguments have **zero runtime cost** — they are a
compile-time reordering.

The precedent this design leans on already exists in the tree: implicit injection (§12.5) *already*
synthesises argument nodes into a call's argument list and pins their type/target
(`Checker.cpp:1237-1244`). Default-filling and name-reordering are the same move.

---

## 2. Surface syntax

Exactly the target syntax the owner specified:

```
void methodName(int myNumber = 2, string myString = 'Hello', string myOtherString = 'World') {
    ...
}

methodName(myOtherString: 'everybody');   // myNumber=2, myString='Hello', myOtherString='everybody'
```

- **Declaration — default with `=`.** In a parameter list, a parameter may be followed by
  `= <constant-expr>`. The `=` reads "defaults to".
- **Call site — named with `:`.** An argument may be written `label: value`. The `:` reads "this
  label maps to this value" (the same shape a key→value binding takes everywhere). It is
  deliberately a *different* token from the declaration's `=`, so the two sites never visually
  collide.

### 2.1 Grammar deltas

```
param      := ['const'] type IDENT ['=' constExpr]          // was: ['const'] type IDENT
arg        := IDENT ':' expr                                 // named
            | expr                                           // positional
argList    := (arg (',' arg)*)?                              // positional args must precede named args
```

**Rules baked into the grammar / parser (syntactic, §6.1):**
- **Positional-before-named** — once a named argument appears, every following argument must be
  named. `f(a, x: 1, b)` is a *parse* error ("positional argument after named argument"). (Same
  rule as Python/C#/Swift; it keeps the mapping unambiguous.)
- **Defaults are a trailing-or-not question decided at resolution, not parse.** A non-trailing
  default (`f(int a = 1, int b)`) is legal to *write* and is resolved normally — a call must then
  either supply `b` positionally-through-`a` or name it. There is no "defaults must be last" parse
  rule; the resolver's binding model (§3) handles gaps. (This is stricter-than-C++ ergonomics for
  free, and falls out of the model rather than being a special case.)

### 2.2 Default-value expressions: constant only (v1)

**A default expression must be a compile-time constant** — a literal, or a `comptime`-foldable
constant expression (info.md §16.5 comptime fold; the recent "pass-1 comptime-root pre-checking"
landing). This is C#'s rule and it buys the whole design its simplicity:

- A constant default **folds to a literal at declaration-check time**, so cloning it into a call
  site (§4) is frame-independent and trivially correct on every engine — no callee-scope capture,
  no per-call evaluation, no "which frame does the default see" question.
- A default that references **another parameter or `this`** is a compile error
  (`"a default value must be a compile-time constant"`). Arbitrary per-call default expressions
  evaluated in the callee's scope are **explicitly deferred** to the roadmap (§11) — nothing in the
  target syntax needs them, and they would force per-engine work this design's leverage avoids.

The target syntax's defaults (`2`, `'Hello'`, `'World'`) are all literals → fully covered. M1 may
restrict to *literal* defaults if comptime-fold integration is more than a one-call hook (§6.2,
Hurdle H3); the constant-expression widening is a mechanical follow-on within M1.

---

## 3. The unified resolution story (the core)

One algorithm resolves defaults + named args + overloads + injection together. It **subsumes**
today's positional rule (a call with no named args and no omitted params is the current path
exactly).

### 3.1 Per-candidate binding

For a call with positional args `P = [p₀..pₖ₋₁]` and named args `N = {label → expr}`, and a
candidate signature with parameters `params`:

1. **Positional bind.** `params[0..k-1]` receive `P[0..k-1]` by position. (`k > params.size()` →
   candidate inapplicable: too many positional args.)
2. **Named bind.** Each `label → expr` in `N` binds the parameter named `label`. The parameter must
   exist and must be **unbound** (not already filled by step 1). An unknown label, or a label that
   collides with a positionally-filled parameter, makes the candidate inapplicable (and, when it is
   the *sole* candidate, raises the precise error — §3.4).
3. **Fill the rest.** Every still-unbound parameter must be satisfiable:
   - it declares a **default** → fill with the (folded-literal) default, **or**
   - its type has a **`bind` in scope** (§12.5) → fill by injection, **or**
   - otherwise the candidate is inapplicable (a required argument is missing).

   **Precedence when both are available:** a parameter that declares a **default is never an
   injection target** — the explicit signature-level default wins over ambient injection. This
   partitions unbound params cleanly (default-declared vs. injectable) and preserves today's
   injection semantics untouched.
4. **Type check.** Every *supplied* argument (positional or named) must be assignable to its mapped
   parameter's type — the existing `pickOverload` score rule (`Checker.cpp:1103-1112`), applied to
   the mapped pairs instead of positional pairs. Defaults/injections do **not** participate in
   scoring (they are not caller-supplied).

A candidate is **applicable** iff steps 1–4 all pass.

### 3.2 Selection among applicable candidates

Unchanged philosophy (info.md §9), evaluated over the *supplied* arg→param pairs:

1. **Most-specific wins** — exact type match (`+2`) beats widening (`+1`), summed over supplied
   args (existing scoring, `Checker.cpp:1109-1110`).
2. **Tighter fit wins** — if scores tie, prefer the candidate that **used fewer defaults/injections**
   (more parameters explicitly supplied). This resolves the classic `f(int a)` vs.
   `f(int a, int b = 0)` on a call `f(1)` → the 1-arg overload (its "normal form" beats the other's
   "optional form", matching C#).
3. **First-declared breaks ties** — unchanged (info.md §2).

No applicable candidate → the existing "no overload matches" error, upgraded with a named-arg-aware
message (§3.4).

### 3.3 Worked examples

```
void f(int a = 1, string s = "x", bool b = false) { ... }
f()                    // a=1,  s="x",  b=false
f(9)                   // a=9,  s="x",  b=false
f(b: true)             // a=1,  s="x",  b=true
f(9, b: true)          // a=9,  s="x",  b=true
f(s: "y", a: 3)        // a=3,  s="y",  b=false   (named args may reorder)
f(a: 1, 2)             // PARSE ERROR: positional after named
f(c: 3)                // ERROR: no parameter named 'c'
f(3, a: 4)             // ERROR: parameter 'a' bound twice (positional + named)
```

Constructors and methods are identical — they run through the same resolver path (§4.2).

### 3.4 Diagnostics (new, precise)

- `"positional argument after named argument"` — parser (§6.1).
- `"no parameter named 'X'"` — named label matches no parameter of the sole candidate.
- `"parameter 'X' is bound both positionally and by name"` — double-fill.
- `"missing required argument 'X'"` — an unbound parameter with no default and no binding
  (replaces the generic "no overload matches" when a *single* candidate all-but-matches).
- `"a default value must be a compile-time constant"` — non-constant default at declaration.
- `"default parameter 'X' cannot reference another parameter or 'this'"` — the constant-scope rule.

When **multiple** overloads exist and none applies, keep the existing "no overload of 'f' matches
the arguments" (`Checker.cpp:798`) — per-candidate reasons would be noise. The precise single-target
messages fire only when exactly one candidate is in play (the common case).

---

## 4. Implementation leverage: normalise in the checker, touch no engine

**The entire runtime cost of this feature is zero, because the checker rewrites the call's argument
list into canonical positional order before any engine sees it.**

### 4.1 Why this works (verified against the tree)

Every engine builds its per-call argument vector by iterating `e->list` **in order** and binding
positionally to `params[i]`:

- Eval: `for (const ExprPtr& a : e->list) args.push_back(eval(a));` (`Eval.cpp:705-707`), then
  `env[fn->params[i].name] = args[i]` (`Eval.cpp:433-434`, ctor `:259`, lambda `:455-456`).
- Lower/IR: `for (const ExprPtr& arg : e->list) argRegs.push_back(lowerExpr(arg));`
  (`Lower.cpp:1048, 1059, 1087, 1102`).
- CGen / LlvmGen / X64Gen consume the same lowered positional arg registers.

So if the checker guarantees, for every resolved `Call`, that **`e->list` is exactly
`params.size()` entries long, in parameter order, with omitted params filled**, then *nothing
downstream changes*. This is precisely what implicit injection already does today — it appends
synthesised `Inject` nodes onto `call->list` (`Checker.cpp:1237-1244`) and every engine evaluates
them like any other argument. **Default-filling and name-reordering are the same synthesis move.**

### 4.2 The normalisation step

Add one checker pass, invoked at the end of the resolve helper in `typeOfCallInner`
(`Checker.cpp:792-800`), after a candidate is chosen and before `genericReturn` /
`inferConstruction` read `argTypes`:

```
normalizeCallArgs(call, chosen):
    if call->argsNormalized: return          // idempotent guard (typeOfCall may re-run; §6.3 H2)
    build ordered[params.size()] = { nullptr... }
    place positional args by index; place named args by matched param index
    for each still-null slot i:
        if params[i] has default: ordered[i] = cloneLiteral(params[i].defaultFolded)
        else (injection): ordered[i] = synthesized Inject node   // reuse pickInjecting path
    call->list = move(ordered)               // now purely positional, full length
    call->argsNormalized = true
```

`argTypes` is then recomputed from the normalised `call->list` (or, better, the mapping helper
returns the mapped `argTypes` directly so scoring in §3 runs on it — see §6.4). Because
`argsNormalized` short-circuits re-entry, the re-type pass the injection code already guards against
(`Checker.cpp:1197-1201`) stays correct.

**Interaction with injection:** injection stops being a separate trailing-only special case. Fold it
into the unbound-param fill (§3.1 step 3): a param with a default is filled by its default; a param
without is offered to `lookupBind` exactly as `pickInjecting` does now. `pickInjecting`'s existing
synthesis loop (`Checker.cpp:1237-1244`) becomes the "else" branch of the fill.

### 4.3 What the engines do *not* need

No changes to `Eval.cpp`, `Lower.cpp`, `Ir.hpp`, `IrInterp.cpp`, `CGen.cpp`, `LlvmGen.cpp`,
`X64Gen.cpp`/`X64.hpp`. **This is what keeps the design clear of the X64Gen freeze**
(`feedback_x64gen-frozen`, `techdesign-00-overview.md` §2.1 STALE note): the ELF/X64 backend sees
only positional calls, so it needs no new work and the freeze is respected by construction. Note the
existing ctor arity check `m->params.size() == e->list.size()` (`Lower.cpp:1039`, `Eval.cpp:246`)
keeps holding — after normalisation `e->list.size() == params.size()`.

---

## 5. File ownership & blast radius (single track)

All edits are the checker/parser front half plus the AST plumbing that must carry two new fields
without dropping them. **No prelude changes, no native changes, no backend changes.**

| file | change |
|---|---|
| `src/Ast.hpp` | `Param`: add `ExprPtr defaultValue` (+ folded cache). `Expr`: add `std::string_view argLabel` (named-arg label; empty = positional) and `bool argsNormalized`. |
| `src/Parser.cpp` | `parseParamList` (`:587-611`): parse `'=' constExpr` default. `parseArgs` (`:264-272`): parse `IDENT ':'` named args + positional-before-named enforcement. |
| `src/Checker.cpp` | the resolver (§3, §4.2): `pickOverload`/`pickInjecting` become mapping-aware; add `normalizeCallArgs`; new diagnostics; default-constant checking at decl. |
| `src/Rules.cpp` | `cloneParam` (`:1621+`) must copy `defaultValue`; `cloneArgList` (called `:1880`) must copy `argLabel`. **Non-negotiable** — the overview warns three times that a hand-rolled clone dropping a new field is the recurring bug (§2.1 note; `isRawSegment`, `isUsing`, `isConst` all bit this). |
| `src/AstPrinter.cpp` | `paramList` (`:71`, shared by decl printing `:596/601/608`): render `= default`. Call arg printer (`exprList`/`srcExprList`, `:93-95/457-458`): render `label: ` when `argLabel` set. Required for `--expand` round-trip (`tests/run_expand_roundtrip.sh`). |
| `docs/reference.md` | §9 below. |
| `tests/corpus/*.lev` + `tests/test_checker.cpp` | §12 below. **New corpus files are `.lev`, never `.ext`** (`feedback_ext-vs-lev`, HARD). |

**Shared-enum etiquette:** no new `TokenKind`/`StmtKind`/`ExprKind` needed (`Colon`, `Eq` already
exist — `Token.hpp:60,72`; `Call` unchanged). The two `Expr` fields and one `Param` field are the
only struct growth — append them at the end of their structs per §2.2 of the overview, and thread
them through `cloneParam`/`cloneArgList` in the *same commit* they are added.

---

## 6. Milestones (ordered; each independently shippable & green)

### M1 — Default parameter values (no named args yet)

The higher-value half and the lower-risk half; ship first.

- **Parser:** `parseParamList` accepts `'=' expr` after the name; store on `Param::defaultValue`.
- **Checker (decl):** verify each default is a constant (literal now; comptime-foldable if H3 is
  cheap), fold it, and reject param/`this` references.
- **Checker (call):** arity match in `pickOverload` becomes a range —
  `nSuppliedArgs ∈ [requiredCount(c), params.size()]` where `requiredCount` = count of params with
  no default and no injectable binding. Score loop iterates over `params`, scoring supplied args and
  skipping defaulted slots. `normalizeCallArgs` fills trailing/omitted defaulted slots with folded
  literals (§4.2). Add the "tighter fit wins" tie-break (§3.2.2).
- **AstPrinter / Rules:** render `= default`; clone `defaultValue`.
- **Acceptance:** `f()`/`f(9)` with `void f(int a = 1, string s = "x")` produce correct values on
  **oracle + IR + emit-C++ + ELF** (LLVM scalar-core where trivial); overload
  `f(int)` vs `f(int, int = 0)` on `f(1)` selects the 1-arg form; churn/leak green; round-trip green.

### M2 — Named arguments at call sites

- **Parser:** `parseArgs` detects `at(Identifier) && peek(1) == Colon` at argument start → named arg
  (store `argLabel`, parse value with `parseExpr(0)`); enforce positional-before-named. A leading
  `IDENT ':'` is unambiguous — a ternary's `:` is always preceded by its `?` (`Parser.cpp:239`,
  bp-2), and the language has no `key: value` literal, so no collision (Hurdle H1).
- **Checker:** the §3 binding maps named labels → param indices; double-fill and unknown-label
  diagnostics (§3.4); `normalizeCallArgs` reorders named args into param order (then clears
  `argLabel` on the placed nodes so the normalised list is positional).
- **AstPrinter / Rules:** render `label: `; clone `argLabel`.
- **Acceptance:** `f(s: "y", a: 3)` reorders correctly on all engines; every §3.3 example behaves as
  documented; the six §3.4 diagnostics have `test_checker.cpp` cases; round-trip green.

### M3 — Unify & harden the one story

- Fold implicit injection into the unbound-param fill (§4.2) so defaults + named + overloads +
  injection are literally one code path; delete the now-redundant trailing-only special-casing in
  `pickInjecting` (keep its synthesis loop as the injection branch).
- Constructors and methods proven on the same path (§4.2) with named-arg + default corpus programs.
- Full engine-coverage sweep per `techdesign-00-overview.md` §4.3; `docs/reference.md` updated (§9).
- **Gate:** all §12 corpus + checker tests green on oracle+IR+native+ELF; reference.md landed;
  `run_expand_roundtrip.sh` green.

### M4 — Attribute named args (discharge the metaprogramming debt)

The whole reason parameter ergonomics was prioritised (`deferal-metaprogramming-tail.md` §4;
`techdesign-metaprogramming.md:169-171`, deviation §14.4). Now trivial:

- `AttrUse::args` (`Ast.hpp:87`) is a positional `vector<ExprPtr>`. Route attribute-argument parsing
  (`Parser.cpp:1082`) through the same named-arg-aware `parseArgs`, and resolve the attribute's field
  list with the same §3 binding (phase4 §9.5: *"nothing attribute-specific to design here"*).
- `@Route(method: "GET", path: "/users")` then works by **reuse**, not new machinery.
- **This milestone is the hand-off point to the metaprogramming tail** — if attribute wiring turns
  out to need rule-stage changes beyond parsing, that is metaprog-track work; STOP and hand back
  (§8). Keep M1–M3 landable without M4.

---

## 7. P-probes (run against `build/lang` before writing code)

Per overview §4.1 — verify assumptions, never assume. Each is a tiny `.lev` program (or a checker
observation):

- **P1 — colon is free in arg position.** Confirm no existing corpus call uses `IDENT ':'` as an
  argument (grep `tests/corpus` + `examples`). Confirms M2's parse trigger is non-breaking.
- **P2 — injection synthesis shape.** Write a `bind`+unfilled-param program, dump with `--expand`
  (or read `call->list` behaviour) to confirm the synthesised-node-into-`list` model is exactly what
  M1/M4 extend (validates §4.1 against the actual injection code, not just the comment).
- **P3 — ctor/method go through `typeOfCallInner`'s resolve helper.** Confirm a constructor call and
  a method call both reach `resolve(...)` (`Checker.cpp:843, 923`) so normalisation covers them for
  free. If a path bypasses it → STOP (design assumed uniform resolution).
- **P4 — comptime fold availability.** Feed `int a = 60 * 60` as a default; confirm the comptime
  pass folds a constant expression to a literal (drives H3 / whether M1 ships literal-only first).
- **P5 — round-trip harness scope.** Run `tests/run_expand_roundtrip.sh` clean *before* changes, so a
  post-change failure is unambiguously ours (validates the AstPrinter obligation in §5).

If a probe contradicts the design → **STOP protocol** (§8), do not improvise.

---

## 8. Foreseeable hurdles & STOP points

- **H1 — named-arg vs. ternary colon.** Resolved: named args are detected only as a *leading*
  `IDENT ':'`; a ternary colon is always preceded by `?`. No object/map `key: value` literal exists
  in the grammar. If P1 surfaces any real collision → STOP.
- **H2 — checker re-entry / double normalisation.** `typeOfCall` can run more than once on a node
  (the injection code guards a "re-type pass", `Checker.cpp:1197-1201`). `argsNormalized` must
  short-circuit re-entry, and normalisation must be a pure function of (raw args, chosen signature).
  Do **not** normalise before a candidate is chosen (different overloads have different param
  names/order).
- **H3 — constant folding for non-literal defaults.** If wiring defaults through the comptime fold
  (`isComptime`, §16.5) is more than a one-call hook, **ship M1 literal-only** and file the
  comptime-widening as an M1 tail item (still M1, same story) — do not block the milestone.
- **H4 — clone/print drops a field.** The single most likely regression (overview §2.1). Add the
  `cloneParam` default copy and `cloneArgList` label copy in the *same commit* as the `Ast.hpp`
  fields, and cover it with a `--expand` round-trip corpus program that uses both a default and a
  named arg.
- **H5 — generic inference + defaults.** A defaulted param whose type mentions a type variable
  (`T item = ...`) can't have a constant default of unknown `T`; forbid defaults on
  type-variable-typed params for v1 (diagnostic), and note it in §11. Named args interact cleanly
  with generic inference because the *supplied* args (which drive inference) are unchanged — only
  their order is.
- **STOP-and-escalate (mandatory, overview §4.2):** if any of the above needs an architectural
  decision the design does not already make (e.g. a call path that bypasses the resolve helper, or a
  requirement for per-call callee-scope default evaluation), **STOP**: log findings in the
  Implementation Log below, commit WIP to the track branch, escalate to a Fable-class model. **Never
  improvise a resolution-order change** — this is the language's core rule and a wrong call here is
  silent-and-distant (info.md §16).

---

## 9. `docs/reference.md` sections (ship in the same commit as the feature)

- **§3.3 Calls and construction** (`:229-239`): add the named-arg call form and the binding rule.
- **§4 Members / parameters** (`:406-411`): add default-parameter syntax and the constant rule.
- **§4.4 Methods, functions, operators** (`:439-449`) and **§4.5 Constructors** (`:451-464`): one
  line each that named args + defaults apply uniformly (the "one story").
- New short subsection **"§4.7-adjacent: argument binding"** stating the §3 algorithm as the single
  normative rule (positional → named → default → injection; most-specific, tighter-fit,
  first-declared).

---

## 10. Timeline (targets; gates have hard "done" criteria)

Start after the current compiler-track queue is clear (this touches `Checker.cpp`/`Parser.cpp`;
coordinate with any in-flight compiler-file track per overview §2.1 merge-order). Estimate: one
implementer, ~2 weeks.

| phase | days | milestone | gate ("done") |
|---|---|---|---|
| N1 | 1–4 | **M1** defaults | `f()`/`f(9)` correct on oracle+IR+native+ELF; overload normal-vs-optional tie-break; churn/round-trip green |
| N2 | 5–8 | **M2** named args | all §3.3 examples correct on all engines; six §3.4 diagnostics tested; round-trip green |
| N3 | 9–11 | **M3** unify | injection folded into one fill path; ctor+method corpus; reference.md landed; full sweep green |
| N4 | 12–14 | **M4** attribute named args | `@Route(method:..., path:...)` works; **unblocks metaprog item M** or hands back per §8 |

**Gate to close the metaprogramming deferral:** M4 green + a note filed against
`designs/deferal-metaprogramming-tail.md` §4 that named arguments now exist, so item M is
schedulable.

---

## 11. Deferred items placed explicitly on the roadmap

- **Non-constant / per-call default expressions** evaluated in the callee's scope (referencing other
  params, `this`, or runtime state). v1 is constant-only (§2.2). Revisit only if a real need appears;
  it would require per-engine work and lose this design's zero-runtime-cost property.
- **Defaults on type-variable-typed parameters** (`T item = ...`) — forbidden in v1 (H5); needs a
  "default of `T`" story that overlaps info.md §3's open generic-default constraint question.
- **Named args in the `([])` indexer / operator call sites** — operators bind by a fixed
  right-operand; no named-arg surface is needed. Out of scope, not a gap.
- **`params`-forwarding + named args** (metaprog `$_params`, `Rules.cpp:1621`) — M4 handles attribute
  args; general template-level named-arg forwarding stays with the metaprog track.

---

## 12. Test corpus plan (all new files `.lev`)

Every feature adds a corpus program exercising it on **all engines it claims** plus checker tests
for the new compile errors (overview §2.2, §4.3). Minimum set:

- `tests/corpus/named_defaults.lev` — free function with all-defaulted params; call with 0, some,
  all supplied. (`.expected` covers oracle+IR+native+ELF.)
- `tests/corpus/named_args.lev` — reordering (`f(s:.., a:..)`), interleaved positional+named,
  method + constructor named calls.
- `tests/corpus/named_overload.lev` — `f(int)` vs `f(int, int=0)` on `f(1)` (tighter-fit), plus a
  named-arg-selected overload.
- `tests/corpus/named_roundtrip.lev` — uses a default **and** a named arg; drive
  `tests/run_expand_roundtrip.sh` to prove clone+print carry both fields (H4).
- `tests/corpus/attr_named.lev` (M4) — `@Route(method: "GET", path: "/x")` on a decl.
- `tests/test_checker.cpp` — the six §3.4 diagnostics (positional-after-named, unknown label,
  double-fill, missing-required, non-constant default, default-refs-param).

Acceptance commands per milestone: the standard `techdesign-00-overview.md` §4.3 block (clean build
zero new warnings; `ctest`; `run_corpus.sh`; `run_elf.sh`; `run_native.sh`; `run_expand_roundtrip.sh`;
`churn_leak.py`).

---

## 13. Implementation Log

*(implementers append here — findings, deviations, STOP escalations, per the overview §4.2/§4.4)*

- **2026-07-10 — M1–M4 landed.** `Param::defaultValue/defaultFolded` and
  `Expr::argLabel/argsNormalized` carry the syntax through parsing, rule cloning, and source
  printing. The checker now maps supplied arguments per candidate, fills defaults before lexical
  injection, scores only caller-supplied values, prefers fewer fills on equal type scores, and
  normalizes the chosen call to a complete positional list. Constructors, methods, free and
  namespaced functions all use that path; the existing engines needed no changes.
- Constant default expressions reuse the hermetic comptime evaluator and are reified once before
  checking/lowering. Parameter/`this` references, non-constant expressions, type mismatches, and
  type-variable-typed defaults are rejected. The idempotence cache prevents the rule stage's
  second walk from evaluating a default twice.
- Attribute uses route through the named-aware argument parser and bind their named/positional
  arguments against attribute fields with field initializers as defaults, completing M4.
- Regression coverage: the six required diagnostics in `test_checker.cpp`; a `.lev` corpus program
  covering free functions, non-trailing defaults, reordering, tighter-fit overloads, methods,
  constructors, and comptime folding; and a metaprogramming corpus program covering named
  attribute arguments plus defaults. Focused oracle, IR, emit-C++, pure ELF, LLVM emission, and
  meta `--expand` round-trip checks are green. No STOP condition was encountered.
