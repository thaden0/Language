#pragma once
#include "Ast.hpp"
#include "Diagnostic.hpp"
#include "Symbols.hpp"
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// A semantic type value computed for an expression. Distinct from TypeRef
// (syntax): this is what type checking flows around.
//
// Unknown is a *compiler-internal* placeholder for "we can't determine this
// yet" (e.g. members of not-yet-modeled library types). It is NOT the rejected
// `any` language feature — it exists only so a missing stdlib doesn't produce
// false errors. Unknown is compatible with everything (lenient).
enum class TKind { Primitive, Class, TypeValue, FuncRef, Union, Unknown, Error };

struct Type {
    TKind kind = TKind::Unknown;
    Symbol* sym = nullptr;              // Class/TypeValue: the class; FuncRef: the function
    std::string canonical;
    std::vector<Type> unionMembers;
    std::shared_ptr<Type> ret;         // FuncRef from a function type: its return type
    std::vector<Type> args;            // Class: instantiated generic arguments
};

// One local binding's checker state: its flow type plus whether it's `const`
// (const.md) — a local/param/for-in const's write view is exactly its own
// declaration, so any assignment reached via env_ is outside the window.
struct LocalBinding {
    Type type;
    bool isConst = false;
    LocalBinding() = default;
    LocalBinding(Type t, bool c = false) : type(std::move(t)), isConst(c) {}
};

// techdesign-readonly §4.3: which of the two write-once modifiers blocked a
// write, for the two constBlockedWrite call sites' diagnostics. `name` empty
// means "not blocked".
enum class BlockedWriteKind { Const, Readonly };
struct BlockedWrite {
    std::string name;
    BlockedWriteKind kind = BlockedWriteKind::Const;
    bool isField = false;   // false: a local/global/param/for-in `const` (unchanged, §3)
};

// Phase-1 type checker: types expressions, resolves names/members by type
// against the shapes, checks assignability, and enforces the rule that a bare
// read of a name with no type in context (a distinct-collided member) is an
// error. Lenient where the stdlib isn't modeled yet.
class Checker {
public:
    Checker(const Sema& sema, const SourceFile& file, DiagnosticSink& sink)
        : sema_(sema), file_(file), sink_(sink) {}

    // `prelude`, when given, is walked read-only to index its namespace-level
    // factory binds (system-binds.md §5.2) so a `use std::IEnv;` in `program`
    // can activate them (Channel 1, §5) — the prelude itself is still never
    // type-checked ([[leviathan-prelude-not-checked]]).
    void run(Program& program, const Program* prelude = nullptr);

    // Metaprog Phase 4 §8: type one comptime-root expression against `scope`
    // (a scope-complete position — comptime roots have no locals), emitting
    // ordinary type errors to the sink. Lets a failed comptime evaluation report
    // a precise type-error message instead of the opaque "comptime evaluation
    // failed" wrapper. A standalone entry — does not touch whole-program state.
    void checkComptimeRoot(const Expr* e, Scope* scope);

private:
    const Sema& sema_;
    const SourceFile& file_;
    DiagnosticSink& sink_;
    const Program* program_ = nullptr;      // Track 03 §2: enum metadata (enumDesugars)

    // Walk state
    Scope* scope_ = nullptr;                // current lexical scope (for globals)
    Symbol* thisClass_ = nullptr;           // enclosing class, or null
    Stmt* curMember_ = nullptr;             // enclosing member (for `mutating` checks)
    Type returnType_;                       // current function's return type
    const TypeRef* returnTypeRef_ = nullptr;// same, as syntax (for target-typed inference)
    // techdesign-02 F1: loop nesting depth, for break/continue-outside-a-loop
    // errors. Reset to 0 at every function/lambda body boundary (a break inside
    // a closure never exits the enclosing function's loop).
    int loopDepth_ = 0;
    // techdesign-02 F3: true for exactly the duration of checking a Block's
    // direct child that is itself that Block's next statement — `using` is
    // legal only as a direct statement of a block, so this flag is set right
    // before checking each direct child and cleared (read) at the top of
    // check(). Not a stack: nested blocks reset it around their own children.
    bool usingOkHere_ = false;
    // Assignment left-hand members are slot targets, not value-position
    // method references. typeOfBinary clears this while typing such an lhs.
    bool methodRefsAllowed_ = true;
    // Some call paths revisit an argument after overload selection. Keep a
    // rejected bound reference from emitting the same diagnostic twice.
    std::unordered_set<const Expr*> diagnosedMethodRefs_;
    std::vector<std::unordered_map<std::string_view, LocalBinding>> env_;  // locals stack
    // When non-null, checking is inside a lambda body whose return type is
    // being inferred: Return statements record their type here instead of
    // checking against returnType_ (which is cleared inside lambdas anyway).
    std::vector<Type>* lambdaReturns_ = nullptr;
    // Flow-narrowing overlay: path ("x", "req.host") -> narrowed type. Consulted
    // before declared types; erased when the path (or a prefix) is assigned.
    std::unordered_map<std::string, Type> narrow_;
    // bind/inject DI (§12.5): a stack of per-lexical-scope, type-keyed factory
    // bindings (canonical type -> the `bind` Stmt). Nearest-wins on lookup. These
    // live ONLY here, never dumped into the shared scope tables (owner ruling,
    // system-binds.md §5) — bulk `uses` must never activate a namespace's binds.
    std::vector<std::unordered_map<std::string, const Stmt*>> bindScopes_;

