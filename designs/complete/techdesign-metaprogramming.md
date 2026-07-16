# Tech Design: Compile-Time Meta-Programming (the rule layer)

**Status:** **COMPLETE** — Phases 1–2 landed 2026-07-04; the separately designed
Phase 3 and Phase 4 work is also archived under `designs/complete/`. This is the
technical design for `designs/proposal-metaprogramming.md` (accepted). The proposal says *what and why*; this document
says *how*, at the level of files, structs, passes, grammars, diagnostics, and tests, grounded
in the codebase as of commit `4c9d4b9`.

**Reading order:** proposal §4 (four layers), §5 (namespace binding), §8 (pipeline) are assumed.
Section numbers like “P-4” refer to the proposal’s prerequisites; “§16.5” refers to `info.md`.

---

## 0. Table of contents

1. Current-state audit — what the substrate already provides
2. Design at a glance — components and pipeline placement
3. Surface syntax and grammar (lexer + parser changes)
4. AST changes (concrete struct deltas)
5. The RuleEngine (new component) — scoping, ordering, matching, expansion
6. The comptime driver — hermetic evaluation, budgets, reification
7. Hygiene mechanics
8. Diagnostics catalog and provenance
9. Pipeline integration (`main.cpp`) and the two-pass resolve
10. CLI surface
11. Engine/backend impact (none, by construction)
12. Testing plan
13. Delivery phases with file-level task lists
14. Deviations from the proposal (explicit, with rationale)
15. Risks
16. Decisions needed before Phase 1

---

## 1. Current-state audit

The proposal’s hard blocker — “a project/file system does not exist yet” — **no longer holds.**
Phase 0 (P-1..P-4) landed between proposal acceptance and this design:

| Prerequisite | Status | Where |
|---|---|---|
| P-1 manifest | **done** | `project { name/entry/sources/deps/... }` in-language literal syntax — `Project.hpp:19-54`, `parseManifest` |
| P-2 multi-file gather | **done** | `loadProject` concatenates all sources (+ local-path deps) into one `SourceFile combined` with a per-file offset map (`ProjectFile`) — `Project.hpp:58-90` |
| P-3 include graph + build order | **done** | `UsesGraph`, `buildUsesGraph`, SCC-condensed deterministic order, `--graph` — `Project.hpp:151-186` |
| P-4 file→imports provenance | **done** | `FileImports { declaresInto, importsExplicit, effective }`, `computeFileImports`, `--imports` — `Project.hpp:108-149` |

Other substrate facts this design leans on (verified against source):

- **Pipeline** (`main.cpp:111-160`): `lex → parse → [validateEntry + checkPhantomDeps for
  projects] → Resolver.run → Checker.run → {Eval | Lower → 5 engines}`. One `SourceFile`
  carries the whole program; **all spans are byte ranges into that one combined buffer**
  (`Source.hpp:6-11`), and project diagnostics are re-attributed to files by offset
  (`renderProjectDiagnostics`).
- **AST** (`Ast.hpp`): single tagged `Stmt`/`Expr` structs with flags, `unique_ptr` ownership,
  `Program = vector<StmtPtr>`. House style is *flags on the umbrella node*, not new node
  classes.
- **Resolver** (`Resolver.hpp`): fresh instance per run; owns its `Sema` (symbol/scope arena)
  and re-parses the prelude (`parsePrelude`). Nothing prevents constructing a second Resolver
  over a mutated tree — resolution annotations (`TypeRef::canonical`, `resolvedSymbol`) are
  plain overwritable fields.
- **Checker** fills `Expr::resolved` (chosen overload) and `resolvedClass`; the **Evaluator
  falls back to dynamic by-name lookup when `resolved` is null** (`Eval.cpp:483,528,533,563`).
  This is what makes “evaluate comptime code after resolve pass 1, before any check” viable.
- **The oracle is reusable as the comptime engine**: `Evaluator(sema, sink)` +
  `initGlobals(prelude)` + call machinery (`callFunction`, `construct`) — `Eval.hpp`. Native
  I/O bottlenecks through `nativeCall` / `RuntimeNatives.cpp` — a single choke point to gate.
- **Diagnostics** already have `Severity::{Error, Warning, Note}` and a non-throwing sink
  (`Diagnostic.hpp`).
- **`$init` precedent**: constructor field-init synthesis happens at **lower time**
  (`Lower.cpp:136-143,247`), not as AST injection. So the rule stage’s AST-level injection is
  *new* machinery, but the “insert code into constructors” concept is proven, and rules
  compose with `$init` naturally (injected ctor statements lower after `$init` exactly like
  hand-written ctor bodies).
- **Value model** for reification: `VKind { Void, Int, Float, Bool, String, Object, Closure,
  Array, Map, None }` (`RuntimeValue.hpp:22`).
- **Test harness**: `tests/run_corpus.sh` drives any engine flag over `tests/corpus/`;
  `run_project.sh` covers manifest builds; unit tests per phase (`test_parser.cpp`, …).

**Consequence:** this design starts directly at the proposal’s Phase 1 (comptime + attributes)
and Phase 2 (rules). No project-system work remains in scope here.

---

## 2. Design at a glance

One new compiler component and one modified engine:

```
                       ┌────────────────────────────────────────────────┐
 lex ─ parse ──────────│ P A S S  1   Resolver #1  (today's resolver)   │
  │                    └────────────────────────────────────────────────┘
  │  (parser sets Program.hasMeta if it saw @/rule/comptime/macro)
  │
  ├─ hasMeta == false ──────────────► Checker ─ lower ─ engines   (byte-identical
  │                                                                to today's path)
  └─ hasMeta == true:
        ┌──────────────────────────────────────────────────────────────┐
        │ RULE STAGE (new: src/Rules.{hpp,cpp}, class RuleEngine)      │
        │  1. files+FileImports  (reuse computeFileImports — P-4)      │
        │  2. collect attribute decls, rules, macros; strip them       │
        │  3. order rules   (UsesGraph namespace topo, then decl order)│
        │  4. run comptime  (Evaluator in hermetic mode — §6)          │
        │  5. match + expand (clone template, substitute holes,        │
        │     hygiene-rename, stamp provenance — §5.5/§7)              │
        │  6. inject at anchors; detect conflicts; dangling-attr warns │
        └──────────────────────────────────────────────────────────────┘
        ┌──────────────────────────────────────────────────────────────┐
        │ P A S S  2   Resolver #2 (fresh Sema) + Checker              │
        │  injected code resolves/checks/lowers like hand-written code │
        │  (P1 cost-identity: nothing below the AST changes at all)    │
        └──────────────────────────────────────────────────────────────┘
                                     │
                              lower ─ engines (unchanged)
```

Component inventory:

