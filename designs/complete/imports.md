# Selective imports — `use Path::name as alias` — Technical Design

**Status:** implemented (2026-07-05) — both keywords, full lexical scoping (file- and
block-level). §5's attribute/rule interaction verified working and pinned with tests
(2026-07-10 — see §11); the only interaction still deferred is bind *activation*, which is
`system-binds.md`'s scope. **Date:** 2026-07-04.
**Staging (2026-07-05, superseded):** implementation was started, and probing the
compiler surfaced that **no block-level symbol-scope substrate existed at the time**
(see §9). This staged v1 to file-level scoping only, deferring block-level `use`/`uses`
behind a loud error. **Superseded the same day**: two follow-on bug fixes (bug.md #8's
`uses` lexical-scoping fix and bug.md #9's `bind`/`inject` DI core) built exactly the
missing substrate — a per-block overlay scope (`Stmt::importScope`, Resolver-populated,
Checker-consulted) for `uses`, and an analogous per-block type-keyed stack
(`bindScopes_`) for `bind`. With that substrate already in place, this implementation
lands the **full §2 ruling** directly: `use` gets file- **and** block-level lexical
scoping, on the first pass, by reusing `uses`'s existing per-block mechanism — no
separate v2 needed. See "Implementation notes" (§10, end of doc) for what shipped, three
runtime-layer alias-blindness bugs found and fixed along the way, and one pre-existing
gap (bug.md #10) found but left open (out of scope: a lowering gap in `uses` itself,
predates this feature).
**Related:** `argv.md` (the `env::args()` surface is a natural consumer), `const.md`
(imported aliases inherit constness), bug.md #8 (the `uses` scoping defects this design
fixes, and whose fix supplied the block-scope substrate `use` reuses), bug.md #10 (a
lowering gap found while verifying this feature — pre-existing, left open),
`system-binds.md` (the type-keyed sibling: `bind` as the other lexical provision system,
riding the same scoping model, per bug.md #9).

The ask: pull **individual declarations — including instantiated values — out of a
namespace into another scope**, with `as` renaming to dodge collisions, targetable at a
scope rather than only whole-file. This document specifies that feature and answers the
scoping question (file-wide vs. scope-wide) in the language's own terms.

Everything below was verified against this tree (`build/lang`, Jul 4) by reading source
and running probe programs.

---

## 1. Current state (verified — three probes, one surprise each)

The language has two import-shaped keywords today:

- **`uses NS;`** — imports *all* of a namespace's names ([Resolver.cpp:789](src/Resolver.cpp#L789),
  `processImports`). Probed behavior:
  - **Probe A — position-independent.** A bare call *above* the `uses` statement
    resolves fine. It is a scope-table dump, not a lexical construct.
  - **Probe B — silently inert inside blocks.** `void f() { uses M; helper(5); }` →
    `error: unknown function 'helper'`. `processImports` only walks top-level items and
    namespace bodies, never function/block bodies — so a block-level `uses` parses,
    does nothing, and *no diagnostic points at it* (the silent-failure family of
    bug.md #1/#5/#7; filed as bug.md #8).
  - **Probe C — leaks program-wide across files.** In a two-file project where only
    `a_lib.ext` says `uses M;`, the entry file resolves bare `helper(7)` having imported
    nothing. Top-level `uses` dumps into the *shared global scope* of the dissolved
    compilation unit — the "import" is program-wide, not file-wide. This also quietly
    **bypasses phantom-dependency strictness** (§8.2 of the reference): file B rides on
    file A's import without declaring anything.
- **`use expr;`** — a **vestigial statement**: parses ([Parser.cpp:767](src/Parser.cpp#L767)),
  type-checks as a bare expression ([Checker.cpp:1092](src/Checker.cpp#L1092)), and is
  explicitly skipped by both engines ([Eval.cpp](src/Eval.cpp) "Bind/Use/decls: not
  executed here"; [Lower.cpp:505](src/Lower.cpp#L505)). **Zero uses in the entire corpus
  and examples** (grepped). It is free real estate — and its name is exactly right.

Meanwhile three existing features already treat **the file** as a real scoping unit:
attribute resolution ("resolves through the namespaces the *declaring file* imports" —
reference §6.9), rule activation (same uses-graph), and phantom-dep checking
([Project.cpp:524](src/Project.cpp#L524), [:673](src/Project.cpp#L673) track `uses` per
file). So the codebase currently holds **two inconsistent scoping models for the same
statement**: per-file for metaprogramming and dependency strictness, program-global for
ordinary names. This design unifies them.

## 2. The scoping question: file-wide or scope-wide?

The user intuition — "imports sit at the top of the file, so file-wide" — is the right
*ergonomics*, but file-wide is the wrong *mechanism* for this language, for one
deep reason: **files dissolve at gather time by design** (§12: namespaces are
declaration-based; "file boundaries dissolve, namespace boundaries persist"). Making
imports file-scoped as a primitive would reintroduce the file as a semantic wall the
language deliberately tore down.

The language already has the right pattern, in `bind` (§12.5): **block-scoped,
lexically resolved, nearest-wins — where "a namespace is just one kind of block."**
Imports should be the same kind of thing:

> **An import is a declaration in its enclosing lexical scope. The top level of a file
> is a lexical scope. Therefore a top-of-file import covers exactly that file — as the
> *consequence* of one rule, not as a special file-wide mechanism.**

**Owner ruling (2026-07-04): confirmed.** Imports are usable anywhere and scoped where
they appear; at the top of a file outside any block, that means file-scoped. Consistent,
and allows either preference.

**Staging note (2026-07-05, superseded — see the doc header).** This section originally
staged the ruling's "anywhere" half as deferred, for want of a per-block symbol scope.
That substrate landed the same day, built by bug.md #8 (block-scoped `uses`, via
`Stmt::importScope`) and bug.md #9 (block-scoped `bind`, via `bindScopes_`) — so the
implementation below lands the **full ruling directly**: both `use` and `uses` are
block-scoped as well as file-scoped, reusing bug #8's per-block overlay mechanism
one-for-one (no new substrate needed; `use` just populates the same overlay `uses`
already does, selectively).

What falls out (the full ruling, as implemented):

- **"Top of the file" works exactly as the idiom suggests** — covering that file and
  nothing else. Probe C's cross-file leak disappears (fixed by bug.md #8); phantom-dep
  strictness is enforceable (a file's names come only from its own imports).
- **Scope-targeted imports** — `use`/`uses` inside a function body or block, scoped to
  that block — **work**, reusing bug.md #8's per-block overlay (`Stmt::importScope`):
  the Resolver populates it with the block's own `use`/`uses` statements, pushed and
  popped through the body walk; the Checker consults the same object. Confirmed
  block-confined (does not leak to an enclosing scope or a sibling block) — see
  `tests/test_checker.cpp`'s `imports.md` section.
- **Nearest-wins shadowing** composes with everything that already scopes (locals shadow
  imports; `bind`'s pattern; inner import shadows outer, block substrate included).
- **Visibility is whole-file (hoisted), not positional.** Probe A's
  position-independence is *kept*: an import anywhere in the file is visible throughout
  it, matching the language's order-independent declaration model (namespaces merge
  regardless of order; entry-function projects gather declarations before running).
  "Top of the file" stays a *convention* the formatter/linter can encourage, not a rule
  the resolver needs.

## 3. Surface

Repurpose the vestigial **`use`** (singular) for the selective form; `uses` (plural)
stays the bulk form. The pairing reads naturally: *this file uses a namespace; pull one
name with use.*

```
use std::read;                        // bind one name into this scope
use std::read as readMode;            // ...renamed (collision-proof)
use std::env::args;                   // a function — bare args() now works here
use Lcurl::Header as HttpHeader;      // a class — type positions and construction
use A::B;                             // a NESTED NAMESPACE — binds B for B::f()
use A::B as C;                        // ...aliased: C::f()

uses Lcurl;                           // unchanged: dump the namespace's names here
```

- **`as` is a contextual keyword**, not reserved — the precedent is explicit
  (reference §1.5: `attribute`, `comptime`, `rule`, `macro` are contextual), and `as`
  already means exactly this in the manifest layer (`dep { ...; as = "Json"; }`). This
  is the manifest's aliasing brought into the language proper.
- **One name per statement** in v1 (a comma list is trivial future sugar; it buys
  nothing semantically).
- **Grammar**: `use qualified-path (as Identifier)? ;` — a strict *restriction* of
  today's `use expr;`, which nothing exercises (§1), so nothing breaks.
- Division of labor stays crisp: **`use` binds one name** (member, value, class, or
  namespace — no member dump); **`uses` dumps a namespace's names**. No `uses ... as`
  — aliasing a namespace is `use`'s job, because it binds one name.

## 4. Semantics: an alias, not a copy

`use` adds a **name → declaration binding** to the enclosing scope's resolution table —
the same mechanical thing `processImports` already does per-name for `uses`, done
selectively and lexically. It is pure resolution:

- **The alias names the same slot.** `use std::read as rm;` — `rm.has(...)` reads the
  one global; there is no runtime copy, no initialization order, no cost. (A copy would
  be the silent-divergence footgun: mutate through one name, the other doesn't see it.)
  Writes through the alias are writes to the global — governed by the bug.md #7 ruling
  (compile error) and, once `const.md` lands, by the slot's constness, which the alias
  inherits *because it is the same symbol*.
- **Every declaration kind imports uniformly** — a value global, a function, a class, a
  nested namespace. One rule: a name is a name (§6.5's "a member is a member," applied
  to scopes). No special "value import" vs "type import" split.
- **Overload sets travel whole.** `use M::helper;` binds every `helper` overload;
  call-site resolution by argument types is unchanged. Two `use`s contributing the same
  name from different namespaces merge overload sets exactly as two `uses` dumps do
  today; the only true collision remains **name + type** (§4 of info.md), and a bare
  use of such a pair errors *at the use site* ("refuse to guess"), same as everywhere.
  (Contrast `bind`, where duplicates are import-time hard errors — bindings are
  type-keyed with one winner per type, so no overload-set merge exists to permit.
  Different structure, honestly different rule.)
- **Shadowing**: a `use`-bound name is a declaration in its scope — it shadows
  `uses`-dumped names in the same scope (specific beats bulk, the most-specific-wins
  instinct), and nearer scopes shadow outer ones as always. Re-importing the *same*
  symbol under the same name is idempotent, not an error.

## 5. Interactions

- **Phantom-dependency strictness** — `use X::y;` counts as the file using namespace
  `X`: the same direct-deps-only rule applies, through the same per-file tracking
  ([Project.cpp:524](src/Project.cpp#L524)). With §2's scoping fix, the strictness stops
  being bypassable (probe C).
- **Attributes and rules — WORKS at namespace grain (verified 2026-07-10; the earlier
  "not yet implemented" note was stale — see §11).** Attribute names resolve through the
  declaring file's imports (§6.9) via `Rules.cpp`'s `resolveAttr`, which iterates
  `imports_[fileIdx].effective`; rule activation gates on the same set
  (`effective.count(r.ns)`, [Rules.cpp:565](src/Rules.cpp#L565), [:1099](src/Rules.cpp#L1099)).
  `computeFileImports` already folds a selective `use NS::name` into `importsExplicit`
  (path minus the imported name — [Project.cpp:359](src/Project.cpp#L359)) → `effective`,
  identically to a bulk `uses NS` and to the phantom-dep treatment (the bullet above). So
  a file with only `use Web::Route;` **does** resolve a bare `@Route` **and does** activate
  Web's rules — no separate selective-attribute set was ever needed; the shared `effective`
  set already carries it, and the same commit that shipped `use` (ec01aee) wired it. Grain
  is **per-namespace, not per-name**: importing *any* Web name (`use Web::other;`) makes
  *every* Web attribute bare-resolvable, exactly as `uses Web` and phantom-deps already do
  (§4's "a name is a name" ∪ §5's namespace-grain phantom rule) — metaprogramming reach is
  deliberately scoped at the namespace, not the individual name. Pinned by
  `tests/corpus/project/attr_scope_use_ok` (attribute) and `rule_scope_use_ok` (rule) — the
  selective-`use` siblings of `attr_scope_ok`/`rule_scope_ok`. A per-name selective-attribute
  set (only the specifically-imported attribute bare-resolvable) is a *possible* future
  precision but is intentionally not built: it would make attribute reach **stricter** than
  phantom-dep reach, a new asymmetry the current uniform model avoids.
- **Binds — NOT YET IMPLEMENTED (follow-up, blocked on the above).** The wall-crossing
  ruling (system-binds.md §5, 2026-07-05) — bulk `uses NS` imports names only and never
  activates a bind; a selective `use NS::IBound;` of a bind-keyed interface imports the
  name **and** activates its bind — is system-binds.md's design, not built here. This
  implementation gives `use` the plain name-import half uniformly for every symbol kind
  (including bind-keyed interfaces, which import and resolve as ordinary types); the
  bind-*activation* trigger on a selective import is a `system-binds.md`-scoped addition
  for whoever implements that design next.
- **`const`** — nothing special: the alias is the symbol; const-ness rides along
  (const.md).
- **comptime** — imports are compile-time resolution facts; nothing to deny, nothing to
  evaluate. `lang --imports` (the file → imports provenance map, §7 of the reference)
  gains `use` rows — the tooling hook already exists.
- **argv.md's nested-namespace caveat** — `use std::env;` (or `as e`) binds the nested
  namespace at one level, which sidesteps the currently-broken two-level qualified
  access (`A::B::f()`, argv.md §5) without waiting for that fix.

## 6. Implementation inventory (as shipped, 2026-07-05)

1. **Parser** ([Parser.cpp](src/Parser.cpp) `parseUse`) — replaced the `use expr;` body
   with path-and-alias parsing, via a `parsePath` helper shared with `parseUses` (both
   walk the same `::`-segment loop); contextual `as` = an Identifier-with-text-"as" peek,
   the attribute/comptime precedent (reference §1.5). Wired at both statement position
   (`parseStatement`'s `KwUse` case) and top-level/namespace-body position
   (`parseTopLevelItemInner`, alongside `KwUses` — the latter was missing a case for
   `KwUse` entirely before this change, so a top-level `use` previously fell through to
   the bare-expression fallback and failed to parse). [Ast.hpp](src/Ast.hpp):
   `StmtKind::Use` reuses `generics` for path segments and `name` for the bound name
   (alias if given, else the last segment) — the same fields `UsesImport` already used,
   as anticipated.
2. **Resolver** ([Resolver.cpp](src/Resolver.cpp) `useOne`) — full lexical scoping
   (file- **and** block-level; see the doc header — the "deferred" framing below was
   superseded the same day):
   - **Selective binding** (`useOne`): navigate all path segments but the last as a
     namespace path (mirroring `importOne`); resolve the final segment as *any* symbol
     kind in that namespace's own scope (`localLookup`, not `lookup` — a qualified name
     must not leak outward); push every matching symbol (the overload-set case) into the
     target scope under the bound name. A single-segment path (no `::`) resolves through
     the ordinary enclosing-scope chain instead of navigating a namespace.
   - **File-level scoping**: reuses bug.md #8's existing per-file overlay
     (`Sema::fileScopeFor`) one-for-one — `processImports` calls `useOne` into the same
     scope `importOne` already targets at top level.
   - **Block-level scoping**: reuses bug.md #8's existing per-block overlay
     (`Stmt::importScope`, created in `resolveStmtTypes`'s `Block` case) — a block
     containing a `use` gets the same overlay a block containing `uses` gets; both
     keywords populate it in the same pass.
   - **Shadowing order**: within a scope, all `use` statements are processed *before*
     any `uses` statement (both in `processImports` and in the per-block pass), so a
     selective import's symbol lands at the front of the scope's name vector —
     `Scope::lookup` returns `.front()`, so `use` shadows a same-named `uses`-dumped
     symbol ("specific beats bulk," §4). Function overloads are unaffected: overload
     collection walks the whole vector, not just the front, so two same-named functions
     still merge regardless of order.
3. **Checker** ([Checker.cpp](src/Checker.cpp)) — `StmtKind::Use` no longer type-checks
   a dangling expression (it carries no `expr` now); like `UsesImport`, it is invisible
   to the Checker's statement switch — all its errors (unknown path segment / unknown
   member) are reported by the Resolver's `useOne`, matching `uses`'s existing pattern.
4. **Project layer** ([Project.cpp](src/Project.cpp)) — `walkImports` (phantom-dep
   check) and `walkProvenance` (`--imports` provenance map) both treat a selective
   `use NS::name` as drawing from namespace `NS` (path minus the imported name) for
   phantom-dep purposes, identically to a bulk `uses NS`; `FileImports` gained a
   `useNames` field so `--imports` also prints a `use:` row per file listing its
   selective imports (with `as` shown when aliased) — the display-only half of
   §5's tooling note.
5. **Eval / IR / Lower / backends — NOT zero changes, as this doc originally predicted.**
   `Lower.cpp` and `Eval.cpp` skip the `Use`/`UsesImport` *statements* themselves (as
   predicted — imports never reach runtime as statements), but probing surfaced that a
   bare **read of a namespaced value global**, and a bare **namespaced-function call
   through an aliased namespace**, both route through alias-blind, string-keyed runtime
   lookups that predate this feature and were never previously alias-tested (nothing
   before `use` could reach a global under any name but its own). Three small fixes
   (Checker recording the resolved declaration on the read so Eval/Lower can key their
   lookup by its real name instead of by whatever alias text read it; the construction
   path using the resolved class's own name as the ctor label instead of the call-site
   text; `Lower.cpp`'s namespace-alias recognition searching the file overlay instead of
   only `sema_.global`) — see "Implementation notes" for detail. A fourth, pre-existing
   gap in the same family (block-scoped namespace aliasing specifically in `Lower.cpp`)
   was found but not fixed — filed as bug.md #10 (predates `use`; reproduces with plain
   `uses`).
6. **Docs** — reference §1.5 (contextual `as`), §4.1 (uses + use), §5 statement list;
   info.md §12 gains the `use` subsection with the lexical-import model and the
   file-top-is-a-scope framing.

## 7. Testing

- **Corpus** (`tests/corpus/use.ext`, five-engine): `use` of a value global (read
  through an alias — the same slot, no copy), a function (bare call, and shadowing a
  same-named bulk-`uses` import), a class (construction + type position, both directly
  and through an alias), a nested namespace (`use A::B as C; C::f()`), and a
  block-scoped selective import. `tests/corpus/project/use_scoped_ok` and
  `use_leak_err` (positive/negative multi-file — file-level scoping and its
  cross-file non-leak); `tests/corpus/project/phantom_dep_use` (negative — a
  selective `use` of a transitive dependency's namespace is a phantom-dep error, same
  as a bulk `uses` would be).
- **checkertests**: every declaration kind imports cleanly; unknown path segment / unknown
  final member errors; `as`-renamed access; file-level position-independence; block-level
  scoping AND confinement (does not leak to an enclosing scope); shadowing (`use` beats a
  same-named bulk `uses`); construction through an aliased class; write-through-a-bare-
  alias-to-a-namespace-global is rejected (bug #7's ruling, extended) while a plain
  top-level global stays reassignable.
- **parsertests**: path + `as` alias parsing, nested path, block position; `uses` grammar
  unaffected.
- **resolvertests**: selective import of a class resolves for type positions; unknown
  member / unknown namespace error.
- **Differential**: `--run`/`--ir`/`--emit-elf`/emit-C++ all agree on `use.ext` (verified
  during implementation — this is where the three Eval/IR/Lower gaps above were caught).
- **Tooling**: `--imports` shows a `use:` row per file (pinned in
  `use_scoped_ok/expected.imports`).

## 8. Open questions

1. **Re-export** (`public use X::y;` — facade modules re-exporting their deps' names) —
   the natural v2; requires deciding what "public" means at file/namespace top level.
   The owner's 2026-07-05 sketch introduced an `export name;` statement; the current
   lean is that **the access system is the export surface** (`public` members of a
   namespace are importable, `private` are not — no parallel statement), with explicit
   export lists remaining open. See system-binds.md §5.1 for the bind-package case.
2. **Comma lists** (`use A::{x, y as z};` or `use A::x, A::y;`) — pure sugar; add when
   demand shows. The owner's sketch also proposed a `from`-grouped form
   (`uses console, binding as binds from std;`) — recorded as the alternative grouping
   surface; path-first is kept for v1 because it reuses the existing `::` grammar and
   needs no new `from` contextual keyword.
3. ~~**Retrofitting `uses` scoping**~~ — **resolved, ahead of this feature.** bug.md #8
   landed `uses`'s lexical scoping (both file- and block-level) before this
   implementation started, so `use` was built directly against the same, already-fixed
   `uses` rather than needing to sequence the two.
4. **Bug.md #10** (new, found during this implementation) — a pre-existing lowering gap:
   a namespace known only through a *block-scoped* import (`uses` or `use`) can't be
   used as a further `::` qualifier on `--ir`. Left open (see "Implementation notes",
   §10); the fix needs the lowerer to carry the enclosing block's import overlay through
   expression lowering, which is more plumbing than a targeted patch and is a gap in
   `uses`'s existing block-scoping (bug #8), not in anything new `use` introduces.

## 9. Block-level scoping — RESOLVED (2026-07-05, same day as the finding below)

**Resolution.** The substrate this section found missing was built the same day, by two
separate bug fixes rather than one joint pass: bug.md #8 gave `uses` (and now `use`) a
per-block **name-keyed** overlay `Scope` (`Stmt::importScope`), and bug.md #9 gave `bind`
a per-block **type-keyed** stack (`Checker::bindScopes_`). They ended up as two distinct
mechanisms, not the single unified structure this section's closing paragraph
anticipated — each keyed by what its own feature needs (a name vs. a canonical type),
and neither needed the other's shape. That turned out fine: building them in the same
week, with each aware of the other (per the cross-references both bugs' entries carry),
avoided the real risk this section flagged — two *incompatible*, mutually-unaware scope
mechanisms — without requiring they be literally the same object. This implementation
reuses bug #8's overlay directly (`useOne` populates the same `Stmt::importScope` a
block's `uses` statements populate); the finding below is kept for its record of *why*
the substrate was believed missing, and remains accurate as a description of the
compiler's state before both fixes landed.

**Finding (2026-07-05, verified by reading the compiler; deferred by owner decision).**
When implementation started, the plan in §6.2's original wording — "process imports…
inside block bodies where the body resolver runs (*where `bind`'s block scoping already
lives*)" — turned out to rest on a false premise. **There is no block-level symbol scope
anywhere in the compiler, for anything**, `bind` included:

- **Resolver's `Scope` chain** branches only at global, namespaces, classes, and
  generic-method type-param scopes ([Resolver.cpp:944-985](src/Resolver.cpp#L944)).
  `resolveStmtTypes` walks into `if`/`while`/`for`/blocks but creates **no** scope for
  them and only resolves declared *type references*, never bare names or calls.
- **The Checker** — where bare calls (`helper(5)`) actually resolve, via
  `functionOverloads` → `scope_->lookup` ([Checker.cpp:841](src/Checker.cpp#L841),
  [:644](src/Checker.cpp#L644)) — holds a single `scope_` pointer set once per
  function/method ([Checker.cpp:1142](src/Checker.cpp#L1142)) and never repositioned
  within a body. Locals live in a *separate* structure, `env_` (a per-block stack of
  `name → Type` — [Checker.cpp:1019-1022](src/Checker.cpp#L1019)), which stores
  **variable types, not symbols/namespaces/imports**.
- **`bind` proves it**: `Checker::check`'s `StmtKind::Bind` case
  ([Checker.cpp:1112](src/Checker.cpp#L1112)) just type-checks the bind's `=>` body as
  an ordinary statement in whatever function encloses it — which is exactly why bug.md
  #9 saw *"cannot return 'Hello' as 'void'"*. There is no per-block binding registry,
  because there is no per-block symbol scope to hang one on.

**Why file-level is separable and cheaper.** File boundaries are known at gather time
([Project.cpp:524](src/Project.cpp#L524)), so a per-file top-level scope layer plus
file-origin selection of the Checker's `scope_` delivers §2's file-level half **without**
a per-statement scope stack. That is v1. Genuine block-level scoping needs a new
**symbol-keyed, push/pop-per-block scope** threaded through `Checker::check` alongside
`env_`, reconciled with `scope_->lookup` so shadowing order (locals ▸ block imports ▸
outer imports ▸ bulk `uses`) resolves correctly. That is the deferred substrate.

**Shared with bug.md #9.** Block-scoped `bind` needs the *identical* substrate —
`use` (name-keyed) and `bind` (type-keyed) are the same missing per-block lexical scope
wearing two names. **Look back later** with both in view: build the substrate once (a
per-block lexical scope the Checker pushes/pops, carrying both an imported-name table and
a type-keyed bind table), then land block-level `use`/`uses` and block-level `bind` on top
of it together. Building it twice, independently, risks two incompatible scope mechanisms
that cost more to reconcile than to have designed jointly. Until then: imports are
file-level (this doc, v1) and binds are whatever bug.md #9 lands at file/global grain.

(As it happened: both fixes landed the same day, per §9's resolution note above, ahead
of this feature — so `use` was implemented directly against the full ruling rather than
staying at v1.)

## 10. Implementation notes (2026-07-05) — differential testing found three alias-blind runtime gaps

`use ... as ...` is the **first** feature that lets a name reach the language's runtime
under something other than its own declared spelling (`uses` dumps names unchanged; a
plain top-level global has exactly one name). Verifying `tests/corpus/use.ext` across
`--run`/`--ir`/`--emit-elf` surfaced three spots that had never been alias-tested before
because nothing previously exercised the combination — each is a small, targeted fix, not
a design change, and none touch the scoping model above:

1. **Reading a namespaced value global through an alias returned void.** The evaluator's
   and lowerer's bare-name read path key their storage lookup by the declaration's own
   name (`Eval.cpp`'s `globals_`, `Lower.cpp`'s `mod_->globalIndex` — both populated
   during gather from `item->name`/`v->name`, never from any alias). Reading the alias
   text found nothing and silently produced void/an unresolved-name error rather than the
   real value. **Fix:** the Checker now records the resolved `Var` declaration on the
   `Name` expression (`e->resolved`, previously used only for calls/operators); both
   engines key their lookup by `e->resolved->name` when set, falling back to `e->text`
   otherwise — so the read reaches the *same slot* regardless of which name found it,
   per §4's requirement.
2. **Constructing a class through an alias built the wrong (empty) object.** A bare
   `T(...)` call picks its constructor by matching the call-site text as the ctor
   *label* — correct when `T` is the class's own name, wrong when `T` is a `use ... as`
   alias (the constructor is still labeled with the class's real name). The mismatch
   silently found zero candidates and default-constructed instead of erroring, so
   `Bx(7)` (aliasing `Box`) built a zeroed object instead of running `new Box(int v)`.
   **Fix:** both construction sites in `Checker.cpp` (`typeOfCall`, `typeInitExpr`) now
   use the resolved class symbol's own name as the label, not the call-site text.
3. **A namespaced call through an aliased namespace failed to lower on `--ir` only.**
   `Lower.cpp`'s recognition of "is this call's qualifier a namespace" re-derives the
   answer itself by searching `sema_.global` directly, never seeing a namespace bound
   only in a file's import overlay (bug.md #8's scoping model). **Fixed for the
   file-level case**: the search now starts from `sema_.fileScopeFor(...)`. **The
   block-level case remains broken** — `Lower.cpp` has no path from an `Expr` to its
   enclosing block's `importScope` — and, notably, **reproduces with plain `uses`, no
   `use`/aliasing involved**, so it predates this feature and is out of scope for it.
   Filed as **bug.md #10** rather than fixed here; `tests/corpus/use.ext` exercises
   namespace aliasing at file scope only (fully fixed) and sticks to functions/classes/
   values for its block-scoped example (already correct), so nothing in this feature's
   own test suite depends on the open gap.

None of the three change the scoping/semantics design above; they make the runtime
honor "an alias reads/writes/constructs through the same slot" (§4), which was already
the specified behavior — `uses` just never had a way to expose the gap before `use`
existed.

## 11. Verification pass (2026-07-10) — the §5 attribute/rule follow-up already shipped

Re-opened to finish the one interaction §5 still flagged as "NOT YET IMPLEMENTED": bare
`@Attr` resolution and rule activation for a file whose only import of the attribute's
namespace is a **selective** `use NS::Attr` (rather than a bulk `uses NS`). Probing the
merged tree showed the gap **does not exist** — it was closed by the original `use` commit
(ec01aee) and the §5 note was simply stale:

- **Why it works.** `computeFileImports`'s `StmtKind::Use` case
  ([Project.cpp:359](src/Project.cpp#L359)) inserts the namespace of a selective import
  (`joinPath(generics, size-1)`, i.e. the path minus the imported name) into
  `importsExplicit`, which `computeFileImports` folds into `effective`
  ([Project.cpp:394](src/Project.cpp#L394)). Both `resolveAttr`
  ([Rules.cpp:782](src/Rules.cpp#L782)) and rule activation
  ([Rules.cpp:565](src/Rules.cpp#L565), [:1099](src/Rules.cpp#L1099)) iterate/gate on
  `effective` — so a selective `use` opts the file into the namespace's attributes and
  rules automatically, via the *same* mechanism that already made phantom-dep strictness
  count a `use NS::x` as "this file uses `NS`" (§5, first bullet). No `resolveAttr`/
  `FileImports` change was required; §5's proposed "record a selective-attribute set and
  thread it in" was unnecessary.
- **Empirically confirmed** (oracle + IR + ELF, via three probe projects and then pinned):
  `use Web::Tag` → `@Tag` fires; `use Web::{IController,Route,Registry}` → Web's `reg` rule
  injects into the importing class's constructor; `use Web::other` (a non-attribute name)
  → `@Tag` still resolves, documenting the per-namespace (not per-name) grain.
- **Grain decision (recorded, not a defect).** Resolution is namespace-grained: importing
  any one name from `NS` exposes all of `NS`'s attributes/rules to the file, matching
  `uses NS` and the phantom-dep rule. A per-name attribute set (only the specifically
  imported attribute bare-resolvable) would make metaprogramming reach *stricter* than
  phantom-dep reach — a new asymmetry against §4's "a name is a name". Left intentionally
  un-built; §5 updated to state the shipped behavior instead of the stale "not yet".
- **Tests added.** `tests/corpus/project/attr_scope_use_ok` and `rule_scope_use_ok` — the
  selective-`use` twins of the existing bulk-`uses` `attr_scope_ok`/`rule_scope_ok`, run by
  `corpus_project` across oracle/IR/ELF (and the concat-identity invariant). Docs:
  reference §6.9 (attribute scope) and the rules-scope paragraph now name selective `use`
  alongside `uses`.
- **Genuinely still deferred:** only bind *activation* on a selective import (§5's third
  bullet) — that is `system-binds.md`'s design, not this one's, and stays out of scope.
  With the attribute/rule interaction verified, `imports.md` is complete as shipped.