    // system-binds.md §5.2 (Channel 1): (namespace symbol) -> (bind's key,
    // same string pushBindScope uses) -> that namespace's top-level factory
    // `bind` for the type. Built once, read-only, by buildNamespaceBindIndex
    // over BOTH the prelude's and the program's namespace bodies (walking is
    // not checking — the pass never descends into a factory body, preserving
    // [[leviathan-prelude-not-checked]]). pushBindScope consults it to
    // activate a `use NS::T;`-stamped bind (§5.3).
    std::unordered_map<const Symbol*, std::unordered_map<std::string, const Stmt*>>
        namespaceBinds_;
    bool namespaceBindsBuilt_ = false;
    void buildNamespaceBindIndex(const Program* prelude, const Program& program);
    void indexNamespaceBinds(const std::vector<StmtPtr>& items, Scope* scope);

    // LA-18 demand-driven monomorphization. The first walk checks generic
    // definitions leniently and records concrete call tuples; the queue is
    // then drained into Program::specializations. Checking a clone may record
    // more demands, which gives transitive specialization to a fixed point.
    struct SpecializationDemand {
        Stmt* generic = nullptr;
        Expr* call = nullptr;
        std::vector<Type> tuple;
        Scope* scope = nullptr;
        Symbol* cls = nullptr;
        SourceSpan instantiationSpan;
        int depth = 0;
    };
    std::vector<SpecializationDemand> specializationDemands_;
    std::unordered_map<std::string, Stmt*> specializationsByKey_;
    std::unordered_map<const Stmt*, Scope*> callableScopes_;
    std::unordered_map<const Stmt*, Symbol*> callableClasses_;
    std::unordered_map<const Stmt*,
        std::vector<std::unordered_map<std::string, const Stmt*>>> callableBindScopes_;
    Stmt* activeSpecialization_ = nullptr;
    int activeSpecializationDepth_ = 0;
    std::unordered_set<const Expr*> diagnosedGenericStatic_;

    void markSpecializationSites(Program& program);
    void materializeSpecializations(Program& program);
    void recordSpecialization(const Stmt* fn,
                              const std::unordered_map<std::string_view, Type>& subst,
                              const Expr* call);
    void genericStaticMissing(const Expr* site, Symbol* concrete,
                              std::string_view member, bool constructor);
    // bug #54: an overloaded call inside a specialized generic body found no
    // applicable overload for the concrete instantiation — the two-span form
    // (use site + instantiation note), the overload-set sibling of
    // genericStaticMissing (§4.3).
    void specializationOverloadMissing(const Expr* call, const char* what,
                                       const std::vector<Type>& args);
    bool callableTypeParam(std::string_view name) const;
    bool classTypeParam(std::string_view name) const;

    // walking
    void walk(std::vector<StmtPtr>& items, Scope* scope);
    void checkFunction(Stmt* fn, Scope* scope, Symbol* thisClass);
    bool writesThisField(Expr* lhs);
    std::string fieldNameOf(Expr* lhs);
    // const.md / techdesign-readonly §4.3: classify `e` (an assignment LHS,
    // or a mutating-method-call receiver) as a write to a const OR readonly
    // slot currently outside its write window. Returns the slot's name (empty
    // if the write is fine) and which modifier blocked it. A `const` field's
    // only legal write is its initializer (never a ctor assignment, §4.2); a
    // `readonly` field's window is the initializer OR the declaring class's
    // own constructor bodies (the window `const` fields used to have).
    BlockedWrite constBlockedWrite(Expr* e);
    // techdesign-readonly §4.2: is `e` a compile-time-constant expression
    // (the grammar a `const` field's initializer must satisfy)?
    bool isCompileTimeConstant(const Expr* e);
    bool typeMayBeValueStruct(const Type& t);
    void check(Stmt* s);

