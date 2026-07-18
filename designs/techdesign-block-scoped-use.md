# Block-scoped `use`/`uses` — resolving the deferred per-block symbol-scope substrate — Technical Design

**Status:** ACTIVE — promoted out of deferral 2026-07-17 (was `deferal-block-scoped-use.md`).
Premise re-verified against master that day: the substrate is **still unbuilt** — `Stmt::importScope`
and the Checker's `bindScopes_` remain two separate mechanisms, `blockScope` does not exist, and the
Lowerer still carries its private `blockImportScopes_`. System-binds Channel 1 (`use NS::T;`
bind-activation, landed 2026-07-14) was built **on top of the split mechanisms** as a Checker-only
scope-table fact — it joins §4.2's behavior-freeze set: the migration must keep it observationally
identical. All `file:line` references below are affa8a4-era (Jul 6) and have drifted; anchors
(function/field names) hold — re-ground them at implementation start. §7's dates are stale;
re-plan the milestones at pickup, the design itself is current. **Date:** 2026-07-06 (design);
2026-07-17 (promoted).
**Resolves:** the logged deferral of block-level `use`/`uses` scoping and, with it, the
**shared per-block lexical-scope substrate** that deferral named as the missing
dependency (imports.md §2 staging note and §9; system-binds.md §7.2).
**Related:** `designs/complete/imports.md` (the feature this substrate carries),
`designs/system-binds.md` (the type-keyed sibling, `bind` — the substrate's second
tenant, per old-era bug.md #9), `designs/complete/const.md` (voice/format precedent;
no semantic interaction — an alias is the same slot, constness rides along).

Everything below was verified against this tree (master @ `affa8a4`, Jul 6) by reading
source; file/line pointers are to the current working copy. Backend ground rules apply
throughout: **LLVM is the primary backend; X64Gen/ELF is frozen** (no substrate work
lands there); **all new test sources are `.lev`, never `.ext`**; no new git branches
(three-branch rule).

---

## 0. Summary

The import-scoping work (imports.md) shipped with block-level `use`/`uses` **staged out
of v1 by owner decision**, because at implementation time the compiler had **no
per-block symbol scope anywhere, for anything**. The deferral was logged in three
places, with one named remedy — a *shared* per-block lexical-scope substrate:

1. **imports.md §2, staging note** (designs/complete/imports.md:90-91): *"This section
   originally staged the ruling's 'anywhere' half as deferred, for want of a per-block
   symbol scope."*
2. **imports.md §9, the Finding** (designs/complete/imports.md:343): *"Finding
   (2026-07-05, verified by reading the compiler; **deferred by owner decision**)"* —
   closing with the substrate spec: *"Genuine block-level scoping needs a new
   symbol-keyed, push/pop-per-block scope threaded through `Checker::check` alongside
   `env_`, reconciled with `scope_->lookup` so shadowing order (locals ▸ block imports ▸
   outer imports ▸ bulk `uses`) resolves correctly. **That is the deferred substrate.**"*
   (imports.md:370-372), and the shared-substrate directive: *"build the substrate once
   (a per-block lexical scope the Checker pushes/pops, carrying both an imported-name
   table and a type-keyed bind table), then land block-level `use`/`uses` and block-level
   `bind` on top of it together"* (imports.md:377-380).
3. **system-binds.md §7.2** (designs/system-binds.md:216-221), still open in the current
   tree: *"imports.md v1 is file-level only (its §9: the compiler has no per-block symbol
   scope); block-level `use` is deferred to a shared substrate that bug #9's block-scoped
   `bind` needs identically. **Coordination point:** that per-block lexical scope should
   be built **once**, carrying both an imported-name table and a type-keyed bind table."*

This document resolves the deferral by **designing that substrate** — one per-block
lexical scope object with a uniform push/pop lifecycle, carrying the imported-name table
and the type-keyed bind table together — and then showing block-level `use`/`uses`
riding on it. §1 records, honestly, how much of the substrate's *behavior* already
landed piecemeal (two same-day bug fixes built feature-private approximations; block
`use`/`uses` does function today), and exactly what the deferral's named remedy still
lacks: **the shared, unified object with one lifecycle** rather than four parallel
mechanisms that each consumer rebuilds by hand. The design below is therefore part
consolidation (fold what exists into the specified shape) and part completion (the
invariants, contracts, and diagnostics none of the partial mechanisms own).

