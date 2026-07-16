#pragma once
#include "Source.hpp"
#include "Token.hpp"
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct Symbol;  // resolution annotations (Symbols.hpp)
struct Scope;   // lexical import scope for a block (Symbols.hpp)
struct AstPayload;

// ---------------------------------------------------------------------------
//  AST
//
//  Tagged class hierarchies (Clang-style): each node carries a Kind for fast
//  switch-dispatch, plus a virtual destructor so unique_ptr cleans up the tree.
//  Ownership is unique_ptr / vector (RAII). An arena is a deferred optimization
//  for once the node set stabilizes.
//
//  Unifications from the design:
//   - "A member is a typed slot at a label": fields, methods, ctors, accessors,
//     and operator members are all Member nodes; a Selector is a plain name OR
//     a symbolic selector (the "()" special member).
//   - "Body is one statement": a member/lambda body is a single Stmt.
// ---------------------------------------------------------------------------

struct TypeRef;
struct Expr;
struct Stmt;
using TypeRefPtr = std::unique_ptr<TypeRef>;
using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;

enum class Access { Default, Public, Private };

// --- Types -----------------------------------------------------------------

enum class TypeKind { Named, Union, Inferred, Function };

struct TypeRef {
    TypeKind kind;
    SourceSpan span;

    // Named: `path::...::name<generics...>`. `path` holds the leading namespace
    // qualifier segments (empty for an unqualified type); `name` is the final
    // segment. E.g. `Room1::TheClass` -> path=["Room1"], name="TheClass".
    std::vector<std::string_view> path;
    std::string_view name;
    std::vector<TypeRefPtr> generics;

    // Union: `members[0] | members[1] | ...`
    std::vector<TypeRefPtr> members;

    // Function: `(funcParams...) => funcRet`
    std::vector<TypeRefPtr> funcParams;
    TypeRefPtr funcRet;

    // Inferred: `var` / `let` — a spelling marker, not a type (see §9).

    // Filled by the resolver: canonical spelling for identity/collision checks
    // ("int", "Widget<string>", "int | string") and the resolved head symbol
    // (null for primitives, type params, or unresolved names).
    std::string canonical;
    Symbol* resolvedSymbol = nullptr;

    explicit TypeRef(TypeKind k) : kind(k) {}
};

// --- Selector: a member's label (name) or symbolic "()" selector -----------

struct Selector {
    bool symbolic = false;       // true => the "( ... )" special-member form
    std::string_view text;       // identifier name, or the source between ( )
    SourceSpan span;
};

// --- Attribute use: `@Name(args)` / `@NS::Name(args)` above a declaration ---
// Inert data (§16.5 Layer A): carries typed arguments to a rule; means nothing
// unless a rule in an imported namespace reads it. Evaluated attribute values
// live in the rule engine, never in the AST (no runtime-value dependency here).

struct AttrUse {
    std::vector<std::string_view> path;   // qualifier segments (may be empty)
    std::string_view name;                // final segment: "Route"
    std::vector<ExprPtr> args;            // positional, comptime-evaluable
    SourceSpan span;
    // filled by the rule stage:
    Symbol* resolved = nullptr;           // the attribute class symbol
    bool consumed = false;                // some in-scope rule matched through it
    bool considered = false;              // an in-scope rule matched THIS attribute
                                          // but a `where`/shape filter excluded the
                                          // decl (intentional) — not a dangling attr
};

struct Param {
    TypeRefPtr type;             // may be null (lambda params can be untyped)
    std::string_view name;
    bool isConst = false;        // `const T name` param (const.md §3.4): reassignment in the body is an error
    SourceSpan span;
    ExprPtr defaultValue;        // `T name = constant` (named-arguments design)
    bool defaultFolded = false;  // rule-stage idempotence/cache
};

// --- Expressions -----------------------------------------------------------