    // typing
    Type typeOf(const Expr* e);
    Type typeOfInner(const Expr* e);
    Type typeOfMember(const Expr* e);
    Type typeOfCall(const Expr* e);
    Type typeOfCallInner(const Expr* e, std::vector<char>& lambdaWalked);
    Type typeOfBinary(const Expr* e);
    Symbol* hygienicClass(const Expr* e) const;
    Symbol* visibleClass(std::string_view name) const;

    // LA-25 + F3 bound references: try to resolve `e`, a Member in VALUE
    // position, as a reference to a callable member. `::` produces an unbound
    // instance method (receiver becomes the first parameter), labeled
    // constructor, or namespace function; `.` produces a bound instance
    // method (a bare local/parameter or `this` is captured). `expected`, when
    // non-null, is the target function type used
    // to disambiguate an overloaded name (§2.2); null forces the
    // ambiguous-without-target error (§8.2) when the name is overloaded. On
    // success, rewrites `e` in place into the eta-expansion lambda it denotes
    // (§4.2 — every engine already runs an ordinary Lambda, P-5) and returns
    // its function type. Sets `isRef` to false when `e` isn't such a
    // reference at all — the caller falls back to its own member handling.
    Type tryResolveMethodRef(Expr* e, const Type* expected, bool& isRef);
    // True if `e` is a callable reference whose name is OVERLOADED (2+
    // candidates) — deferred exactly like a Lambda call argument (§2.2/§6)
    // until the outer call's overload is chosen. Doesn't itself resolve or
    // rewrite `e` (not `const`: it calls `typeOf` on `e`'s base, which — like
    // every other typeOf call — records checker state such as narrowing).
    bool isDeferredMethodRefArg(const Expr* e);
    // Pick among same-named candidates by the LA-25 unbound-reference shape:
    // one candidate needs no target; 2+ requires `expected` to equal exactly
    // one candidate's synthesized (recv?, params...) => ret signature.
    const Stmt* pickMethodRefOverload(const std::vector<const Stmt*>& cands,
                                      const std::string& recvCanon,
                                      const std::string& ctorRetCanon,
                                      const Type* expected, bool& ambiguous) const;
    std::string methodRefCanonical(const Stmt* fn, const std::string& recvCanon,
                                   const std::string& retCanon) const;
    // §4.2: perform the eta-expansion AST rewrite (Member -> Lambda) once a
    // single candidate is chosen; returns the synthesized function Type.
    Type rewriteAsMethodRef(Expr* e, const Stmt* target, Symbol* recvClass,
                           Symbol* ctorClass, const std::string& recvCanon,
                           const Type& retType);

    // Walk a lambda's body with its parameters bound to `paramTypes` (falling
    // back to declared types / unknown), returning the body's inferred return
    // type (expr body: its type; block body: the uniform Return type, else
    // unknown). This is the lambda-last leg of generic inference (Track 05 §1).
    Type checkLambdaBody(const Expr* lam, const std::vector<Type>& paramTypes);

    // An initializer/return expression, typed with a target type available so
    // generic construction can infer type args from the target (§9).
    Type typeInitExpr(const Expr* e, const TypeRef* expected);
    // Instantiate a (possibly generic) class from a chosen constructor + target.
    // Errors at `span` when a type argument has no inference source (§9).
    Type inferConstruction(Symbol* cls, const Stmt* ctor,
                           const std::vector<Type>& args, const TypeRef* expected,
                           SourceSpan span);

    // Overload resolution: choose among same-named candidates by argument types
    // (arity + applicability + most-specific, first-declared breaking ties).
    // Returns {chosen, applicableCount}; chosen is null if none apply.
    // `call`, when given, lets a deferred LAMBDA LITERAL argument (its Type is
    // Unknown — inferred later from whichever parameter wins, §9 "lambda-last")
    // score against only a function-typed parameter instead of every parameter
    // leniently (bug.md #34). Callers with no arguments (empty `args`) never
    // need it; omit to keep those call sites unchanged.
    const Stmt* pickOverload(const std::vector<const Stmt*>& cands,
                             const std::vector<Type>& args, bool& anyApplicable,
                             const Expr* call = nullptr);
    std::vector<const Stmt*> methodOverloads(Symbol* cls, std::string_view name);
    std::vector<const Stmt*> ctorOverloads(Symbol* cls, std::string_view label);
    std::vector<const Stmt*> functionOverloads(std::string_view name);

