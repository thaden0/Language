# Tech design — namespace-local class constructor dispatch (bug.md #32)

**Status:** LANDED IN FULL (2026-07-11) — M1 (qualified form) and M2 (bare
same-namespace form) both implemented and verified. **Date:** 2026-07-11.
**Source:** hit implementing `techdesign-regex-engine.md` M1 (2026-07-11);
owner requested this design same day; M2 landed same-day closing bug.md #32
in full (owner directive to work the bug backlog to completion).
**Owns (regions):** the two mirrored constructor-fallback sites — `Evaluator::ctorTarget`
in `src/Eval.cpp`, the ctor fallback in `src/Lower.cpp` `lowerCall`; regression corpus
`tests/corpus/namespace_ctor/`; the re-namespacing of the regex engine internals in
`src/Resolver.cpp` (`kPreludeRegexCore` only). **Does NOT own:** the Checker (the
prelude stays unchecked — see §3 "not the fix"), the parser, `Scope::lookup` semantics,
any other prelude segment.

---

## 1. Problem

A class declared inside a `namespace` block cannot be constructed from **unchecked**
code. Both forms fail to dispatch:

- bare `ClassName(...)` from inside the same namespace, and
- qualified `ns::ClassName(...)` from anywhere.

Free *functions* in namespaces dispatch fine (`ns::fn(...)` works in the prelude
today). Checked user code is unaffected: the Checker walks the user program with the
real nested scopes and bakes `Expr::resolvedClass` / `Expr::resolved`, which both
engines honor — verified empirically on all four maintained lanes. The prelude is never
checked (deliberate; the attempt was reverted in `1c76034` because checking the prelude
mis-resolves native `at`/`length` calls and breaks compiled backends), so every prelude
call site takes the name-based fallback, and that fallback is global-only for
constructors.

**Impact.** The regex track (Track 10, `techdesign-regex-engine.md`) was forced to
hoist its engine internals (`RegexCoreCompiler`, `RegexCoreVm`, `RegexCoreFragment`)
to uniquely-prefixed **top-level** classes — which makes engine internals
user-instantiable, defeating the `kPreludeRegexCore` encapsulation boundary that doc 1
specifies. Every future prelude track that nests a class in a namespace hits the same
wall from scratch. Filed as bug.md #32 [P1 / marker P1.2: the only workaround is
per-use naming convention].

## 2. Root cause (verified against source, 2026-07-11)

- Namespace members live in a **child scope**: `Resolver::gatherInto`
  (`src/Resolver.cpp:3580` region) creates `ns->scope = sema_.newScope(scope)` and
  gathers the namespace body into it.
- `Scope::lookup` (`src/Symbols.hpp:79` region) walks `this → parent` only. That is
  correct scoping; descent into a namespace's child scope must be explicit at
  qualified-name sites — and for constructors it never is:
  - `Evaluator::ctorTarget` (`src/Eval.cpp:172`): the `Name` branch does
    `sema_.global->lookup(text)`; the `Member` branch does
    `sema_.global->lookup(callee->a->text)` and requires the base to be a **Class**
    (that branch exists for the labeled-ctor form `C::Label(...)`). A `Namespace` base
    never matches, so `ns::C(...)` is not recognized as construction at all.
  - `Lower.cpp` ctor fallback (`src/Lower.cpp:1015` region): same shape —
    `sema_.global->localLookup(nameExpr->text)`, no namespace case. This single site
    feeds `--ir`, emit-cpp (`--build`), and LLVM (`--build-native`).