enum class ExprKind {
    IntLit, FloatLit, StringLit, BoolLit,
    Name, This,
    Member,      // base '.' name   OR   base '::' name
    Call,        // callee ( args )
    Index,       // base [ index ]
    Unary,       // prefix op operand
    Binary,      // lhs op rhs   (includes assignment '=')
    Ternary,     // cond ? then : else
    Array,       // [ elems ]
    Lambda,      // ( params ) => bodyExpr  OR  => { block }
    Await,       // await operand
    Inject,      // inject Type
    Extract,     // stream >>   (extract with no rhs, from project.ext)
    Range,       // a .. b
    Is,          // expr is Type -> bool
    Match,       // match (subject) { pattern => body; ... }
    ForSplice,   // `$for` <ident> in <expr> : <element>  (Phase 3 §5; template-only:
                 // array-literal element position, expanded by cloneExpr's Array
                 // case — text=loop var, a=iterator expr, b=element template)
};

// One arm of a `match`. The pattern is a type (`Type => ...`), a value/range
// expression (`0 => ...`, `1..9 => ...`), or the catch-all `else`. Selection is
// resolution by type/value, first-match-wins — the same rule as catch clauses.
// The body is an expression (`=> expr`) or a block (`=> { ... }`), like a lambda.
struct MatchArm {
    bool isElse = false;
    TypeRefPtr type;         // type pattern (null otherwise)
    ExprPtr value;           // value / range pattern (null otherwise)
    ExprPtr bodyExpr;        // => expr
    StmtPtr bodyBlock;       // => { block }
    SourceSpan span;
};

struct Expr {
    ExprKind kind;
    SourceSpan span;

    // literals / names / operators
    std::string_view text;       // literal text, identifier, or member name
    TokenKind op = TokenKind::End;  // for Unary/Binary
    bool colon = false;          // Member: true => '::', false => '.'
    bool optChain = false;       // Member/callee: '?.' (None short-circuits)
    bool isComptime = false;     // `comptime expr` — folded to a literal by the
                                 // rule stage before pass-2 resolve (§16.5)
    bool isMacroCall = false;    // `name!(args)` (Phase 3 §7) — kind is Call;
                                 // expanded by the rule stage's expr walk, same
                                 // pass that folds `comptime` exprs
    // StringLit only, F4: a synthesized literal SEGMENT of an interpolated
    // string — `text` is a bare content slice (no surrounding quotes; it's a
    // real view into the source buffer between two `${...}` holes, never
    // synthesized text, so it stays stable with zero pool/lifetime machinery)
    // and must be escape-decoded directly, not quote-stripped first.
    bool isRawSegment = false;
    // Procedural-macro call arguments only: a backtick payload is a byte-exact
    // string (no quote stripping and no escape decoding). It remains an
    // ordinary StringLit node so the comptime evaluator needs no parser-only
    // value path; the flag is consumed before lowering because macro calls are.
    bool isQuasiPayload = false;
    // request-string-literal-tail: a `r"..."`/`r'...'` raw string literal —
    // `text` is already bare content (quotes and the 'r' prefix stripped by
    // the parser) and must be used byte-exact, with NO escape decoding and
    // no interpolation (that is the entire point of "raw").
    bool isRawString = false;
    // Track 03 §1: set by the parser on a simple (single-segment, no
    // interpolation) SINGLE-quoted string literal — the only literals that can
    // become `char`. The parser strips quotes into a raw segment, so quote style
    // would otherwise be unrecoverable; the checker reads this to decide char.
    bool singleQuoted = false;
    // Track 03 §1: set by the checker when a single-quoted, single-scalar
    // StringLit meets a `char`-expected context (declared type, char comparison,
    // char parameter/return). The engines then produce a `char` value (the
    // decoded scalar) instead of a string. Purely a re-typing of the literal —
    // the token stays a StringLiteral; only the expected type flips it.
    bool charLit = false;
    // F5 checker annotation: this Name/Member denotes a weak field slot. It is
    // consumed by Lower and also prevents flow facts from treating repeated
    // field reads as one stable value.
    bool weakField = false;
    bool weakDirect = false; // no accessor intercepts this statically-known slot

