#pragma once
#include "core/Ast.hpp"
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
//  Semantic model: scopes, symbols, and class shapes.
//
//  The whole program gathers into one tree of named scopes (§12): file
//  boundaries dissolve, namespace boundaries persist. A Symbol is a declared
//  entity; a Scope maps names to (possibly several) Symbols — several because
//  the language resolves by type, so one name can bind to many differently-
//  typed slots / overloads.
// ---------------------------------------------------------------------------

enum class SymbolKind {
    Namespace,
    Class,        // includes interfaces (isInterface on the decl)
    Function,
    Var,
    TypeParam,    // a class's <T>
    Primitive,    // int, string, bool, void
};

struct Scope;

// One slot in a class's shape: a typed member at a label. Fields and methods
// are both slots (§6.5); constructors and accessors are not (ctors aren't
// inherited; get/set are views over a field slot).
struct Slot {
    std::string_view name;
    std::string canonical;    // field: type; method: "(params) -> ret"
    // F6: keep the callable boundary recorded at construction time.  Parsing
    // `canonical` later is not sound because a parameter may itself be a
    // function type containing an arrow.
    std::string paramsCanon;   // method only: "(params)"
    std::string retCanon;      // method only: return type
    Symbol* source = nullptr; // the class that declared this slot (for ::)
    const Stmt* decl = nullptr;
    Access access = Access::Default;
    bool isMethod = false;
    bool distinct = false;    // kept separate under a same-name+same-type clash
    bool isConst = false;     // const.md: field's write view is initializer + own ctors only
    bool isReadonly = false;  // techdesign-readonly: field's write view is initializer OR any
                               // declaring-class ctor, exactly once (fields only)
    bool isWeak = false;      // F5: slot stores a weak proxy, not the value itself
};

struct Shape {
    std::vector<Slot> slots;
    bool built = false;
    bool building = false;    // inheritance-cycle guard
};

struct Symbol {
    SymbolKind kind;
    std::string_view name;
    const Stmt* decl = nullptr;   // AST node (null for primitives)
    Scope* scope = nullptr;       // inner scope for namespaces/classes
    Shape shape;                  // meaningful for classes
    bool isPrimitive = false;     // int/string/bool/float: value-type object mask
    bool isValue = false;         // `struct`: user value type (the object mask, generalized)

    bool isInterface() const {
        return kind == SymbolKind::Class && decl && decl->isInterface;
    }
    // A value type is copied by value and has no identity: the built-in object
    // mask (primitives) and user `struct`s are the same category (§9).
    bool isValueType() const { return isPrimitive || isValue; }
};

// ---- Columnar Array<struct> eligibility (techdesign-columnar-arrays.md §3) ----
// One source of truth shared by Lower (fusion + ownership), LlvmGen and CGen
// (descriptor emission). The runtime agrees because codegen emits lv_col_eligible/
// lv_col_typecode from exactly these functions, and the field-slot enumeration
// (non-method shape order) matches fieldKeysOf/fieldSlotOf.

// A field's scalar LV tag, or 0 if it is not a columnar scalar. Values equal the
// LvValue tags so tag synthesis on gather is a copy, not a mapping.
inline int columnarTypecodeOf(const std::string& canonical) {
    if (canonical == "int")   return 1;    // LV_INT
    if (canonical == "float") return 2;    // LV_FLOAT
    if (canonical == "bool")  return 3;    // LV_BOOL
    if (canonical == "char")  return 10;   // LV_CHAR
    return 0;
}

// A value struct is columnar-eligible iff it has >=1 field slot and every field
// slot is a plain scalar (int/float/bool/char), none weak or distinct. Structs
// are flat (no multiple inheritance) so distinct never actually occurs, but the
// check is explicit.
inline bool columnarEligibleStruct(const Symbol* cls) {
    if (!cls || !cls->isValue) return false;
    int fields = 0;
    for (const Slot& s : cls->shape.slots) {
        if (s.isMethod) continue;
        if (s.isWeak || s.distinct) return false;
        if (columnarTypecodeOf(s.canonical) == 0) return false;
        fields++;
    }
    return fields > 0;
}

// The columnar column index (== runtime slot k, non-method shape order) of a
// field, or -1 if not a plain field slot of `cls`.
inline int columnarFieldSlot(const Symbol* cls, std::string_view name) {
    if (!cls) return -1;
    int k = 0;
    for (const Slot& s : cls->shape.slots) {
        if (s.isMethod) continue;
        if (s.name == name) return k;
        k++;
    }
    return -1;
}