| Component | File(s) | New/changed |
|---|---|---|
| Tokens: `@`, quasiquote literal, `$`-holes (fragment mode) | `Token.hpp`, `Lexer.{hpp,cpp}` | changed |
| Attribute uses/decls, rules, comptime, quasi parsing | `Parser.{hpp,cpp}`, `Ast.hpp` | changed |
| RuleEngine (scoping, matching, expansion, injection) | `src/Rules.{hpp,cpp}` | **new** |
| Comptime driver (hermetic flag, step budget, reifier) | `Eval.{hpp,cpp}`, `RuntimeNatives.cpp` | changed |
| Driver modes `--expand`, `--rules`, `--no-rules` | `main.cpp` | changed |
| AST printing of attrs/rules/provenance | `AstPrinter.cpp` | changed |
| Build | `CMakeLists.txt` (+`Rules.cpp`) | changed |
| Resolver / Checker / Lower / engines | — | **skip-only changes** (ignore consumed meta nodes) |

---

## 3. Surface syntax and grammar

### 3.1 Token-level changes (`Token.hpp`, `Lexer`)

Exactly **three** lexical additions; no new reserved words:

1. **`At` token (`@`).** Unused today (`reference.md` §1.5). Lexes unconditionally.
2. **`QuasiLiteral` token (`` `…` ``).** Backtick-delimited raw capture, like a string literal
   but with no escape processing except `` \` `` (literal backtick) — the payload is re-lexed
   as language source, so its own strings/escapes must pass through untouched. The token’s
   `text` is the slice *between* the backticks; its span covers the whole literal. Newlines
   allowed (templates are frequently multi-line).
3. **`Dollar` token (`$`) — fragment mode only.** `Lexer` gains a constructor flag
   `allowHoles` (default `false`). The main lex never produces `Dollar`; only the fragment
   re-lex inside quasiquotes does (§3.4). Outside fragment mode `$` stays an error character,
   so no existing program changes meaning.

**Keyword policy — everything else is contextual.** `rule`, `attribute`, `macro`, `comptime`,
`rewrites`, `on`, `of`, `at`, `top`, `bottom`, `member`, `constructor`, `one`, `where`,
`marker` are **ordinary identifiers** given meaning by parse position, exactly like the
existing “`get`/`set` are keywords only at declaration start” stance (`reference.md` §3.2 —
here taken further: not keywords at all). Rationale: zero breakage of existing programs, no
growth of the reserved set, and the rule grammar is closed enough that one-token lookahead
disambiguates every case (§3.3). The rule-body verbs reuse the **existing** `KwMatch` and
`KwInject` tokens — no collision, because rule bodies are their own parse context.

### 3.2 Attributes

```
AttrUse       ::= '@' AttrPath ( '(' ArgList? ')' )?
AttrPath      ::= Identifier ( '::' Identifier )*          // @Route or @Web::Route
AttrArgs      ::= Expr ( ',' Expr )*                        // positional; comptime-evaluable
AttributeDecl ::= AccessMod? 'attribute' Identifier '{' FieldDecl* '}'
```

- **Placement:** zero or more `AttrUse` immediately before a declaration: class, struct,
  interface, member (field/method/ctor/accessor), namespace, or top-level function. The parser
  collects them in `parseTopLevelItem` / `parseClassMember` and attaches to the produced
  `Stmt` (§4).
- **Arguments are positional against the attribute’s fields in declaration order**; trailing
  fields with initializers are defaultable (`@Column` ≡ all defaults). *Named* arguments are
  deferred until the language itself has named arguments (deviation §14.4).
- **`attribute` declarations** parse as a class-shaped body restricted to fields (a method,
  ctor, or accessor inside `attribute { }` is a parse-time error). Internally it *is* a class
  node with `isAttribute = true` — one member concept, one node (§4).
- Disambiguation: `attribute` at declaration position followed by `Identifier '{'` — nothing
  else in the grammar matches that shape, so a variable or class named `attribute` still works.
- `@anchor("name")` in *statement* position is reserved (marker anchors, Phase 3); the parser
  accepts and stores it from Phase 2 so corpora don’t churn.

### 3.3 Rules

```
RuleDecl   ::= 'rule' Identifier 'rewrites'? '{' MatchClause InjectClause+ '}'
MatchClause::= 'match' 'one'? AttrPat? 'on' DeclKind Identifier
               ( 'in' DeclKind Identifier ( ':' TypeRef )? )*
               ( 'where' Expr )?                            // Phase 3
AttrPat    ::= '@' AttrPath ( '(' Identifier ')' )?         // binds the attr value
DeclKind   ::= 'method' | 'function' | 'class' | 'struct' | 'field'
             | 'constructor' | 'interface' | 'namespace'
InjectClause ::= 'inject' QuasiLiteral 'at' Anchor
Anchor     ::= ('top' | 'bottom') 'of' Identifier '.' 'constructor'
             | 'member' 'of' Identifier
             | ('top' | 'bottom') 'of' 'body'               // Phase 3
             | 'marker' StringLit                           // Phase 3
             | 'namespace' Identifier                       // Phase 3