    ExprPtr a, b, c;             // operands (meaning depends on kind)
    std::vector<ExprPtr> list;   // call args, array elems
    std::vector<Param> params;   // Lambda params
    StmtPtr block;               // Lambda: statement-block body (a is null then)
    std::vector<MatchArm> arms;  // Match: the arms (a = subject)
    TypeRefPtr type;             // Inject target type / Is target type

    // Set by the checker's overload resolution: the specific member/function/
    // constructor this Call or Binary(operator) resolves to, so the evaluator
    // invokes the same overload the checker chose (static resolution by type).
    const Stmt* resolved = nullptr;
    int evalKind = 0;   // checked kind hint for codegen: 0 int/other, 1 bool, 2 string, 3 float
    bool mayBeValueStruct = true;   // §9: might this expr's type be a value struct? (copy gate)
    bool definiteValueStruct = false;   // §9/§15: is it a CONCRETE value struct? (arena-owned copy)
    // For construction calls: the class being constructed (needed when the
    // class name is scope-resolved, e.g. inside a namespace).
    Symbol* resolvedClass = nullptr;
    // techdesign-columnar-arrays.md: when definiteValueStruct is true, the
    // concrete value-struct Symbol this expr's type resolves to (else null).
    // Lets Lower recognize `arr[i].field` on a columnar-eligible element type
    // (fusion) and mark gathered element reads for VFree (ownership §5.3).
    Symbol* valueClass = nullptr;

    // Rule hygiene: a declaration hole spliced in expression/call position
    // carries the declaration identity it denotes. Pass-2 resolution can then
    // bind `$C()` to that class even if the generated enclosing declaration is
    // also named C and would capture a bare source spelling by ordinary lookup.
    const Stmt* hygienicDecl = nullptr;

    // Calls are authored as positional/named argument bindings.  The checker
    // rewrites them to a full positional list before any engine sees them.
    std::string_view argLabel;   // empty => positional; otherwise `label: expr`
    bool argsNormalized = false;

    // LA-18: this `::` member was authored with a callable-level type
    // parameter on the left (`T::member`).  The definition-site checker leaves
    // it duck-typed; specialized clones retain the marker so a failed concrete
    // lookup can name both T and the instantiating call site.
    bool genericStaticSite = false;
    std::string_view genericStaticParam;

    explicit Expr(ExprKind k) : kind(k) {}
};

// --- Statements & Declarations (one umbrella: "everything is the same stuff") --

enum class StmtKind {
    // declarations
    Namespace, Class, Member, Bind,
    Enum,        // `enum Name : carrier { M1, M2 = v, ... }` (Track 03 §2) —
                 // a parse-only node the Resolver desugars to a value struct +
                 // per-member mangled const globals + a `fromCode` free function
                 // (keeps the parser dumb; honest `--ast` dump). `name` = enum
                 // name; `type` = carrier TypeRef (null => int); `body` holds one
                 // Member per enum member (`name` = member name, `init` = explicit
                 // carrier value expr or null for auto).
    Rule,        // `rule Name { match ... inject ... }` (§16.5 Layer B)
    // executable
    Var, Block, ExprStmt, Return, If, While, For, ForIn, Use, UsesImport,
    Try, Throw, Empty,
    Break, Continue, DoWhile,
};

// One `catch (Type name?) body` clause. Selection is resolution by type: the
// thrown value's dynamic type picks the first clause it is assignable to.
struct CatchClause {
    TypeRefPtr type;
    std::string_view name;   // optional binding
    StmtPtr body;
};

// --- Rules (§16.5 Layer B) ---------------------------------------------------
// A rule is `match <shape> inject <template> at <anchor>`. The match selects and
// binds declarations by structural shape; the inject splices a quasiquoted AST
// template (holes = `$name` filled from the bindings) at a named anchor. Both
// halves are parse products interpreted only by the RuleEngine (Rules.cpp).