---

## 1. Current state (verified 2026-07-06 — what exists, and what the deferral still lacks)

### 1.1 What landed after the deferral was logged

The same day the deferral was recorded, two bug fixes built *feature-private* per-block
mechanisms (imports.md §9's resolution note; commits `1192486` bug.md #8, `3722ded`
old-era bug.md #9), and a later fix extended one of them into the lowerer (commit
`d51366a`, old-era numbering rolled over — the lowering gap imports.md filed as "bug.md
#10" was fixed under the new-era "bug.md #1"; the *current* bug.md's #10 is an unrelated
div-by-zero entry). Concretely, in today's tree:

- **`Stmt::importScope`** ([Ast.hpp:259-264](src/Ast.hpp#L259)) — a per-block,
  name-keyed overlay `Scope`, created **lazily** by the Resolver's
  `resolveStmtTypes` Block case only when the block *textually contains* a `use`/`uses`
  (`hasImports` gate, [Resolver.cpp:1585](src/Resolver.cpp#L1585); populated
  `use`-before-`uses`, [Resolver.cpp:1587-1592](src/Resolver.cpp#L1587)).
- **The Checker consults it** by *replacing* its scope pointer for the block's duration:
  `if (s->importScope) scope_ = s->importScope;` with a saved/restored `scope_`
  ([Checker.cpp:1433-1440](src/Checker.cpp#L1433)).
- **The Lowerer carries its own parallel stack**, `blockImportScopes_`
  ([Lower.hpp:80-84](src/Lower.hpp#L80)), pushed/popped in its own Block case
  ([Lower.cpp:509-517](src/Lower.cpp#L509)) and searched by `namespaceSym`
  ([Lower.cpp:862-880](src/Lower.cpp#L862)) before the file overlay.
- **`bindScopes_`** ([Checker.cpp:1030-1055](src/Checker.cpp#L1030)) — a Checker-only,
  **type-keyed** stack for `bind` (old-era bug.md #9), pushed for *every* block
  ([Checker.cpp:1436](src/Checker.cpp#L1436)), every namespace body
  ([Checker.cpp:1724](src/Checker.cpp#L1724)), and once for the whole program's
  top level ([Checker.cpp:1805](src/Checker.cpp#L1805)) — invisible to the Resolver
  and the Lowerer.
- **`env_`** ([Checker.cpp:180-191](src/Checker.cpp#L180)) — the third per-block stack:
  locals as `name → {Type, isConst}`, not symbols.
- **No other engine touches any of these.** Grepped: zero `importScope` references in
  `Eval.cpp`, `IrInterp.cpp`, `CGen.cpp`, `LlvmGen.cpp`, `Rules.cpp` — they consume
  Checker-resolved declaration pointers (`e->resolved`, imports.md §10) or the IR.

So the *user-visible feature* — a `use`/`uses` inside a block, scoped to that block —
**works today**, end-to-end, and is corpus-tested (`tests/corpus/block_uses.ext`,
`tests/corpus/use.ext`).

### 1.2 What the deferral's named remedy still lacks

The deferral did not merely ask for block-level `use` to function; it named a **shared
substrate, built once** (§0's citations). Measured against that spec, today's tree has:

1. **Two substrates, not one.** Names live in `Stmt::importScope`; binds live in
   `bindScopes_`. The one object system-binds.md §7.2 specifies — *"carrying both an
   imported-name table and a type-keyed bind table"* — does not exist. imports.md §9's
   resolution note concedes this openly: *"They ended up as two distinct mechanisms, not
   the single unified structure this section's closing paragraph anticipated."*
2. **No uniform lifecycle.** The Checker's Block case performs **four** independent
   push/pop-shaped operations that must stay in lockstep by hand: save/restore `scope_`,
   `env_.emplace_back/pop_back`, `pushBindScope/popBindScope`
   ([Checker.cpp:1429-1441](src/Checker.cpp#L1429)) — and the Lowerer repeats a fifth,
   privately ([Lower.cpp:511-515](src/Lower.cpp#L511)). Every future consumer of block
   scoping (the metaprogramming engine, a future borrow checker, `--imports`-style
   tooling wanting block rows) must re-implement the walk.
3. **A fragile materialization invariant.** `s->importScope` is only *assigned* when the
   block currently has imports ([Resolver.cpp:1585-1592](src/Resolver.cpp#L1585)); it is
   never *reset*. Under the two-pass pipeline (pass-1 Resolver → rule stage → fresh
   pass-2 Resolver on the folded tree, [main.cpp:312-342](src/main.cpp#L312)), a rule
   that removed a block's only import between passes would leave `importScope` pointing
   at a **pass-1 scope chain** holding pass-1 `Symbol*` objects — stale, not dangling
   (both Resolvers outlive checking), and therefore silently wrong rather than loudly
   wrong. No current fold path provably produces this (comptime-if branches are skipped
   in pass 1 entirely, [Resolver.cpp:1607-1612](src/Resolver.cpp#L1607), so their blocks
   were never assigned), but the invariant is owned by nobody and one rule-engine change
   away from live.
4. **Two authorities for the lexical chain.** The Checker replaces `scope_` with a
   Resolver-built chain (`scope_ = s->importScope`), trusting that the chain the
   Resolver parented at resolve time matches the chain the Checker would otherwise be
   holding. They can diverge: for a generic method, the Resolver parents body scopes
   under the method's type-param scope ([Resolver.cpp:1649-1657](src/Resolver.cpp#L1649))
   while the Checker sets `scope_` to the class scope ([Checker.cpp:1751](src/Checker.cpp#L1751)).
   Today the divergence is benign (the type-param scope's parent is the class scope);
   it is the *pattern* that is fragile.
5. **Bind's file grain never got the bug-#8 treatment.** Top-level binds are pushed
   **program-wide** (`pushBindScope(program.items)`, [Checker.cpp:1805](src/Checker.cpp#L1805))
   — the exact cross-file-leak shape bug #8 fixed for `uses` (imports.md §1 probe C).
   Out of scope to *change* here (system-binds.md owns bind-activation semantics), but
   the unified substrate must leave the per-file slot ready (§4.3).

**This design's job:** keep §1.1's working behavior bit-for-bit, and retire §1.2's five
gaps by building the substrate as originally specified.

---

## 2. Why it was deferred (the record)

Restated from imports.md §9's Finding, verified against the pre-fix tree it describes:

- The Resolver's `Scope` chain branched only at global, namespace, class, and
  generic-method type-param scopes; `resolveStmtTypes` walked into blocks but created
  no scope for them.
- The Checker held a single `scope_` set once per function/method and never repositioned
  inside a body; locals lived (and still live) in the separate `env_` stack, which
  stores types, not symbols.
- `bind` proved the absence: its checking ran as an ordinary statement of the enclosing
  function, with no per-block registry to hang a binding on.

File-level scoping was **separable and cheaper** — file boundaries are known at gather
time, so per-file overlay scopes (`Sema::fileScopes`,
[Symbols.hpp:94-110](src/Symbols.hpp#L94)) plus file-origin selection of the resolution
scope delivered the ruling's file half without any per-statement scope stack. The owner
decision was to ship that as v1 and stage the block half behind the substrate — with the
explicit look-back instruction to **build the substrate once, shared with block-scoped
`bind`**, precisely to avoid "two incompatible, mutually-unaware scope mechanisms"
(imports.md §9). Two mechanisms is what the same-day bug fixes then delivered anyway
(aware of each other, so compatible — but not shared). This design is the look-back.

---

## 3. Resolution design — the per-block lexical-scope substrate

### 3.1 Data structure: one object, both tables

Extend the existing `Scope` ([Symbols.hpp:69-86](src/Symbols.hpp#L69)) rather than
introducing a parallel class — every consumer already speaks `Scope*`, and the file
overlays, namespace scopes, and class scopes stay the same type they are today:

```cpp
struct Scope {
    Scope* parent = nullptr;
    std::unordered_map<std::string_view, std::vector<Symbol*>> names;   // unchanged

    // NEW — the type-keyed bind table (system-binds.md §7.2's second half).
    // Key: the bound type's canonical string; value: the factory `bind` stmt.
    // Empty for scopes that carry no binds (file overlays today, most blocks).
    std::unordered_map<std::string, const Stmt*> binds;

    // localLookup / lookup — unchanged (front-of-vector shadowing preserved).
    const Stmt* localBind(const std::string& canonical) const;   // NEW, this scope only
};
```

- **One arena, one lifetime.** Scopes stay `Sema`-owned (`Sema::newScope`,
  [Symbols.hpp:118-123](src/Symbols.hpp#L118)) — compilation-long, no per-block
  allocation churn beyond what `importScope` already costs, no ownership questions.
- **`Stmt::importScope` is renamed `Stmt::blockScope`** and its comment rewritten: it is
  no longer "the import overlay" but *the block's lexical scope* — names **and** binds.
  (Mechanical rename across the three current consumers: Resolver, Checker, Lower.)
- **Laziness is kept** — a block gets a `blockScope` only if it contains at least one
  `use`, `uses`, or factory `bind` as a direct child. The common block allocates
  nothing, exactly as today. What changes is that laziness becomes an *implementation
  detail behind a uniform API* (§3.4), not a fact every consumer branches on.

**Deliberately not folded in (v1): `env_`.** Locals are typed slots with narrowing
state and const flags, not `Symbol`s; merging them into `Scope` drags
`invalidatePath`/flow-narrowing ([Checker.cpp:193+](src/Checker.cpp#L193)) into a
refactor this deferral does not require. The substrate's lookup **contract** (§3.3)
pins locals-win ordering; the *storage* stays `env_`. Folding locals in later is a
compatible extension (they would become front-inserted `Var` symbols in the block's
`names`), noted as future work, not scheduled.

### 3.2 Lifecycle: materialize in the Resolver, one push/pop everywhere else

**Materialization (Resolver, `resolveStmtTypes` Block case — replaces
[Resolver.cpp:1579-1595](src/Resolver.cpp#L1579)):**

```
case StmtKind::Block: {
    s->blockScope = nullptr;                        // (a) unconditional reset — §5 P3
    Scope* inner = scope;
    if (hasImports(s->body) || hasFactoryBinds(s->body)) {
        inner = sema_.newScope(scope);
        for (c : s->body) if (c->kind == Use)        useOne(c, inner);    // use first —
        for (c : s->body) if (c->kind == UsesImport) importOne(c, inner); // §3.3 order
        s->blockScope = inner;
    }
    for (c : s->body) resolveStmtTypes(c, inner);   // children resolve THROUGH it
    if (s->blockScope) fillBinds(s->body, s->blockScope);   // (b) after children — see below
    break;
}
```

- **(a) Unconditional reset** closes §1.2.3: pass 2 can never inherit a pass-1 scope.
  One line; owner of the invariant is the substrate, not luck.
- **(b) Bind-table filling runs *after* the child walk** because a bind's key is its
  bound type's **canonical** string, and `resolveStmtTypes`' Bind case is what resolves
  that type ([Resolver.cpp:1640-1643](src/Resolver.cpp#L1640)). `fillBinds` records
  each direct-child factory `bind` (`s->type` present — the factory form, same filter
  `pushBindScope` applies today, [Checker.cpp:1033](src/Checker.cpp#L1033)) into
  `blockScope->binds`, reporting **duplicate-in-scope** with today's exact message text
  (*"duplicate binding for 'K' in this scope"*, [Checker.cpp:1038](src/Checker.cpp#L1038))
  — the check moves from Checker to Resolver, where the substrate lives. Same pass
  ordering fills namespace scopes' and file overlays'/global's bind tables in
  `processImports`-adjacent walks (top level and namespace bodies), replacing the
  Checker's three `pushBindScope` sites one-for-one at *identical grain* (program-wide
  top level included — §4.3; changing that grain is system-binds.md's call, not ours).

**Traversal (Checker, Lower, future consumers) — one guard:**

```cpp
// Shared header (Symbols.hpp or a small LexicalStack.hpp) — RAII, misuse-proof:
struct BlockScopeGuard {
    BlockScopeGuard(LexicalStack& lx, const Stmt* block);  // pushes iff block->blockScope
    ~BlockScopeGuard();                                    // pops iff it pushed
};
```

`LexicalStack` owns the current `Scope*` chain position plus the bind-lookup walk:
`lookupName`, `collectOverloads` (front-to-back across the whole chain — preserving
[Checker.cpp:1012-1021](src/Checker.cpp#L1012)'s cross-scope overload merge),
`lookupBind` (nearest-wins across the chain's `binds` tables, replacing
[Checker.cpp:1049-1055](src/Checker.cpp#L1049)). The Checker's Block case shrinks to
one guard plus `env_` (which stays until locals fold in); `bindScopes_`,
`pushBindScope`, `popBindScope`, and the Lowerer's private `blockImportScopes_` are
**deleted**, their consumers repointed at the stack. The Checker stops *replacing*
`scope_` with a Resolver-era chain (§1.2.4): the stack layers `blockScope` **onto the
Checker's own current position** — the chain has one authority at any moment, the
walker holding the stack. (Resolve-time parenting of `blockScope` remains what makes
*type references inside the block* see the imports during pass-N resolution — both
mechanisms coexist, each used by the pass that owns it.)

**Object-install binds (`bind expr;`)** stay on their existing separately-staged Checker
path ([Checker.cpp:1029](src/Checker.cpp#L1029) comment) — they are expression-typed and
Checker-discovered; nothing about them moves in v1.

### 3.3 The lookup contract (pinned)

For a bare name at any point in a body, resolution order is:

1. **Locals/params** — `env_`, innermost frame outward ([Checker.cpp:180-186](src/Checker.cpp#L180)). Locals shadow everything.
2. **Enclosing block scopes, innermost outward** — each consulted via `localLookup`;
   within one scope, a selective `use` beats a bulk `uses` for the *same name* because
   `use` populates first and `Scope::lookup` returns `.front()`
   ([Resolver.cpp:1446-1452](src/Resolver.cpp#L1446), [Symbols.hpp:79-85](src/Symbols.hpp#L79))
   — "specific beats bulk," imports.md §4.
3. **The enclosing class scope / generic type-param scope** (methods), then
4. **The file overlay** (`fileScopeFor`) — the file's own top-level `use`/`uses`, then
5. **Global** — gathered declarations plus the implicit `uses std`
   ([Resolver.cpp:1885-1890](src/Resolver.cpp#L1885)), the outermost and most
   shadowable layer.

**Across levels, nearest wins — unconditionally.** An inner bulk `uses M` that dumps
`x` shadows an outer selective `use N::x`. The deferral sentence's ordering ("locals ▸
block imports ▸ outer imports ▸ bulk `uses`", imports.md §9) is hereby pinned as
describing the *within-one-scope* specific-beats-bulk rule composed with nearest-wins —
**not** a global fourth axis where bulk always loses. Rationale: nearest-wins is the
language's one shadowing law (imports.md §4: "nearer scopes shadow outer ones as
always"; `bind`'s nearest-wins, §12.5), and a cross-scope bulk-loses rule would make
resolution non-local (you could not read a block and know its names without classifying
every enclosing import by kind). This matches what the current code already does;
flagged for owner eyes in §7's M1 gate because it *interprets* a design sentence rather
than quoting one.

**Exception, pinned as-is:** function **overload collection** merges across all levels
rather than stopping at the nearest ([Checker.cpp:1012-1021](src/Checker.cpp#L1012)) —
overload sets travel and merge (imports.md §4); shadowing applies to name *binding*,
not to set *membership*. The substrate's `collectOverloads` preserves this asymmetry
and pins it with a test (§5 P4 has the failure mode this prevents).

**Binds:** `lookupBind` walks the same chain, nearest-wins, one winner per type per
scope (duplicate = error at registration, §3.2) — unchanged semantics, new home.

### 3.4 Block-level `use`/`uses` riding on the substrate

With §3.1-§3.3 in place, block-level `use`/`uses` is a *tenant*, not a feature build:

- **Parse** — unchanged; both keywords already parse at statement position
  (imports.md §6.1).
- **Resolve** — `useOne`/`importOne` populate the block's `blockScope` exactly as they
  populate a file overlay ([Resolver.cpp:1390-1424](src/Resolver.cpp#L1390),
  [:1370-1384](src/Resolver.cpp#L1370)); the only change from today is the target
  field's name and the reset/fill ordering of §3.2. Diagnostics (unknown namespace /
  unknown name) stay Resolver-owned and textually identical.
- **Check** — names in the block resolve through the `LexicalStack`; confinement
  (no leak to enclosing scope or sibling block) falls out of pop.
- **Lower** — `namespaceSym`'s overlay search ([Lower.cpp:868-880](src/Lower.cpp#L868))
  reads the shared stack instead of its private one; behavior identical (that path is
  already corpus-covered since commit `d51366a`).
- **Eval / IrInterp / LlvmGen / CGen / X64Gen** — zero changes. The first four consume
  Checker-resolved decls or IR (verified, §1.1); X64Gen is frozen.
- **Semantics inherited wholesale from imports.md §4:** alias = same slot; every
  declaration kind imports uniformly; overload sets travel whole; idempotent
  re-import; `use` shadows same-scope `uses`.

---

## 4. Dependencies and shared-substrate coordination (bug #9 / system-binds)

4.1 **Old-era bug.md #9 (bind/inject DI core)** — fixed (`3722ded`); its block-scoped
`bind` is exactly the `bindScopes_` mechanism this design folds into the substrate.
The coordination point the deferral recorded — *build it once, both tables* — is
satisfied by §3.1/§3.2: **`bind` and `use` become tenants of one object**, which is
what system-binds.md §7.2 gates on. This design therefore *unblocks* system-binds.md's
staging item 2 ("scoping a bind or import to an inner block waits on the shared
substrate") at full generality: after M2, both features scope to any block through the
same chain, and system-binds' channel-activation work (its §5's wall-crossing ruling,
imports.md §5's two NOT-YET-IMPLEMENTED follow-ups) can target one lookup path instead
of two.

4.2 **Behavior freeze during migration.** Everything in §1.1 is corpus-pinned
(`block_uses`, `use`, the bind/inject checkertests, `use_scoped_ok`/`use_leak_err`
project tests). The migration must be **observationally identical** — same programs
accepted, same diagnostics byte-for-byte, same runtime output on all actively
maintained engines. Any intentional behavior delta is out of scope and a STOP (§7).

4.3 **Bind's file grain — flagged, not changed.** Top-level binds remain program-wide
([Checker.cpp:1805](src/Checker.cpp#L1805)) even though top-level imports are per-file.
The substrate makes the per-file fix one line when system-binds.md wants it (fill
`fileScopes[i]->binds` instead of a program-wide table), but *whether* file-grain binds
are the ruling is that design's call. Recorded here so the two tracks don't collide:
**this design owns the tables; system-binds.md owns bind activation semantics.**

4.4 **File ownership (parallel-track hygiene).** This design touches:
`src/Symbols.hpp`, `src/Ast.hpp` (one field rename), `src/Resolver.{hpp,cpp}`,
`src/Checker.{hpp,cpp}`, `src/Lower.{hpp,cpp}`, `tests/` (corpus + unit + runner
globs). It does **not** touch `Eval.cpp`, `IrInterp.cpp`, `LlvmGen.cpp`, `CGen.cpp`,
`Rules.cpp`, `Project.cpp`, or anything frozen. Track 03 is STOPped on an unrelated
statics ruling; no live track owns these files per `designs/complete/techdesign-00-overview.md`
— re-verify at implementation start.

---

## 5. Potential problems

**P1 — Shadowing an outer import, and the "outer imports ▸ bulk uses" ambiguity.**
An inner `uses M` dumping `x` shadows an outer `use N::x`. If the owner intended the
deferral sentence literally (bulk always loses, even across scopes), the pinned
contract in §3.3 is wrong — and the two readings differ silently, on working programs.
Adjacent trap: a `use`-bound name in a block shadows a *local* of an enclosing scope in
the reader's mental model, but never in the compiler's (locals are `env_` and always
win) — surprising when the block import names something an outer local also names.

**P2 — Scope-exit cleanup and stack desynchronization.** Today's Block case performs
four paired operations plus the Lowerer's fifth; nothing enforces pairing. A future
early `return`/`goto`-shaped edit inside any of these cases desynchronizes a stack and
the failure is downstream misresolution, not a crash. The migration itself is the
highest-risk moment: while `bindScopes_` and the substrate coexist mid-refactor,
lookups could consult the wrong one.

**P3 — Two-pass (comptime/macro) resolution ordering.** Three sub-risks: (a) the stale
`importScope` pointer of §1.2.3 when a fold removes a block's only import between
passes; (b) an import *spliced into* a block by a macro must be seen by pass 2's
materialization (it is — pass 2 re-runs `resolveStmtTypes` on the folded tree, and the
rule engine already recomputes imports post-fold for its own map,
[main.cpp:328-331](src/main.cpp#L328) — but nothing pins it); (c) a `use` inside a
comptime-if branch is skipped by pass 1 entirely ([Resolver.cpp:1607-1612](src/Resolver.cpp#L1607)),
so any pass-1 consumer of block scopes (none today) would see a hole.

**P4 — Overload merge vs. shadowing asymmetry.** `collectOverloads` walks all levels;
`lookup` stops at the nearest. If the substrate accidentally "fixes" the asymmetry in
either direction, real programs break: stop-at-nearest loses cross-scope overload
merging (imports.md §4's pinned behavior); merge-for-everything makes an inner `use`
unable to shadow an outer same-named value.

**P5 — Out-of-scope `use` diagnostics.** A name imported in a block and used outside it
errors as bare *"unknown name 'x'"* / *"unknown function 'f'"*
([Checker.cpp:423](src/Checker.cpp#L423), [:775](src/Checker.cpp#L775)) — no hint that
the name exists one block over, imported but confined. This is the silent-failure
*family* (loud, but unexplained) that bug #8 was filed over; block confinement makes it
common enough to deserve a pointed message.

**P6 — Harness cannot run `.lev` corpus files.** New test sources must be `.lev` (hard
rule), but every corpus runner globs `*.ext` only (verified:
`tests/run_native_llvm.sh:17`, `tests/run_native.sh:9`, et al.). Writing the new tests
as `.ext` is forbidden; writing them as `.lev` without runner support silently skips
them — worse than no tests.

**P7 — Rename fallout.** `importScope → blockScope` is mechanical but crosses three
compilands plus comments/design docs that name the old field (imports.md, Lower.hpp's
comment). A partial rename leaves the codebase describing a substrate that doesn't
match the code — the exact documentation drift this deferral file exists to prevent.

---

## 6. Recommended solutions

**S1 (for P1).** Pin nearest-wins-then-specific-beats-bulk in the contract (§3.3), with
the rationale written down, and make M1's exit gate include an **owner-visible note**
quoting the pinned rule (one paragraph in the PR/commit message referencing this §).
Add two corpus programs that *disambiguate the readings*: inner-bulk-vs-outer-selective
(asserts inner wins) and outer-local-vs-inner-block-import (asserts the local wins).
If the owner overrules, the change is localized to `LexicalStack::lookupName`'s walk
order — the contract is one function.

**S2 (for P2).** RAII everywhere: `BlockScopeGuard` (§3.2) is the only way to enter a
block scope — constructor pushes, destructor pops, no manual call sites survive the
migration. Sequence the refactor so exactly one mechanism is live per commit: (i) land
`Scope::binds` + Resolver fill + reset, with the Checker still reading `bindScopes_`
(now populated *from* the prebuilt tables — a shim); (ii) repoint lookups at the
`LexicalStack`; (iii) delete the old stacks in the same commit that removes their last
reader, so a desync is a compile error, not a behavior. Full five-engine corpus run
between each step.

**S3 (for P3).** (a) The unconditional `s->blockScope = nullptr` reset (§3.2(a)) —
one line, closes the staleness class forever, cheap enough to be unconditional.
(b) Pin macro-spliced block imports with a meta test: a macro that splices `use M::f;`
into a block, asserting `f()` resolves in-block and *not* after it
(`tests/test_meta.cpp` already exercises post-fold resolution; add the block case).
(c) Document (in the Block case comment) that pass-1 block scopes are advisory and
pass-2's are authoritative; no pass-1 consumer exists, and the reset makes the
authority switch mechanical.

**S4 (for P4).** Encode the asymmetry as two named `LexicalStack` methods with
contract comments — `lookupName` (nearest-wins, `.front()`) and `collectOverloads`
(walk-all, merge) — ported verbatim from today's logic, plus a regression test:
an inner block `use A::f;` where `f` overloads also exist at file level, asserting the
call still sees the merged set; and an inner `use A::x;` (value) asserting it shadows a
file-level `x`.

**S5 (for P5).** On unknown-name/unknown-function, an **error-path-only** secondary
scan: walk the current function's block tree (and the file's other top-level blocks)
for a `blockScope` whose `names` holds the failing identifier; if found, append a note —
`note: 'helper' is imported by a 'use' at line N, but that import is scoped to its
enclosing block (imports are lexical — imports.md §2)`. Zero cost on the happy path;
checkertest pins the note's presence and its absence for genuinely unknown names.

**S6 (for P6).** Extend the corpus runners' globs to `*.ext *.lev` (runner-script
change, not `src/`), keeping every existing `.ext` file untouched; new tests land as
`.lev` (`tests/corpus/block_use_shadowing.lev`, `block_use_out_of_scope_err.lev`,
`block_bind_shared_scope.lev`, plus the S1/S3/S4 cases). If any runner turns out to
resist the two-glob change (e.g., name-derivation assumptions beyond `basename .ext`),
**STOP** and escalate rather than shipping `.ext` — per the hard rule.

**S7 (for P7).** Do the rename with a repo-wide grep gate in the same commit
(`grep -rn importScope src/ tests/` must return zero) and update the two source
comments that describe the old shape ([Ast.hpp:259-263](src/Ast.hpp#L259),
[Lower.hpp:80-83](src/Lower.hpp#L80)). Design-doc cross-references (imports.md §9,
system-binds.md §7.2) are updated by the implementation agent *after* code lands, as a
docs-only follow-up commit — this document stays the authoritative record of the
mapping old-name → new-name.

---

## 7. Milestones, timeline, STOP conditions

Single implementation agent, existing branch only (three-branch rule), push to master
per the guardrail when each milestone's gate is green. Dates assume start 2026-07-07.

- **M1 — Substrate lands, behavior frozen (2026-07-07).**
  `Scope::binds` + `localBind`; `importScope → blockScope` rename (S7 grep gate);
  Resolver: unconditional reset, `hasFactoryBinds`, `fillBinds` post-child-walk with
  the duplicate-bind error moved (identical text); Checker shim per S2(i).
  **Gate:** full test suite + five-engine corpus byte-identical to pre-M1; the S1
  owner-visible note written. **STOP if:** any existing diagnostic changes text or
  position; any corpus output diverges on any actively-maintained engine; moving
  duplicate-bind detection surfaces an ordering difference the shim can't hide.
- **M2 — One lifecycle (2026-07-08).** `LexicalStack` + `BlockScopeGuard`; Checker's
  Block/namespace/top-level sites and `lookupBind`/`functionOverloads` repointed;
  `bindScopes_`, `pushBindScope`/`popBindScope`, `blockImportScopes_` deleted (S2(iii),
  same-commit rule). **Gate:** suite + corpus green; `grep -rn "bindScopes_\|blockImportScopes_" src/`
  returns zero. **STOP if:** the scope\_-replacement removal (§3.2) changes resolution
  for any existing test — that means a real divergence between Resolver-time and
  Checker-time chains (§1.2.4) was load-bearing; file it in /bug.md with the repro and
  halt rather than papering over it.
- **M3 — Tests + diagnostics (2026-07-09).** Runner globs extended (S6); new `.lev`
  corpus/checkertests for S1, S3(b), S4, S5; the out-of-scope note implemented
  (error-path only). **Gate:** new tests pass on `--run`, `--ir`, emit-C++, LLVM
  (`--emit-elf` excluded — frozen; do not extend X64Gen even if a block-scope test
  happens to fail there; record any such failure as a P3.2 bug.md entry instead).
  **STOP if:** the harness can't take `.lev` after the two-glob change (S6's escalation);
  never fall back to `.ext`.
- **M4 — Docs follow-up (2026-07-10).** imports.md §9 and system-binds.md §7.2 gain
  pointers to this document as the deferral's resolution; source comments verified per
  S7; this file moves to `designs/complete/` once the owner accepts the S1 pin.
  **Gate:** owner sign-off on §3.3's pinned ordering (the one interpretive call this
  design makes).

**Global STOP conditions** (any milestone): a change would require touching frozen
X64Gen/ELF to keep a gate green; a change would require a new git branch; an
existing-behavior delta is discovered that this doc classed as impossible (§4.2's
freeze) — in every case, stop, commit nothing further, record the finding in /bug.md
with repro + ruling request, and escalate.
