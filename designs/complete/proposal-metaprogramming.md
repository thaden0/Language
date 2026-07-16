# Proposal: Compile-Time Meta-Programming (the rule layer)

**Status:** design proposal — supersedes and details the `info.md` §16.5 sketch and evaluates
Leonard's anchor sketch.
**Location note:** placed in `docs/` alongside `reference.md` and the companion
`designs/proposal-web-framework.md`. (Both proposals live under `docs/`; the task allowed either
root or `docs/` — `docs/` keeps the root clean and sits next to the formal reference the
proposal repeatedly cites.)

> **One-line thesis.** A compile-time layer where **you** author source-to-source transforms —
> but the transform operates on the **parsed, resolved tree** (never raw text), is written in
> **the base language run at compile time** (never a second macro language), is **scoped to a
> namespace** (fires only where imported), is **hygienic and additive by default**, and always
> has a **`--expand` window** so the magic is invisible at the use site but greppable and
> readable at the rule site.

This document is opinionated. Where it departs from Leonard's initial sketch it says so and why.
The short version: **keep the goal, change the substrate.** The goal — `@Route("/hello")` above a
method morphing into real registration wiring, authored by us rather than hardcoded in the
compiler — is exactly right and is retained. The substrate — bracket/comment anchors, `.clear()`,
`writeBelow('...')` string injection, token `.nextToken` munging — is the C-preprocessor model,
and every language that has lived with that model warns against it. We replace the substrate with
an AST-level, hygienic, namespace-scoped rule system.

---

## 0. Table of contents

1. Why this exists, and the one hard constraint the language imposes
2. Evaluation of Leonard's sketch (kept / changed / cut, with reasons)
3. Cross-language survey — what works, what's hated, what to steal
4. The recommended design — four layers over one substrate
5. Namespace binding (the hard requirement) — rules fire only where imported
6. The compile-time API surface
7. Hygiene, determinism, error reporting, source-mapping
8. Where it sits in the pipeline (phases and staging)
9. Prerequisites — everything that must exist first (the project/file system does not exist yet)
10. Worked examples
11. Risks and mitigations
12. Phasing / delivery plan
13. Open questions

---

## 1. Why this exists, and the one hard constraint

The language's whole personality is in `info.md` §1: **one rule over many special cases,
explicit over implicit, honesty over hidden magic, resolution by type everywhere, and gate the
dangerous while guaranteeing the safe.** A metaprogramming system is the single most dangerous
thing you can add to a language with that personality, because its default failure mode is
*invisible magic* — precisely the thing §1 exists to forbid.

So the design constraint is not "add macros." It is: **add author-defined compile-time rules
without eroding static legibility.** §16.5 already found the resolution: *explicitness moves up a
level* — from the use site to the rule site. The annotation at the use site is terse; the rule
that gives it meaning is explicit, typed, greppable, and lives in a named namespace. That single
idea is the spine of everything below.

Three properties are therefore **non-negotiable** and everything else bends around them:

- **P1 — Cost-identical to hand-writing.** A rule-wired `addRoute(path, m)` must compile to the
  identical machine code the hand-written call compiles to. Rules run **AST→AST before lowering**,
  so injected code goes through the *same* resolver / checker / shape-offset / ownership passes as
  hand-written code (§16.5). The payoff is not "reflection made convenient"; it is **zero runtime
  reflection at all** — the framework ergonomics in `designs/proposal-web-framework.md` depend on
  this.
- **P2 — Legible at the rule site.** The transform is opt-in, located in a namespace, written in
  the base language, and dumpable with `--expand`. No ambient, file-global rewriting.
- **P3 — Scoped by namespace (§5).** A rule only touches code that imports the namespace defining
  it. This is a *hard requirement* from the brief and it also happens to be the mechanism that
  makes P2 true: "where can this magic reach?" is answered by "which namespaces does this file
  `uses`?" — structure the compiler already tracks.

---

## 2. Evaluation of Leonard's sketch

Leonard's sketch has three moving parts: **anchors** (`[text]`, `(text)`, `// text`), **anchor
blocks** (`[top] … [bottom]` captured and rewritten in-language), and **keyword macros**
(`safe_strip myString`, queried via `Anchors::queryWord`, mutating `.nextToken`). Evaluated
honestly against the language's own philosophy and the cross-language record (§3):

### 2.1 What is exactly right and is kept

- **Author-defined, not compiler-hardcoded.** The core instinct — *we* write the `[Route]`→wiring
  rule, in a rule layer, not the compiler — is the whole point and is the heart of the recommended
  design.
- **Declaration-position annotations that morph into wiring.** `@Route("/hello")` (or `[Route(...)]`)
  above a handler is the ASP.NET-attribute ergonomic everybody actually wants (§3, §5 of the
  web-framework research). Kept as **Layer A**.
- **An anchor object with structure** — a `.class` (the leading word, e.g. `top` / `Route`) and a
  `.text` / parsed arguments (`value='val' size=7`). This is a good shape; we keep it, but as a
  **typed AST node handed to rule code**, not a substring of raw source.
- **Query + inject as the two verbs.** "Find the shape, put code here" is the correct mental model
  and survives verbatim as `match … inject …`.
