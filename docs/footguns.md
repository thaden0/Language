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

## Multi-mixin composition & Surface painting (cluster E, Sonar T05)

| construct that breaks | bug | sanctioned workaround | debt sites |
|---|---|---|---|
| A leaf's in-place content update to a SHORTER string leaves stale glyphs under the default/empty theme (repaint doesn't clear the box; `paintBackground` fills only for a themed non-default bg) | sonar-bugs.md #4 | pad mutable fields to a fixed width so each repaint fully overwrites | `sonar/examples/file-manager` (`showFile` pad) |

## Names, generics, overloads (clusters C & D)

| construct that breaks | bug | sanctioned workaround | debt sites |
|---|---|---|---|
| A `struct` with no explicit `(==)` compares unequal to a field-identical instance (not field-wise by default) | #77 | define an explicit `(==)` on the struct, or compare field-by-field | `harpoon/src/assert.lev`'s `assertEqual<T>` doc comment |
| A bulk `uses NS;` import's function silently wins over a same-named top-level function declared in the importing file itself (no ambiguity error) | #78 | never give a file's own top-level function the same bare name as anything reachable via its `uses` imports; call the imported form qualified when in doubt | `harpoon/tests/assertions/main.lev` (self-test entry renamed `runSelfTest`, never `main`, since `harpoon::main()` exists) |

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
| A bare class-typed field (`App host_;`) auto-constructs (§3) before the ctor body — a guarded ctor (App's single-app rule) throws, and a type with no nullary ctor (`Document(App)`) won't compile | bare declaration = auto-construct; the field default runs the nullary path | make it `T? = None`, set it in the ctor, narrow on read (Sonar DOM `Document.host_`/`SonarApp.doc_`) |