- The working precedent to mirror: function dispatch has explicit namespace descent in
  both engines — `Evaluator::resolveFunction` (`src/Eval.cpp:187`) and the Lower
  function path (`src/Lower.cpp:1200` region, via `namespaceSym(...)`, which also
  resolves block `uses`/`use ... as` overlays per bug #1).

## 3. Fix design — mirror the function path

**M1 — the qualified form `ns::Class(...)`** (sufficient for the prelude use case):

1. `Evaluator::ctorTarget`, `Member` branch: after the existing Class-base check
   (labeled ctors keep today's meaning), if the base name resolves to a `Namespace`
   symbol with a scope, `ns->scope->localLookup(callee->text)`; if that yields a
   `Class` symbol → `cls = it, label = class name` (default / class-named ctor).
2. `Lower.cpp` ctor fallback: identical descent, but through `namespaceSym(...)` rather
   than raw global lookup, so import overlays behave exactly as they do for function
   calls.
3. Disambiguation rule, stated once: in `X::Y(...)`, if `X` is a class → labeled ctor
   `Y` on `X` (today's semantics, unchanged); if `X` is a namespace → construction of
   class `Y` in `X`. Symbol kinds are disjoint within one scope, so the two cannot
   collide; check Class first to preserve existing behavior byte-for-byte.
4. The nested labeled form `ns::Class::Label(...)` (callee =
   `Member(Member(ns, Class), Label)`): resolve the inner member to a class via the
   same descent, then treat as labeled ctor. If deferred out of M1, the failure mode
   stays a loud unknown-callable error, not a silent mis-dispatch — acceptable to
   defer, must be noted in the log if so.

**M2 — bare `Class(...)` inside its own namespace, unchecked code** (landed
2026-07-11, see §7): `Stmt::enclosingNs` records the innermost namespace a free
function was gathered directly inside (`Resolver::gatherInto`, threaded through
the recursive-descent parameter, not written for methods/ctors — those go
through `gatherClass` instead). `Evaluator::callFunction` sets a new
`curNamespace_` runtime field from it around a free-function's body (cleared
inside `runCtor`, which is never itself namespace-scoped); `Lowerer::lowerPending`
sets the lowering-time equivalent from `Pending::cls ? nullptr : decl->enclosingNs`.
`ctorTarget`'s (Eval.cpp) and the ctor fallback's (Lower.cpp) bare-`Name` branches
try `curNamespace_->scope` after the existing global lookup misses, mirroring the
qualified form's namespace descent one level up.

**Not the fix, considered and rejected:**
- Running the Checker over the prelude — tried, reverted (`1c76034`), breaks compiled
  backends; standing rule is the prelude is never checked.
- Making `Scope::lookup` descend into namespace child scopes implicitly — changes
  scoping semantics globally for a local dispatch gap.

## 4. Acceptance

1. New corpus dir `tests/corpus/namespace_ctor/` (`.lev`): user-code qualified
   construction, labeled-ctor-on-class (regression guard for the disambiguation), and
   a namespace-local class used through a namespace function — green on all four
   maintained lanes (`--run`, `--ir`, emit-cpp, LLVM).
2. The real unchecked-path proof: move `RegexCoreFragment` / `RegexCoreCompiler` /
   `RegexCoreVm` back inside `namespace regex` in `kPreludeRegexCore`, constructing via
   qualified names; `tests/corpus/regex_engine/` stays green on all four lanes. This
   closes the encapsulation deviation recorded in `techdesign-regex-engine.md` §11.
3. bug.md #32 closed with the commit ref (commit message prefixed `bug.md #32`).

## 5. Milestones

| M | deliverable | date |
|---|---|---|
| M1 | qualified-form fix in both engines + corpus + regex re-namespacing | Jul 12 (0.5 day) |
| M2 | bare-name enclosing-scope fallback | landed 2026-07-11 (bug backlog pass) |

## 6. STOP conditions (Sonnet protocol)

- Any real ambiguity in the `X::Y(...)` disambiguation (a name that is somehow both a
  class and a namespace in one scope) — escalate, do not invent precedence.
- Regex re-namespacing still failing on any maintained lane **after** the fix — file
  the divergence with the differential output, keep the top-level names, escalate.
- Any temptation to widen into checking the prelude — that is a standing owner ruling,
  not open for re-litigation here.

## 7. Implementation log

**2026-07-11 — M1 landed.**

- `Evaluator::ctorTarget` (`src/Eval.cpp`) `Member` branch: after the existing
  Class-base check, a `Namespace` base now descends via `scope->localLookup`
  for a same-named `Class`, exactly as designed in §3 step 1.
- `Lower.cpp` ctor fallback: same descent, through `namespaceSym(...)` so
  block import overlays behave like the function-call path (§3 step 2).
  Gated `if (!ctorClass && ...)` after the existing Class-base lookup, so the
  labeled-ctor byte-for-byte precedence (§3 step 3) is unchanged.
- M1 stopped at the qualified form only; the nested labeled form
  `ns::Class::Label(...)` (§3 step 4) was left as a loud unknown-callable
  error, per the doc's acceptable-to-defer note — no ambiguity encountered
  requiring escalation.
- `tests/corpus/namespace_ctor/` added (checked-user-code regression guard:
  qualified construction, the `C::Label(...)` disambiguation, bare
  construction via a namespace function) — green on all four maintained
  lanes, wired into CTest (`corpus_namespace_ctor_{treewalk,ir,cpp,llvm}`).
- `RegexCoreFragment`/`RegexCoreCompiler`/`RegexCoreVm` moved back inside
  `namespace regex` in `kPreludeRegexCore`, with every internal construction
  site qualified (`regex::Name(...)`) since M2 stays deferred.
  `tests/corpus/regex_engine/` re-verified green on all four lanes — the real
  unchecked-path proof (§4 acceptance item 2).
- Full suite (130 CTest cases) green after the change. bug.md #32's qualified
  form closed.

**2026-07-11 — M2 landed** (same day, second pass — bug.md #32 reopened
because its repro covers both the bare and qualified forms, and the owner
directive was to close reported bugs, not partially).

- `Ast.hpp`: new `Stmt::enclosingNs` (`Symbol*`, default null) — the innermost
  namespace a free-function `Member` decl was gathered directly inside.
- `Resolver::gatherInto` (`Resolver.cpp`/`.hpp`): threaded an `enclosingNs`
  parameter through the existing recursive descent (passed as the reopened/
  new `Symbol* ns` at the `Namespace` case), and stamps it onto every
  gathered free-function `Member`. Methods/ctors are unaffected — they are
  gathered via the separate `gatherClass` path and never touch this field.
- `Eval.hpp`/`.cpp`: new `Evaluator::curNamespace_` (the enclosing namespace
  of the free function currently executing). Set from `fn->enclosingNs` in
  `callFunction` only when `!self` (free functions, not methods), saved/
  restored around the call; explicitly cleared in `runCtor` (a ctor body is
  never itself namespace-scoped, so a caller's `curNamespace_` must not leak
  into one). `ctorTarget`'s `Name` branch tries `curNamespace_->scope` after
  the existing global lookup misses.
- `Lower.hpp`/`.cpp`: new `Lowerer::curNamespace_`, set in `lowerPending` as
  `p.cls ? nullptr : p.decl->enclosingNs` (the same class-vs-free-function
  gate `runCtor` applies at runtime, expressed at lowering time instead). The
  ctor fallback's bare-`Name` branch tries `curNamespace_->scope` the same
  way, right after the existing qualified (`ns::Class`) branch.
- Verified via a standalone harness that resolves+lowers a program WITHOUT
  running the Checker (the same "unchecked" treatment the real prelude gets,
  without touching prelude content) — bug.md #32's exact repro (`probe()`
  bare form and `probe2()` qualified form) both return `7` on the oracle and
  the IR interpreter; since IR/emit-cpp/LLVM all consume the identical
  `Lower.cpp`-resolved `Op::Call` (a static function-index target, not a
  per-backend name lookup), the one shared fix covers all three lowered
  lanes the same way M1's did. `tests/corpus/regex_engine/`,
  `corpus_namespace_ctor_*`, and the full suite stay green.
- bug.md #32 closed in full (both M1 and M2).
