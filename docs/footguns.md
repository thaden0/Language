# Known footguns & workaround-debt registry

**Read this before writing any Leviathan library or application code.** One row per known
footgun: the construct that breaks, the tracking bug (bug.md), the sanctioned workaround,
and the **debt sites** currently carrying that workaround so a fix can un-workaround them.

Maintenance protocol (policy of 2026-07-13, `designs/techdesign-composition-corpus.md` §1):

- **Finding a new footgun?** File the bug.md entry as usual, then add a row here in the
  same commit, listing every site where you applied the workaround.
- **Fixing bug #N?** Grep this file for `#N`, revert the workaround at each listed debt
  site, delete the row, and promote the red-lane corpus repro to green — all in the fix
  commit. Reverting a workaround for a fixed bug is sanctioned; the row is the audit trail.
- Rows marked **by design** have no bug number — they are permanent language semantics
  that repeatedly surprise track authors. Never "fix" code by fighting them.

## P0 — never build on these constructs (silent state corruption)

_None currently._ (#74 — repeated boxed `Array<Struct>.add(...)` corrupting the
LLVM-native allocator — was FIXED 2026-07-15: boxed `.at()` now deep-copies a
value-struct element instead of returning a bare alias the caller then VFree'd.
Regression pin: `tests/corpus/composition/aggregates/green/struct_array_append_corruption.lev`.
The `packages/atlantis/tests/corpus/serialization` native leg is unblocked.)

## Function values (cluster A)

| construct that breaks | bug | sanctioned workaround | debt sites |
|---|---|---|---|

## Aggregates & narrowing (cluster B)

| construct that breaks | bug | sanctioned workaround | debt sites |
|---|---|---|---|

## Names, generics, overloads (clusters C & D)

| construct that breaks | bug | sanctioned workaround | debt sites |
|---|---|---|---|
| _(cluster clear — #77 struct default `==` field-wise and #78 uses-import local shadow both FIXED 2026-07-15)_ | | | |

## By design — permanent semantics that read like bugs

| surprise | rule | idiom |
|---|---|---|
| An `Array`/`Map` parameter can't accumulate for the caller — `errs = errs.add(x)` rebinds a local copy | collections are pure values; parameters copy | a reference-class collector whose `add` mutates the shared instance (`Atlantis::Config::Errors`) |
| Flow narrowing doesn't survive an early `return`/`continue` | narrowing is branch-scoped | use `else` arms for every optional guard |
| Writes to a namespace global spelled `NS::x = …` inside `NS` don't lower | qualified-write lowering gap | bare `x = …` inside the namespace |
| Structs snapshot `this` by value inside closures — later mutations invisible | a struct IS its fields; copy semantics | use a `class` for anything a closure must observe |
| Prelude-only: no `T?` flow-narrowing (LLVM misreads), no checker annotations, eager-global-instance on emit-C++, interp buffers stdout | the checker never runs on the prelude | `-1`-sentinel int helpers; data-holder global + free fns; fixed delays in test drivers |
| `match @Attr on class C` does not match `struct` declarations (subject-kind is exact) | filed as ergonomics ask, §4 of `designs/requests/request-metaprog-splices.md` | ship explicit `…Class`/`…Struct` rule pairs |
| A `namespace`-scoped global's initializer sees other globals at their AUTO-CONSTRUCTED default, not their top-level explicit value | namespace-scoped initializers run at startup (before the top-level statement sequence); a top-level global's explicit `= …` is a body statement that runs after — every global is default-constructed first, so the init sees `[]`/`0`/`""`, never an absent slot (mutating one works: the append persists) | to seed a namespace global FROM a value, use another namespace-scoped global/`const` as the source, or compute it in the initializer itself |
| A bare class-typed field (`App host_;`) that NO constructor assigns auto-constructs (§3) before any ctor body — a guarded nullary ctor (App's single-app rule) throws, and a type with no nullary ctor (`Document(App)`) has nothing to run | bare + never-ctor-assigned = auto-construct via the nullary path | give it an initializer, or assign it in every ctor (see next row), or `T? = None` + narrow on read |
| ~~A bare class field a ctor DOES assign (`App app; new W(App a){ app = a; }`) still auto-constructs a throwaway first~~ FIXED | a bare field that every ctor definite-first-assigns (a plain top-level `f = rhs;` before any read) now skips §3 auto-construction and takes the plain default; the ctor supplies the value | just write `App app; new W(App a){ app = a; }` — no `T? = None` dance needed for the ctor-supplied case |
