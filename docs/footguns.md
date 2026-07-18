# Known footguns & workaround-debt registry

**Read this before writing any Leviathan library or application code.** One row per known
footgun: the construct that breaks, the tracking bug (`known_bugs_*.md`), the sanctioned
workaround, and the **debt sites** currently carrying that workaround so a fix can
un-workaround them.

Maintenance protocol (policy of 2026-07-13, `designs/complete/techdesign-composition-corpus.md` §1):

- **Finding a new footgun?** File the `known_bugs_*.md` entry as usual, then add a row here
  in the same commit, listing every site where you applied the workaround.
- **Fixing bug #N?** Grep this file for `#N`, revert the workaround at each listed debt
  site, delete the row, and promote the red-lane corpus repro to green — all in the fix
  commit. Reverting a workaround for a fixed bug is sanctioned; the row is the audit trail.
- Rows marked **by design** have no bug number — they are permanent language semantics
  that repeatedly surprise track authors. Never "fix" code by fighting them.

## P0 — never build on these constructs (silent state corruption)

_None currently._

## Function values (cluster A)

| construct that breaks | bug | sanctioned workaround | debt sites |
|---|---|---|---|

## Aggregates & narrowing (cluster B)

| construct that breaks | bug | sanctioned workaround | debt sites |
|---|---|---|---|

## Names, generics, overloads (clusters C & D)

| construct that breaks | bug | sanctioned workaround | debt sites |
|---|---|---|---|

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
| A bare class-typed field (`App host_;`) that NO constructor assigns auto-constructs (§3) before any ctor body — a guarded nullary ctor (App's single-app rule) throws, and a type with no nullary ctor (`Document(App)`) has nothing to run | bare + never-ctor-assigned = auto-construct via the nullary path | give it an initializer, or assign it in every ctor, or `T? = None` + narrow on read |
| Positions from a `chars()` scan fed to `subStr`/`indexOfFrom` mangle content after any multibyte scalar (a `§` in a parsed payload shifted every later read — D02 drift corpus, 2026-07-17) | `length()`/`subStr()`/`indexOfFrom()` are BYTE-indexed; `chars()` yields code points — the two index spaces disagree past the first non-ASCII scalar | scan AND slice over the `chars()` array only; build substrings by accumulation (`Sonar::Dom::DomParser`, `DomXParser`) |