// One binding level of a match clause: `on method m`, `in class C : IController`.
struct RuleLevel {
    std::string_view kindWord;   // "method" | "class" | "struct" | "field" | ...
    std::string_view bind;       // the name introduced (m, C)
    TypeRefPtr constraint;       // `: IFace` / `: Base` (null if none)
    SourceSpan span;
};

struct RuleMatch {
    bool one = false;            // `match one @Attr ...` — at most one attr, else error
    bool hasAttr = false;
    std::vector<std::string_view> attrPath;   // qualifier segments of @NS::Name
    std::string_view attrName;                // "Route"
    std::string_view attrBind;                // `@Route(r)` binds r to the attr value
    RuleLevel subject;                        // `on method m`
    std::vector<RuleLevel> enclosers;         // `in class C : IController` (0+)
    ExprPtr where;                            // `where <expr>` (Phase 3; parsed, unused)
    SourceSpan span;
};

enum class AnchorKind {
    CtorTop, CtorBottom,   // top/bottom of C.constructor
    MemberOf,              // member of C
    BodyTop, BodyBottom,   // top/bottom of body (matched method) — Phase 3
    Marker,                // marker "name" — Phase 3
    NamespaceScope,        // namespace N — Phase 3
    BodyReplace,           // replace `...` — Layer D, Phase 4 (§2.3): overwrites the
                           // `rewrites body of <bind>` target's body; `$body` splices
                           // the original body back in (composition, not obliteration).
};

struct RuleAction {
    AnchorKind anchor = AnchorKind::CtorBottom;
    std::string_view target;         // the bound name the anchor refers to (C, m)
    std::string_view markerName;     // Marker anchors (Phase 3)
    // The parsed quasiquote template; which field is set follows the anchor kind
    // (ctor/body anchors -> statement list; member of -> one member; expr later).
    std::vector<StmtPtr> tmplStmts;
    StmtPtr tmplMember;
    ExprPtr tmplExpr;
    SourceSpan quasiSpan;            // the `...` literal's span (rule-site provenance)
    SourceSpan span;
};

struct Stmt {
    StmtKind kind;
    SourceSpan span;

    // common
    Access access = Access::Default;
    std::string_view name;
    std::vector<AttrUse> attrs;   // `@Name(args)` annotations on this decl (§16.5)

    // Namespace / Class / Block bodies
    std::vector<StmtPtr> body;

    // Block: the lexical import overlay scope for this block, when it contains
    // `uses` statements (bug.md #8). Created by the Resolver (a child of the
    // enclosing scope holding the block's imports) and consulted by the Checker
    // so a block-level `uses` is visible for exactly that block. Null when the
    // block imports nothing (the common case).
    Scope* importScope = nullptr;

    // Member (free function, i.e. a namespace-level callable, not a method):
    // the innermost `namespace` this decl was gathered directly inside, or
    // null at file/global scope. Set by Resolver::gatherInto (bug.md #32 M2)
    // so the evaluator/lowerer can recover "what namespace is this unchecked
    // prelude body running in" for bare same-namespace construction
    // (`Class(...)` with no `ns::` qualifier), the same way the qualified
    // form already descends via an explicit namespace name.
    Symbol* enclosingNs = nullptr;

    // Class
    bool isInterface = false;
    bool isValue = false;                      // `struct`: value type (§9 object mask generalized)
    bool isAttribute = false;                  // `attribute Name { fields }` (§16.5 Layer A)
    // Class: type params <T, U>.
    // Use / UsesImport: the `::`-separated path segments (imports.md); `name`
    // is the bound name — the path's last segment, or the `as` alias for Use.
    std::vector<std::string_view> generics;
    std::vector<TypeRefPtr> bases;