// ---- Bare-field auto-construction (§3) ---------------------------------------
// One source of truth for the oracle (Eval::initFields) and the lowerer
// (Lowerer::synthesizeInit): a bare (no-initializer) field auto-constructs on
// declaration exactly when its type is a *constructable class* — a user
// class/struct with a decl, not a primitive, an interface, or a native
// collection (Array/Map/Block carry their own value default). Optional/union
// fields resolve to a null field symbol here (the TypeRef is a union node, not
// a class) and so are excluded — they default to `None`.
inline bool isConstructableClassField(const Symbol* fcls) {
    return fcls && fcls->kind == SymbolKind::Class && !fcls->isPrimitive &&
           fcls->decl && !fcls->decl->isInterface &&
           fcls->name != "Array" && fcls->name != "Map" && fcls->name != "Block";
}

// The field type of a slot, or null (methods, optionals/unions, primitives-by-
// canonical, ...). Shared so every walker reads the type the same way.
inline const Symbol* fieldTypeOf(const Slot& s) {
    return (s.decl && s.decl->type) ? s.decl->type->resolvedSymbol : nullptr;
}

// Does `from` reach `target` through must-construct field edges? An edge exists
// for every non-method field whose type is a constructable class (value struct
// or reference class); optionals/collections/primitives are not edges, which is
// how `Node? next` breaks the cycle that `Node next` would form.
inline bool reachesThroughConstructFields(const Symbol* from, const Symbol* target,
                                          std::unordered_set<const Symbol*>& seen) {
    for (const Slot& s : from->shape.slots) {
        if (s.isMethod) continue;
        if (s.decl && s.decl->init) continue;          // an initialized field never auto-constructs
        const Symbol* ft = fieldTypeOf(s);
        if (!isConstructableClassField(ft)) continue;
        if (ft == target) return true;
        if (seen.insert(ft).second && reachesThroughConstructFields(ft, target, seen))
            return true;
    }
    return false;
}

// A bare reference-class field whose type sits on a construction cycle (it can
// reach itself through non-optional constructable fields, e.g. `class Node {
// Node next; }` or a mutual `A{B b} B{A a}`) has no finite default, so it keeps
// the void/None default rather than recursing forever. Value structs cannot
// form such a cycle by value (infinite size — already rejected), so in practice
// this only ever gates reference-class fields.
inline bool onConstructionCycle(const Symbol* cls) {
    if (!isConstructableClassField(cls)) return false;
    std::unordered_set<const Symbol*> seen;
    return reachesThroughConstructFields(cls, cls, seen);
}

// Should a bare (no-initializer) field of this type auto-construct on
// declaration? Value structs always do (they cannot cycle); reference classes
// do unless they sit on a construction cycle. The one predicate both engines
// consult, so their field defaults stay byte-identical.
inline bool bareFieldAutoConstructs(const Symbol* fcls) {
    if (!isConstructableClassField(fcls)) return false;
    if (fcls->isValue) return true;
    return !onConstructionCycle(fcls);
}

// ---- Ctor-supplied bare fields (definite first-assignment elision) ----------
// §3 auto-construction of a bare reference-class field is a footgun when the
// constructor is going to overwrite that field anyway: the throwaway default is
// not merely wasted work, it is OBSERVABLE — the discarded object's own ctor can
// have side effects or throw before the real value is ever bound. Sonar's App is
// the canonical case: a `class W { App app; new W(App a) { app = a; } }` wrapper
// eagerly default-constructs a SECOND `App()` at $init time, which the single-app
// rule turns into a construction-time throw before `app = a` ever runs.
//
// The fix generalizes the existing "an initialized field never auto-constructs"
// rule to "a ctor-supplied field never auto-constructs": if every constructor
// definite-first-assigns the field (a plain top-level `f = rhs;` reached before
// any read of `f`, rhs not reading `f`), the ctor supplies the value and the
// field takes the plain void/None default instead — exactly the slot state a
// construction-cycle field already relies on the ctor to fill. Scope is kept
// deliberately tight, sound-not-complete, mirroring the readonly write-once
// pass's top-level-statements-only discipline: reference classes only (value
// structs keep their byte-identical co-allocation path), own directly-declared
// fields only, and the class must have at least one constructor.

