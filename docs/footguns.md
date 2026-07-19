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
| A relational compare (`</<=/>/>=`) of a `None`-valued `T?` against a literal produces a `bool` whose `.toString()` silently prints **empty** (branching on it with `if` is fine — only stringifying is wrong) | #87 | narrow the optional (`match`) before a relational compare; never `.toString()` a `bool` derived from one directly | `known_bugs_1.md` #87 (no corpus debt site — the row is omitted from `tests/corpus/expr_diff/expr_diff_none.lev`, not worked around) |

## Names, generics, overloads (clusters C & D)

| construct that breaks | bug | sanctioned workaround | debt sites |
|---|---|---|---|
| A dependency's nested-namespace `const int` read fully-qualified (`Pkg::Ns::CONST`) from consumer code reads as **0/empty**, silently | #82 | import with `uses Pkg::Ns;` and use the bare name; keep constants internal to the owning package where possible | `packages/atlantis-mysql/tests/*` (all consume `MySql::FieldType`/`MySql::Caps` via `uses` + bare) |
| Implementing a dependency's interface with **alias-qualified** member types (`A::Data::Foo`) fails interface satisfaction (`not assignable to Foo`); and a package `.lev` file missing `uses A::Data;` breaks bare-name lookup in its sibling files | #83 | spell C3/interface member types **bare** via `uses A::Data;`, put that `uses` in **every** src file of the package; the `class X : A::Data::IFace` clause may stay qualified | every file in `packages/atlantis-mysql/src/` (8 files) + `packages/atlantis-mysql/tests/{loopback,pool}/main.lev` |

## Expression reification (`namespace expr`, LA-31)

| construct that breaks | bug | sanctioned workaround | debt sites |
|---|---|---|---|
| A reified `!x` (logical not) stores the op text `"?"` in the `Un` node, not `"!"` — any tree-walker (SQL renderer, `expr::eval`) dispatching on `n.op == "!"` silently mis-handles it; `.fn` (the closure leg) is unaffected | #86 | do not reify a lambda body containing a bare `!`; rewrite as `x == false` (reifies as an ordinary `Bin`) if a not-shaped predicate is needed | `tests/corpus/expr_reify/` (the `!`-unary row is omitted, not worked around) |
| `--expand`-then-recompile of a file with **two** reified `expr::Call` sites sharing the same whitelisted name but different receiver kinds (e.g. one `string.contains`, one `Array.contains`) silently breaks the Array-receiver site's tree walk (`.fn` still correct; `expr::eval`-style consumers get the wrong branch) | #88 | don't rely on `--expand` output of a file with two same-named, different-receiver-kind reified calls for anything beyond visibility; split them across files if round-tripping matters | none (avoided, not worked around, in `tests/corpus/expr_diff/`) |
| A whitelisted `Call` (`.like`/`.ilike`/`.startsWith`/`.endsWith`/`Array.contains`) inside a lambda whose parameter type flows through a **generic type parameter** (e.g. `Query<E>.where(expr::Expr<(E)=>bool>)`) is wrongly rejected as "non-whitelisted"; the identical body reifies fine against a concrete, non-generic parameter type, and plain comparisons reify fine through the same generic method | #89 | give the consuming method a concrete (non-generic) parameter type if a whitelisted call must appear in the reified body; the ORM's `Query<E>.where` shape (`designs/atlantis/techdesign-06-orm.md` §3) cannot yet host its own headline `.like` example | `tests/corpus/expr_reify/expr_orm_smoke_1.lev` (demonstrates only plain comparisons/arithmetic through `Query<E>`) |

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
| `--expand` now runs the full Checker before printing (LA-31 ruling R1) — an ill-typed program fails `--expand` with the same diagnostics as a normal compile, where it used to just print the raw parse | `--expand`'s job changed from "show me the parse" to "show me the checked, rewritten program" (method-ref eta-lambdas, LA-31 `expr::Expr` constructions, named-arg/default normalization all now visible) | use `--ast-after-rules` (`ExpandAst`) to debug the expansion of an ill-typed program — it stays the pre-Checker view |
| A captured `Array<T>` (or any non-`string\|int\|float\|bool\|T?`-typed capture) used as the receiver of a reified `Array<T>.contains(x)` gets a `binds` slot holding **`None`**, not the array | the `binds` element union is DbValue-shaped (`string\|int\|float\|bool\|None`) and cannot carry an Array; the closure leg still captures the real array normally | a consumer walking `.tree`/`.binds` (SQL renderer, `expr::eval`) must keep its OWN reference to the captured array's contents — it cannot read them out of `binds` |
| A reified `expr::Field` path (`["age"]`) carries no marker for which lambda parameter it came from | the ask's `Field` node taxonomy is path-only, by design (`expr_reify_r20_multiparam.lev`) | a multi-parameter reified lambda (`(a, b) => a.age < b.age`) with same-named fields on different params dumps identically for both sides; a consumer that needs per-parameter roots must declare single-parameter lambdas |