    // Use only (system-binds.md §5.3, Channel 1): set by the Resolver's
    // useOne only when this `use NS::T;` resolves to a class/interface —
    // the namespace T was found in, and T's own name (the same key
    // pushBindScope registers a same-spelled `bind T => ...;` under). Read
    // once, by Checker::pushBindScope, to activate NS's exported bind for T
    // in the use's scope. Null ns = not a type import (function/var/nested-
    // namespace `use`, or a bare `use name;` with no `::`): activates nothing.
    Symbol* useResolvedNs = nullptr;
    std::string_view useResolvedTypeKey;

    // Member (field / method / ctor / accessor / operator)
    //   isCtor      => 'new' constructor; `name` is the label
    //   isGet/isSet => accessor view over a slot
    //   selector    => label or symbolic "()" selector (methods/operators)
    //   callable     => has a parameter list (method/ctor/accessor/operator)
    //   type        => field/return type (null for ctors)
    //   params, memberBody
    bool isCtor = false, isGet = false, isSet = false;
    bool isMutating = false;                   // `mutating` method: may write `this` (value types)
    bool distinct = false;
    // `const` (const.md): a field's write view exists only for the field
    // initializer + the declaring class's own constructor bodies. Also reused
    // on Var (local/global) and ForIn (loop binding), whose write view exists
    // only for the declaration's own initializer/binding, never afterward.
    bool isConst = false;
    // `readonly` (techdesign-readonly, LA-28): instance-field-only, runtime
    // write-once — its write view is the initializer OR any declaring-class
    // constructor, exactly once (§4.3/§4.4). Never set on Var/ForIn/param
    // (parseClassMemberInner only applies it to a typed class member).
    bool isReadonly = false;
    // F5: a non-owning, field-only reference slot. Reads are fresh optional
    // values; stores never keep the referenced class object alive.
    bool isWeak = false;
    // `using` (techdesign-02 F3): a Var-only modifier (v1: locals, direct block
    // children only). Forces isConst (never reassigned — reassignment would
    // orphan the cleanup obligation). `usingClose` is the checker-resolved
    // zero-arg `close()` decl the Lowerer calls at every block-exit edge.
    bool isUsing = false;
    const Stmt* usingClose = nullptr;
    bool callable = false;
    Selector selector;
    TypeRefPtr type;
    std::vector<Param> params;
    StmtPtr memberBody;          // body-is-one-statement (null if none, e.g. iface)

    // Var:  type name = init;   (type null + inferred=true for var/let)
    bool inferred = false;
    ExprPtr init;
    // `comptime` marker on Var (fold the init) / If (compile the taken branch
    // only) — consumed by the rule stage (§16.5 Layer C).
    bool isComptime = false;

    // Bind:  bind Type => body  |  bind Type { ... }  |  bind object;
    //   type set => factory binding (memberBody is the factory body)
    //   init set => object-install form (`bind expr;`)
    // (reuses `type`, `memberBody`, `init`)

    // Rule (StmtKind::Rule): `name` is the rule label. The RuleEngine detaches
    // these from the tree before pass 2, so no later pass ever sees one.
    std::unique_ptr<RuleMatch> ruleMatch;
    std::vector<RuleAction> ruleActions;
    bool ruleRewrites = false;                 // `rewrites` marker (Layer D, Phase 4)
    bool ruleReentrant = false;                // `reentrant` marker (Layer D, Phase 4 §4):
                                               // this rule may re-trigger on rule-generated
                                               // code (the gated fixpoint opt-in)
    std::string_view rewritesTarget;           // `rewrites body of <bind>`: the match bind
                                               // whose body a `replace` action overwrites
    // `macro Name(params) => \`expr\`;` (Phase 3 §7) reuses this same node shape:
    // ruleMatch/ruleActions/ruleRewrites are unused; `generics` repurposed for the
    // macro's parameter names (both are string_view vectors; documented reuse per
    // the main design's note); `ruleActions[0].tmplExpr` holds the template.
    bool isMacroDecl = false;
    // F4 procedural form: `macro name(string payload) comptime <body>`.
    // `generics[0]` is the parameter name and memberBody is the comptime body.
    // Template macros leave this false and keep their quasiquote in
    // ruleActions[0].tmplExpr as before.
    bool isProceduralMacro = false;

