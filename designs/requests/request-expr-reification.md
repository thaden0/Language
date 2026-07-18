# Request: Expression Reification — `expr::Expr<F>` (LA-31)

**From:** Atlantis Track 06 (ORM v2, `designs/atlantis/techdesign-06-orm.md` §3).
**Date:** 2026-07-18. **Priority: P0 — gates AG-4's query surface (Track 06 M2,
2026-09-25).** Owner implements per R9 ("make a ticket, I'll get it done").

## 0. Why (the strategic frame)

Owner direction 2026-07-18: **Leviathan is the query language.** Application code must
express data predicates in Leviathan itself — not SQL strings, not a descriptor DSL
imitating the type system (the purged ORM v1's `TypedColumn.like()` inconsistency is the
canonical failure of imitation). The mechanism: a lambda literal, in specific parameter
positions, is compiled to **both** its ordinary closure **and** a runtime data
representation of its body — one meaning, two executions (run it in memory; or translate
it behind a seam). This is deliberately a **language** capability, not an ORM feature:
any library (in-memory indexes, validation, a future storage engine of whatever shape
the post-stage-3 research chooses) can consume reified lambdas. No commitment to any
future engine's plan format is made or implied — the tree earns its place on today's
merits: compile-checked queries, injection-proof parameterization, stable per-call-site
rendering.

Nothing landed reaches this: procedural macros take a `string` and return opaque `Ast`
(not walkable data); comptime reifies literals/structs only. This is a new, scoped
compiler capability.

## 1. Surface

### 1.1 The carrier type (prelude, `namespace expr`)

```
class Expr<F> {
    F fn;                                              // the ordinary closure — leg 1
    expr::Node tree;                                   // the body as data — leg 2
    Array<string | int | float | bool | None> binds;   // captured values, slot order
    int siteId;                                        // compile-time-unique per lambda site
}
```

**Conversion rule:** a **lambda literal** in a position typed `expr::Expr<F>` (parameter,
bind, return — ordinary target-typing) compiles to an `Expr<F>` construction: the
closure, generated `expr::*` constructor calls building the tree, the binds array
(each captured value evaluated once, at construction, in slot order), and the site id.
A non-literal lambda *value* does **not** convert (no runtime reification exists —
compile error: "only a lambda literal can be reified"). `Expr<F>` is otherwise an
ordinary class — storable, passable; calling `.fn` is calling the lambda.

### 1.2 The node taxonomy (prelude classes; closed set v1, `match`-dispatched)

```
namespace expr {
    class Node { }
    class Field  : Node { Array<string> path; }        // u.name -> ["name"]; u.a.b -> ["a","b"]
    class Lit    : Node { string | int | float | bool | None v; }
    class Bind   : Node { int slot; }                  // captured local/param -> binds[slot]
    class Bin    : Node { string op; Node l; Node r; } // == != < <= > >= && || + - * / %
    class Un     : Node { string op; Node e; }         // ! -
    class Call   : Node { string name; Node recv; Array<Node> args; }  // whitelist §1.3
    class Assign : Node { Field target; Node value; }  // set()-shaped lambdas only
}
```

Reference classes are fine here: the tree is **runtime** data built by emitted ctor
calls (comptime non-reifiability is irrelevant); it is immutable by convention and
consumers only `match` over it.

### 1.3 The reifiable subset (compile error outside it, at the lambda site)

Given lambda parameter `u`:

- `u.field`, chained `u.field.field` → `Field(path)`. Enum member constants
  (`RelationType::Friend`) → `Lit(carrier int)`. Other literals → `Lit`.
- A captured local/param/`this.x` read → `Bind(slot)` + the value into `binds[slot]`.
- Comparisons/logic/arithmetic per §1.2 ops → `Bin`/`Un`. `x == None` / `x != None` on
  `T?` → `Bin("==", Field, Lit(None))` shape (consumer renders IS NULL).
- Whitelisted calls → `Call`: `string.like(p)`, `string.ilike(p)`,
  `string.startsWith(p)`, `string.endsWith(p)`, `string.contains(p)`,
  `Array<T>.contains(x)`. Receiver/args must themselves be reifiable.
- Single assignment `u.field = <reifiable expr>` → `Assign` — legal only when the target
  type is `Expr<(E) => int>`-shaped set-lambda positions (assignment is an expression;
  the int result is `expr`-conventional).
- **Everything else is a compile error naming the construct**: non-whitelisted calls,
  `await`, nested lambdas, statements/blocks, mutation of captures, `is`/`match`, string
  interpolation over fields. Diagnostic points at this ticket's whitelist section.

The whitelist grows **only by ticket amendment** carrying, per new op: both legs (in-
memory semantics + the consumer-facing rendering contract) and a differential-corpus
row. That is the consistency law (design §3.4) enforced at the register.

### 1.4 New prelude string methods (part of this ask; in-language, no natives)

- `bool string.like(string pattern)` — SQL-wildcard match (`%` any run, `_` one char,
  `\%`/`\_` literals), **byte-exact, case-sensitive**.
- `bool string.ilike(string pattern)` — same, ASCII-case-insensitive (A–Z folded);
  beyond ASCII, bytes compare exact (the documented bound).

These are ordinary prelude methods (usable outside queries); the ORM's renderer maps
them to `LIKE BINARY ?` / `LOWER(x) LIKE LOWER(?)` respectively.

## 2. Semantics pins

1. **Two legs, one meaning:** for every reifiable lambda, `e.fn(x)` and a consumer
   faithfully interpreting `e.tree` (with `binds`) must agree. The language guarantees
   the tree *mirrors the checked AST* (post-typecheck shapes: resolved members, folded
   comptime constants); consumers own their rendering fidelity, proven by their own
   differential corpora.
2. **Capture snapshot:** `binds` values are evaluated once at `Expr` construction (same
   moment the closure would capture); later mutation of a captured original affects
   neither leg's already-built `Expr`. (Closures capture by closure; the snapshot rule
   makes the legs agree — pinned by a corpus row.)
3. **Literals fold; captures bind.** Compile-time constants appear as `Lit` (inline —
   stable tree text per site); anything runtime-valued is a `Bind`. `siteId` is stable
   across runs of the same build (deterministic compilation).
4. **`--expand` shows everything:** the emitted `Expr` construction (tree ctors, binds
   array) renders as compilable source — the zero-magic discipline, unchanged.
5. **Cost:** O(nodes) ctor calls per `Expr` construction, no reflection, no parsing at
   runtime. Engines: oracle + IR + LLVM (emit-C++/ELF: standard deferral posture).

## 3. Acceptance

1. Corpus: each §1.3 form reifies; each non-reifiable form errors with the named
   construct; hand-written-twin byte-equivalence for the emitted construction
   (`rule_orm` discipline).
2. Differential rows: per whitelisted op, closure leg vs a reference tree-interpreter
   leg (a ~60-line in-language `expr::eval` shipped with the corpus) agree on fixture
   inputs — including the `like`/`ilike` pins and the capture-snapshot rule.
3. `string.like`/`ilike` land with their own unit rows (wildcards, escapes, ASCII fold).
4. Track 06 M2 activates on landing: `Query.where/orderBy/with/set` compile against
   `expr::Expr<F>` with zero ORM-side changes to M1 code.

## 4. Fallback until landed

Track 06 M1 ships the full write path + `find`/`all` + raw hatch **without** any interim
query DSL (no fragments, no descriptors — ruled). Consumers wait on this ticket; the
same-week landing precedent (threads, TLS, readonly, target::) is the planning
assumption, escalation per R4 if it slips past 2026-09-01.
