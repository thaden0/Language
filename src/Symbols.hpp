#pragma once
#include "Ast.hpp"
#include <cstdint>
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

struct Scope {
    Scope* parent = nullptr;
    std::unordered_map<std::string_view, std::vector<Symbol*>> names;

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