    // Unified call binding: map positional/named args per candidate, fill each
    // omitted parameter from its default or lexical bind, choose by type score
    // then fewest fills, and normalize the call to a full positional list.
    const Stmt* pickInjecting(const std::vector<const Stmt*>& cands,
                              std::vector<Type>& argTypes, Expr* call, bool& ok,
                              bool& diagnosed);
    // bind/inject scope management. pushBindScope pre-scans a body's direct
    // factory `bind`s (dup-in-scope = error), so binds are visible block-wide.
    void pushBindScope(const std::vector<StmtPtr>& items);
    void popBindScope();
    const Stmt* lookupBind(const std::string& canonical);   // nearest-wins

    // Generic call inference/substitution (uniform for methods and functions).
    // When `call`/`lambdaWalked` are supplied, lambda arguments are checked
    // LAST (after value arguments bind type vars): each lambda's param types
    // come from its function-type parameter position (substituted), its body
    // is walked once, and its inferred return type unifies against the
    // function type's return position — binding U in `map<U>((T) => U fn)`.
    Type genericReturn(Symbol* cls, const Stmt* fn, const Type& receiver,
                       const std::vector<Type>& args,
                       const Expr* call = nullptr,
                       std::vector<char>* lambdaWalked = nullptr);
    void unify(const TypeRef* param, const Type& arg,
               std::unordered_map<std::string_view, Type>& subst);
    Type substitute(const TypeRef* t,
                    const std::unordered_map<std::string_view, Type>& subst);

    // narrowing
    struct Fact {
        std::string path;
        Type thenType, elseType;
        bool hasThen = false, hasElse = false;
    };
    static std::string pathOf(const Expr* e);
    Type unionMinus(const Type& u, const std::string& removeCanonical) const;
    void analyzeCond(const Expr* cond, std::vector<Fact>& out, bool negated);
    void applyFacts(const std::vector<Fact>& facts, bool thenSide,
                    std::unordered_map<std::string, Type>& saved);
    void invalidatePath(const std::string& path);

    // helpers
    Type primType(const char* name) const;   // int/string/bool/float class type
    Type fromTypeRef(const TypeRef* t) const;
    bool assignable(const Type& from, const Type& to) const;
    bool isSubclass(Symbol* a, Symbol* b) const;
    Type* localLookup(std::string_view name);
    LocalBinding* localBinding(std::string_view name);
    Type error(SourceSpan span, std::string msg);

    // designs/complete/techdesign-class-method-dispatch.md §4.1: a program-wide index —
    // (T, "name\x1f canonical-sig") present iff some strict subclass of T
    // declares its own method with that name+signature (i.e. it overrides the
    // method T resolves to). Built once, lazily, on first use.
    std::unordered_map<const Symbol*, std::unordered_set<std::string>> overriddenBelow_;
    bool overrideIndexBuilt_ = false;
    void buildOverrideIndex();
    bool isOverriddenBelow(const Symbol* T, const Stmt* m) const;
    // §2: whether a call resolving to method `m` on receiver static type `T`
    // must dispatch on the runtime object rather than bind statically.
    bool dispatchesDynamically(const Symbol* T, const Stmt* m) const;
    // §5.4 (M3): true when `m` shares its (name, arity) with another method
    // slot on `T`'s own shape — the runtime name+arity lookup can't
    // disambiguate them by type. By the collapse rule, a sibling present on
    // T's shape is present on every subclass's shape too, so checking T is
    // sufficient for the shape this diagnostic targets (§5.4's worked case).
    bool overrideDispatchAmbiguous(const Symbol* T, const Stmt* m) const;
    // The one shared S1/S2/S3 helper (§4.2): decides dispatchesDynamically,
    // building the override index on first use, and — when dynamic because of
    // an override (not the interface case) — emits the §5.4 loud diagnostic
    // if the runtime lookup would be ambiguous. Returns true (dynamic).
    bool resolveDispatch(Symbol* T, const Stmt* m, SourceSpan span);
};