    // LA-18: generic callables containing `T::member` are never emitted in
    // their erased form.  The checker appends concrete, non-generic clones to
    // Program::specializations and rewires every concrete call to one of them.
    bool specializationRequired = false;
    bool isSpecialization = false;
    const Stmt* specializationOf = nullptr;
    Symbol* specializationClass = nullptr;  // non-null for specialized methods
    SourceSpan instantiationSpan;

    // ExprStmt / Return / If condition
    ExprPtr expr;

    // If: expr=cond, thenBranch, elseBranch
    // While: expr=cond, thenBranch=body
    // DoWhile: expr=cond, thenBranch=body (post-test: body runs before the
    //   first cond check, unlike While)
    // For: forInit; expr=cond; forStep; thenBranch=body
    // Try: thenBranch=try body, catches
    // Throw: expr=the thrown value
    StmtPtr thenBranch, elseBranch;
    StmtPtr forInit;
    ExprPtr forStep;
    std::vector<CatchClause> catches;

    // ForIn (techdesign-07 §2): the path the checker chose for this loop,
    // recorded statically so Eval/Lower never re-derive it (contract C5's
    // "the checker decides the path"). False = a built-in fast path
    // (Range counted-loop / Array·Map IterLen·IterAt). True = the iterator
    // protocol: `e.iterator()` then `hasNext()`/`next()` via ordinary CallDyn.
    bool forInProtocol = false;

    explicit Stmt(StmtKind k) : kind(k) {}
};

// F4's opaque comptime code carrier. RuntimeValue.hpp only holds a shared_ptr
// to this forward-declared type, keeping the evaluator/value layer independent
// of parser ownership while allowing an Ast value to be copied and re-spliced.
enum class AstFragmentKind { Expr, Stmts };
struct AstPayload {
    AstFragmentKind kind = AstFragmentKind::Expr;
    ExprPtr expr;
    std::vector<StmtPtr> stmts;
};

// Track 03 §2: the Resolver-side record of one desugared `enum`. The Resolver
// lowers each `enum` to a value struct + per-member mangled const globals + a
// `fromCode` free function, and records this so the Checker can (a) type
// `Enum::Member` value reads and `Enum::fromCode(...)` calls, and (b) drive
// `match` exhaustiveness. It lives on the Program (not Sema) so it survives the
// metaprogramming re-resolve, whose second Resolver uses a fresh Sema but the
// same Program (main.cpp: `R = resolver2`).
struct EnumDesugar {
    std::string_view name;                 // the enum/struct name (e.g. "Method")
    struct Member {
        std::string_view name;             // member name (e.g. "GET")
        long long carrier = 0;             // carrier value (int v1)
        const Stmt* global = nullptr;      // the mangled `Method$GET` const global
    };
    std::vector<Member> members;
    const Stmt* fromCode = nullptr;        // the mangled `Method$fromCode` free function
};

// The whole program: a flat list of top-level items (decls and statements).
struct Program {
    std::vector<StmtPtr> items;
    // LA-18 concrete callable bodies. Kept outside `items` so they are emitted
    // as functions but never executed as top-level statements.
    std::vector<StmtPtr> specializations;
    std::vector<std::string> specializationReport;
    // Set by the parser when it saw any metaprogramming surface (attributes,
    // attribute decls, comptime, rules). False = the rule stage never runs and
    // the pipeline is byte-identical to the pre-§16.5 compiler (zero cost).
    bool hasMeta = false;

    // Track 03 §2 enum desugaring. `synthFiles`/`synthNames` are the persistent
    // string_view backing for synthesized source and mangled `$`-names (deque =
    // stable element addresses); `enumDesugars` is the per-enum metadata above.
    // All three must outlive every stage the front-end feeds — the Program does.
    std::deque<SourceFile> synthFiles;
    std::deque<std::string> synthNames;
    std::vector<EnumDesugar> enumDesugars;
};