// Does this expr subtree reference field `f` (bare implicit-this name or an
// explicit `this.f`)? A same-named local over-matches, which only makes the
// caller MORE conservative (it keeps auto-construction), so this stays sound.
inline bool stmtReferencesField(const Stmt* s, std::string_view f);
inline bool exprReferencesField(const Expr* e, std::string_view f) {
    if (!e) return false;
    if (e->kind == ExprKind::Name && e->text == f) return true;
    if (e->kind == ExprKind::Member && !e->colon && e->a &&
        e->a->kind == ExprKind::This && e->text == f) return true;
    if (exprReferencesField(e->a.get(), f) || exprReferencesField(e->b.get(), f) ||
        exprReferencesField(e->c.get(), f)) return true;
    for (const ExprPtr& x : e->list) if (exprReferencesField(x.get(), f)) return true;
    if (e->block && stmtReferencesField(e->block.get(), f)) return true;   // lambda block
    for (const MatchArm& a : e->arms) {
        if (exprReferencesField(a.value.get(), f) ||
            exprReferencesField(a.bodyExpr.get(), f)) return true;
        if (a.bodyBlock && stmtReferencesField(a.bodyBlock.get(), f)) return true;
    }
    return false;
}
inline bool stmtReferencesField(const Stmt* s, std::string_view f) {
    if (!s) return false;
    if (exprReferencesField(s->expr.get(), f) || exprReferencesField(s->init.get(), f) ||
        exprReferencesField(s->forStep.get(), f)) return true;
    for (const StmtPtr& b : s->body) if (stmtReferencesField(b.get(), f)) return true;
    if (stmtReferencesField(s->thenBranch.get(), f) ||
        stmtReferencesField(s->elseBranch.get(), f) ||
        stmtReferencesField(s->forInit.get(), f)) return true;
    for (const CatchClause& c : s->catches) if (stmtReferencesField(c.body.get(), f)) return true;
    return false;
}

// If `e` is a plain (non-compound) assignment whose target is field `f`, return
// its rhs; else null. `f = rhs` and `this.f = rhs` both count; `f += rhs` (a
// read) does not.
inline const Expr* fieldPlainAssignRhs(const Expr* e, std::string_view f) {
    if (!e || e->kind != ExprKind::Binary || e->op != TokenKind::Eq) return nullptr;
    const Expr* lhs = e->a.get();
    if (!lhs) return nullptr;
    bool isTarget = (lhs->kind == ExprKind::Name && lhs->text == f) ||
                    (lhs->kind == ExprKind::Member && !lhs->colon && lhs->a &&
                     lhs->a->kind == ExprKind::This && lhs->text == f);
    return isTarget ? e->b.get() : nullptr;
}

// Does this ctor definite-first-assign field `f`? Scan its top-level statements
// in order; the FIRST one that references `f` must be a plain `f = rhs;` whose
// rhs does not itself read `f`. A brace-less single-statement body is one stmt.
inline bool ctorFirstAssignsField(const Stmt* ctor, std::string_view f) {
    const Stmt* body = ctor ? ctor->memberBody.get() : nullptr;
    if (!body) return false;
    if (body->kind == StmtKind::Block) {
        for (const StmtPtr& st : body->body) {
            const Stmt* s = st.get();
            if (s->kind == StmtKind::ExprStmt)
                if (const Expr* rhs = fieldPlainAssignRhs(s->expr.get(), f))
                    return !exprReferencesField(rhs, f);
            if (stmtReferencesField(s, f)) return false;   // read/nested-assign first
        }
        return false;   // never assigned at top level
    }
    if (body->kind == StmtKind::ExprStmt)
        if (const Expr* rhs = fieldPlainAssignRhs(body->expr.get(), f))
            return !exprReferencesField(rhs, f);
    return false;
}

// The predicate both engines consult for a bare field slot: skip §3 auto-
// construction because a constructor supplies the value.
inline bool bareFieldSuppliedByCtor(const Symbol* cls, const Slot& s) {
    if (s.isMethod) return false;
    if (s.decl && s.decl->init) return false;                 // initialized elsewhere
    const Symbol* fcls = fieldTypeOf(s);
    if (!bareFieldAutoConstructs(fcls) || fcls->isValue) return false;  // ref classes only
    if (!cls || !cls->decl) return false;
    // Own, directly-declared bare field only — an inherited/distinct slot's
    // construction + assignment discipline belongs to its declaring class.
    bool ownField = false;
    for (const StmtPtr& m : cls->decl->body)
        if (m->kind == StmtKind::Member && !m->isCtor && !m->callable &&
            !m->isGet && !m->isSet && !m->init && m->name == s.name) { ownField = true; break; }
    if (!ownField) return false;
    bool anyCtor = false;
    for (const StmtPtr& m : cls->decl->body) {
        if (m->kind != StmtKind::Member || !m->isCtor) continue;
        anyCtor = true;
        if (!ctorFirstAssignsField(m.get(), s.name)) return false;
    }
    return anyCtor;
}