- **In-language processing.** Rules are written in the Language itself. Kept, and strengthened:
  the *whole* rule (matcher predicate + action template) is base-language code run at compile time
  — no separate macro sublanguage (§3's Zig-comptime lesson, §4.3).

### 2.2 What is changed (same intent, safer substrate)

- **Anchors operate on the AST, not on characters.** "Syntax-shaped regex: you match over parsed
  structure, not a character stream" (§16.5). `queryBlocks('top','bottom')` becomes "give me the
  statements in this method body between the `top` and `bottom` **markers**," where markers are
  parsed nodes and the payload is a list of AST statements, not lines of text. `[]`-line iteration
  over a block becomes iteration over `block.statements`.
- **`.top.writeBelow('...')` string injection becomes quasiquoted AST injection.**
  `writeBelow("this.app.addRoute(path, m)")` — a string of source spliced back in — is the single
  most dangerous idea in the sketch (it is `#define`/`writeLine` textual codegen; §3.4). It is
  replaced by a **quasiquote**: `` inject `this.app.addRoute($path, $m)` at top of body`` where the
  backtick template is parsed *once, at rule-compile time*, type-checked, and its holes (`$path`,
  `$m`) are filled with bound AST nodes. Precedence, hygiene and parse errors are caught in the
  template, not discovered after re-lexing a string.
- **Keyword/token macros (`safe_strip myString`, `.nextToken`) become expression macros over the
  AST.** Bare-word token adjacency (`safe_strip myString`) has no delimiter, no precedence story,
  and no hygiene — it is the `DOUBLE(x)` footgun waiting to happen. Replaced by a **marked
  expression-macro call** whose argument is a parsed expression node (§4.2), e.g. `safeStrip!(name)`
  or `@safeStrip(name)`, so the boundary is explicit and the argument is a real sub-tree.

### 2.3 What is cut (and why)

- **`(text text)` and `// text` as anchors are cut.** `()` is grouping and (future) call selectors;
  `//` is a comment. Overloading either as an anchor sigil re-introduces exactly the C-preprocessor
  disease where `#define Stream …` silently mangles `class Stream` — the parser can no longer tell a
  comment from a directive. The language's §1 rule ("refuse unprincipled sugar / one meaning per
  form") forbids giving `//` two meanings. One annotation sigil, chosen for non-ambiguity (§4.1).
- **`.clear()` a block "so nothing in it compiles" is cut as a primitive.** Wholesale "delete this
  region of source" is a rewriting hammer that defeats P1/P2 (the reader sees code that never runs).
  The legitimate uses — conditional compilation, replacing a body — are served by **`comptime if`**
  (Layer C) and by **body-replacing rules that state their replacement as a quasiquote**, both of
  which are legible and additive rather than a silent delete.
- **Raw `.nextToken` mutation is cut.** Mutating the token stream in place is un-hygienic by
  construction and unrecoverable for the IDE/formatter (§3.1, §3.10). Expression macros take and
  return **nodes**, not token cursors.

### 2.4 Is `[]` the best anchor? (asked directly)

**No, not for the annotation sigil — but it can work with a rule, and there is a better default.**
`[` opens an array literal and an index (`a[i]`, `[1,2,3]`); those live in *expression* position.
Annotations live in *declaration* position (immediately above a `class`/`method`/`field`). A
position-sensitive parser *can* disambiguate `[Route("/x")]` on its own line above a method from an
array-literal statement — §16.5 itself writes `[Route("/hello")]`. But "can disambiguate" is weaker
than "cannot be confused," and the language prefers forms that cannot be confused (§1). Two viable
choices, recommended in order:

1. **`@Name(args)` (recommended default).** A dedicated sigil, zero grammar collision, instantly
   greppable (`grep -rn '@'`), and the exact shape every reader already associates with
   attributes/decorators (Java, C#, Python, TS, Swift). `@` is currently unused in the operator
   table (§1.5 of `reference.md` lists `?` but not `@`).
2. **`[Name(args)]` (compatible alternative).** Matches §16.5's existing spelling and C#'s
   attribute look. Requires the "declaration-position only" parse rule and is very slightly more
   ceremony. Kept as an accepted alias if the C#-attribute aesthetic is wanted.

This proposal standardizes on **`@`** for attributes and **`@name!(…)` / `name!(…)`** for the rare
expression macro, and treats `[…]` attributes as an optional surface alias decided later (§13).

---

## 3. Cross-language survey (what to steal, what to avoid)

Full sourced survey is summarized here; the design choices in §4 cite back to these findings.

### 3.1 The textual-vs-AST verdict (settles the substrate)
The **C/C++ preprocessor** is the universal cautionary tale: textual substitution "lacks syntactic
awareness," causes precedence bugs (`DOUBLE(1+2)` → `4`), double-evaluation of side-effecting args,
no scoping, and clashes that produce baffling errors. The taxonomy is **textual → token-tree →
AST**, and *hygiene and precedence-safety only become possible at the token/AST levels*. **Nobody
chooses textual for a new statically-typed language.** Even Zig — which refuses a macro DSL —
*also* refuses string-injection codegen (no `#eval`/mixins), choosing partial evaluation over
textual synthesis. → **We operate on the resolved AST; never on raw source. This directly cuts
Leonard's raw-text anchors and `writeBelow("…")`.**

### 3.2 Hygiene by default (Lisp/Scheme, Rust, Racket)
Every AST system that is hygienic (Scheme `syntax-rules`, Rust `macro_rules!`, Racket scope-sets) is
praised; every un-hygienic one (C preprocessor, Rust **proc** macros, Swift macros, Nim `untyped`)
reproduces the same capture/clash bugs. Rust proc macros must write `::std::option::Option` and
`__internal_foo` by hand to dodge capture; "hygiene 2.0" is a years-open issue. → **Hygiene is the
default; deliberate capture is an explicit, marked escape hatch (§7.1).**

### 3.3 Error reporting at the *use* site, not the expansion (Racket `syntax-parse`)
The single most-cited "done right" feature: Racket reports `swap: expected identifier at (+ foo 1)`
— pointing at the caller's mistake, not leaking an internal `set!: not an identifier`. Nim's
open bug (errors point at the caller, not the generated code) and Rust's untraceable macro chains
are the anti-pattern. → **Matchers declare what they expect; a use that doesn't fit gets a message
attributed to the use site (§7.3).**

### 3.4 "Not a second language" (Zig comptime) — the design axis
Zig's whole pitch: "regular code and comptime code are the same language… same syntax, same
semantics, different execution phase." No `syn`, no template syntax, no macro crate. Its limits are
instructive and deliberate: **no I/O** (so comptime is "hermetic, reproducible, safe, cacheable"),
no custom syntax, and — crucially — **it cannot synthesize new declarations or attach methods**
("inert bundles of fields"). → **We take the "run the language at compile time" ergonomic (Layer
C) AND add the narrow codegen facility Zig lacks (Layer B, additive AST injection), because the
framework needs exactly the "generate a registration/route from a declaration" case comptime can't
reach.**

### 3.5 Additive-only, non-rewriting where possible (Roslyn, Kotlin KSP, Java APT)
Roslyn source generators **may add code but may not modify existing user code** — explicitly to stop
generators creating "new dialects of C# incompatible with the compiler." Same fear names Rust's
"proc macros risk creating de facto dialects." → **The default action is *additive injection at a
named anchor* (a call at the top/bottom of a constructor, a member added to a class), not arbitrary
rewriting. Body replacement exists but is a named, gated action (§4.2), not the default.**

### 3.6 Determinism & incremental caching (Roslyn incremental generators, Zig)
Roslyn's incremental generators cache per-item so a keystroke doesn't re-run everything; Zig enforces
determinism by forbidding compile-time I/O; Template Haskell's compile-time I/O is the warning
("must produce results identical to the compiler's"). → **Compile-time rule evaluation is hermetic:
no file/network/clock/RNG access (§7.2). This is the same stance the runtime already takes with the
gate pattern (§16), applied to the compiler.**

### 3.7 Expansion inspection is table stakes (everybody builds it eventually)
`-ddump-splices` (TH), `expandMacros`/`repr` (Nim), `EmitCompilerGeneratedFiles` (Roslyn),
macro-expansion tests (Swift). → **`--expand` / `--ast-after-rules` ships in v1, not as an
afterthought (§16.5 already calls it "a required window").**

### 3.8 Macro-API stability is a hard compatibility commitment (Scala 2→3)
Scala's macro reset "made a lot of useful macro-heavy libraries unusable without a complete rewrite"
(chimney). If rules can read the AST/type shape, that shape is a promised API. → **We expose a
*small, deliberately minimal* reflection surface (§6.3) and version it, rather than the whole
internal AST.**

### 3.9 Compile-time & dependency taxes (Rust `syn`/`watt`, Swift SwiftSyntax)
Heavy macro machinery pulled into every dependent's build is a standing tax (Rust proc macros 20s+;
Swift statically links SwiftSyntax + 10 deps). → **Rule evaluation reuses the *existing* tree-walk
oracle (already built, §7.1 of `reference.md`); there is no new parser/host to link. The rule
interpreter is the interpreter we already ship.**

### 3.10 Tooling/IDE cannot be an afterthought
"IDE-hostile" (Rust), "rustfmt can't cope with macro bodies," Roslyn's "navigate to files that don't
exist." → **Because rules are additive AST nodes with source spans (§7.4), the formatter/IDE keep
working on the hand-written surface; the generated surface is viewable via `--expand` and carries
spans back to its origin.**

**Net synthesis the design implements:** *AST-level + hygienic + additive-by-default + hermetic +
namespace-scoped + no-second-language + call-site errors + always-inspectable.* Every one of those
adjectives maps to a concrete decision below.

---

## 4. The recommended design — four layers over one substrate

One substrate: **the resolved AST of the whole-program compilation unit.** Four author-facing
layers, ordered from "reach for this constantly" to "reach for this almost never," each strictly
more powerful and strictly more gated than the last.

| Layer | Name | What it is | Power | Gate |
|---|---|---|---|---|
| **A** | **Attributes** | Declaration-position annotations `@Name(args)` that *carry data* to rules | none on their own | none (inert until a rule reads them) |
| **B** | **Rules** | `rule … match … inject …` — match an AST shape, inject quasiquoted AST at a named anchor | additive codegen | namespace-scoped; `--expand`-visible |
| **C** | **Comptime** | `comptime` expressions / `comptime if` — ordinary language code evaluated at compile time (const-fold, table-gen, conditional compilation) | computation, specialization | hermetic (no I/O) |
| **D** | **Rewriters** | body-replacing / node-replacing rules and expression macros | arbitrary same-scope rewrite | explicit `rewrites` marker; loudest `--expand` diff |

Layers A+B are the 95% case (the framework surface). C is for computed constants and conditional
code. D is the marked bazooka-room (§16 gate pattern) for the rare transform that genuinely must
replace code rather than add to it.

### 4.1 Layer A — Attributes (the terse use-site surface)

An **attribute** is a declaration-position annotation. It **allocates nothing and does nothing by
itself** — exactly like an interface (§8): it is a contract/marker that a *rule* later reads. This
keeps §1 honesty: an attribute never "secretly means something"; it means whatever the greppable
rule in its namespace says it means, and nothing if no rule reads it (a warning fires for an
attribute no imported rule consumes — a dangling attribute is almost always a missing `uses`).

```
@Route("GET", "/users/:id")          // attribute on a method
int getUser(int id) => ...;

@Table("users")                       // attribute on a class
class User { @Column public int id;  @Column public string name; }
```

An attribute is itself declared as an ordinary type so its arguments are **type-checked** (unlike a
`#define` or a stringly Django decorator):

```
namespace Web {
    // An attribute is a struct marked `attribute`; its fields ARE its arguments.
    public attribute Route { string method; string path; }
    public attribute Column { string name = ""; }   // defaulted -> @Column or @Column("id")
}
```

`@Route("GET","/users/:id")` constructs a compile-time `Route` value (positional or named args,
the ordinary constructor rules of §2/§3). Rule code reads it as typed data — `attr.method`,
`attr.path` — never as a substring. This is Leonard's `.class` / `.text` shape, upgraded from
"leading word + raw text" to "typed node + typed fields."

### 4.2 Layer B — Rules (match a shape, inject quasiquoted AST)

A **rule** is a declaration inside a namespace. It has the two verbs from the sketch — **match** and
**inject** — but both operate on the resolved AST.

```
namespace Web {
    rule registerRoutes {
        match @Route(r) on method m in class C : IController      // matcher: shape + predicate
        inject `this.router.add($r.method, $r.path, () => this.$m($_args))`
               at bottom of C.constructor                          // action: quasiquoted AST @ anchor
    }
}
```

Reading it:

- **`match`** binds names by destructuring a syntactic shape:
  - `@Route(r)` — fires on any declaration carrying a `Route` attribute; binds `r` to the typed
    attribute value (§4.1).
  - `on method m` — the attributed declaration is a method; bind it as `m`.
  - `in class C : IController` — its enclosing class, required to implement `IController`; bind `C`.
    The `: IController` clause is a **post-resolve predicate** (it reads the resolved interface set,
    §8) — this is the §3.3 "declare what you expect" that powers good errors.
- **`inject`** is the action. The backtick literal is a **quasiquote**: base-language source parsed
  once at rule-compile time and type-checked as a template. `$r`, `$m`, `$_args` are **holes**
  filled from the match bindings. `at bottom of C.constructor` names the **anchor** — the same
  `$init`-style insertion point the compiler already uses internally (§16.5; the constructor-field-
  init synthesis is exactly this mechanism, now exposed to authors).

**Anchors** are the disciplined replacement for Leonard's `[top]…[bottom]`. Instead of *arbitrary
text markers a rule can put anywhere*, anchors are a **fixed, named set of injection points** the
compiler guarantees are safe insertion sites:

| Anchor | Meaning |
|---|---|
| `top of X.constructor` / `bottom of X.constructor` | before/after the user body of a constructor (post base-ctor calls) |
| `member of X` | add a new member (method/field/accessor) to a class/struct |
| `top of body` / `bottom of body` (of the matched method) | prologue/epilogue statements in the matched method |
| `namespace N` | add a declaration at namespace scope |
| named marker `@anchor("name")` | an author-placed marker inside a body (the principled `[top]`), injected at by name |

The named-marker anchor is where Leonard's block idea survives: you may write `@anchor("audit")`
inside a method body and a rule may `inject … at marker "audit"`. But the payload is **statements
spliced into a real block**, hygienic, span-tracked — not text pasted into a character range, and
there is no `.clear()` that silently deletes a region.

**Additive by default (§3.5).** `inject` only *adds* nodes. A rule cannot delete or silently
rewrite existing user code. That covers the entire framework surface (route tables, ORM column
maps, DI registration, validation wiring). The rare "replace the body" case is Layer D.

### 4.3 Layer C — Comptime (run the language at compile time)

The Zig-comptime lesson (§3.4): the least surprising metaprogramming is *no metaprogramming
language at all* — just the base language, evaluated earlier. The language **already ships the
evaluator** (the tree-walk oracle is the semantics oracle, `reference.md` §7.1), so compile-time
evaluation is a *driver*, not a new engine.

```
comptime int TABLE_SIZE = nextPrime(1000);            // folded to a constant at compile time
Array<int> sines = comptime buildSineTable(256);      // table computed once, baked in

comptime if (Platform::current == Platform::Linux) {   // conditional compilation, legible
    uses PosixSockets;
} else {
    uses WinSockets;
}
```

- A `comptime` expression is evaluated by the oracle during compilation; its **result value is
  reified** as a literal AST node (an int, a string, an array of structs — the value model already
  exists). This is how you generate a lookup table without a codegen DSL.
- `comptime if` is the **principled replacement for Leonard's `.clear()`**: the untaken branch is
  *not compiled*, but the reader sees exactly which branch and why — conditional, not a silent
  delete.
- Comptime is **hermetic** (§3.6/§7.2): no I/O, bounded step budget (a runaway `comptime` loop is a
  compile error with a budget message, exactly like Zig's eval-branch-quota). This is the §16 gate
  pattern turned on the compiler: the dangerous capability (arbitrary evaluation) is fenced by "no
  side effects, bounded steps."

Rule bodies (Layer B) are themselves ordinary language code run under this same comptime evaluator —
so matchers and injection logic can call helper functions, loop, branch, and build node lists using
the language you already know. *That* is the "not a second language" commitment (§3.4): the matcher
clause is a small declarative shape, but everything computational is base-language comptime code.

### 4.4 Layer D — Rewriters (the marked bazooka room)

Two capabilities that *can* change existing code, both behind an explicit `rewrites` marker so they
are greppable and their `--expand` diff is the loudest:

- **Body-replacing rule:** `rule memoize rewrites body of m { … original as $body … }` — wraps or
  replaces the matched method body. The original body is available as a hole (`$body`) so
  replacement is composition, not obliteration.
- **Expression macro:** `macro safeStrip(e) => `($e ?? "").trim()`` — a `name!(expr)` /
  `@name(expr)` call whose **argument is a parsed expression node** and whose result is a
  quasiquoted expression. This is Leonard's `safe_strip myString`, made hygienic and delimited:
  `safeStrip!(myString)`. The argument is captured **once** as a node (killing double-evaluation)
  and hygiene renames any introduced temporaries (killing capture).

Layer D exists because some real transforms (memoization, tracing, `@Timed`, contract assertions)
must wrap a body. It is deliberately last and deliberately marked: if you find yourself in Layer D
often, the language would rather you reconsider (the §16 "protect the safe majority's guarantee"
stance).

---

## 5. Namespace binding (the hard requirement)

> *Rules must be scoped — a rule only affects code that imports the namespace defining it; code
> that doesn't import stays untouched.*

This is both a requirement and the mechanism that makes the whole system legible (§1, P3). It keys
off structure §12 already maintains — no new scoping concept is invented.

### 5.1 The rule
**A rule fires on a declaration `D` iff the file that declares `D` imports (via `uses`, or by being
in) the namespace that declares the rule.** Concretely:

- A rule `R` lives in namespace `N` (`rule R { … }` inside `namespace N { … }`).
- For each source **file** `F`, compute `imports(F)` = the set of namespaces `F` brings into scope:
  the namespaces `F` itself opens, everything `F` names in a `uses`, plus the always-on `std`.
- `R`'s matcher is applied **only** to declarations physically written in files where
  `N ∈ imports(F)`.
- A file that never `uses N` is *invisible* to `R`. Its attributes-that-happen-to-share-a-name are
  inert (and warned as dangling, §4.1). Its code is byte-for-byte unaffected.

```
// file: web/routing.ext
namespace Web {
    public attribute Route { string method; string path; }
    rule registerRoutes { match @Route(r) on method m … inject … }
}

// file: app/users.ext
uses Web;                                  // <-- opts THIS file into Web's rules
namespace App {
    class UserController : IController {
        @Route("GET", "/users")            // fires: this file uses Web
        Array<User> list() => ...;
    }
}

// file: app/legacy.ext
namespace App {
    class Report {
        @Route("GET", "/legacy")           // does NOT fire: this file never `uses Web`
    }                                       //   -> dangling-attribute warning, code untouched
}
```

### 5.2 Why file-granularity (not namespace-merge granularity)
§12 says namespaces are **declaration-based and re-openable** — the same namespace can be opened in
many files, and file boundaries "dissolve" into one compilation unit. That dissolution is a problem
for scoping rules: if `App` is opened in both `users.ext` (which `uses Web`) and `legacy.ext` (which
doesn't), a namespace-granularity rule would either over-reach into `legacy.ext` or under-reach in
`users.ext`. **So rule application is computed at file granularity**, using a *retained*
`file → imports` map (Prerequisite P-4, §9). This is the one place the compiler must remember which
file a declaration came from *after* the whole-program gather — a small, contained addition.

### 5.3 Import is opt-in and greppable
The consequence is the §1/§16 legibility guarantee stated precisely: **to know every rule that can
touch a file, read its `uses` list.** No rule reaches across a namespace boundary it wasn't
imported through. This is the exact same "explicit selector only where needed" shape as `::`,
constructor labels, and `inject` (§12.5) — metaprogramming reach is governed by the ordinary import
mechanism, not a new ambient one.

### 5.4 Collision and ordering across imported rules
- **Determinism.** If a file imports several namespaces each contributing rules, rules run in a
  **deterministic order**: topological by namespace dependency, then declaration order within a
  namespace. Never source-file order (which the whole-program gather doesn't preserve semantically).
- **Confluence.** Two rules that inject at the same anchor both apply; their combined effect must be
  order-independent *or* the compiler reports a **rule conflict** at the anchor (two rewrites of the
  same body, or two members of the same name+type — the latter reuses the §4.3 `distinct`/collision
  machinery). Additive injections at `bottom of ctor` compose (both calls appended, in rule order).
- **Duplicate-fire guard.** A single declaration matched by the same rule twice (e.g. two `@Route`
  on one method) fires the rule twice by design (multiple routes); a rule may declare
  `match one @Route` to require at most one and error otherwise (the §12.5 "duplicate binding is a
  hard error" stance, available opt-in).

---

## 6. The compile-time API surface

Kept **deliberately minimal** (§3.8 — every exposed shape is a forever-API). Three parts.

### 6.1 The matcher clause (a small declarative shape language)
Not Turing-complete; it only *selects and binds*. Grammar sketch:

```
match  <attr-pat>? on <decl-kind> <bind>  (in <decl-kind> <bind> <constraint>?)*  (where <comptime-bool>)?
```

- `<attr-pat>` — `@Name(bind)` | `@Name` | omitted (match by shape alone).
- `<decl-kind>` — `method` | `function` | `class` | `struct` | `field` | `constructor` |
  `interface` | `namespace`.
- `<constraint>` — `: IName` (implements), `: Base` (inherits), read from resolved facts.
- `where <comptime-bool>` — an arbitrary base-language predicate over the bindings, run as comptime
  (Layer C). This is the escape valve so the matcher stays tiny while predicates stay expressive:
  `where m.returnType == Void` etc.

Everything the matcher can't express, the `where` clause expresses in ordinary code — the §3.4
"small declarative shape + base-language computation" split.

### 6.2 The quasiquote (`` `…` ``) and holes (`$name`)
- A backtick literal is **base-language source parsed at rule-compile time** into an AST template.
  It is type-checked as a template against the anchor's context, so a malformed injection is a
  compile error *in the rule*, not a mystery after re-lexing.
- `$name` splices a bound node. `$name.field` splices a field of a bound attribute/struct value
  (evaluated at comptime). `$_args` / `$_params` are the well-known holes for "the matched method's
  argument/parameter list."
- Splices are **typed**: `$m` where `m : method` splices a call target; splicing it where an
  expression is required is a rule-compile error (§7.3).
- List splicing: `` `[ $for c in cols: $c.name ]` `` builds a node list by comptime iteration.

### 6.3 The reflection value model (what rule/comptime code can read)
A minimal, versioned, **read-mostly** reflection surface — typed structs in `namespace meta`, so it
obeys ordinary type rules and is discoverable:

```
namespace meta {
    struct Decl   { string name; DeclKind kind; Array<Attr> attributes; Span span; }
    struct Method { string name; Type returnType; Array<Param> params; Array<Attr> attributes; }
    struct Class  { string name; Array<Type> bases; Array<Method> methods; Array<Field> fields; }
    struct Attr   { string name; /* typed args via the attribute struct, §4.1 */ }
    struct Field  { string name; Type type; Array<Attr> attributes; }
    struct Param  { string name; Type type; }
    struct Type   { string name; Array<Type> args; bool isValue; /* struct vs class */ }
    struct Span   { string file; int line; int col; }
}
```

Rule/comptime code receives these as ordinary values (bound by the matcher) and **reads** them.
Writing back to the tree happens *only* through `inject` (additive) or a Layer-D `rewrites` action —
never by mutating a `meta.*` value. This preserves "the AST is not an ambient mutable global."

### 6.4 CLI surface (new compiler modes)
Extends the existing driver (`main.cpp` mode enum):

- `lang --expand file.ext` / `lang --ast-after-rules file.ext` — dump the post-rule AST (the
  §16.5 "required window"). Shows every injected node with a `// from rule Web::registerRoutes @
  users.ext:12` provenance comment.
- `lang --rules file.ext` — list every rule in scope for each file and which declarations it
  matched (the "what can touch this file" report, §5.3).
- `lang --no-rules file.ext` — compile with the rule pass disabled (diff against `--expand` to see
  exactly what rules contributed; also the escape hatch if a rule misbehaves).

---

## 7. Hygiene, determinism, errors, source-mapping

### 7.1 Hygiene (default on, capture is marked)
- **Definition-site resolution.** Identifiers *inside a quasiquote* resolve in the **rule's**
  namespace, not the injection site's. `` `logger.write(...)` `` refers to the `logger` visible
  where the rule is written; it cannot accidentally bind to a local named `logger` at the use site
  (the §3.2 Scheme/Rust guarantee).
- **Fresh temporaries.** Any binding a template introduces (`var $tmp = …`) is alpha-renamed to a
  fresh, un-nameable symbol, so it cannot shadow or be captured by use-site code (kills the C `tmp`
  footgun).
- **Deliberate capture is explicit.** When a rule *wants* to refer to a use-site name (anaphora),
  it must bind it through the matcher (`$m`, `$C`) — i.e. capture is only ever of *matched* names,
  never accidental. There is no "capture whatever `x` happens to be in scope."
- This mirrors the language's existing stance: resolution is by type and by lexical scope, never by
  guess (§1, §3.4 of `reference.md`).

### 7.2 Determinism (hermetic compile-time)
- Comptime/rule code **may not perform I/O** (no file, socket, clock, RNG, env) — enforced by
  denying the `std::sys*` floor and the timer/socket surfaces to the comptime evaluator. (§3.6; the
  §16 gate, pointed at the compiler.)
- **Bounded evaluation.** A per-compilation step budget; exceeding it is a compile error naming the
  offending rule/`comptime` site (Zig's eval-quota, Roslyn's incrementality mindset).
- **Order is fixed** (§5.4). Same inputs → same expansion, always. This is what makes `--expand`
  diffs meaningful and lets a future incremental build cache per-file rule output (§3.6).

### 7.3 Error reporting (Racket `syntax-parse` as the north star, §3.3)
- **Matcher mismatch is a use-site error.** `@Route` on a `field` when the rule says `on method`
  reports at the attribute: *"`@Route` applies to methods; `count` is a field (rule
  `Web::registerRoutes` at routing.ext:7)."* Both spans named — use site and rule site.
- **Template type errors are rule-site errors.** A quasiquote that doesn't type-check reports in the
  rule, before any expansion, with the hole types shown.
- **Post-injection errors carry dual provenance.** If injected code fails the *normal* checker
  (e.g. `$m` has the wrong arity for the template call), the diagnostic shows the synthesized line
  *and* "introduced by rule R expanding `@Route` at users.ext:12" (§7.4).
- **Dangling attribute** (`@Route` with no imported rule to read it) is a warning suggesting the
  missing `uses` (§4.1, §5.1).

### 7.4 Source-mapping / span attribution (the known soft spot, §16.5)
Every synthesized node carries a **two-part span**: `origin` (the use-site node that triggered it —
the attribute/marker/macro call) and `via` (the rule and its site). The existing `$init`
synthesis already needs span attribution; this generalizes that. Diagnostics prefer the `origin`
span (what the author wrote) and mention `via` for the rule. `--expand` prints both as provenance
comments. This is the concrete answer to "diagnostics on generated code" that Rust/Nim/Roslyn all
struggle with (§3.3, §3.10).

---

## 8. Where it sits in the pipeline (phases and staging)

The current pipeline (`main.cpp`): `lex → parse → resolve → check → lower(IR) → {5 engines}`. Rules
insert **between resolve and check**, with a re-resolve, exactly as §16.5 specifies:

```
lex
parse                         (attributes & quasiquotes parsed to AST; NOT executed)
resolve (pass 1)              (bind names, interfaces, bases — gives matchers resolved facts §6.1)
────────── RULE STAGE ──────────
  compute imports(F) per file (§5.1)                        [needs Prerequisite P-4]
  select rules per declaration by namespace scope (§5)
  run matchers (predicates read resolved facts, §3.3)
  evaluate rule/comptime bodies on the oracle (hermetic, §7.2)   [reuses existing evaluator]
  inject quasiquoted AST at anchors (additive; Layer D rewrites)
────────────────────────────────
re-resolve + check (pass 2)   (augmented tree; injected code checked like hand-written, P1)
lower (IR)                    (unchanged — rules are gone by here; zero runtime cost)
{5 engines}                   (unchanged)
```

Key staging points:

- **Attributes and quasiquotes are parsed but inert** until the rule stage. Parsing `` `expr` ``
  early is what lets template type errors be rule-site errors (§7.3).
- **Two-pass resolve.** Pass 1 gives matchers the resolved facts (`: IController` predicates).
  Pass 2 re-resolves the augmented tree so injected calls get the §7 fixed-offset fast path and the
  ownership analysis (§15) sees them — this is what makes injected code **cost-identical** (P1).
- **A purely-syntactic rule** (matches on attribute + decl-kind only, no `: Interface` predicate)
  can run in a *single* resolve pass — a cheaper fast path for the common `@Route` case, deferring
  the two-pass cost to rules that actually read semantic facts (§16.5's "syntactic-only matcher can
  run in a single resolve to start").
- **The evaluator is the one we already ship.** Rule bodies run on the tree-walk oracle
  (`reference.md` §7.1) with the I/O floor denied. No new interpreter, no `syn`-style host to link
  (§3.9). This is the bootstrapping answer: **compile-time execution = the existing interpreter,
  run on rule code, before lowering.**
- **Fixpoint policy.** Rules do **not** re-trigger on each other by default (injected code is not
  re-matched) — this bounds the stage to one pass and keeps it deterministic and fast. Rule-on-rule
  composition, if ever wanted, is a gated opt-in (`rule … reentrant`) with the step budget as the
  backstop (§13).

---

## 9. Prerequisites — what must exist first

This is the section the brief flags hardest: **a project / file system does not exist yet.** The
driver (`main.cpp`) reads exactly one file (`readFile(path)`), and `uses` currently resolves within
that single gathered unit. Namespaced rules are impossible until the compiler can (a) ingest many
files, (b) know which namespaces each file imports, and (c) build them in a defined order. Below is
every prerequisite, ordered, with the ones that are *hard blockers* marked **[BLOCKER]**.

### P-1 [BLOCKER] — A project manifest
A declarative project file (the existing `project.ext` is a *demo program*, not a manifest — reuse
the name or pick `project.toml`/`build.ext`). Minimum fields:

```
name    = "myapp"
entry   = "app/main.ext"          # or an explicit entry function
sources = ["src/**/*.ext"]        # globs -> the file set to gather
deps    = []                       # external packages (future; empty for now)
```

Minimally needed: **the file set** (so the compiler stops being single-file) and **the entry
point**. Everything else (deps, versions, output config) can follow.

### P-2 [BLOCKER] — A multi-file loader / gather step
Replace single-`readFile` with: read all `sources`, lex+parse each into a per-file AST, then gather
into the one whole-program unit §12 describes ("file boundaries dissolve; namespace boundaries
persist"). This is mostly plumbing over existing parser code, but it is the load-bearing change.

### P-3 [BLOCKER] — Module / namespace resolution across files + include graph + build order
- **Cross-file namespace merge:** the same `namespace App { }` opened in N files must merge into one
  scope (§12 says disk layout is irrelevant — so resolution must span files). The resolver already
  merges re-opened namespaces *within* a file; extend to *across* files.
- **Include graph:** edges are `uses` (file→namespace) plus, later, package deps. Used for
  (a) build/resolve order and (b) **rule scoping** (§5).
- **Build order:** topological over the include graph; cycles among namespaces are fine for
  resolution (whole-program) but the *rule dependency* order (§5.4) must be acyclic or reported.

### P-4 [BLOCKER] — Retained `file → imports` provenance
The one genuinely new bookkeeping item: after the whole-program gather, **each declaration must
still know which file it came from, and each file its `uses` set** (§5.2). Today the gather can
discard file identity; rule scoping needs it kept. Small, contained, but essential and easy to
forget.

### P-5 — A stable AST with a construction/quotation API (the quasiquote)
The `` `…` `` quasiquote needs: a parser entry point that parses a fragment (expression / statement
list / member) into AST, and a hole-substitution step. The parser exists; this exposes a fragment
mode and a node-builder. Also the minimal `meta.*` reflection structs (§6.3).

### P-6 — A compile-time evaluation driver
A hook that runs the **existing** tree-walk oracle on rule/comptime code during compilation, with:
(a) the I/O floor denied (hermetic, §7.2), (b) a step budget, (c) result-reification (turn a
returned value into a literal AST node, §4.3). No new engine — a driver around
`Evaluator`/`preludeProgram()` (which `main.cpp` already constructs for `--run`).

### P-7 — Two-pass resolve/check plumbing + `--expand`
Make resolve/check re-runnable on a mutated tree (pass 2, §8) and add the `--expand` /
`--ast-after-rules` / `--rules` / `--no-rules` modes to the driver enum. `--ast` already exists as a
starting point.

### P-8 — Span attribution for synthesized nodes
Generalize the `$init`/prelude-origin span handling (already a known soft spot, §16.5) to the
two-part `origin`/`via` span (§7.4). Needed for §7.3 error quality.

**Dependency order:** P-1 → P-2 → P-3 → P-4 must land first and in that order (they are one coherent
"the language gets a project system" milestone). P-5/P-6/P-7/P-8 are the "the language gets rules"
milestone and can proceed once P-1..P-4 exist. **Attributes (Layer A) and `comptime` (Layer C) can
ship the moment P-5/P-6 exist even before full rule scoping**, since a self-contained single-project
build trivially has every file importing the project's namespaces — a useful incremental beachhead.

---

## 10. Worked examples

### 10.1 The motivating case — routes (end to end)
```
// framework file: web/routing.ext
namespace Web {
    public interface IController { Router router; }
    public attribute Route { string method; string path; }

    rule registerRoutes {
        match @Route(r) on method m in class C : IController
        inject `this.router.add($r.method, $r.path, (HttpRequest req) => this.$m(req))`
               at bottom of C.constructor
    }
}

// app file: app/users.ext
uses Web;
namespace App {
    class UserController : IController {
        public Router router;
        new UserController() { }                    // rule appends router.add(...) calls here

        @Route("GET", "/users")     Array<User> list(HttpRequest req)  => userStore.all();
        @Route("POST", "/users")    User         create(HttpRequest req) => userStore.add(req.body);
    }
}
```
`lang --expand app/users.ext` shows the constructor after expansion:
```
new UserController() {
    // from rule Web::registerRoutes @ app/users.ext:8
    this.router.add("GET",  "/users", (HttpRequest req) => this.list(req));
    // from rule Web::registerRoutes @ app/users.ext:9
    this.router.add("POST", "/users", (HttpRequest req) => this.create(req));
}
```
No runtime reflection; the `router.add` calls compile to the same code you'd hand-write (P1).

### 10.2 An ORM column map (attributes as typed data)
```
namespace Orm {
    public attribute Table  { string name; }
    public attribute Column { string name = ""; }
    rule buildSchema {
        match @Table(t) on class C
        inject `static Schema schema() => Schema($t.name, [ $for f in C.fields where f.hasAttr(Column):
                    Col(f.attr(Column).name == "" ? f.name : f.attr(Column).name, f.type) ]);`
               at member of C
    }
}
uses Orm;
@Table("users")
class User { @Column public int id;  @Column("full_name") public string name; }
// User::schema() now exists, computed at compile time from the fields.
```

### 10.3 Comptime table (no codegen DSL, §4.3)
```
comptime Array<int> primes = sieve(1000);     // sieve() is ordinary language code, run once
int nthPrime(int n) => primes[n];             // primes is baked in as a literal array
```

### 10.4 Expression macro (Leonard's `safe_strip`, hygienic — Layer D)
```
namespace Text {
    macro safeStrip(e) => `($e ?? "").trim()`;   // $e captured once as a node; no double-eval
}
uses Text;
console.writeln(safeStrip!(userInput));          // -> console.writeln((userInput ?? "").trim());
```

### 10.5 Body wrapper (Layer D, marked)
```
namespace Diag {
    public attribute Timed;
    rule timed rewrites body of m {
        match @Timed on method m
        replace `{ var __t = Clock::now(); var __r = $body; log(m.name, Clock::since(__t)); return __r; }`
    }                                             // __t/__r alpha-renamed (hygiene); $body is the original
}
```

---

## 11. Risks and mitigations

| Risk (from the survey) | Mitigation in this design |
|---|---|
| Textual footguns (precedence, double-eval, capture) — §3.1 | AST substrate; quasiquote parsed once; hole = node captured once; hygiene default (§4.2, §7.1) |
| Invisible magic erodes §1 legibility | Namespace-scoped (read the `uses` list, §5.3); `--expand`/`--rules` required (§6.4); additive-by-default (§4.2) |
| Un-hygienic capture bugs — §3.2 | Definition-site resolution + fresh temporaries; capture only of matched names (§7.1) |
| Opaque errors in generated code — §3.3 | Matcher "declares what it expects" → use-site errors; dual `origin/via` spans (§7.3–7.4) |
| Compile-time blowups / infinite loops — §3.6 | Hermetic + step budget; syntactic-only fast path single-pass (§7.2, §8) |
| Dependency/host tax (`syn`, SwiftSyntax) — §3.9 | Reuse the shipped oracle; no new host linked (§8, P-6) |
| Non-determinism (TH compile-time IO) — §3.6 | No I/O at comptime; fixed rule order; reproducible expansion (§7.2) |
| Dialect fragmentation (Roslyn/Rust fear) — §3.5 | Additive default; Layer-D rewrites marked & discouraged; rules scoped so no ambient global rewrite (§4.4, §5) |
| Macro-API instability strands libraries (Scala) — §3.8 | Minimal, versioned `meta.*` surface, not the raw internal AST (§6.3) |
| Span attribution soft spot (existing `$init`) — §16.5 | Generalized two-part span is a named prerequisite (P-8) |
| Rules-on-rules non-termination | No re-trigger by default; `reentrant` is gated opt-in with budget backstop (§8) |

---

## 12. Phasing / delivery plan

- **Phase 0 — Project system (P-1..P-4).** Manifest, multi-file gather, cross-file namespace
  resolution, retained `file→imports`. *No metaprogramming yet* — but the language finally builds
  real multi-file projects, which the web framework (Doc 2) also hard-requires. Highest-leverage
  work; unblocks everything.
- **Phase 1 — Comptime (Layer C) + Attributes (Layer A).** P-5 (quasiquote/`meta` structs, partial),
  P-6 (comptime driver). `comptime` const/table folding and `comptime if`; attributes that parse and
  type-check but need only project-local scope. Ship `--expand`.
- **Phase 2 — Rules (Layer B), single-pass syntactic matchers.** `match @Attr on method … inject …
  at anchor`, additive only, syntactic matchers (no interface predicates yet). Namespace scoping
  (§5) fully on. This is enough to power the web framework's route/DI/ORM ergonomics.
- **Phase 3 — Semantic matchers (two-pass) + expression macros (Layer D-lite).** `: IController`
  predicates, `where` comptime clauses (P-7 two-pass), `macro name(e)` expression macros.
- **Phase 4 — Body rewriters (Layer D) + polish.** `rewrites body of m`, `--rules`/`--no-rules`,
  full dual-span diagnostics (P-8), incremental per-file rule caching.

Each phase is independently useful and independently testable against the shared corpus (the
five-engine agreement discipline extends to "`--expand` output compiles and runs identically to the
hand-written equivalent").

---

## 13. Open questions (carried forward, refines `info.md` §19 #15)

1. **Attribute sigil final call:** `@Name` (recommended) vs `[Name]` (matches §16.5 / C#) vs both.
   Decide before Phase 1 ships attributes.
2. **Does this subsume `bind`/`inject`?** §16.5/§12.5 flag it. Tentative answer: **no, keep them
   distinct** — DI is a *value-flow* mechanism (a factory chosen by type, resolved at compile time),
   rules are a *code-shape* mechanism. A rule could *emit* a `bind`, but the two stay separate
   constructs (one rule over many special cases cuts both ways: don't collapse things that are
   genuinely different). Revisit once Layer B exists.
3. **Reentrancy:** do rules ever need to trigger on rule-generated code? Default no (§8); if a real
   case appears, gate it (`reentrant`) with the step budget.
4. **`meta.*` surface scope:** how much of the type/AST shape to expose — every field is a forever
   API (§3.8). Start minimal (§6.3), grow by demand.
5. **Matcher timing default:** syntactic-only (single-pass, fast) vs post-resolve (two-pass, richer)
   as the *default* — proposal defaults to syntactic and opts into two-pass per-rule (§8).
6. **`comptime` reach:** may comptime code call *user* functions freely (yes, hermetic) — but what
   about constructing reference `class` instances at compile time and reifying them? Value structs
   reify cleanly (§4.3); reference identity at comptime is fuzzier — restrict comptime results to
   value/`struct`/primitive/array initially.
7. **Incremental builds:** per-file rule-output caching (§3.6) — design the cache key
   (file hash + imported-rule-set hash) when Phase 4 lands.

---

### Headline recommendations (for the report)
1. **Keep the goal, change the substrate:** author-defined `@Route`→wiring is exactly right; replace
   raw-text anchors / `writeBelow("…")` / token `.nextToken` with **AST-level, hygienic,
   quasiquoted** rules — the entire cross-language record says textual is a dead end.
2. **Four layers over one substrate:** Attributes (terse use site) → Rules (match shape, inject
   quasiquoted AST at named anchors, **additive by default**) → Comptime (**run the language at
   compile time**, no second macro language) → Rewriters (marked bazooka room).
3. **Namespace binding via the existing `uses` graph:** a rule fires on a file iff the file imports
   the rule's namespace — computed at **file granularity** with a retained `file→imports` map. To
   know what magic can touch a file, read its `uses` list.
4. **Reuse what exists:** compile-time execution = the **already-shipped tree-walk oracle**, run
   hermetically before lowering; injected code goes through the normal resolver/checker so it is
   **cost-identical to hand-writing** (zero runtime reflection).
5. **The hard prerequisite is a project/file system, which does not exist yet** — manifest,
   multi-file gather, cross-file namespace resolution, and a retained `file→imports` provenance map
   are **blockers** (P-1..P-4) and must land as "Phase 0" before any namespaced rule is buildable.