```

- `rule` is recognized at declaration position by the triple
  `Identifier("rule") Identifier ('{' | 'rewrites')`.
- The identifiers in anchors (`C`, `m`) must be names bound by the match clause; `constructor`
  after `.` selects **every** constructor of the bound class (§5.6).
- `rewrites` (Layer D, Phase 4) changes the verb set inside the body (`replace` instead of
  `inject`); grammar reserved now, rejected with “Layer D lands in Phase 4” until then.
- `where` takes an ordinary expression evaluated at comptime with the match bindings in scope
  as `meta.*` values (Phase 3, §14.3).

### 3.4 Quasiquotes and holes

A `QuasiLiteral`’s payload is parsed **at rule-parse time** (immediately after `Parser`
produces the enclosing rule/macro node — not lazily at expansion) by a **fragment re-lex**:

- Because the payload is a slice of the *same combined buffer*, the fragment lexer runs over
  `[span.offset+1, span.end()-1)` of the original text with `allowHoles = true` — **all spans
  inside the template are ordinary spans into the ordinary buffer.** Template parse errors
  render like any other diagnostic, pointing into the rule’s source (proposal §7.3
  “template errors are rule-site errors” falls out for free).
- Fragment grammar entry points (new public `Parser` methods, P-5):
  - `parseExprFragment()` — for macro bodies and expression templates;
  - `parseStmtsFragment()` — statement list, for body/ctor anchors;
  - `parseMemberFragment()` — one member declaration, for `member of C` anchors.
  The anchor kind selects the entry point (ctor/body anchors → statements; `member of` →
  member; macro → expression).
- **Holes lex as identifiers spelled `$name`**: fragment mode fuses `Dollar + Identifier` into
  a single `Identifier` token whose text includes the `$`. No new AST node kind — a hole is a
  `Name` (or member-name) whose text begins with `$`. This keeps the template a 100% ordinary
  AST; only the substitution pass (§5.5) treats `$`-names specially. `$`-names surviving to
  pass 2 (unbound holes) are impossible by construction — substitution errors first (§8).
- `$for` list-splice (`` `[ $for c in cols: expr ]` ``) is **Phase 3**, parsed as a dedicated
  fragment form then.

### 3.5 Comptime

```
ComptimeVar  ::= 'comptime' VarDecl                 // comptime int N = f();
ComptimeExpr ::= 'comptime' Expr                    // in expression position
ComptimeIf   ::= 'comptime' 'if' '(' Expr ')' Stmt ('else' Stmt)?
```

`comptime` is contextual: at statement position, `Identifier("comptime")` followed by
`if` / a var-decl shape / an expression selects the form (same lookahead machinery as
`looksLikeVarDecl`). A class named `comptime` would be shadowed in these positions — accepted
and documented (reserved-in-practice).

`comptime if` restriction (v1): its branches may not contain `uses` — import-set mutation
during the rule stage would invalidate the already-computed FileImports/rule scoping
(deviation §14.6). Diagnostic: *“`uses` inside `comptime if` is not supported yet.”*

### 3.6 Macros (Phase 3)

```
MacroDecl ::= 'macro' Identifier '(' IdentList? ')' '=>' QuasiLiteral ';'
MacroCall ::= Identifier '!' '(' ArgList? ')'        // postfix-bang call form
```

`name!(args)`: the Pratt postfix loop treats `Bang` followed immediately by `LParen` after a
name as a macro call (prefix `!` is unary-not on the *next* primary; postfix bang before `(`
is currently a parse error, so the form is free). Arguments are ordinary parsed expressions —
captured once as nodes (kills double-evaluation by construction).

---

## 4. AST changes (`Ast.hpp`)

House style: extend the two umbrella nodes with flags/fields; add **no** new node classes.
Concrete deltas:

```cpp
// --- attributes -------------------------------------------------------------
struct AttrUse {
    std::vector<std::string_view> path;   // qualifier segments (may be empty)
    std::string_view name;                // final segment: "Route"
    std::vector<ExprPtr> args;            // positional, comptime-evaluable
    SourceSpan span;
    // filled by the rule stage:
    Symbol* resolved = nullptr;           // the attribute class symbol (pass-1 Sema)
    bool consumed = false;                // some in-scope rule matched through it
};

enum class StmtKind {
    Namespace, Class, Member, Bind,
    Rule,                                  // NEW: rule declaration (stripped pre-pass-2)
    Var, Block, ExprStmt, Return, If, While, For, ForIn, Use, UsesImport,
    Try, Throw, Empty,
};

// --- on Stmt ------------------------------------------------------------------
std::vector<AttrUse> attrs;      // attributes attached to this declaration
bool isAttribute = false;        // Class node: declared with `attribute`
bool isComptime  = false;        // Var/If/ExprStmt: comptime-marked (§3.5)

// Rule (StmtKind::Rule) — reuses `name`; adds:
struct RuleMatch {
    bool one = false;
    // attribute pattern (optional)
    bool hasAttr = false;
    std::vector<std::string_view> attrPath;
    std::string_view attrName, attrBind;   // @Route(r): name="Route", bind="r"
    // subject + enclosers
    struct Level { std::string_view kindWord; std::string_view bind;
                   TypeRefPtr constraint; SourceSpan span; };
    Level subject;                          // on method m
    std::vector<Level> enclosers;           // in class C : IController
    ExprPtr where;                          // Phase 3
    SourceSpan span;
};
struct RuleAction {
    enum class AnchorKind { CtorTop, CtorBottom, MemberOf,
                            BodyTop, BodyBottom, Marker, NamespaceScope };
    AnchorKind anchor;
    std::string_view target;               // bound name the anchor refers to ("C", "m")
    std::string_view markerName;           // Marker anchors
    // the parsed template (fragment kind implied by anchor):
    std::vector<StmtPtr> tmplStmts;        // statement-list fragments
    StmtPtr tmplMember;                    // member fragment
    ExprPtr tmplExpr;                      // expression fragment (macros)
    SourceSpan quasiSpan;
};
std::unique_ptr<RuleMatch> ruleMatch;       // on StmtKind::Rule
std::vector<RuleAction> ruleActions;
bool ruleRewrites = false;                  // Layer D marker (Phase 4)

// --- Program -----------------------------------------------------------------
struct Program {
    std::vector<StmtPtr> items;
    bool hasMeta = false;    // parser saw @ / rule / comptime / macro — gates the stage
};
```

Notes:

- `RuleMatch`/`RuleAction` live in `Ast.hpp` (they are parse products) but are *interpreted*
  only by the RuleEngine. `Resolver`, `Checker`, `Lower` each get a one-line
  `case StmtKind::Rule: break;` — and in practice never see one, because the RuleEngine
  detaches rule nodes from the tree before pass 2 (§5.7).
- Macro declarations (Phase 3) reuse `StmtKind::Rule` with a `isMacroDecl` flag rather than a
  new kind — a macro is a degenerate rule (match = call site, action = expression template).
- No `provenanceId` field in Phases 1–3 (range-based attribution instead, §8.2); Phase 4 adds
  `uint32_t provenanceId = 0;` to `Stmt`/`Expr` if the range-based story proves insufficient.

---

## 5. The RuleEngine (`src/Rules.hpp` / `Rules.cpp`, new)

```cpp
struct ExpansionRecord {              // one rule firing (drives --expand/--rules/§8)
    const Stmt* rule;                 // the rule decl
    std::string ruleName;             // "Web::registerRoutes"
    SourceSpan origin;                // the attribute / matched decl at the use site
    SourceSpan templateSpan;          // the quasiquote's span (rule site)
    SourceSpan anchorSite;            // where nodes landed
    int fileIndex;                    // owning file of the origin
};

class RuleEngine {
public:
    RuleEngine(const std::vector<ProjectFile>& files,
               const std::vector<FileImports>& imports,
               const UsesGraph& graph,
               Sema& pass1Sema, Program& prelude,
               const SourceFile& file, DiagnosticSink& sink);

    // Runs comptime + rules over `program`. Returns true if the tree changed
    // (caller then re-resolves). Never throws; failures are sink diagnostics.
    bool run(Program& program);

    const std::vector<ExpansionRecord>& expansions() const;
    std::string renderRulesReport() const;          // --rules
private:
    // …§5.1–§5.7 below…
    std::deque<std::string> synthNames_;            // owns gensym'd identifier text
    std::deque<std::string> reifiedText_;           // owns literal text for reified values
};
```

The two `deque<string>` members answer a codebase-specific constraint: **`string_view`s in the
AST point into source text.** Synthesized identifiers (`__r3_tmp`) and reified literals
(`"1009"`) have no source text to point at, so the engine owns their backing storage for the
life of the compilation (deques never invalidate on growth). This is load-bearing; a vector
would dangle.

### 5.1 Collection and stripping

One walk over `program.items` (recursing into namespaces only — rules/attribute decls are
namespace-level declarations):

- **Attribute decls** (`isAttribute`) — recorded in a name→symbol table per namespace. They
  *stay in the tree* (pass 2 gathers them as ordinary class symbols; `Lower` skips
  `isAttribute` classes since nothing constructs them at runtime).
- **Rules** — detached from the tree into the engine (`std::vector<OwnedRule>`), remembering
  their declaring namespace and declaration index. Detaching (rather than skip-flagging)
  guarantees pass 2 / lowering literally cannot see them, and avoids dangling pass-1
  `Symbol*`s inside rule bodies after pass-1 `Sema` is discarded.
- **Declaration index** — every attributed or matchable declaration, recorded with its owning
  file index (binary search of `stmt->span.offset` over the `ProjectFile` offset map — same
  technique `renderProjectDiagnostics` uses).

### 5.2 Scoping (proposal §5, verbatim onto P-4)

Rule `R` declared in namespace `N` may fire on declaration `D` iff
`N ∈ imports[fileOf(D)].effective`. That is **one set-membership test against data that
already exists** (`FileImports::effective` includes declaresInto ∪ importsExplicit ∪ std).
Single-file (non-project) builds synthesize a one-entry `files` vector exactly as the
`--imports` mode already does (`main.cpp:121-127`), and scoping degenerates to “everything in
the file sees the file’s own namespaces” — the proposal’s intended beachhead behavior.

Attribute-name resolution at a use site follows the same rule: search the using file’s
`effective` namespaces for an `attribute` symbol named `X` (qualified `@Web::Route` bypasses
the search). Zero hits → error; multiple hits → ambiguity error naming the candidates and the
qualification syntax.

### 5.3 Ordering (proposal §5.4)

Deterministic total order over rule firings:

1. **Namespace order:** the topological order of the declaring namespaces, derived from the
   already-built `UsesGraph` component order (`Project.hpp:172-186`); ties (same SCC, or
   namespaces with no edges) break by first-declaration source offset — stable because the
   gather order is the manifest’s `sources` order.
2. **Within a namespace:** rule declaration order (source offset).
3. **Within a rule:** matched declarations in program order (source offset), and for one
   declaration carrying N matching attributes, attribute order left-to-right (each firing
   independently — multiple `@Route`s on one method register multiple routes, per proposal
   §5.4; `match one` makes >1 a hard error).

Injections appended at one anchor accumulate in firing order. The order is *recorded* in the
`ExpansionRecord` list, so `--expand` output and diagnostics are reproducible byte-for-byte.

### 5.4 Matching

A match test for `(rule, decl)` is purely structural against pass-1 facts:

- decl-kind test (`on method` ⇒ `Stmt` is `Member`+`callable`, not ctor/accessor; `on class`
  ⇒ `StmtKind::Class` non-interface; etc. — a small closed table);
- attribute test: an `AttrUse` on the decl whose `resolved` symbol equals the rule’s attribute
  symbol (resolved per §5.2);
- encloser chain: walk the recorded parent chain (the collection walk records
  decl→enclosing-class/namespace); each `in <kind> <bind>` level must match, and a
  `: IFace` constraint checks the encloser’s **pass-1 resolved base list** — the resolver has
  already resolved `bases` TypeRefs and built shapes, so “implements, directly or through the
  base chain” is answerable from `Symbol::shape` + base symbols without new analysis.
- On success, produce `Bindings`: name → one of
  `{ AttrValue (comptime Value), DeclRef (const Stmt* + Symbol*) }`.

Matcher *mismatch* on an attribute the rule declares (e.g. `@Route` sits on a field) is the
proposal-§7.3 use-site error: *“`@Route` applies to methods; `count` is a field (rule
`Web::registerRoutes` at web/routing.ext:7).”* Emitted once per offending use, pointing at the
attribute span, with a `Note` at the rule span.

### 5.5 Expansion: evaluate → clone → substitute

Per firing:

1. **Evaluate the attribute value** (once per AttrUse, cached on it): construct the attribute
   class via the comptime evaluator (§6) — arguments are evaluated hermetically, assigned to
   fields positionally, defaults from field initializers. Result: a `Value` (an `Object` of
   the attribute class). Non-comptime-evaluable argument → error at the argument span.
2. **Clone the template fragment** (`tmplStmts`/`tmplMember`): a deep structural copy
   (`cloneStmt`/`cloneExpr`/`cloneType` — new, mechanical, in `Rules.cpp`). Cloned nodes keep
   their template spans (rule-site provenance for free, §8.2).
3. **Substitute holes** during the clone walk. A `Name` or member-name whose text starts with
   `$` resolves against the bindings:
   - **Decl-hole in name position** (`this.$m(req)` → member name): splice the bound decl’s
     selector text. The clone of the `Member` node gets `text = m->selector.text` (a view into
     real source — no synthesis needed).
   - **Value-hole root** (`$r`): the substitution walker folds the *maximal postfix chain* of
     field accesses rooted at the hole (`$r.method`) by reading the comptime `Object`’s fields,
     then **reifies** the resulting `Value` to a literal node (§6.3). `$r` alone (whole-struct
     splice) is an error in v1 — only leaf fields reify (deviation §14.5).
   - **Decl-hole in expression position** (`$C` as a type/name): splice a `Name`/`TypeRef`
     referring to the bound symbol, spelled with its qualified path (hygiene-safe: qualified
     names bypass import ambiguity).
   - Unbound `$x`: error at the template span naming the rule’s available bindings.
4. **Hygiene pass** over the cloned fragment (§7).
5. **Anchor placement** (§5.6).

### 5.6 Anchors (v1 set: `CtorTop`, `CtorBottom`, `MemberOf`)

- **`bottom of C.constructor`** — for **each** constructor of the bound class: append cloned
  statements after the last statement of the ctor body. Body shapes normalize first:
  a block body appends in place; an arrow/single-statement body wraps into a Block
  `{ original; injected... }`. **A class with no declared constructor gets one synthesized**:
  `new C() { }` appended to the class body (the language guarantees the implicit nullary path,
  `info.md` §3 — this makes it explicit so there is a body to inject into; `$init` still
  handles field defaults at lower time, unchanged).
- **`top of C.constructor`** — same, but inserted **after the trailing run of base-ctor
  calls** (`Base::Ctor(...)` expression-statements at the head of the body), per proposal
  §4.2’s table (“post base-ctor calls”). If there are none, position 0.
- **`member of C`** — append the cloned member `Stmt` to the class body. **Conflict check at
  injection time:** if the class already declares (or another rule already injected) a member
  with the same name *and* same canonical type, that is a **rule conflict error** naming both
  rules (or the rule and the user decl) — the proposal-§5.4 reuse of the distinct/collision
  rule, surfaced eagerly with rule context rather than letting pass 2 report a bare collision.
  Different-type same-name members coexist (resolution by type, as everywhere).
- Phase 3 adds `BodyTop`/`BodyBottom` (matched method; `BodyBottom` on an arrow body whose
  value returns is an error — unreachable injection), `Marker`, and `NamespaceScope`.

### 5.7 After expansion

- Attribute uses that never matched any in-scope rule (and whose name resolved to an
  attribute) get the **dangling-attribute warning** with the likely-missing `uses` named:
  scan which visible-anywhere namespaces declare a rule reading that attribute and suggest the
  nearest (proposal §4.1/§7.3). Attributes stay attached to the AST (inert data, printable).
- Rules are already detached (§5.1). `comptime` markers are consumed (§6). The tree handed to
  pass 2 contains only ordinary language + attrs (ignored by Checker) + `isAttribute` classes.
- The engine reports `changed = (expansions ∪ comptime folds) ≠ ∅`; `main.cpp` skips pass 2
  when nothing changed (attributes alone with no firing rules still skip — they’re inert).

---

## 6. The comptime driver (`Eval` changes)

The proposal’s P-6, as a thin mode on the existing oracle — **no new engine**:

```cpp
struct ComptimeOptions {
    bool hermetic = true;
    long long stepBudget = 25'000'000;     // default; --comptime-budget overrides
};
class Evaluator {
    // existing…
    void setComptime(const ComptimeOptions& o);   // enables the three gates below
    …
};
```

Three gates, all at existing choke points:

1. **Hermeticity** — `nativeCall` (`Eval.cpp`) checks a deny-set when comptime mode is on:
   every `sys*` native (write/read/open/close/stat/recv/send/watch/timer floor,
   `reference.md` §6.6.5+) and the event-loop entry points. Violation → a *compile* diagnostic
   at the current call span: *“comptime code may not perform I/O (`sysOpen` called via rule
   `Orm::buildSchema`)”* — and evaluation of that firing aborts (the rule contributes
   nothing; compilation continues to collect further errors).
   **Exception:** `console.write/writeln` at comptime are *allowed* and routed to **stderr**
   prefixed `[comptime] ` — Zig’s `@compileLog` ergonomic for rule debugging at near-zero
   cost. They do not count as output for `--run` capture.
2. **Step budget** — a counter decremented in `exec`/`eval` dispatch; exhaustion → *“comptime
   step budget exceeded (25000000) in rule `X` / comptime initializer at …; runaway loop?”*
   Applies per rule-stage run (shared pool), so N small rules can’t be starved by one runaway.
3. **Reification** (`Value` → AST literal, §5.5/§3.5): 
   `Int/Float/Bool/String → literal Expr` (text materialized into `reifiedText_`);
   `Array → ExprKind::Array` of reified elements; `None → Name("None")`.
   `Object/Closure/Map` → error *“comptime result of type X is not reifiable (v1: primitives,
   arrays, None)”* — the proposal’s open-question 6 resolved conservatively (§14.5; struct
   reification arrives Phase 3 as a labeled constructor call).

**What runs under the driver, when:**

- `comptime` var initializers and `comptime` expressions, in program order, **before** rule
  matching (rules may read the folded constants via attribute args).
- `comptime if`: evaluate the condition; splice the taken branch’s statement in place of the
  node (untaken branch is dropped from the tree — the principled `.clear()`); branches were
  parsed normally so the dropped branch still had to *lex/parse* (syntax errors are never
  conditional).
- Attribute-argument evaluation and (Phase 3) `where` clauses, per firing.
- Rule action bodies in v1 are *templates only* (no imperative rule bodies) — the quasiquote +
  holes cover Phase-2 scope; imperative helpers around templates ride on ordinary functions
  called from `where`/args (they run under the same driver).

The driver is constructed once per rule stage over **pass-1 `Sema`** with
`initGlobals(resolver1.preludeProgram())`. The §1 audit fact makes this sound: the oracle
resolves calls dynamically when `Expr::resolved` is null, so *checker-less* evaluation works;
a type error inside comptime code surfaces as a comptime diagnostic at the offending span
(good enough for v1; targeted pass-1 checking of comptime roots is Phase 4 polish).

---

## 7. Hygiene mechanics (proposal §7.1, made concrete)

Two mechanisms, both applied during the clone-substitute walk:

1. **Fresh temporaries (alpha-rename).** Any local the template *declares* (`Var` stmt, lambda
   param, `for` induction variable, catch binding) is renamed to a gensym
   `__r<ruleIdx>_<n>_<origName>` (text owned by `synthNames_`), and every in-template
   reference to it follows. Use-site code cannot collide with or capture template locals, and
   vice versa. The `__r` prefix is unlexable as a user identifier by convention only — v1
   accepts that (a user *could* write `__r1_0_t`); if it ever matters, the gensym text gains a
   NUL-adjacent char that the lexer can’t produce, which only synthesized views can hold.
2. **Definition-site qualification.** After substitution, every remaining free `Name` in the
   template (not a hole result, not a template-local, not `this`, not a parameter of an
   enclosing template lambda) is resolved **against the rule’s namespace scope chain in
   pass-1 Sema**. If it resolves to a namespace-level entity (function, class, global), the
   node is rewritten to its **fully qualified** form (`Member(Name(NS), ::, x)` chain), which
   pass 2 resolves identically regardless of what names the injection site shadows. If it does
   not resolve in the rule’s scope, it is left bare and pass 2 resolves it at the injection
   site — this is the *deliberate* channel for `this`-relative member references like
   `this.router` (proposal templates lean on it), and for prelude names, which resolve the
   same everywhere. Member names after `.` are never touched (they resolve by receiver type —
   resolution-by-type is itself hygiene for the member namespace).

What is *not* implemented (explicitly out): capture of arbitrary use-site names from
templates (“anaphora”) — the only use-site names a template can reference are the matcher
bindings, exactly the proposal’s stance.

---

## 8. Diagnostics catalog and provenance

### 8.1 New diagnostics (complete v1 list)

| # | Sev | Trigger | Message shape (spans) |
|---|---|---|---|
| M01 | E | unknown attribute name | “no attribute `Rte` in scope; nearest: `Route` (Web)” @AttrUse |
| M02 | E | ambiguous attribute | “`@Route` is ambiguous: Web::Route, Rpc::Route — qualify (`@Web::Route`)” @AttrUse |
| M03 | E | attr args don’t fit fields | “`@Route` takes (string, string); got (string)” @AttrUse |
| M04 | E | attr arg not comptime-evaluable / not reifiable | @arg |
| M05 | W | dangling attribute | “`@Route` matched no imported rule; missing `uses Web`?” @AttrUse |
| M06 | E | matcher kind mismatch (attr present, wrong decl kind) | “`@Route` applies to methods; `count` is a field” @AttrUse + Note @rule |
| M07 | E | `match one` violated | “rule `X` requires at most one `@Route`; found 2” @2nd AttrUse |
| M08 | E | rule conflict at anchor (same name+type member; Phase-4: double body-rewrite) | both rule spans as Notes |
| M09 | E | unbound hole | “`$path` is not bound by this rule’s match (bindings: r, m, C)” @template |
| M10 | E | hole shape misuse (value-hole in name position, `$r` whole-struct splice) | @template |
| M11 | E | template fragment parse/shape error | ordinary parse diagnostic @rule source |
| M12 | E | comptime I/O (hermeticity) | “comptime code may not perform I/O (`sysOpen`)” @call, Note @rule/site |
| M13 | E | comptime budget exhausted | @rule or @comptime site |
| M14 | E | comptime result not reifiable | @site |
| M15 | E | `uses` inside `comptime if` | @uses |
| M16 | E | anchor target unbound / wrong kind (“`member of m` — `m` is a method”) | @anchor |
| M17 | E | attribute decl contains non-field member | @member |
| M18 | E | `rewrites` before Phase 4 / unknown anchor keyword | @rule |

All errors are sink diagnostics (never aborts); the stage completes as much as it can, pass 2
runs only if the stage produced no errors (a half-expanded tree must never reach lowering).

### 8.2 Provenance (P-8), v1 = range-based, exact IDs deferred

Injected nodes carry **template spans** (they’re clones — §5.5), so any pass-2 diagnostic in
injected code already points at readable rule source. The missing half — *which use site
triggered it* — is recovered at **render time**, with zero threading through
Resolver/Checker:

- `RuleEngine::expansions()` is handed to the driver; the render step
  (`renderProjectDiagnostics` and single-file `sink.render`) checks each diagnostic’s span for
  containment in any `ExpansionRecord::templateSpan`. On hit, it appends Notes:
  *“in expansion of rule Web::registerRoutes — triggered by `@Route` at app/users.ext:8”* —
  listing up to 3 origins when one template expanded at several sites (the known ambiguity:
  N clones share one template span). Render signatures gain an optional
  `const std::vector<ExpansionRecord>*`.
- Phase 4 (if the 3-origin ambiguity bites in practice): `provenanceId` stamped per clone
  (§4), giving exact one-origin attribution; the render-side plumbing stays identical.

`--expand` prints each injection under a provenance banner from the same records
(`// from rule Web::registerRoutes @ app/users.ext:8`).

---

## 9. Pipeline integration (`main.cpp`) and the two-pass resolve

Sketch of the changed driver section (replacing `main.cpp:141-148`):

```cpp
Resolver resolver1(file, sink);
resolver1.run(program);

std::unique_ptr<RuleEngine> engine;
bool expanded = false;
if (program.hasMeta && mode != NoRules && !sink.hasErrors()) {
    std::vector<ProjectFile> files = project.files;         // single-file: synthesize,
    if (files.empty()) files.push_back({file.name, 0,       // exactly as --imports does
                        (uint32_t)file.text.size(), "", ""});
    auto imports = computeFileImports(files, program);
    auto graph   = buildUsesGraph(imports);
    engine = std::make_unique<RuleEngine>(files, imports, graph,
                 resolver1.sema(), resolver1.preludeProgram(), file, sink);
    expanded = engine->run(program);
}

if (mode == Expand) { std::printf("%s", printProgram(program,
                          engine ? &engine->expansions() : nullptr).c_str()); return …; }
if (mode == Rules)  { std::printf("%s", engine ? engine->renderRulesReport().c_str()
                                               : "no rules\n"); return …; }

Resolver resolver2(file, sink);                    // pass 2 — only if the tree changed
Resolver& R = expanded ? (resolver2.run(program), resolver2) : resolver1;
if (needsCheck(mode)) { Checker checker(R.sema(), file, sink); checker.run(program); }
// … existing Eval / Lower / engines consume R.sema() / R.preludeProgram() unchanged …
```

Key properties:

- **Zero-cost preserved path:** `hasMeta == false` (every existing program) runs *exactly*
  today’s pipeline — one resolve, no engine construction, no second pass. The corpus proves
  this stays true (§12).
- **Pass-2 re-runnability:** a fresh `Resolver` builds a fresh `Sema`; AST annotations
  (`canonical`, `resolvedSymbol`, later `resolved`) are overwritten wholesale. The only
  pass-1 pointers that could dangle after `resolver1`’s Sema is superseded live inside
  detached rule nodes (owned by the engine, never re-walked) and inside `ExpansionRecord`
  (spans and strings only, no `Symbol*`). The engine keeps `resolver1` alive by reference
  until end of compile (`main` scope), so even those are safe.
- **Uniform staging:** the rule stage always runs post-resolve-1 (never “syntactic-only before
  resolve”) — one code path, and `: IFace` constraints work from Phase 2 (deviation §14.1).
- `--no-rules` = force-skip the stage (escape hatch + A/B debugging, proposal §6.4). With
  rules skipped, rule decls would reach pass 2 — the engine still runs in “collect+strip
  only” mode (no matching, no comptime) so the tree stays lowerable.

---

## 10. CLI surface (driver mode enum additions)

```
lang --expand   file.ext | --project m.ext   # post-rule AST with provenance banners
lang --rules    file.ext | --project m.ext   # per-file: rules in scope, what each matched
lang --no-rules file.ext | --project m.ext   # compile with matching/comptime disabled
lang --comptime-budget N …                   # override step budget (tests use tiny N)
```

`--expand` prints via `AstPrinter` (extended to render attrs, comptime folds, and — given the
expansion records — provenance comments). It is an **AST dump, not source** in v1; a
source-shaped pretty-printer is Phase-4 polish (§14.7). `--ast` continues to show the
*pre*-rule tree (attributes and rules included, templates as quasi nodes) — the pair
`--ast` / `--expand` brackets the stage.

---

## 11. Engine/backend impact: none, by construction

The stage runs strictly above the IR: pass 2 hands `Lower` an ordinary resolved+checked tree.
No change to `Ir.hpp`, `IrInterp`, `CGen`, `LlvmGen`, `X64Gen`, ownership analysis, or the
runtime loop. Injected calls get `$init` interplay (ctor-bottom statements lower after field
init exactly like hand-written ctor statements), fixed-offset field access, escape analysis,
and identical machine code — P1 cost-identity is a *structural* consequence, and §12’s twin
tests verify it empirically on every engine. Two skip-cases only: `Lower` ignores
`isAttribute` classes (never constructed; emitting them would only waste bytes), and — belt
and braces — `case StmtKind::Rule: break;` in the three tree walkers, unreachable after §5.7.

---

## 12. Testing plan

New corpus directory `tests/corpus/meta/` (rides `run_corpus.sh` on **all** engine flags —
the stage precedes lowering, so five-engine agreement extends to expanded programs for free):

1. **Feature corpus** (`.ext` + `.expected`): comptime folding (const, table, `comptime if`
   both arms), attributes inert-by-default, routes end-to-end (proposal §10.1 verbatim),
   multi-attr multi-fire, `member of` injection, ctor-less class injection, cross-namespace
   scoping (file with `uses` fires / file without stays untouched — two-file project via
   `run_project.sh`), rule-order determinism (two rules, one anchor).
2. **Twin equivalence tests** (the P1 check): for each rule corpus program, a hand-written
   twin with the expansion written out; harness asserts identical output on `--run`, `--ir`,
   and `--emit-elf`, and (stronger) identical `--emit-cpp` text modulo the injected spans.
3. **Golden `--expand`** snapshots, byte-compared → determinism regression net (same input
   twice must be identical; ordering bugs show up here first).
4. **Negative suite** (driven like the checker unit tests, `test_checker.cpp` pattern): one
   test per diagnostic M01–M18, plus hygiene attempts (template local vs same-named use-site
   local; use-site `logger` shadowing a rule-namespace `logger` — must bind to the rule’s),
   hermeticity (`sysOpen` in comptime), budget (`--comptime-budget 1000` + loop).
5. **Zero-cost guard:** the entire existing corpus runs with a build-time assert counter that
   the RuleEngine was never constructed (`hasMeta == false` everywhere legacy).
6. **Unit tests:** `test_parser.cpp` — attr/rule/quasi/comptime parse shapes incl. fragment
   spans; new `test_rules.cpp` — scoping table, ordering, clone/substitute/hygiene on
   synthetic trees.
7. **Fuzz:** extend `fuzz/` dictionary with `@`, backtick templates, `$holes`, `rule`,
   `comptime` so the grammar additions get hammered; the fragment re-lex is the main new
   attack surface (backtick nesting, unterminated templates, `$` at EOF).

---

## 13. Delivery phases (updated to post-Phase-0 reality)

### Phase 1 — Comptime (Layer C) + Attributes (Layer A) — **DONE (2026-07-04)**
*Goal: `comptime` folding works on all engines; attributes parse, resolve, type-check, and
sit inert. `--expand` exists.*

- [x] `Token.hpp`/`Lexer`: `At`, `QuasiLiteral`, `$`-holes via `allowHoles` (fused into one
      Identifier token — no separate `Dollar` kind needed) + `tokenizeRange` for fragments
- [x] `Ast.hpp`: `AttrUse`, `Stmt::attrs`, `isAttribute`, `isComptime` (Stmt + Expr),
      `Program::hasMeta`
- [x] `Parser`: attr-use wrappers at top-level + class-member positions; `attribute` decls
      (fields-only); `comptime` var/if/expr (contextual, `tryParseComptime` + `parseUnary`)
- [x] `Eval`: `ComptimeOptions` — hermetic sys*-deny (stdout/stderr writes excepted; `await`
      denied), step budget (uncatchable exhaustion), `evalComptime`/`defineGlobal`
- [x] `Rules.{hpp,cpp}` v0: full-tree walk, attr resolution per-file (M01/M02 w/ symbol-
      identity dedupe), arg eval + typing (M03/M04), comptime fold/splice (M12–M15),
      reifier with `reifiedText_` deque
- [x] `main.cpp`: stage gated on `hasMeta`, pass-2 Resolver on change, `--expand`
      (`--ast-after-rules` alias), `--no-rules`, `--comptime-budget`; AstPrinter attrs+folds
- [x] Tests: `metatests` (34 checks: M01–M04, M12–M15, M17, zero-cost guard, budget-not-
      catchable), corpus `meta/{comptime_fold,attr_inert,comptime_log}` on --run/--ir,
      projects `attr_scope_ok`/`attr_scope_err` (per-file §5 scoping, oracle+IR+ELF)
- **Acceptance met:** comptime-table programs run identically on oracle/IR/ELF (verified);
  existing corpus untouched; `--expand` shows folded literals.
- **Scope addition (decision 1):** `console` became a real prelude object — `class Console`
  with generic `write<T>`/`writeln<T>`/`writeln()` natives and the `(<<)` operator, plus the
  `console` global. The checker exception is gone (only `System` remains); aliased receivers
  dispatch via checker resolution (oracle `callFunction` native path + `Lower` resolved-set
  intercept). Comptime console goes to the REAL stdout during compilation (not the design's
  stderr prefix — Leonard's call). Byproduct: CGen gained `LoadGlobal`/`StoreGlobal`/@ginit
  and the `&`/`|`/`<<`/`>>` operator codes (bringing it in line with X64Gen).
- **Deviations discovered:** budget default is 100M steps (not 25M — compile speed deprio'd);
  single-file programs see their own namespaces' attributes without `uses` (§5.1's
  imports(F) definition, confirmed intended).

### Phase 2 — Rules (Layer B): additive injection, namespace-scoped — **DONE (2026-07-04)**
*Goal: proposal §10.1 (routes) end-to-end. The web-framework proposal unblocks here.*

- [x] `Parser`: `rule` decls (contextual keyword), match clause (attr + subject + enclosers
      with `:` constraints + `where` parsed for Phase 3), inject clauses, anchors
      (`top/bottom of C.constructor`, `member of C`; body/marker/namespace parsed, engine
      rejects as Phase 3); fragment parsers (`parseStmtsFragment`/`parseMemberFragment`/
      `parseExprFragment`) re-lexing the template in place with `$`-holes; trailing-`;`
      tolerance for single-expression templates
- [x] `RuleEngine`: scoping (§5.2, one `effective`-set test), ordering (§5.3, source-offset
      total order), matching (§5.4: decl-kind, attr pattern + `match one`, encloser chain with
      `:IFace` from resolved bases), expansion (§5.5: clone + hole substitution + reify),
      hygiene (§7.1: gensym template-local `Var`s), anchors + conflicts (§5.6: ctor synth,
      base-ctor-call insertion point, member same-name+type conflict), rule detaching,
      dangling-attr warning (M05, only when a reading rule exists), `ExpansionRecord`s
- [x] Two-pass resolve wiring (already from Phase 1); `--rules`, `--no-rules`
- [x] Tests: `meta/rule_routes` + hand-written twin (identical on oracle/IR/ELF),
      `meta/rule_member` (member injection), project `rule_scope_ok` (cross-file fire via
      `uses`, oracle==IR==ELF); metatests +8 (unbound hole, whole-attr splice, `:IFace`
      gating, member conflict, `rewrites`-rejected, hygiene)
- **Acceptance met:** §10.1 routes output identical to its hand-written twin on run/IR/ELF;
  the scoping project shows fire-on-`uses`; full suite 26/26.
- **Deviations from the design:** (1) ordering uses source offset, not the `UsesGraph`
  namespace topo — offset is a deterministic total order and a rule only fires where its
  namespace is imported anyway, so the topo refinement is unnecessary for correctness.
  (2) Dangling-attr warning fires only when *some* rule reads that attribute name (the
  actionable missing-`uses` case); an attribute no rule anywhere consumes is legitimate
  inert data and stays silent — otherwise Phase 1's inert-attribute corpus would warn.
  (3) Full definition-site qualification (§7.1 hygiene half two) and provenance-in-diagnostic
  notes (§8.2) are deferred: injected nodes keep template spans so pass-2 errors already point
  at rule source, and `--rules`/`--expand` provide the inspection window; inline `origin`
  attribution can follow if practice demands. (4) Member-conflict compares the *syntactic*
  return type (`TypeRef::name`), since a freshly-cloned injected member is not yet resolved.

### Phase 3 — Semantic depth: `where`, `meta.*`, `$for`, macros, more anchors
*Full technical design: `designs/complete/techdesign-metaprog-phase3.md` (supersedes this checklist —
adds conditional `uses` (§9 there), def-site qualification, and the commit sequence).*
- [x] `meta.*` value structs in the prelude (proposal §6.3, minimal: Method/Class/Field/
      Param/Type/Span) + binding materialization for `where` clauses (comptime predicates)
- [x] `$for` list splice; struct-value reification (labeled ctor call) — unlocks the ORM
      example §10.2 (adapted to the actual `meta.*` surface: attribute values aren't
      reflected, so `rule_orm` names columns by field, not an `@Column("alias")` override)
- [x] Expression macros: `macro` decls + `name!(args)` call-site expansion (same hygiene
      path — definition-site qualification, §10). Two follow-on positions surfaced by the
      §9 pipeline restructure (a macro call under a `comptime` fold; a macro call inside a
      rule/anchor template) are resolved — see the Phase-3 doc's "§7×§9 interaction".
- [x] Anchors: `top/bottom of body` (arrow-body wrap rules, unreachable-bottom error),
      `marker`, `namespace N`
- [x] Conditional `uses` in `comptime if` (§9 of the Phase-3 doc): pipeline split A–F,
      engine-owned post-fold imports map, item-level splice fold, M15 removed. Landing this
      exposed and fixed a pre-existing `Lower.cpp` gap — closures never captured the
      receiver, so `this`/bare-member access inside any lambda broke on IR/ELF (see the
      Phase-3 doc §14 field note).
- [x] Definition-site qualification (§10 of the Phase-3 doc) and struct reification (§11,
      stretch — done anyway).
- **Acceptance:** proposal §10.2 and §10.4 corpus programs pass with twins/output-proof —
  met. Phase 3 is feature-complete; per-section detail lives in the Phase-3 doc.

### Phase 4 — Layer D + polish
*Full technical design: `designs/complete/techdesign-metaprog-phase4.md` — covers every item below plus
the deferred tail (in-phase deferrals from Phases 1–3: `meta.*` structured Type /
attribute-value reflection, statement-position `$for`, def-site `homeNs`, class-wide
markers, attribute named args, the single-resolve fast path, macro auto-hoisting, and the
golden-`--expand` fixture). Grounded, sequenced (§11 there), M30–M35 catalog.*
- [ ] `rewrites body of m` + `$body` hole (`replace` verb), loudest `--expand` diff (§10.5)
      — phase4 §2, with confluence detection (§3) and the `reentrant` gate (§4)
- [ ] Exact provenance (`provenanceId` per clone) if the 3-origin range attribution proved
      insufficient; targeted pass-1 checking of comptime roots for earlier type errors
      — phase4 §5, §8 (both demand-gated)
- [ ] Source-shaped pretty-printer behind `--expand` (AST dump remains via `--ast-after-rules`
      alias); incremental per-file rule-output cache design (key: file hash ⊕ imported-rule-set
      hash — proposal §13.7); `reentrant` gate decision — phase4 §6, §7
- **Acceptance:** §10.5 memoize/timed corpus; `--expand` readable as source (the round-trip
  test: `--expand` output compiles + runs byte-identical — phase4 §6/§12).

Sequencing note: Phases 1 and 2 are independent PR trains against today’s master; Phase 2
consumes Phase 1’s driver but the parser/engine work can proceed in parallel behind the
`hasMeta` gate.

---

## 14. Deviations from the proposal (each deliberate, none silent)

1. **Uniform post-resolve-1 staging; no “syntactic-only single-pass” mode.** Proposal §8
   offered a single-resolve fast path for syntactic matchers. Building two staging modes
   doubles the state space for a cost that only exists when rules fire (pass 2 is skipped
   entirely when nothing expands). Bonus: `: IFace` constraints work in Phase 2 instead of
   waiting for Phase 3. The optimization slot stays open (Phase 4 caching subsumes it).
2. **Attribute sigil is `@`, final; the `[Name]` alias is dropped** (proposal §13.1 asked for
   a call — recommended default taken; `[` keeps exactly one meaning, per §1).
3. **`meta.*` reflection and `where` land Phase 3, not Phase 2.** V1 bindings are
   engine-internal (holes only). Keeps the forever-API surface (§3.8 risk) unshipped until a
   real consumer (`where`, `$for`) exists.
4. **Attribute arguments are positional-only** until the language itself grows named
   arguments — the proposal’s “positional or named args” assumed a facility that doesn’t
   exist anywhere in the language; inventing it for attributes first would be a special case
   (§1 violation).
5. **Comptime results and value-holes reify primitives/arrays only in v1** (proposal open
   question 6, taken conservatively); structs Phase 3 via labeled-ctor reification; reference
   classes stay non-reifiable indefinitely (identity has no compile-time meaning).
6. **`uses` is forbidden inside `comptime if` v1** — conditional imports would mutate the
   FileImports the running stage is scoped by (a fixpoint the proposal never asked for).
   Platform-conditional imports (the §4.3 example) need a revisit: likely “comptime-if resolves
   *before* scoping in a pre-stage” in a later phase; flagged as an open question (§16).
7. **`--expand` v1 is an annotated AST dump, not source-shaped** — the semantics window ships
   in Phase 2; the pretty-printer is Phase-4 polish. The proposal’s “required window” is
   satisfied by information content, then by form.
8. **Range-based provenance first** (3-origin ambiguity accepted, documented) — exact
   per-clone IDs only if practice demands (§8.2), keeping v1 out of every diagnostic call
   site in Resolver/Checker.

---

## 15. Risks (implementation-level; proposal §11 covered design-level)

| Risk | Exposure | Mitigation |
|---|---|---|
| `string_view` dangling from synthesized names/literals | crashes far from cause | single owner (`synthNames_`/`reifiedText_` deques) held by the engine for compile lifetime; rule: **no AST view may point at engine-local temporaries** — code-review checklist item |
| Pass-1 `Symbol*` surviving into pass-2 consumers | UB on discarded Sema | rules detached (§5.1); `ExpansionRecord` carries no symbols; resolver1 kept alive in `main` scope; assert in debug builds that pass-2 tree contains no `StmtKind::Rule` |
| Clone function drifting from AST evolution | silently dropped fields on new nodes | `cloneStmt/cloneExpr` written as exhaustive switch over Kind enums with `static_assert`-style default that fails on unknown kind; parser unit test round-trips every node kind through clone |
| Oracle-as-comptime nondeterminism (map iteration, address-keyed behavior) | `--expand` instability | Map is insertion-ordered already; golden-expand determinism tests in CI from Phase 1; deny-set includes clock/RNG (none exist as natives today — keep it that way for comptime) |
| Fragment re-lex edge cases (nested backticks, `$` handling) | parser crashes on fuzz | fuzz dictionary extension (§12.7) from Phase 1, before rules ship |
| Second resolve cost on big meta-heavy projects | compile-time regression | measured, not guessed: stage prints timing under existing verbosity; per-file caching is the designed answer (Phase 4), whole-program resolve is already the accepted §17 trade |

---

## 16. Decisions needed before Phase 1 (small, all decidable by Leonard in minutes)

1. **`[comptime]` console log to stderr — keep or drop?** (Design says keep; it’s the rule
   author’s only printf.)
2. **Default step budget** (design proposes 25M steps ≈ well under a second on the oracle;
   `--comptime-budget` overrides either way).
3. **`comptime`/`rule`/`attribute`/`macro` as contextual identifiers (design) vs. real
   keywords** — contextual preserves every existing program and matches the `get`/`set`
   precedent; real keywords simplify the parser marginally. Design assumes contextual.
4. **Platform-conditional `uses`** (deviation 6): acceptable to defer past Phase 3? The web
   framework doesn’t need it; self-hosting eventually might.

---

*Companion documents: `designs/proposal-metaprogramming.md` (accepted design),
`designs/proposal-web-framework.md` (first consumer), `designs/complete/proposal-project-system.md` +
`designs/proposal-package-manager.md` (landed substrate this design builds on).*