struct Scope {
    Scope* parent = nullptr;
    std::unordered_map<std::string_view, std::vector<Symbol*>> names;

    // Type-keyed bind table (system-binds.md §7.2 / designs/complete/techdesign-block-scoped-use.md
    // §3.1): the factory `bind T => …;` statements this scope owns, keyed by the
    // bound type's canonical string (value = the factory `Bind` stmt). Filled by
    // the Resolver (fillBinds) at resolve time for block, namespace, and global
    // scopes; empty for scopes that carry no binds (file overlays, most blocks).
    // One scope object now carries both tables — names AND binds — with one
    // lifecycle, retiring the Checker's parallel per-scope bind stack.
    std::unordered_map<std::string, const Stmt*> binds;

    // Look up in this scope only.
    const std::vector<Symbol*>* localLookup(std::string_view n) const {
        auto it = names.find(n);
        return it == names.end() ? nullptr : &it->second;
    }
    // Look up here, then walk outward through parents.
    Symbol* lookup(std::string_view n) const {
        for (const Scope* s = this; s; s = s->parent) {
            auto it = s->names.find(n);
            if (it != s->names.end() && !it->second.empty()) return it->second.front();
        }
        return nullptr;
    }
    // The factory `bind` registered for `canonical` in THIS scope only, or null.
    const Stmt* localBind(const std::string& canonical) const {
        auto it = binds.find(canonical);
        return it == binds.end() ? nullptr : it->second;
    }
};

// Owns all symbols and scopes for one compilation (arena-style pools).
struct Sema {
    std::vector<std::unique_ptr<Symbol>> symbols;
    std::vector<std::unique_ptr<Scope>> scopes;
    Scope* global = nullptr;

    // Per-file lexical import scopes (bug.md #8). Each source file's top-level
    // `uses` imports into its own overlay scope (a child of `global`), so a
    // top-of-file import covers exactly that file and does not leak program-wide.
    // `fileRanges[i]` is [offset, end) of file i in the combined buffer;
    // `fileScopes[i]` is that file's overlay scope. A lone (non-project) source
    // is one file covering the whole buffer.
    std::vector<std::pair<uint32_t, uint32_t>> fileRanges;
    std::vector<Scope*> fileScopes;

    // The import overlay scope owning a combined-buffer offset, or `global` if
    // the offset predates every file (synthesized nodes) or no file map is set.
    Scope* fileScopeFor(uint32_t offset) const {
        for (size_t i = 0; i < fileRanges.size() && i < fileScopes.size(); ++i)
            if (offset >= fileRanges[i].first && offset < fileRanges[i].second)
                return fileScopes[i];
        return global;
    }

    // True when `sc` is one of the per-file `uses`/`use` import overlays rather
    // than a genuine declaration scope. Lets name resolution rank a file's own
    // declarations ahead of names merely pulled in by a bulk import (bug.md #78).
    bool isFileOverlay(const Scope* sc) const {
        for (const Scope* fs : fileScopes)
            if (fs == sc) return true;
        return false;
    }

    // Symbols minted AFTER resolution — the checker monomorphizes columnar-eligible
    // generic value structs (techdesign-generic-value-struct-columnar.md) from a
    // `const Sema&`. They live in a `mutable` append-only arena (const minting
    // methods), owned by the same Sema that outlives codegen, and are reached only
    // through IR `Op` symbol pointers — never registered in any scope, so name
    // resolution is unaffected. The deque keeps `Symbol::name` (a string_view)
    // backing addresses stable across growth, which a vector would not.
    mutable std::deque<std::string> nameArena_;
    mutable std::vector<std::unique_ptr<Symbol>> lateSymbols_;
    std::string_view intern(std::string s) const {
        nameArena_.push_back(std::move(s));
        return nameArena_.back();
    }
    Symbol* newLateSymbol(SymbolKind k, std::string_view name, const Stmt* decl) const {
        lateSymbols_.push_back(std::make_unique<Symbol>());
        Symbol* s = lateSymbols_.back().get();
        s->kind = k; s->name = name; s->decl = decl;
        return s;
    }

    Symbol* newSymbol(SymbolKind k, std::string_view name, const Stmt* decl = nullptr) {
        symbols.push_back(std::make_unique<Symbol>());
        Symbol* s = symbols.back().get();
        s->kind = k; s->name = name; s->decl = decl;
        return s;
    }
    Scope* newScope(Scope* parent) {
        scopes.push_back(std::make_unique<Scope>());
        Scope* s = scopes.back().get();
        s->parent = parent;
        return s;
    }
};
