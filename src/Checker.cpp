#include "Checker.hpp"
#include <algorithm>
#include <functional>
#include <iterator>

namespace {

Type unknown() { return Type{TKind::Unknown, nullptr, "", {}, nullptr, {}}; }
Type primitive(std::string c) { return Type{TKind::Primitive, nullptr, std::move(c), {}, nullptr, {}}; }
Type classType(Symbol* s) { return Type{TKind::Class, s, std::string(s->name), {}, nullptr, {}}; }
Type typeValue(Symbol* s) { return Type{TKind::TypeValue, s, std::string(s->name), {}, nullptr, {}}; }
Type funcRef(Symbol* s) { return Type{TKind::FuncRef, s, std::string(s->name), {}, nullptr, {}}; }

// bug #37/#46: resolve a namespace qualifier that may be a NESTED chain
// (`A::B::…`) to its innermost namespace Symbol. The root is looked up through
// the ordinary scope chain (honoring `uses`/file overlays); each further
// segment descends into that namespace's own scope.
static Symbol* nsChainSym(Scope* scope, const Expr* e) {
    if (!e || !scope) return nullptr;
    if (e->kind == ExprKind::Name) {
        Symbol* s = scope->lookup(e->text);
        return (s && s->kind == SymbolKind::Namespace) ? s : nullptr;
    }
    if (e->kind == ExprKind::Member && e->colon) {
        Symbol* base = nsChainSym(scope, e->a.get());
        if (base && base->scope)
            if (const std::vector<Symbol*>* v = base->scope->localLookup(e->text))
                for (Symbol* s : *v)
                    if (s->kind == SymbolKind::Namespace) return s;
    }
    return nullptr;
}

const char* opSymbol(TokenKind k) {
    switch (k) {
        case TokenKind::Plus: return "+";      case TokenKind::Minus: return "-";
        case TokenKind::Star: return "*";      case TokenKind::Slash: return "/";
        case TokenKind::Percent: return "%";   case TokenKind::EqEq: return "==";
        case TokenKind::BangEq: return "!=";   case TokenKind::Lt: return "<";
        case TokenKind::Gt: return ">";        case TokenKind::Le: return "<=";
        case TokenKind::Ge: return ">=";       case TokenKind::LtLt: return "<<";
        case TokenKind::GtGt: return ">>";
        case TokenKind::Pipe: return "|";   case TokenKind::Amp: return "&";
        case TokenKind::Caret: return "^";  case TokenKind::Tilde: return "~";
        case TokenKind::Bang: return "!";
        default: return "?";
    }
}

bool isComparison(TokenKind k) {
    switch (k) {
        case TokenKind::EqEq: case TokenKind::BangEq: case TokenKind::Lt:
        case TokenKind::Gt:   case TokenKind::Le:     case TokenKind::Ge: return true;
        default: return false;
    }
}

bool isCompoundAssign(TokenKind k) {
    switch (k) {
        case TokenKind::PlusEq: case TokenKind::MinusEq: case TokenKind::StarEq:
        case TokenKind::SlashEq: case TokenKind::PercentEq: return true;
        default: return false;
    }
}

// techdesign-readonly §4.2 OQ-1: the v1 compile-time-constant *operator* set
// for `const` field initializers — arithmetic/bitwise binary ops over
// already-constant operands (`const int BOTH = A | B;`), plus unary +/-/~.
bool isConstFoldableOp(TokenKind k) {
    switch (k) {
        case TokenKind::Plus: case TokenKind::Minus: case TokenKind::Star:
        case TokenKind::Slash: case TokenKind::Percent:
        case TokenKind::LtLt: case TokenKind::GtGt:
        case TokenKind::Amp: case TokenKind::Pipe: case TokenKind::Caret:
            return true;
        default: return false;
    }
}
bool isConstFoldableUnaryOp(TokenKind k) {
    switch (k) {
        case TokenKind::Plus: case TokenKind::Minus: case TokenKind::Tilde:
            return true;
        default: return false;
    }
}

// techdesign-readonly §4.4: does top-level statement-expression `e` assign
// (plain or compound) to the bare field `fieldName` (or `this.fieldName`)?
// Used by the readonly definite-assignment / write-once pass, which only
// looks at a constructor's TOP-LEVEL statements (v1 restriction, §4.4/§8 OQ-2).
bool exprAssignsField(const Expr* e, std::string_view fieldName) {
    if (!e || e->kind != ExprKind::Binary) return false;
    if (e->op != TokenKind::Eq && !isCompoundAssign(e->op)) return false;
    const Expr* lhs = e->a.get();
    if (!lhs) return false;
    if (lhs->kind == ExprKind::Name && lhs->text == fieldName) return true;
    if (lhs->kind == ExprKind::Member && lhs->a && lhs->a->kind == ExprKind::This &&
        lhs->text == fieldName) return true;
    return false;
}

// The top-level statements of a member body: a Block's direct children, or
// (for a brace-less single-statement ctor body, e.g. `new C(T x) this.f = x;`)
// the one statement itself.
std::vector<Stmt*> topLevelStmts(Stmt* body) {
    std::vector<Stmt*> out;
    if (!body) return out;
    if (body->kind == StmtKind::Block) {
        for (StmtPtr& st : body->body) out.push_back(st.get());
    } else {
        out.push_back(body);
    }
    return out;
}

// Slots in a shape with a given name.
std::vector<const Slot*> slotsNamed(const Shape& sh, std::string_view name) {
    std::vector<const Slot*> out;
    for (const Slot& s : sh.slots)
        if (s.name == name) out.push_back(&s);
    return out;
}

bool allSameType(const std::vector<const Slot*>& slots) {
    for (size_t i = 1; i < slots.size(); ++i)
        if (slots[i]->canonical != slots[0]->canonical) return false;
    return true;
}

// Does a declared type mention a type parameter anywhere (so it unifies rather
// than requiring an exact/assignable match during overload resolution)?
bool mentionsTypeParam(const TypeRef* t) {
    if (!t) return false;
    if (t->kind == TypeKind::Named) {
        if (t->resolvedSymbol && t->resolvedSymbol->kind == SymbolKind::TypeParam) return true;
        for (const TypeRefPtr& g : t->generics) if (mentionsTypeParam(g.get())) return true;
        return false;
    }
    if (t->kind == TypeKind::Union)
        for (const TypeRefPtr& m : t->members) { if (mentionsTypeParam(m.get())) return true; }
    if (t->kind == TypeKind::Function) {
        for (const TypeRefPtr& p : t->funcParams) if (mentionsTypeParam(p.get())) return true;
        if (mentionsTypeParam(t->funcRet.get())) return true;
    }
    return false;
}

// A deep copy of a TypeRef, sufficient for a synthesized `inject` argument's
// type (fromTypeRef reads name/canonical/resolvedSymbol/generics).
TypeRefPtr copyTypeRef(const TypeRef* t) {
    if (!t) return nullptr;
    auto r = std::make_unique<TypeRef>(t->kind);
    r->span = t->span;
    r->path = t->path;
    r->name = t->name;
    r->canonical = t->canonical;
    r->resolvedSymbol = t->resolvedSymbol;
    for (const TypeRefPtr& g : t->generics) r->generics.push_back(copyTypeRef(g.get()));
    for (const TypeRefPtr& m : t->members) r->members.push_back(copyTypeRef(m.get()));
    for (const TypeRefPtr& p : t->funcParams) r->funcParams.push_back(copyTypeRef(p.get()));
    if (t->funcRet) r->funcRet = copyTypeRef(t->funcRet.get());
    return r;
}

// LA-18 concrete-body cloner. This is deliberately semantic-preserving rather
// than source-reparsing: spans stay on the authored nodes, while all checker
// resolution annotations are cleared and rebuilt for the concrete tuple.
struct SpecializationCloner {
    Program& program;
    const std::unordered_map<std::string_view, Type>& subst;

    TypeRefPtr typeFrom(const Type& t, SourceSpan span) {
        if (t.kind == TKind::Union) {
            auto out = std::make_unique<TypeRef>(TypeKind::Union);
            out->span = span; out->canonical = t.canonical;
            for (const Type& m : t.unionMembers) out->members.push_back(typeFrom(m, span));
            return out;
        }
        auto out = std::make_unique<TypeRef>(TypeKind::Named);
        out->span = span; out->canonical = t.canonical;
        out->resolvedSymbol = t.sym;
        if (t.sym) out->name = t.sym->name;
        else {
            program.synthNames.push_back(t.canonical);
            out->name = program.synthNames.back();
        }
        for (const Type& a : t.args) out->generics.push_back(typeFrom(a, span));
        return out;
    }

    TypeRefPtr type(const TypeRef* t) {
        if (!t) return nullptr;
        if (t->kind == TypeKind::Named && t->resolvedSymbol &&
            t->resolvedSymbol->kind == SymbolKind::TypeParam) {
            auto it = subst.find(t->name);
            if (it != subst.end()) {
                if (t->generics.empty()) return typeFrom(it->second, t->span);
                // Higher-kinded substitution is rejected before cloning.
            }
        }
        auto out = std::make_unique<TypeRef>(t->kind);
        out->span = t->span; out->path = t->path; out->name = t->name;
        out->canonical = t->canonical; out->resolvedSymbol = t->resolvedSymbol;
        for (const TypeRefPtr& g : t->generics) out->generics.push_back(type(g.get()));
        for (const TypeRefPtr& m : t->members) out->members.push_back(type(m.get()));
        for (const TypeRefPtr& p : t->funcParams) out->funcParams.push_back(type(p.get()));
        out->funcRet = type(t->funcRet.get());
        return out;
    }

    Param param(const Param& p) {
        Param out;
        out.type = type(p.type.get()); out.name = p.name; out.isConst = p.isConst;
        out.span = p.span; out.defaultValue = expr(p.defaultValue.get());
        out.defaultFolded = p.defaultFolded;
        return out;
    }

    ExprPtr expr(const Expr* e) {
        if (!e) return nullptr;
        auto out = std::make_unique<Expr>(e->kind);
        out->span = e->span; out->text = e->text; out->op = e->op;
        out->colon = e->colon; out->optChain = e->optChain;
        out->isComptime = e->isComptime; out->isMacroCall = e->isMacroCall;
        out->isRawSegment = e->isRawSegment; out->isQuasiPayload = e->isQuasiPayload;
        out->isRawString = e->isRawString;
        out->singleQuoted = e->singleQuoted;
        out->charLit = e->charLit; out->argLabel = e->argLabel;
        out->weakField = e->weakField;
        out->weakDirect = e->weakDirect;
        out->argsNormalized = e->argsNormalized;
        out->hygienicDecl = e->hygienicDecl;
        out->genericStaticSite = e->genericStaticSite;
        out->genericStaticParam = e->genericStaticParam;
        out->a = expr(e->a.get()); out->b = expr(e->b.get()); out->c = expr(e->c.get());
        for (const ExprPtr& x : e->list) out->list.push_back(expr(x.get()));
        for (const Param& p : e->params) out->params.push_back(param(p));
        out->block = stmt(e->block.get()); out->type = type(e->type.get());
        for (const MatchArm& a : e->arms) {
            MatchArm b;
            b.isElse = a.isElse; b.span = a.span; b.type = type(a.type.get());
            b.value = expr(a.value.get()); b.bodyExpr = expr(a.bodyExpr.get());
            b.bodyBlock = stmt(a.bodyBlock.get()); out->arms.push_back(std::move(b));
        }
        // Rewrite only the marked left operand, not value variables that happen
        // to share the type parameter's spelling.
        if (out->genericStaticSite && out->a && out->a->kind == ExprKind::Name) {
            auto it = subst.find(out->genericStaticParam);
            if (it != subst.end() && it->second.sym) {
                out->a->text = it->second.sym->name;
                if (it->second.sym->decl) out->a->hygienicDecl = it->second.sym->decl;
            }
        }
        return out;
    }

    StmtPtr stmt(const Stmt* s) {
        if (!s) return nullptr;
        auto out = std::make_unique<Stmt>(s->kind);
        out->span = s->span; out->access = s->access; out->name = s->name;
        out->blockScope = s->blockScope; out->enclosingNs = s->enclosingNs;
        out->isInterface = s->isInterface; out->isValue = s->isValue;
        out->isAttribute = s->isAttribute;
        out->isCtor = s->isCtor; out->isGet = s->isGet; out->isSet = s->isSet;
        out->isMutating = s->isMutating; out->distinct = s->distinct;
        out->isConst = s->isConst; out->isReadonly = s->isReadonly; out->isWeak = s->isWeak;
        out->isUsing = s->isUsing; out->usingClose = s->usingClose;
        out->callable = s->callable; out->selector = s->selector;
        out->inferred = s->inferred; out->isComptime = s->isComptime;
        out->forInProtocol = s->forInProtocol;
        out->type = type(s->type.get());
        for (const Param& p : s->params) out->params.push_back(param(p));
        out->memberBody = stmt(s->memberBody.get()); out->init = expr(s->init.get());
        out->expr = expr(s->expr.get());
        for (const StmtPtr& x : s->body) out->body.push_back(stmt(x.get()));
        for (const TypeRefPtr& b : s->bases) out->bases.push_back(type(b.get()));
        out->thenBranch = stmt(s->thenBranch.get()); out->elseBranch = stmt(s->elseBranch.get());
        out->forInit = stmt(s->forInit.get()); out->forStep = expr(s->forStep.get());
        for (const CatchClause& c : s->catches) {
            CatchClause d; d.type = type(c.type.get()); d.name = c.name;
            d.body = stmt(c.body.get()); out->catches.push_back(std::move(d));
        }
        return out;
    }
};

// LA-25 §4.2: fixed parameter-name pool for synthesized eta-expansion lambdas.
// Reusing these names across every reference is safe — each synthesized
// lambda is its own fresh scope with no captures (info.md §15: "these
// closures capture nothing"), so identical names across different lambdas
// never collide.
//
// LA-31 R1: these names must be re-lexable, because `--expand` now runs the
// Checker and prints these eta-lambdas as ordinary source that the round-trip
// harness recompiles. A `$` prefix is reserved for quasiquote holes and fails
// re-lexing, so use a `__mr`-prefixed identifier (a user collision inside the
// exact synthesized lambda scope is not reachable — the body references only
// these params and the receiver).
const std::string& methodRefParamName(size_t i) {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> v;
        for (int i = 0; i < 24; ++i) v.push_back("__mr" + std::to_string(i));
        return v;
    }();
    return names.at(i);
}

// Accessors aren't shape slots, so find one by scanning the class + its bases.
const Stmt* findAccessorDecl(Symbol* cls, std::string_view name, bool wantGet) {
    if (!cls || !cls->decl) return nullptr;
    for (const StmtPtr& m : cls->decl->body)
        if (((wantGet && m->isGet) || (!wantGet && m->isSet)) && m->name == name)
            return m.get();
    for (const TypeRefPtr& base : cls->decl->bases)
        if (const Stmt* a = findAccessorDecl(base->resolvedSymbol, name, wantGet)) return a;
    return nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
//  helpers
// ---------------------------------------------------------------------------

Type Checker::error(SourceSpan span, std::string msg) {
    sink_.error(span, std::move(msg));
    return Type{TKind::Error, nullptr, "", {}, nullptr, {}};
}

void Checker::noteBlockScopedImport(std::string_view name) {
    // DFS a statement subtree for a Block whose blockScope imports `name`, and
    // cite the import that brought it in. Returns true once a note is emitted.
    auto scan = [&](const Stmt* root) -> bool {
        std::vector<const Stmt*> stack{root};
        while (!stack.empty()) {
            const Stmt* s = stack.back();
            stack.pop_back();
            if (!s) continue;
            if (s->kind == StmtKind::Block && s->blockScope &&
                s->blockScope->localLookup(name)) {
                // Prefer a selective `use` naming exactly this name; else the
                // first bulk `uses`/`use` that could have supplied it.
                const Stmt* cite = nullptr;
                for (const StmtPtr& c : s->body) {
                    if (!c) continue;
                    if (c->kind == StmtKind::Use && c->name == name) { cite = c.get(); break; }
                    if (!cite && (c->kind == StmtKind::UsesImport || c->kind == StmtKind::Use))
                        cite = c.get();
                }
                if (cite) {
                    const char* kw = cite->kind == StmtKind::Use ? "use" : "uses";
                    sink_.note(cite->span, "'" + std::string(name) + "' is imported by a '" +
                               kw + "' here, but that import is scoped to its enclosing block "
                               "(imports are lexical — imports.md §2)");
                    return true;
                }
            }
            for (const StmtPtr& c : s->body) stack.push_back(c.get());
            stack.push_back(s->thenBranch.get());
            stack.push_back(s->elseBranch.get());
            stack.push_back(s->forInit.get());
            for (const CatchClause& cc : s->catches) stack.push_back(cc.body.get());
        }
        return false;
    };
    if (curMember_ && curMember_->memberBody && scan(curMember_->memberBody.get())) return;
    if (program_)
        for (const StmtPtr& top : program_->items)
            if (top && top->kind == StmtKind::Block && scan(top.get())) return;
}

Type Checker::primType(const char* name) const {
    Symbol* s = sema_.global ? sema_.global->lookup(name) : nullptr;
    return (s && s->kind == SymbolKind::Class) ? classType(s) : primitive(name);
}

Type Checker::fromTypeRef(const TypeRef* t) const {
    if (!t) return unknown();
    switch (t->kind) {
        case TypeKind::Inferred:
            return unknown();
        case TypeKind::Union: {
            Type u{TKind::Union, nullptr, t->canonical, {}, nullptr, {}};
            for (const TypeRefPtr& m : t->members) u.unionMembers.push_back(fromTypeRef(m.get()));
            return u;
        }
        case TypeKind::Function: {
            Type f{TKind::FuncRef, nullptr, t->canonical, {}, nullptr, {}};
            f.ret = std::make_shared<Type>(fromTypeRef(t->funcRet.get()));
            return f;
        }
        case TypeKind::Named: {
            Symbol* s = t->resolvedSymbol;
            if (s && s->kind == SymbolKind::Primitive) return primitive(t->canonical);
            if (s && s->kind == SymbolKind::Class) {
                Type r{TKind::Class, s, t->canonical, {}, nullptr, {}};
                for (const TypeRefPtr& g : t->generics) r.args.push_back(fromTypeRef(g.get()));
                return r;
            }
            return unknown();   // type param or unresolved
        }
    }
    return unknown();
}

bool Checker::isSubclass(Symbol* a, Symbol* b) const {
    if (!a || !b) return false;
    if (a == b) return true;
    if (!a->decl) return false;
    for (const TypeRefPtr& base : a->decl->bases)
        if (isSubclass(base->resolvedSymbol, b)) return true;
    return false;
}

namespace {
// Call f on every proper ancestor of X (X's transitive bases, not X itself) —
// the same base-graph walk as Checker::isSubclass. A diamond visits an
// ancestor more than once; harmless, callers insert into a set.
void walkProperAncestors(const Symbol* X, const std::function<void(const Symbol*)>& f) {
    if (!X || !X->decl) return;
    for (const TypeRefPtr& base : X->decl->bases) {
        const Symbol* a = base->resolvedSymbol;
        if (!a) continue;
        f(a);
        walkProperAncestors(a, f);
    }
}
} // namespace

// designs/complete/techdesign-class-method-dispatch.md §4.1: precompute, once, which
// (class, name+signature) pairs are overridden by some strict subclass —
// O(1) per call-site query afterward.
void Checker::buildOverrideIndex() {
    if (overrideIndexBuilt_) return;
    overrideIndexBuilt_ = true;
    for (const std::unique_ptr<Symbol>& dp : sema_.symbols) {
        const Symbol* D = dp.get();
        if (!D || D->kind != SymbolKind::Class) continue;
        // For each EFFECTIVE method slot of D, mark (name+signature) as
        // "overridden below T" for every proper ancestor T whose OWN effective
        // decl for the same (name, signature) differs from D's — i.e. a
        // D-shaped runtime object would resolve the method to a decl a T-typed
        // static call would not. This is what forces dynamic dispatch.
        //
        // The previous formulation walked ancestors of the OVERRIDING class and
        // recorded the key unconditionally. That misses an override introduced
        // by a SIBLING mixin composed only at a leaf: for
        // `Panel : Container, Bordered` where `Bordered` overrides `kind`, the
        // caller `Container` is not an ancestor of `Bordered`, so a `this.kind()`
        // in `Container` devirtualized to the base — even though a `Panel`
        // receiver resolves `kind` to `Bordered::kind` (bug.md #56). Comparing
        // D's effective slot against each ancestor's catches that case and stays
        // identical to the old walk for single inheritance.
        for (const Slot& s : D->shape.slots) {
            if (!s.isMethod) continue;
            std::string key = std::string(s.name) + "\x1f" + s.canonical;
            walkProperAncestors(D, [&](const Symbol* T) {
                for (const Slot& ts : T->shape.slots)
                    if (ts.isMethod && ts.name == s.name && ts.canonical == s.canonical) {
                        if (ts.decl != s.decl) overriddenBelow_[T].insert(key);
                        return;
                    }
            });
        }
    }
}

bool Checker::isOverriddenBelow(const Symbol* T, const Stmt* m) const {
    if (!T || !m) return false;
    for (const Slot& s : T->shape.slots) {
        if (s.decl != m) continue;
        auto it = overriddenBelow_.find(T);
        return it != overriddenBelow_.end() &&
               it->second.count(std::string(s.name) + "\x1f" + s.canonical) != 0;
    }
    return false;
}

bool Checker::dispatchesDynamically(const Symbol* T, const Stmt* m) const {
    if (T && T->decl && T->decl->isInterface) return true;   // interfaces: unchanged
    return isOverriddenBelow(T, m);
}

bool Checker::overrideDispatchAmbiguous(const Symbol* T, const Stmt* m) const {
    if (!T || !m) return false;
    for (const Slot& s : T->shape.slots) {
        if (s.decl != m) continue;
        for (const Slot& other : T->shape.slots)
            if (other.decl != m && other.isMethod && other.name == s.name &&
                other.decl && other.decl->params.size() == m->params.size())
                return true;
        return false;
    }
    return false;
}

bool Checker::resolveDispatch(Symbol* T, const Stmt* m, SourceSpan span) {
    buildOverrideIndex();
    bool iface = T && T->decl && T->decl->isInterface;
    bool dyn = iface || isOverriddenBelow(T, m);
    if (dyn && !iface && overrideDispatchAmbiguous(T, m)) {
        std::string name(m->selector.text);
        error(span, "method '" + name + "' is overridden below '" + std::string(T->name) +
              "' but shares its arity with another overload '" + name + "' — the runtime "
              "dispatch cannot disambiguate by type; give the overloads distinct arities or "
              "names, or qualify the call explicitly");
    }
    return dyn;
}

// bug #48: two spellings of the same nominal type — a bare `Foo` and a
// namespace-qualified `B::Foo` that resolve to the SAME class symbol — must be
// recognized as identical for INVARIANT generic-argument matching, even though
// their rendered canonical strings differ. Compares by resolved symbol identity
// (recursing through generic args); falls back to canonical-string equality for
// primitives / type params that carry no symbol.
static bool sameNominalType(const Type& a, const Type& b) {
    if (a.canonical == b.canonical) return true;
    if (a.kind == TKind::Class && b.kind == TKind::Class && a.sym && a.sym == b.sym) {
        if (a.args.size() != b.args.size()) return false;
        for (size_t i = 0; i < a.args.size(); ++i)
            if (!sameNominalType(a.args[i], b.args[i])) return false;
        return true;
    }
    return false;
}

bool Checker::assignable(const Type& from, const Type& to) const {
    if (from.kind == TKind::Unknown || from.kind == TKind::Error) return true;   // lenient
    if (to.kind == TKind::Unknown || to.kind == TKind::Error) return true;
    if (from.kind == TKind::TypeValue || from.kind == TKind::FuncRef) return true;

    if (from.kind == TKind::Union && to.kind == TKind::Union) {
        // Namespace qualification can give two spellings to the same nominal
        // member (`Color | None` inside its namespace versus
        // `Palette::Color | None` outside). Compare union members nominally,
        // not only by the rendered canonical string.
        for (const Type& member : from.unionMembers)
            if (!assignable(member, to)) return false;
        return true;
    }
    if (to.kind == TKind::Union) {
        for (const Type& m : to.unionMembers)
            if (assignable(from, m)) return true;
        return from.canonical == to.canonical;
    }
    if (from.canonical == to.canonical) return true;
    if (from.kind == TKind::Class && to.kind == TKind::Class) {
        if (from.sym == to.sym) {
            // Raw (unparameterized) form is compatible with any instantiation;
            // two *parameterized* forms are invariant (Array<int> != Array<string>).
            bool fromRaw = from.canonical.find('<') == std::string::npos;
            bool toRaw = to.canonical.find('<') == std::string::npos;
            if (fromRaw || toRaw) return true;
            // bug #48: invariant, but compare type arguments by resolved identity
            // so `Array<B::Foo>` (qualified) matches `Array<Foo>` (bare) when both
            // element types are the same class across a namespace boundary.
            if (from.args.size() == to.args.size()) {
                for (size_t i = 0; i < from.args.size(); ++i)
                    if (!sameNominalType(from.args[i], to.args[i])) return false;
                return true;
            }
            return false;
        }
        return isSubclass(from.sym, to.sym);
    }
    return false;
}

LocalBinding* Checker::localBinding(std::string_view name) {
    for (auto it = env_.rbegin(); it != env_.rend(); ++it) {
        auto f = it->find(name);
        if (f != it->end()) return &f->second;
    }
    return nullptr;
}

Type* Checker::localLookup(std::string_view name) {
    LocalBinding* b = localBinding(name);
    return b ? &b->type : nullptr;
}

// ---------------------------------------------------------------------------
//  flow narrowing
// ---------------------------------------------------------------------------

// Types embedded in EXPRESSIONS (`x is T`) aren't walked by the resolver;
// resolve them here, in place, against the current scope.
static void resolveExprType(TypeRef* t, Scope* scope) {
    if (!t || !t->canonical.empty()) return;
    if (t->kind == TypeKind::Named) {
        if (scope && !t->resolvedSymbol) t->resolvedSymbol = scope->lookup(t->name);
        std::string c(t->name);
        if (!t->generics.empty()) {
            c += "<";
            for (size_t i = 0; i < t->generics.size(); ++i) {
                if (i) c += ", ";
                resolveExprType(t->generics[i].get(), scope);
                c += t->generics[i]->canonical;
            }
            c += ">";
        }
        t->canonical = c;
    } else if (t->kind == TypeKind::Union) {
        std::string c;
        for (size_t i = 0; i < t->members.size(); ++i) {
            if (i) c += " | ";
            resolveExprType(t->members[i].get(), scope);
            c += t->members[i]->canonical;
        }
        t->canonical = c;
    }
}

std::string Checker::pathOf(const Expr* e) {
    if (!e) return "";
    if (e->weakField) return "";       // every weak-field read is a fresh lock()
    if (e->kind == ExprKind::Name) return std::string(e->text);
    if (e->kind == ExprKind::This) return "this";
    if (e->kind == ExprKind::Member && !e->colon && !e->optChain) {
        std::string base = pathOf(e->a.get());
        if (base.empty()) return "";
        return base + "." + std::string(e->text);
    }
    return "";
}

Type Checker::unionMinus(const Type& u, const std::string& removeCanonical) const {
    if (u.kind != TKind::Union) return u;
    std::vector<Type> rest;
    for (const Type& m : u.unionMembers)
        if (m.canonical != removeCanonical) rest.push_back(m);
    if (rest.empty()) return u;
    if (rest.size() == 1) return rest[0];
    Type r{TKind::Union, nullptr, "", {}, nullptr, {}};
    for (size_t i = 0; i < rest.size(); ++i) {
        if (i) r.canonical += " | ";
        r.canonical += rest[i].canonical;
    }
    r.unionMembers = rest;
    return r;
}

void Checker::analyzeCond(const Expr* cond, std::vector<Fact>& out, bool negated) {
    if (!cond) return;
    if (cond->kind == ExprKind::Unary && cond->op == TokenKind::Bang) {
        analyzeCond(cond->a.get(), out, !negated);
        return;
    }
    if (cond->kind == ExprKind::Is) {
        std::string path = pathOf(cond->a.get());
        if (path.empty()) return;
        resolveExprType(const_cast<TypeRef*>(cond->type.get()), scope_);
        Fact f;
        f.path = path;
        f.thenType = fromTypeRef(cond->type.get());
        f.hasThen = true;
        Type cur = typeOf(cond->a.get());
        f.elseType = unionMinus(cur, f.thenType.canonical);
        f.hasElse = (cur.kind == TKind::Union);
        if (negated) { std::swap(f.thenType, f.elseType); std::swap(f.hasThen, f.hasElse); }
        out.push_back(std::move(f));
        return;
    }
    if (cond->kind == ExprKind::Binary &&
        (cond->op == TokenKind::EqEq || cond->op == TokenKind::BangEq)) {
        const Expr* pathSide = nullptr;
        const Expr* noneSide = nullptr;
        auto isNoneExpr = [](const Expr* e) {
            return e->kind == ExprKind::Name && e->text == "None";
        };
        if (isNoneExpr(cond->b.get())) { pathSide = cond->a.get(); noneSide = cond->b.get(); }
        else if (isNoneExpr(cond->a.get())) { pathSide = cond->b.get(); noneSide = cond->a.get(); }
        if (!pathSide || !noneSide) return;
        std::string path = pathOf(pathSide);
        if (path.empty()) return;
        Type cur = typeOf(pathSide);
        bool eqNone = (cond->op == TokenKind::EqEq) != negated;
        Fact f;
        f.path = path;
        if (eqNone) {
            f.thenType = primType("None"); f.hasThen = true;
            f.elseType = unionMinus(cur, "None"); f.hasElse = (cur.kind == TKind::Union);
        } else {
            f.thenType = unionMinus(cur, "None"); f.hasThen = (cur.kind == TKind::Union);
            f.elseType = primType("None"); f.hasElse = true;
        }
        out.push_back(std::move(f));
        return;
    }
    if (cond->kind == ExprKind::Binary && cond->op == TokenKind::AmpAmp && !negated) {
        // both sides' then-facts hold when the whole && holds
        analyzeCond(cond->a.get(), out, false);
        analyzeCond(cond->b.get(), out, false);
        // (else side unknowable: which conjunct failed?)
        for (Fact& f : out) f.hasElse = false;
        return;
    }
}

void Checker::applyFacts(const std::vector<Fact>& facts, bool thenSide,
                         std::unordered_map<std::string, Type>& saved) {
    saved = narrow_;
    for (const Fact& f : facts) {
        if (thenSide && f.hasThen) narrow_[f.path] = f.thenType;
        if (!thenSide && f.hasElse) narrow_[f.path] = f.elseType;
    }
}

void Checker::invalidatePath(const std::string& path) {
    if (path.empty()) return;
    for (auto it = narrow_.begin(); it != narrow_.end();) {
        if (it->first == path || it->first.rfind(path + ".", 0) == 0)
            it = narrow_.erase(it);
        else
            ++it;
    }
}

// OQ1: leaving a scope drops any of its locals still recorded as write-open
// consts. A const that reaches end of scope still open is simply dead, not
// wrong (§2.2 step 5); dropping it here also keeps constPending_ from leaking a
// stale name into an outer/shadowing binding or the next function body.
void Checker::exitEnvScope() {
    if (!env_.empty() && !constPending_.empty())
        for (const auto& kv : env_.back())
            constPending_.erase(std::string(kv.first));
    env_.pop_back();
}

// ---------------------------------------------------------------------------
//  expression typing
// ---------------------------------------------------------------------------

// Track 03 §1: is `e` a single-quoted, single-scalar string literal? Such a
// literal re-types to `char` at a char-expected site (declared type, char
// comparison, char match arm). Double-quoted / multi-scalar / empty never.
static bool isCharLiteral(const Expr* e) {
    if (!e || e->kind != ExprKind::StringLit || !e->singleQuoted) return false;
    // A single-quoted simple literal is a raw content segment (quotes already
    // stripped by the parser) — decode escapes directly, then require exactly
    // one Unicode scalar.
    std::string c = decodeEscapes(e->text);
    if (c.empty()) return false;
    unsigned char b0 = (unsigned char)c[0];
    size_t len = b0 < 0x80 ? 1 : (b0 & 0xE0) == 0xC0 ? 2 :
                 (b0 & 0xF0) == 0xE0 ? 3 : (b0 & 0xF8) == 0xF0 ? 4 : 0;
    return len != 0 && len == c.size();
}
// Flip a char-able literal to a char value: the engines emit a char, and the
// codegen kind hint drops to the int-scalar comparison path (kc: 'i').
static void markCharLiteral(const Expr* e) {
    Expr* x = const_cast<Expr*>(e);
    x->charLit = true;
    x->evalKind = 0;
}
// Does this declared/expected type admit a `char` literal? A bare `char`, or a
// union that has `char` as a member (`char?` == `char | None`).
static bool expectsChar(const TypeRef* t) {
    if (!t) return false;
    if (t->kind == TypeKind::Named && t->path.empty() && t->name == "char") return true;
    if (t->kind == TypeKind::Union)
        for (const TypeRefPtr& m : t->members)
            if (m->kind == TypeKind::Named && m->path.empty() && m->name == "char") return true;
    return false;
}

Type Checker::typeOf(const Expr* e) {
    Type t = typeOfInner(e);
    if (e) {
        int k = 0;
        if (t.canonical == "bool") k = 1;
        else if (t.canonical == "string") k = 2;
        else if (t.canonical == "float") k = 3;
        const_cast<Expr*>(e)->evalKind = k;
        // §9 copy gating: the lowerer copies value structs at binding sites, but
        // may skip the copy when the static type is *definitely* not one. Only a
        // concrete non-value class / primitive / array / map / closure / none is
        // provably safe to skip; interfaces, unions, generics and unknowns might
        // hold a struct, so they stay conservative (copy).
        const_cast<Expr*>(e)->mayBeValueStruct = typeMayBeValueStruct(t);
        // §15: a CONCRETE value struct (not an interface/generic that might hold one)
        // is uniquely owned and copied by value, so its copies are arena-owned.
        const_cast<Expr*>(e)->definiteValueStruct =
            t.kind == TKind::Class && t.sym && !t.sym->isPrimitive &&
            !t.sym->isInterface() && t.sym->isValue;
        // techdesign-columnar: carry the concrete struct symbol for the lowerer.
        // techdesign-generic-value-struct-columnar: for an eligible instantiation
        // of a generic value struct (`Pair<int,int>`), carry the MONOMORPHIZED
        // symbol instead of the shared generic one, so the lowerer stamps the
        // instantiation's own classId (→ columnar) and reads its concrete-scalar
        // shape. Ineligible/non-generic value structs keep `t.sym`, unchanged.
        // `valueClass` is a native-codegen-only channel (the oracle never reads
        // it), which is what keeps engine neutrality (§5) automatic.
        Symbol* vc = nullptr;
        if (const_cast<Expr*>(e)->definiteValueStruct) {
            vc = t.sym;
            if (!t.args.empty())
                if (Symbol* spec = specializeValueStruct(t.sym, t.args)) vc = spec;
        }
        const_cast<Expr*>(e)->valueClass = vc;
    }
    return t;
}

// Conservative: true unless the type is certainly not a value struct.
bool Checker::typeMayBeValueStruct(const Type& t) {
    switch (t.kind) {
        case TKind::Class:
            if (!t.sym) return true;                 // unresolved — stay safe
            if (t.sym->isPrimitive) return false;    // primitives copy trivially (no heap obj)
            if (t.sym->isInterface()) return true;   // may hold a struct implementer
            return t.sym->isValue;                   // a concrete class: value struct or not
        case TKind::Union:
            for (const Type& m : t.unionMembers)
                if (typeMayBeValueStruct(m)) return true;
            return false;
        case TKind::Primitive: return false;         // int/string/bool/float/void/None
        case TKind::FuncRef:   return false;
        case TKind::TypeValue: return false;
        default:               return true;          // Unknown / generic / Error — stay safe
    }
}

Type Checker::typeOfInner(const Expr* e) {
    if (!e) return unknown();
    switch (e->kind) {
        case ExprKind::IntLit:    return primType("int");
        case ExprKind::FloatLit:  return primType("float");
        case ExprKind::StringLit: return primType("string");
        case ExprKind::BoolLit:   return primType("bool");
        case ExprKind::This:
            return thisClass_ ? classType(thisClass_) : unknown();

        case ExprKind::Name: {
            if (e->text == "None") return primType("None");
            // LA-18: only the left operand of an authored `T::member` carries
            // this marker. Callable-level T is a type value whose concrete
            // symbol is intentionally deferred; class-level T is the v1
            // soundness error from §7.6.
            if (e->genericStaticSite) {
                if (callableTypeParam(e->genericStaticParam))
                    return Type{TKind::TypeValue, nullptr,
                                std::string(e->genericStaticParam), {}, nullptr, {}};
                if (classTypeParam(e->genericStaticParam)) {
                    if (diagnosedGenericStatic_.insert(e).second)
                        return error(e->span, "'" + std::string(e->genericStaticParam) +
                            "' is a class-level type parameter; '::' on type parameters "
                            "is supported for function-level parameters only (v1)");
                    else
                        return Type{TKind::Error, nullptr, "", {}, nullptr, {}};
                }
            }
            {
                auto n = narrow_.find(std::string(e->text));
                if (n != narrow_.end()) return n->second;
            }
            if (LocalBinding* lb = localBinding(e->text)) {
                // OQ1: reading a write-open const before it is definitely
                // assigned is an error — the safety property that makes
                // suppressing its default-construction sound (no path observes
                // the un-assigned slot). Suppressed while typing the write
                // target of the assignment that is about to close the window.
                if (lb->isConst && !typingLhs_ &&
                    constPending_.count(std::string(e->text)))
                    return error(e->span, "const '" + std::string(e->text) +
                                 "' is read before it is definitely assigned");
                return lb->type;
            }
            // implicit member of `this`
            if (thisClass_) {
                auto fields = slotsNamed(thisClass_->shape, e->text);
                std::vector<const Slot*> data;
                for (const Slot* s : fields) if (!s->isMethod) data.push_back(s);
                if (data.size() == 1) {
                    const_cast<Expr*>(e)->weakField = data[0]->isWeak;
                    const_cast<Expr*>(e)->weakDirect = data[0]->isWeak &&
                        !findAccessorDecl(thisClass_, e->text, true) &&
                        !findAccessorDecl(thisClass_, e->text, false);
                    return fromTypeRef(data[0]->decl->type.get());
                }
                if (data.size() > 1 && allSameType(data))
                    return error(e->span, "ambiguous read of '" + std::string(e->text) +
                                 " : " + data[0]->canonical +
                                 "' (distinct on multiple bases); qualify with '::'");
                if (data.size() > 1) return unknown();   // diff types: needs context (phase 2)
                if (!fields.empty()) return unknown();    // a method referenced bare
            }
            // globals
            if (Symbol* sym = scope_ ? scope_->lookup(e->text) : nullptr) {
                if (sym->kind == SymbolKind::Class) return typeValue(sym);
                if (sym->kind == SymbolKind::Function) return funcRef(sym);
                if (sym->kind == SymbolKind::Var && sym->decl) {
                    // Record the declaration so a read through a `use ... as`
                    // alias (imports.md §4: "the alias names the same slot")
                    // still reaches the same runtime global — the evaluator's
                    // and lowerer's global storage is keyed by the var's own
                    // declared name, not whatever alias text read it here.
                    const_cast<Expr*>(e)->resolved = sym->decl;
                    return fromTypeRef(sym->decl->type.get());
                }
                return unknown();   // known name of another kind (namespace, type param)
            }
            if (e->text == "System")
                return unknown();   // builtin global not yet modeled
                                    // (console IS modeled: prelude Console class)
            {
                Type err = error(e->span, "unknown name '" + std::string(e->text) + "'");
                noteBlockScopedImport(e->text);   // S5: block-confined import hint
                return err;
            }
        }

        case ExprKind::Member:   return typeOfMember(e);
        case ExprKind::Call:     return typeOfCall(e);
        case ExprKind::Binary:   return typeOfBinary(e);
        case ExprKind::Index: {                       // a[i] -> the ([]) get accessor
            Type bt = typeOf(e->a.get());
            Type idx = typeOf(e->b.get());
            if (bt.kind == TKind::Class && bt.sym) {
                // Built-in containers: element/value type from the type arguments.
                if (bt.sym->name == "Array" && !bt.args.empty()) return bt.args[0];
                if (bt.sym->name == "Map" && bt.args.size() == 2) return bt.args[1];
                // A user get([]) with a declared return type (accessors currently
                // lack return-type syntax, so this stays lenient for now).
                if (const Stmt* g = findAccessorDecl(bt.sym, "[]", true))
                    if (g->type) return genericReturn(bt.sym, g, bt, {idx});
            }
            return unknown();
        }

        case ExprKind::Unary:
            // Always type the operand — it may contain a call that must be
            // statically resolved (its `resolved` set) and type-checked, even
            // for `!`, whose result is bool regardless of the operand's type.
            // (Previously `!<call>` skipped this, so the call fell through to a
            // by-name runtime/lower fallback that only worked while `uses`
            // leaked names into the global scope — see bug.md #8.)
            {
                Type operand = typeOf(e->a.get());
                if (e->op == TokenKind::Bang) return primitive("bool");
                // ~x is int-only (Track 01 F1) — no float form, per spec.
                if (e->op == TokenKind::Tilde) {
                    if (operand.kind == TKind::Class && operand.sym &&
                        operand.sym->isPrimitive && operand.sym->name != "int")
                        return error(e->span, "no operator '~' on '" +
                                     std::string(operand.sym->name) + "'");
                    return operand;
                }
                // unary minus needs a numeric operand (§3.7 loudness; used to
                // type as the operand and yield a silent runtime garbage int)
                if (operand.kind == TKind::Class && operand.sym &&
                    operand.sym->isPrimitive && operand.sym->name != "int" &&
                    operand.sym->name != "float")
                    return error(e->span, "no operator '-' on '" +
                                 std::string(operand.sym->name) + "'");
                return operand;
            }
        case ExprKind::Ternary: {
            typeOf(e->a.get());
            std::vector<Fact> facts;
            analyzeCond(e->a.get(), facts, false);
            std::unordered_map<std::string, Type> saved;
            applyFacts(facts, true, saved);
            Type t = typeOf(e->b.get());
            narrow_ = saved;
            applyFacts(facts, false, saved);
            Type f = typeOf(e->c.get());
            narrow_ = saved;
            // bug #63: a single-quoted char literal in ONE ternary branch
            // re-types to char when the other branch is char — the same
            // by-expected-type retyping #50 applies to a call argument, extended
            // to ternary-branch position (which #50 didn't cover). Without this
            // the literal stays `string`, the branch types diverge, and the
            // char value carried to a later native call is silently wrong.
            if (f.canonical == "char" && isCharLiteral(e->b.get())) {
                markCharLiteral(e->b.get()); t = primType("char");
            } else if (t.canonical == "char" && isCharLiteral(e->c.get())) {
                markCharLiteral(e->c.get()); f = primType("char");
            }
            return t.canonical == f.canonical ? t : unknown();
        }
        case ExprKind::Inject: {                 // `inject Type` — explicit selector (§12.5)
            resolveExprType(const_cast<TypeRef*>(e->type.get()), scope_);
            std::string key = e->type ? e->type->canonical : std::string();
            const Stmt* b = key.empty() ? nullptr : lookupBind(key);
            if (!b)
                return error(e->span, "no binding in scope for injected type '" + key + "'");
            const_cast<Expr*>(e)->resolved = b;
            return fromTypeRef(e->type.get());
        }
        case ExprKind::Await: {                          // unwrap Promise<T> -> T
            Type t = typeOf(e->a.get());
            // Unwrap Promise<T> -> T, including subclasses of Promise
            // (Track 10: Worker<T> : Promise<T> is awaitable as its own T).
            // Worker's first type parameter maps to Promise's T by declaration,
            // so t.args[0] is the awaited element for both.
            std::function<bool(Symbol*)> derivesPromise = [&](Symbol* s) -> bool {
                if (!s) return false;
                if (s->name == "Promise") return true;
                if (!s->decl) return false;
                for (const TypeRefPtr& base : s->decl->bases)
                    if (derivesPromise(base->resolvedSymbol)) return true;
                return false;
            };
            if (t.kind == TKind::Class && t.sym && !t.args.empty() &&
                derivesPromise(t.sym))
                return t.args[0];
            return t.kind == TKind::Unknown ? unknown() : t;
        }
        case ExprKind::Array: {
            // Infer Array<Elem> when all elements share a type; else raw Array.
            Type elem = unknown();
            bool uniform = true;
            for (size_t i = 0; i < e->list.size(); ++i) {
                Type t = typeOf(e->list[i].get());
                if (e->list[i]->kind == ExprKind::Range) t = primType("int");  // [1..5] spreads ints
                if (i == 0) elem = t;
                else if (t.canonical != elem.canonical) uniform = false;
            }
            Symbol* arrSym = scope_ ? scope_->lookup("Array") : nullptr;
            if (!arrSym) return unknown();
            if (e->list.empty() || !uniform || elem.canonical.empty())
                return classType(arrSym);                    // raw Array
            Type r{TKind::Class, arrSym, "Array<" + elem.canonical + ">", {}, nullptr, {}};
            r.args.push_back(elem);
            return r;
        }
        case ExprKind::Lambda:
            // Walk the body so calls inside it are statically resolved (their
            // `resolved` set) and checked — otherwise a call in a lambda body
            // falls through to a by-name runtime/lower fallback that only worked
            // while `uses` leaked names into the global scope (bug.md #8). The
            // lambda's own type stays unmodeled OUTSIDE a generic call: params
            // bind as their declared type (else unknown). Lambdas in call-
            // argument position never reach here — typeOfCall defers them and
            // walks them via checkLambdaBody with real param types instead.
            checkLambdaBody(e, {});
            return unknown();
        case ExprKind::Is: {
            typeOf(e->a.get());
            resolveExprType(const_cast<TypeRef*>(e->type.get()), scope_);
            return primType("bool");
        }
        case ExprKind::Match: {
            Type subj = typeOf(e->a.get());
            std::string path = pathOf(e->a.get());
            std::vector<std::string> covered;     // canonical types matched by arms
            std::vector<std::string_view> enumCovered;   // enum members matched (Track 03 §2)
            bool hasElse = false;
            bool sawNaNArm = false;       // struct-equality §6 (packet 07): dup check
            Type result; bool haveResult = false, uniform = true;
            // OQ1: definite assignment across the arms — the match analogue of
            // the if/else join (§2.3). Each arm starts from the pre-match open
            // set; a const is definitely assigned after the match iff it closed
            // on EVERY arm, so the post-join open set is the UNION of the arms'
            // still-open sets. A non-exhaustive match (no `else`) has an implicit
            // fall-through arm that assigns nothing, so `pendingBefore` joins in.
            std::unordered_map<std::string, PendingConst> pendingBefore = constPending_;
            std::unordered_map<std::string, PendingConst> pendingJoin;
            for (const MatchArm& arm : e->arms) {
                std::unordered_map<std::string, Type> saved = narrow_;
                constPending_ = pendingBefore;
                if (arm.isElse) {
                    hasElse = true;
                } else if (arm.type) {            // TYPE pattern: narrow the subject
                    resolveExprType(const_cast<TypeRef*>(arm.type.get()), scope_);
                    covered.push_back(arm.type->canonical);
                    if (!path.empty()) narrow_[path] = fromTypeRef(arm.type.get());
                } else if (arm.value) {           // VALUE / range pattern
                    // Track 03 §1: char-subject match — `match (c) { 'a' => ... }`
                    // re-types a single-scalar arm literal to char (compares by
                    // scalar). Done before typeOf so the pattern types as char.
                    if (subj.canonical == "char" && isCharLiteral(arm.value.get()))
                        markCharLiteral(arm.value.get());
                    Type pat = typeOf(arm.value.get());
                    // R4 (005): a value pattern that types as a TypeValue (a bare
                    // type used where a value is expected — e.g. class-rooted
                    // `C::field`/`C::T`) can never equal the subject, so it would
                    // silently take `else`. Make that loud. Enum members type as
                    // the enum's value struct and `float::NaN` as `float`, so no
                    // legitimate value pattern is caught here.
                    if (pat.kind == TKind::TypeValue)
                        return error(arm.value->span,
                                     "match pattern is a type ('" + pat.canonical +
                                     "') used as a value — this arm can never match");
                    // Enum-member arm `Method::GET`: my typeOfMember rule stamps
                    // `resolved` on a genuine enum member — record it for closure.
                    if (arm.value->kind == ExprKind::Member && arm.value->colon &&
                        arm.value->resolved)
                        enumCovered.push_back(arm.value->text);
                    // struct-equality §6 (packet 07): `float::NaN` is a reachable
                    // match arm (canonical relation), so its two failure modes get
                    // narrow, per-constant diagnostics — no general framework (no
                    // duplicate/unreachable checks exist for any other type). An
                    // `else` seen earlier makes a later NaN arm dead (its canonical
                    // compare can never run); a second NaN arm is a redundant dup.
                    if (isFloatNaNConst(arm.value.get())) {
                        if (hasElse)
                            return error(arm.value->span,
                                         "unreachable 'float::NaN' arm after 'else'");
                        if (sawNaNArm)
                            return error(arm.value->span, "duplicate 'float::NaN' arm");
                        sawNaNArm = true;
                    }
                }
                Type bt = arm.bodyBlock
                    ? (check(const_cast<Stmt*>(arm.bodyBlock.get())), unknown())
                    : typeOf(arm.bodyExpr.get());
                narrow_ = saved;
                for (const auto& kv : constPending_) pendingJoin.insert(kv);
                if (arm.bodyExpr) {
                    if (!haveResult) { result = bt; haveResult = true; }
                    else if (bt.canonical != result.canonical) uniform = false;
                }
            }
            if (!hasElse)
                for (const auto& kv : pendingBefore) pendingJoin.insert(kv);
            constPending_ = std::move(pendingJoin);
            // Exhaustiveness: a closed union must be fully covered; otherwise an
            // `else` is required (an open hierarchy / scalar can't be exhaustive).
            if (!hasElse) {
                // Track 03 §2: an enum is a closed set — exhaustive without `else`
                // iff every member appears. Missing members are named.
                const EnumDesugar* ed = nullptr;
                if (subj.kind == TKind::Class && subj.sym && program_)
                    for (const EnumDesugar& d : program_->enumDesugars)
                        if (d.name == subj.sym->name) { ed = &d; break; }
                if (ed) {
                    std::string missing;
                    for (const EnumDesugar::Member& m : ed->members) {
                        bool ok = false;
                        for (std::string_view c : enumCovered) if (c == m.name) ok = true;
                        if (!ok)
                            missing += (missing.empty() ? "" : ", ") +
                                       (std::string(ed->name) + "::" + std::string(m.name));
                    }
                    if (!missing.empty())
                        return error(e->span, "non-exhaustive match on enum '" +
                                     std::string(ed->name) + "': missing " + missing +
                                     " (add the arm(s) or an 'else')");
                } else if (subj.kind == TKind::Union) {
                    for (const Type& m : subj.unionMembers) {
                        bool ok = false;
                        for (const std::string& c : covered) if (c == m.canonical) ok = true;
                        if (!ok)
                            return error(e->span, "non-exhaustive match: '" + m.canonical +
                                         "' is not covered (add it or an 'else')");
                    }
                } else {
                    return error(e->span, "match must be exhaustive: add an 'else' arm");
                }
            }
            return haveResult && uniform ? result : unknown();
        }
        case ExprKind::Extract: {                    // `stream >>` == stream.pull()
            Type bt = typeOf(e->a.get());
            if (bt.kind == TKind::Class && bt.sym) {
                auto cands = methodOverloads(bt.sym, "pull");
                bool okp = false;
                if (const Stmt* m = pickOverload(cands, {}, okp)) {
                    const_cast<Expr*>(e)->resolved = m;
                    return genericReturn(bt.sym, m, bt, {});
                }
            }
            return unknown();
        }
        case ExprKind::Range: {
            typeOf(e->a.get()); typeOf(e->b.get());
            Symbol* r = scope_ ? scope_->lookup("Range") : nullptr;
            return r ? classType(r) : unknown();
        }
        default:                 return unknown();
    }
}

Type Checker::typeOfMember(const Expr* e) {
    // Item Q (techdesign-target-predicate.md): inside a comptime root the
    // reserved namespace `target` provides the target constants (strings).
    // Handled here so the §8 precheck neither flags a valid read as
    // "unknown name 'target'" nor masks the oracle's targeted message for an
    // unknown member with that generic one.
    if (comptimeRoot_ && e->colon && e->a && e->a->kind == ExprKind::Name &&
        e->a->text == "target") {
        if (e->text == "os" || e->text == "arch" || e->text == "triple")
            return primType("string");
        return error(e->span, "unknown target:: constant '" +
                     std::string(e->text) +
                     "' (target::os, target::arch, target::triple)");
    }
    // Flow-narrowed path?
    {
        std::string p = pathOf(e);
        if (!p.empty()) {
            auto n = narrow_.find(p);
            if (n != narrow_.end()) return n->second;
        }
    }
    // LA-18 definition-site deferral. Concrete clones keep the marker but no
    // longer have generic parameters, so they continue into ordinary LA-25 /
    // member resolution below.
    if (e->genericStaticSite && callableTypeParam(e->genericStaticParam)) {
        typeOf(e->a.get());
        return unknown();
    }

    // LA-25/F3: a callable member in VALUE position (unbound through `::`, or
    // bound through `.`) becomes its eta-expansion lambda. No target type is
    // known here, so an overloaded name is the ambiguous-without-target error.
    // Namespace functions, instance methods, and labeled constructors are
    // handled uniformly — see tryResolveMethodRef. Reached here BEFORE
    // the plain namespace-qualified block below because that block's old
    // per-symbol loop only ever picked one arbitrary overload for a Function
    // symbol; this replaces that with real overload handling.
    if (methodRefsAllowed_) {
        bool isRef = false;
        Type t = tryResolveMethodRef(const_cast<Expr*>(e), nullptr, isRef);
        if (isRef) return t;
    }
    // Namespace-qualified access: NS::name — type through the namespace scope.
    // bug.md #82: NS can itself be a `::`-qualified chain of namespaces
    // (`P::K::FLAG`, arbitrary depth) — resolveNamespaceExpr walks it the same
    // way for one segment or many, so a nested namespace's const isn't left
    // unresolved (which used to silently read as 0/empty downstream).
    if (Symbol* ns = resolveNamespaceExpr(e->a.get())) {
        if (ns->scope)
            if (const std::vector<Symbol*>* v = ns->scope->localLookup(e->text))
                for (Symbol* s : *v) {
                    if (s->kind == SymbolKind::Class) return typeValue(s);
                    if (s->kind == SymbolKind::Var && s->decl) {
                        // Namespace-qualified globals must retain declaration
                        // identity for evaluation/lowering. This is especially
                        // important for nested namespaces imported from a
                        // package (`Attr::Bold` after `uses Sonar`).
                        const_cast<Expr*>(e)->resolved = s->decl;
                        return fromTypeRef(s->decl->type.get());
                    }
                }
    }
    Type bt = typeOf(e->a.get());
    std::string_view name = e->text;

    bool addNone = false;
    if (e->optChain) {                       // a?.b — strip None, re-add to result
        if (bt.kind == TKind::Union) {
            bt = unionMinus(bt, "None");
            addNone = true;
        }
    } else if (bt.kind == TKind::Union) {
        return error(e->span, "narrow the union before member access "
                              "(use '!= None' or 'is')");
    }
    if (bt.canonical == "None")
        return error(e->span, "'None' has no members (this branch narrowed it to None)");
    auto wrap = [&](Type t) {
        if (!addNone || t.kind == TKind::Unknown || t.kind == TKind::Error) return t;
        Type u{TKind::Union, nullptr, t.canonical + " | None", {}, nullptr, {}};
        u.unionMembers = {t, primType("None")};
        return u;
    };

    // Base-view narrowing: `base.Ancestor` re-views base as one of its bases,
    // which is how a distinct-collided member gets qualified (this.Counter::value).
    if (bt.kind == TKind::Class) {
        if (Symbol* nameSym = scope_ ? scope_->lookup(name) : nullptr)
            if (nameSym->kind == SymbolKind::Class && isSubclass(bt.sym, nameSym))
                return classType(nameSym);
    }
    // Track 03 §2: `Enum::Member` value read. Gated on the class being enum-
    // registered so it never fires for ordinary user classes. Types as the enum's
    // value struct and stamps the resolved mangled const global so every engine
    // reads the same constant. A genuine unknown member falls through to the
    // tightened passthrough below, which now errors on it (bug.md #28 fix).
    if (bt.kind == TKind::TypeValue && bt.sym && program_)
        for (const EnumDesugar& ed : program_->enumDesugars)
            if (ed.name == bt.sym->name) {
                for (const EnumDesugar::Member& m : ed.members)
                    if (m.name == name) {
                        const_cast<Expr*>(e)->resolved = m.global;
                        return classType(bt.sym);
                    }
                break;
            }
    // struct-equality §6 (packet 06): `float::NaN` — the one language constant.
    // Mirrors the enum-member path above: resolve the read to the synthesized
    // const global so ALL engines read the same global (zero per-engine work),
    // and type it as `float`. Match on the primitive symbol, not spelling.
    if (bt.kind == TKind::TypeValue && bt.sym && bt.sym->isPrimitive &&
        bt.sym->name == "float" && name == "NaN" && program_ && program_->floatNaNGlobal) {
        const_cast<Expr*>(e)->resolved = program_->floatNaNGlobal;
        return primType("float");
    }
    // Static on a type value: keep referring to the class, but only for a name
    // the class actually declares. An unknown member after `::`/`.` on a
    // class-used-as-a-value used to pass through unconditionally as the class
    // type (typing as TypeValue, then evaluating to void) — a silent loudness
    // hole (bug.md #28; reference §3.7 "unknown name is a compile error,
    // refuse to guess"). Resolve the name against the class's member slots and
    // any nested scope symbol; error only on a genuine miss. Call-position
    // ctor labels (`T::Label(...)`) and base-qualified access
    // (`this.Base::field`) never reach here — they are handled in
    // typeOfCallInner and the TKind::Class branch above, respectively.
    // (Constructor LABELS in value position — `T::Label` — are claimed above
    // by the LA-25 method-reference check, so they never reach this fallback.)
    if (bt.kind == TKind::TypeValue) {
        if (bt.sym) {
            if (!slotsNamed(bt.sym->shape, name).empty()) return bt;
            if (bt.sym->scope && bt.sym->scope->localLookup(name)) return bt;
        }
        if (e->genericStaticSite && activeSpecialization_ && bt.sym) {
            genericStaticMissing(e, bt.sym, name, false);
            return Type{TKind::Error, nullptr, "", {}, nullptr, {}};
        }
        return error(e->span, "type '" + bt.canonical + "' has no member '" +
                     std::string(name) + "'");
    }

    if (bt.kind == TKind::Class) {
        auto slots = slotsNamed(bt.sym->shape, name);
        if (slots.empty()) return wrap(unknown());           // unmodeled: lenient
        if (slots.size() == 1) {
            const Slot* s = slots[0];
            const_cast<Expr*>(e)->weakField = s->isWeak;
            const_cast<Expr*>(e)->weakDirect = s->isWeak &&
                !findAccessorDecl(bt.sym, name, true) &&
                !findAccessorDecl(bt.sym, name, false);
            return s->isMethod ? wrap(unknown())
                               : wrap(fromTypeRef(s->decl->type.get()));
        }
        if (allSameType(slots))
            return error(e->span, "ambiguous read of '" + std::string(name) + " : " +
                         slots[0]->canonical +
                         "' (distinct on multiple bases); qualify with '::'");
        return unknown();   // different types: resolution needs a target (phase 2)
    }
    return unknown();
}

// ---------------------------------------------------------------------------
//  LA-25: method/function/constructor references as values
//  (designs/techdesign-method-references.md)
// ---------------------------------------------------------------------------

// The LA-25 unbound-reference signature for candidate `fn`: an instance
// method's receiver becomes its OWN first parameter (`recvCanon` non-empty);
// a namespace function or labeled constructor has none. `retCanon` is the
// caller-supplied return spelling — `fn->type`'s canonical for a method/
// function, or the constructed class's own canonical for a labeled
// constructor (ctors have no `type`, so the caller can't derive it here).
std::string Checker::methodRefCanonical(const Stmt* fn, const std::string& recvCanon,
                                        const std::string& retCanon) const {
    std::string c = "(";
    bool first = true;
    if (!recvCanon.empty()) { c += recvCanon; first = false; }
    for (const Param& p : fn->params) {
        if (!first) c += ", ";
        first = false;
        c += fromTypeRef(p.type.get()).canonical;
    }
    c += ") => " + retCanon;
    return c;
}

// Pick among same-named candidates (§2.2): one candidate needs no target,
// first-declared/arity rules don't apply (a value-position reference has no
// argument list to score against, §8.2) — 2+ requires `expected` to equal
// exactly one candidate's synthesized signature. `ctorRetCanon` non-empty
// means every candidate is a labeled constructor sharing that one return
// spelling; empty means each candidate's own declared return type applies.
const Stmt* Checker::pickMethodRefOverload(const std::vector<const Stmt*>& cands,
                                           const std::string& recvCanon,
                                           const std::string& ctorRetCanon,
                                           const Type* expected, bool& ambiguous) const {
    ambiguous = false;
    if (cands.size() == 1) return cands[0];
    if (!expected || expected->kind != TKind::FuncRef) { ambiguous = true; return nullptr; }
    const Stmt* match = nullptr; int count = 0;
    for (const Stmt* c : cands) {
        std::string retCanon = !ctorRetCanon.empty()
            ? ctorRetCanon : fromTypeRef(c->type.get()).canonical;
        if (methodRefCanonical(c, recvCanon, retCanon) == expected->canonical) {
            match = c; ++count;
        }
    }
    if (count == 1) return match;
    ambiguous = true;
    return nullptr;
}

// §4.2: synthesize the eta-expansion closure `target` denotes and rewrite `e`
// (a value-position callable reference) into it in place — an ordinary
// Lambda every engine already executes (P-5), so this is the whole
// implementation: no new IR, no Lower/Eval code. `recvClass` non-null =>
// unbound instance-method reference (receiver becomes the lambda's first
// parameter, dot-called by name so an override on a derived instance still
// runs — the same CallDyn-by-name a hand-written `(c) => c.m()` produces).
// `ctorClass` non-null => labeled-constructor reference (construction, not a
// `.`-call). Neither set => a namespace/free function reference, which
// reuses the ORIGINAL `Base::name` node verbatim as the synthesized call's
// callee (P-1 already proves that shape resolves correctly in call position).
Type Checker::rewriteAsMethodRef(Expr* e, const Stmt* target, Symbol* recvClass,
                                 Symbol* ctorClass, const std::string& recvCanon,
                                 const Type& retType) {
    SourceSpan sp = e->span;
    const bool bound = recvClass && !e->colon;

    ExprPtr callee;
    if (recvClass && !bound) {
        auto m = std::make_unique<Expr>(ExprKind::Member);
        m->span = sp; m->colon = false; m->text = e->text;
        auto recv = std::make_unique<Expr>(ExprKind::Name);
        recv->span = sp; recv->text = methodRefParamName(0);
        m->a = std::move(recv);
        callee = std::move(m);
    } else if (e->kind == ExprKind::Name) {
        // bug #55: bare free-function reference — the eta-expansion's callee is
        // just the function name (no receiver / no namespace qualifier).
        auto n = std::make_unique<Expr>(ExprKind::Name);
        n->span = sp; n->text = e->text;
        callee = std::move(n);
    } else {
        auto orig = std::make_unique<Expr>(ExprKind::Member);
        orig->span = sp; orig->colon = e->colon; orig->text = e->text;
        orig->a = std::move(e->a);
        callee = std::move(orig);
    }

    auto call = std::make_unique<Expr>(ExprKind::Call);
    call->span = sp;
    call->resolved = target;
    if (ctorClass) call->resolvedClass = ctorClass;
    // S3 (§4.2): an unbound reference through an interface-typed base, or a
    // class-typed base whose resolved method is overridden below it, has an
    // open candidate set — leave `resolved` null so both engines dispatch by
    // name at runtime, exactly the rule typeOfCallInner already applies to a
    // hand-written `(c) => c.m()` call (Checker.cpp's method-call branch
    // above). `target` is the synthesized call's callee decl set above.
    if (recvClass && resolveDispatch(recvClass, target, sp))
        call->resolved = nullptr;

    size_t nParams = target->params.size();
    size_t recvOffset = recvClass && !bound ? 1 : 0;
    for (size_t i = 0; i < nParams; ++i) {
        auto p = std::make_unique<Expr>(ExprKind::Name);
        p->span = sp; p->text = methodRefParamName(i + recvOffset);
        call->list.push_back(std::move(p));
    }
    call->a = std::move(callee);

    std::vector<Param> params;
    std::string sig = "(";
    bool first = true;
    if (recvClass && !bound) {
        Param rp; rp.span = sp; rp.name = methodRefParamName(0);
        params.push_back(std::move(rp));
        sig += recvCanon; first = false;
    }
    for (size_t i = 0; i < nParams; ++i) {
        Param p; p.span = sp; p.name = methodRefParamName(i + recvOffset);
        params.push_back(std::move(p));
        if (!first) sig += ", ";
        first = false;
        sig += fromTypeRef(target->params[i].type.get()).canonical;
    }
    sig += ") => " + retType.canonical;

    e->kind = ExprKind::Lambda;
    e->text = std::string_view();
    e->colon = false;
    e->a = std::move(call);        // expression body
    e->block = nullptr;
    e->params = std::move(params);
    e->list.clear();
    e->resolved = nullptr;

    Type ft{TKind::FuncRef, nullptr, sig, {}, nullptr, {}};
    ft.ret = std::make_shared<Type>(retType);
    return ft;
}

Type Checker::tryResolveMethodRef(Expr* e, const Type* expected, bool& isRef) {
    isRef = false;
    if (!e) return unknown();
    // bug #55: a BARE free-function name in value position (`add`, not `NS::add`
    // or `obj.add`) is a callable reference too — rewrite it to its eta-expansion
    // lambda `(p0,…) => add(p0,…)`, exactly as the NS::fn / obj.method cases
    // below do. Without this, the name typed as a plain FuncRef but no engine
    // reified it: the oracle read it as void, the IR lowerer errored ("not yet
    // lowerable: name"), so storing it in a function-typed field and calling
    // back through the field failed while a hand-written lambda wrapper worked.
    if (e->kind == ExprKind::Name) {
        if (localLookup(e->text)) return unknown();   // a local/param shadows: not a fn ref
        const std::vector<Symbol*>* v = nullptr;
        for (const Scope* s = scope_; s; s = s->parent)
            if (const std::vector<Symbol*>* found = s->localLookup(e->text)) { v = found; break; }
        if (!v) return unknown();
        std::vector<const Stmt*> cands;
        for (Symbol* s : *v)
            if (s->kind == SymbolKind::Function && s->decl) cands.push_back(s->decl);
        if (cands.empty()) return unknown();
        isRef = true;
        bool ambiguous = false;
        const Stmt* fn = pickMethodRefOverload(cands, "", "", expected, ambiguous);
        if (!fn) {
            if (ambiguous)
                return error(e->span, "ambiguous function reference '" +
                             std::string(e->text) + "'; annotate the target function type");
            return unknown();
        }
        if (!fn->generics.empty())
            return error(e->span, "cannot reference generic function '" +
                         std::string(e->text) +
                         "' - its type parameters are unbound in value position");
        return rewriteAsMethodRef(e, fn, nullptr, nullptr, "", fromTypeRef(fn->type.get()));
    }
    if (e->kind != ExprKind::Member) return unknown();
    std::string_view name = e->text;

    // namespace function: NS::fn (single-segment base only, matching the rest
    // of the compiler's namespace-qualified-call support — a nested
    // `A::B::fn` base isn't reachable through the call-lowering machinery
    // either, so this doesn't narrow anything real).
    if (e->colon && e->a->kind == ExprKind::Name) {
        if (Symbol* ns = scope_ ? scope_->lookup(e->a->text) : nullptr) {
            if (ns->kind == SymbolKind::Namespace && ns->scope) {
                std::vector<const Stmt*> cands;
                if (const std::vector<Symbol*>* v = ns->scope->localLookup(name))
                    for (Symbol* s : *v)
                        if (s->kind == SymbolKind::Function && s->decl) cands.push_back(s->decl);
                if (!cands.empty()) {
                    isRef = true;
                    bool ambiguous = false;
                    const Stmt* fn = pickMethodRefOverload(cands, "", "", expected, ambiguous);
                    if (!fn) {
                        if (ambiguous)
                            return error(e->span, "ambiguous method reference '" +
                                        std::string(e->a->text) + "::" + std::string(name) +
                                        "'; annotate the target function type");
                        return unknown();
                    }
                    if (!fn->generics.empty())
                        return error(e->span, "cannot reference generic function '" +
                                    std::string(name) +
                                    "' - its type parameters are unbound in value position");
                    return rewriteAsMethodRef(e, fn, nullptr, nullptr, "",
                                              fromTypeRef(fn->type.get()));
                }
            }
        }
    }

    // Unbound instance method / labeled constructor: T::name where T is a
    // class value.
    Type bt = typeOf(e->a.get());
    if (e->colon && bt.kind == TKind::TypeValue && bt.sym) {
        auto ctors = ctorOverloads(bt.sym, name);
        if (!ctors.empty()) {
            isRef = true;
            bool ambiguous = false;
            const Stmt* ctor = pickMethodRefOverload(ctors, "", bt.canonical, expected, ambiguous);
            if (!ctor) {
                if (ambiguous)
                    return error(e->span, "ambiguous method reference '" + bt.canonical +
                                "::" + std::string(name) + "'; annotate the target function type");
                return unknown();
            }
            return rewriteAsMethodRef(e, ctor, nullptr, bt.sym, "", classType(bt.sym));
        }
        auto methods = methodOverloads(bt.sym, name);
        if (!methods.empty()) {
            isRef = true;
            bool ambiguous = false;
            const Stmt* m = pickMethodRefOverload(methods, bt.canonical, "", expected, ambiguous);
            if (!m) {
                if (ambiguous)
                    return error(e->span, "ambiguous method reference '" + bt.canonical +
                                "::" + std::string(name) + "'; annotate the target function type");
                return unknown();
            }
            if (!m->generics.empty())
                return error(e->span, "cannot reference generic function '" +
                            std::string(name) +
                            "' - its type parameters are unbound in value position");
            return rewriteAsMethodRef(e, m, bt.sym, nullptr, bt.canonical,
                                      fromTypeRef(m->type.get()));
        }
    }

    // F3: `obj.method` in value position binds `obj`. A data slot of the same
    // name keeps ordinary field-read precedence; only a resolved method slot
    // is eligible for this rewrite.
    if (!e->colon && bt.kind == TKind::Class && bt.sym) {
        auto slots = slotsNamed(bt.sym->shape, name);
        for (const Slot* s : slots)
            if (!s->isMethod) return unknown();
        auto methods = methodOverloads(bt.sym, name);
        if (!methods.empty()) {
            isRef = true;
            if (diagnosedMethodRefs_.count(e))
                return Type{TKind::Error, nullptr, "", {}, nullptr, {}};
            bool ambiguous = false;
            const Stmt* m = pickMethodRefOverload(methods, "", "", expected, ambiguous);
            if (!m) {
                if (ambiguous) {
                    diagnosedMethodRefs_.insert(e);
                    return error(e->span, "ambiguous bound method reference '" +
                                 bt.canonical + "." + std::string(name) +
                                 "'; annotate the target function type");
                }
                return unknown();
            }
            if (!m->generics.empty()) {
                diagnosedMethodRefs_.insert(e);
                return error(e->span, "cannot reference generic function '" +
                            std::string(name) +
                            "' - its type parameters are unbound in value position");
            }

            const bool localReceiver = e->a->kind == ExprKind::Name &&
                                       localLookup(e->a->text);
            if (e->a->kind != ExprKind::This && !localReceiver) {
                diagnosedMethodRefs_.insert(e);
                Type result = error(e->span,
                    "bound method reference receiver must be a bare local, parameter, "
                    "or 'this'; bind the receiver to a local first");
                std::string receiver = "<receiver>";
                const SourceSpan rsp = e->a->span;
                if (rsp.length && rsp.end() <= file_.text.size())
                    receiver = file_.text.substr(rsp.offset, rsp.length);
                // Call/member spans currently retain the first token's span.
                // Recover the authored receiver up to the selector so the
                // diagnostic can still show the concrete two-line rewrite.
                const std::string selector = "." + std::string(name);
                size_t dot = file_.text.find(selector, rsp.offset);
                if (dot != std::string::npos) {
                    size_t lineEnd = file_.text.find('\n', rsp.offset);
                    if (lineEnd == std::string::npos) lineEnd = file_.text.size();
                    if (dot < lineEnd)
                        receiver = file_.text.substr(rsp.offset, dot - rsp.offset);
                }
                sink_.note(e->span, "fix: var r = " + receiver +
                                      "; then use r." + std::string(name));
                return result;
            }
            return rewriteAsMethodRef(e, m, bt.sym, nullptr, "",
                                      fromTypeRef(m->type.get()));
        }
    }
    return unknown();
}

// True if `a` is a `::`-qualified value-position reference whose name is
// OVERLOADED (2+ candidates) — deferred like a Lambda call argument (§2.2/§6)
// until the outer call's overload is chosen, then resolved against the chosen
// candidate's declared parameter type. A single-candidate reference needs no
// deferral: it resolves immediately (typeOf -> typeOfMember), no target
// needed since there's nothing to disambiguate.
bool Checker::isDeferredMethodRefArg(const Expr* e) {
    if (!e || e->kind != ExprKind::Member) return false;
    std::string_view name = e->text;
    if (e->colon && e->a->kind == ExprKind::Name) {
        if (Symbol* ns = scope_ ? scope_->lookup(e->a->text) : nullptr) {
            if (ns->kind == SymbolKind::Namespace && ns->scope) {
                int n = 0;
                if (const std::vector<Symbol*>* v = ns->scope->localLookup(name))
                    for (Symbol* s : *v) if (s->kind == SymbolKind::Function) ++n;
                if (n > 0) return n > 1;
            }
        }
    }
    Type bt = typeOf(e->a.get());
    if (e->colon && bt.kind == TKind::TypeValue && bt.sym) {
        if (ctorOverloads(bt.sym, name).size() > 1) return true;
        if (methodOverloads(bt.sym, name).size() > 1) return true;
    }
    if (!e->colon && bt.kind == TKind::Class && bt.sym) {
        auto slots = slotsNamed(bt.sym->shape, name);
        for (const Slot* s : slots) if (!s->isMethod) return false;
        return methodOverloads(bt.sym, name).size() > 1;
    }
    return false;
}

// Lambda arguments are DEFERRED here (typed unknown for overload choice — same
// score they got before) and walked exactly once afterward: the success paths
// walk them inside genericReturn with parameter types substituted from the
// bound type vars (lambda-last inference, Track 05 §1); any path that returns
// without resolving a callee falls through to the sweep below, which walks
// them with declared/unknown params — the old behavior (bug.md #8 still holds:
// every lambda body is walked so its inner calls get `resolved` pinned).
Type Checker::typeOfCall(const Expr* e) {
    std::vector<char> lambdaWalked(e->list.size(), 0);
    Type r = typeOfCallInner(e, lambdaWalked);
    // bug #57: an immediately-invoked lambda `(() => {...})()` carries its
    // lambda as the CALLEE (e->a), not an argument, so the argument sweep below
    // never reached its body — inner calls (e.g. an imported `Text()`) stayed
    // unresolved, and the oracle then failed to dispatch them at runtime while
    // the IR lowerer couldn't lower them. Walk a callee lambda's body here too.
    if (e->a && e->a->kind == ExprKind::Lambda) checkLambdaBody(e->a.get(), {});
    for (size_t i = 0; i < e->list.size(); ++i) {
        if (lambdaWalked[i]) continue;
        Expr* a = e->list[i].get();
        if (a->kind == ExprKind::Lambda) checkLambdaBody(a, {});
        // LA-25 §2.2/§6: an overloaded method reference that reached here
        // unresolved had no call site walk it against a chosen overload's
        // parameter type (e.g. a callee this checker doesn't specially
        // resolve, like a local closure variable) — there's genuinely no
        // target, so this is the ambiguous-without-target error (§8.2).
        else if (a->kind == ExprKind::Member) {
            bool isRef = false;
            tryResolveMethodRef(a, nullptr, isRef);
        }
    }
    return r;
}

// A rule-spliced class hole denotes its matched declaration, independent of
// names introduced around it by the expansion. Resolver pass 2 rebuilds the
// Symbol arena but keeps the AST nodes, so declaration identity is the stable
// bridge from the cloned `$C` expression to the new class Symbol.
Symbol* Checker::hygienicClass(const Expr* e) const {
    if (!e || !e->hygienicDecl || e->hygienicDecl->kind != StmtKind::Class)
        return nullptr;
    for (const std::unique_ptr<Symbol>& s : sema_.symbols)
        if (s && s->kind == SymbolKind::Class && s->decl == e->hygienicDecl)
            return s.get();
    return nullptr;
}

// Find a class candidate without letting a same-named function in the same
// scope hide it. Used only for the explicit `C::Label()` constructor spelling,
// where the qualifier itself states that a type/constructor is intended.
Symbol* Checker::visibleClass(std::string_view name) const {
    for (const Scope* sc = scope_; sc; sc = sc->parent)
        if (const std::vector<Symbol*>* syms = sc->localLookup(name))
            for (Symbol* s : *syms)
                if (s->kind == SymbolKind::Class) return s;
    return nullptr;
}

// bug.md #82: mirrors Resolver::resolveType's Named-path walk (§12) for
// value-position namespace qualifiers. The first segment searches outward
// from scope_ (an ordinary name lookup); every later segment is a
// localLookup on the prior namespace's OWN scope, so a qualified path can't
// leak into an enclosing scope. Returns null as soon as a segment isn't a
// namespace (a plain name, or a class/value used as the base).
Symbol* Checker::resolveNamespaceExpr(const Expr* e) const {
    if (!e) return nullptr;
    if (e->kind == ExprKind::Name) {
        Symbol* ns = scope_ ? scope_->lookup(e->text) : nullptr;
        return (ns && ns->kind == SymbolKind::Namespace) ? ns : nullptr;
    }
    if (e->kind == ExprKind::Member && e->colon) {
        Symbol* base = resolveNamespaceExpr(e->a.get());
        if (!base || !base->scope) return nullptr;
        if (const std::vector<Symbol*>* v = base->scope->localLookup(e->text))
            for (Symbol* s : *v)
                if (s->kind == SymbolKind::Namespace) return s;
    }
    return nullptr;
}

Type Checker::typeOfCallInner(const Expr* e, std::vector<char>& lambdaWalked) {
    std::vector<Type> argTypes;
    for (size_t i = 0; i < e->list.size(); ++i) {
        Expr* a = e->list[i].get();
        // LA-25 §2.2/§6 (M3): an OVERLOADED method reference can't type without
        // knowing which candidate the outer call picks — deferred exactly like
        // a Lambda argument until the overload below is chosen, then walked
        // against its parameter type (the ctor-arg loop and genericReturn's
        // lambda-walk loop, further down).
        if (a->kind == ExprKind::Lambda || isDeferredMethodRefArg(a)) {
            argTypes.push_back(unknown());
            continue;
        }
        Type t = typeOf(a);
        // A single-candidate method reference resolves immediately (no target
        // needed) and rewrites `a` into the eta-expansion Lambda right here —
        // mark it walked so the loops below don't re-walk the synthesized body.
        if (a->kind == ExprKind::Lambda) {
            if (lambdaWalked.size() <= i) lambdaWalked.resize(i + 1, 0);
            lambdaWalked[i] = 1;
        }
        argTypes.push_back(t);
    }
    Expr* call = const_cast<Expr*>(e);   // to record the resolved overload
    const Expr* callee = e->a.get();

    // helper: choose among candidates, record it, or error if none apply. With
    // injection on (§12.5), an unfilled trailing parameter that has a `bind` in
    // scope is filled implicitly (exact-arity overloads shadow injecting ones).
    auto resolve = [&](const std::vector<const Stmt*>& cands, const char* what,
                       bool allowInject = true) -> const Stmt* {
        if (cands.empty()) return nullptr;
        bool ok = false, diagnosed = false;
        const Stmt* picked = allowInject
            ? pickInjecting(cands, argTypes, call, ok, diagnosed)
            : pickOverload(cands, argTypes, ok);
        if (!ok) {
            if (!diagnosed) {
                // bug #54: inside a specialized generic body, a failed overload
                // resolution IS the missing-overload-at-instantiation case — emit
                // the two-span diagnostic (use site + instantiation note).
                if (activeSpecialization_)
                    specializationOverloadMissing(e, what, argTypes);
                else
                    error(e->span, std::string("no overload of '") + what +
                          "' matches the arguments");
            }
            return nullptr;
        }
        lambdaWalked.resize(call->list.size(), 0);
        return picked;
    };

    // --- Track 03 §2: `Enum::fromCode(...)` -> the mangled free function ---
    if (callee->kind == ExprKind::Member && callee->colon && callee->text == "fromCode" &&
        program_) {
        Type bt = typeOf(callee->a.get());
        if (bt.kind == TKind::TypeValue && bt.sym)
            for (const EnumDesugar& ed : program_->enumDesugars)
                if (ed.name == bt.sym->name && ed.fromCode) {
                    call->resolved = ed.fromCode;
                    return fromTypeRef(ed.fromCode->type.get());   // Enum?
                }
    }

    // --- struct-equality design §8: `float::fromBits(int)` -> float ---
    // No `static` keyword exists, so the static-on-primitive factory is homed
    // as the prelude free function math::floatFromBits and this `::` spelling
    // routes to it (same mechanism as Enum::fromCode above).
    if (callee->kind == ExprKind::Member && callee->colon && callee->text == "fromBits") {
        Type bt = typeOf(callee->a.get());
        if (bt.kind == TKind::TypeValue && bt.sym && bt.sym->isPrimitive &&
            bt.sym->name == "float") {
            Symbol* mathNs = sema_.global ? sema_.global->lookup("math") : nullptr;
            if (mathNs && mathNs->kind == SymbolKind::Namespace && mathNs->scope)
                if (const std::vector<Symbol*>* v = mathNs->scope->localLookup("floatFromBits"))
                    for (Symbol* s : *v)
                        if (s->decl) {
                            call->resolved = s->decl;
                            return fromTypeRef(s->decl->type.get());   // float
                        }
        }
    }

    // LA-18 missing-member check must run before the legacy construction path:
    // that path deliberately treats an absent concrete label as an implicit
    // construction fallback. A specialized `T::Label` is duck-typed instead,
    // so absence on the pinned type is the required instantiation error.
    if (activeSpecialization_ && callee->kind == ExprKind::Member && callee->colon &&
        (callee->genericStaticSite || (callee->a && callee->a->genericStaticSite))) {
        Type concrete = typeOf(callee->a.get());
        if (concrete.kind == TKind::TypeValue && concrete.sym &&
            ctorOverloads(concrete.sym, callee->text).empty()) {
            genericStaticMissing(callee, concrete.sym, callee->text, true);
            return Type{TKind::Error, nullptr, "", {}, nullptr, {}};
        }
    }

    // --- construction: T(...) or T::Label(...) ---
    Symbol* ctorClass = nullptr; std::string_view label;
    if (callee->kind == ExprKind::Name) {
        // A bare `T(...)` selects the ctor labeled with the CLASS's own name
        // (`s->name`), not the call-site text — they differ when `T` is a
        // `use ... as T;` alias (imports.md): the alias names the same slot,
        // so construction through it must still find the real "new T() {..}".
        if (Symbol* hs = hygienicClass(callee)) {
            ctorClass = hs; label = hs->name;
        } else if (Symbol* s = scope_ ? scope_->lookup(callee->text) : nullptr)
            if (s->kind == SymbolKind::Class) { ctorClass = s; label = s->name; }
    } else if (callee->kind == ExprKind::Member) {
        Type bt = typeOf(callee->a.get());
        if (bt.kind == TKind::TypeValue) { ctorClass = bt.sym; label = callee->text; }
        else if (callee->a->kind == ExprKind::Name && scope_) {
            // `C::Label()` is explicitly constructor-shaped. A generated
            // function may itself be named C, but that callable must not hide
            // the class from this qualified spelling (bug.md #22 round-trip).
            if (Symbol* cls = hygienicClass(callee->a.get())) {
                ctorClass = cls; label = callee->text;
            } else if (Symbol* cls = visibleClass(callee->a->text)) {
                ctorClass = cls; label = callee->text;
            }
            // NS::Class(...) — a class reached through a namespace (§12): the
            // member names a class, so this is default construction of it.
            if (!ctorClass) if (Symbol* ns = scope_->lookup(callee->a->text))
                if (ns->kind == SymbolKind::Namespace && ns->scope)
                    if (const std::vector<Symbol*>* v = ns->scope->localLookup(callee->text))
                        for (Symbol* s : *v)
                            if (s->kind == SymbolKind::Class) { ctorClass = s; label = callee->text; break; }
        }
        // bug #37: `A::B::T(...)` — the qualifier is a nested namespace chain
        // (callee->a is itself a `::`-Member), so the single-hop branch above
        // never reached it. Resolve the whole chain, then find the class.
        if (!ctorClass && callee->colon && callee->a->kind == ExprKind::Member &&
            callee->a->colon && scope_)
            if (Symbol* ns = nsChainSym(scope_, callee->a.get()))
                if (ns->scope)
                    if (const std::vector<Symbol*>* v = ns->scope->localLookup(callee->text))
                        for (Symbol* s : *v)
                            if (s->kind == SymbolKind::Class) { ctorClass = s; label = callee->text; break; }
    }
    if (ctorClass) {
        // Constructor parameters get the same implicit-injection fill as function
        // and method parameters (reference §4.7; bug.md #24): an unfilled trailing
        // parameter whose type has a `bind` in scope is filled from the binding.
        // pickInjecting synthesizes ExprKind::Inject arg nodes into call->list,
        // which construction evaluates like any other argument, so the runtime
        // side needs no change. Exact-arity overloads still shadow injecting ones.
        std::vector<const Stmt*> ctorCands = ctorOverloads(ctorClass, label);
        const Stmt* ctor = resolve(ctorCands, "constructor",
                                   /*allowInject=*/true);
        call->resolved = ctor;
        call->resolvedClass = ctorClass;
        // Lambda ctor args: walk each with its declared function-type param
        // types (no generic binding here — inferConstruction owns the class's
        // type args); marks them walked so the sweep doesn't re-walk.
        if (ctor)
            for (size_t i = 0; i < ctor->params.size() && i < e->list.size(); ++i) {
                if (lambdaWalked[i]) continue;
                Expr* a = e->list[i].get();
                const TypeRef* pt = ctor->params[i].type.get();
                if (a->kind == ExprKind::Lambda) {
                    // LA-31 §2.2: an `expr::Expr<Fn>` constructor parameter
                    // reifies the lambda in place, just like a function argument.
                    if (const TypeRef* fn = exprTargetFn(pt)) {
                        reifyLambda(a, fn);
                        lambdaWalked[i] = 1;
                        continue;
                    }
                    std::vector<Type> ptypes;
                    if (pt && pt->kind == TypeKind::Function)
                        for (const TypeRefPtr& fp : pt->funcParams)
                            ptypes.push_back(fromTypeRef(fp.get()));
                    checkLambdaBody(a, ptypes);
                    lambdaWalked[i] = 1;
                } else if (a->kind == ExprKind::Member) {
                    // LA-25 §2.2/§6 (M3): an overloaded method reference passed
                    // as a constructor argument (the route-table shape) —
                    // disambiguate against the declared parameter's function type.
                    Type expected = fromTypeRef(pt);
                    bool isRef = false;
                    tryResolveMethodRef(a, &expected, isRef);
                    if (isRef) lambdaWalked[i] = 1;
                }
            }
        return inferConstruction(ctorClass, ctor, argTypes, nullptr, e->span);
    }

    // --- free function (overloaded) ---
    if (callee->kind == ExprKind::Name) {
        if (Type* loc = localLookup(callee->text)) {              // function-typed local
            for (const ExprPtr& arg : e->list)
                if (!arg->argLabel.empty())
                    return error(e->span,
                        "named arguments require a declared function, method, "
                        "constructor, or attribute parameter name");
            if (loc->kind == TKind::FuncRef && loc->ret) return *loc->ret;
            return unknown();                                     // callable-ish local
        }
        auto cands = functionOverloads(callee->text);
        if (!cands.empty()) {
            if (const Stmt* fn = resolve(cands, "function")) {
                call->resolved = fn;
                return genericReturn(nullptr, fn, unknown(), argTypes, e, &lambdaWalked);
            }
            return unknown();   // overloads existed but none applied (already errored)
        }
        // a method of `this` called unqualified?
        if (thisClass_ && !slotsNamed(thisClass_->shape, callee->text).empty()) {
            if (const Stmt* m = resolve(methodOverloads(thisClass_, callee->text), "method")) {
                // S2 (§4.2): a bare this-call inside an inherited method — `this`
                // flowing through a base method is not necessarily the
                // most-derived class, so the same override-openness test applies.
                call->resolved = resolveDispatch(thisClass_, m, e->span) ? nullptr : m;
                return genericReturn(thisClass_, m, classType(thisClass_), argTypes, e, &lambdaWalked);
            }
            return unknown();
        }
        if (callee->text != "System") {
            Type err = error(e->span, "unknown function '" + std::string(callee->text) + "'");
            noteBlockScopedImport(callee->text);   // S5: block-confined import hint
            return err;
        }
        return unknown();
    }

    // --- namespaced function: A::fn(...), or a nested/sibling A::B::fn(...) ---
    // bug #37/#46: the qualifier may be a nested chain (`A::B::fn`), and the
    // chain root resolves through the ordinary scope chain so a SIBLING
    // namespace (`B::C::fn` written inside `A::D`, where B is A::B) is found too.
    // call->resolved is pinned here so every engine dispatches the same decl
    // without re-deriving the path (the oracle/IR used to fall back to a
    // global-only lookup that missed both the nested hop and the sibling root).
    if (callee->kind == ExprKind::Member && callee->colon &&
        (callee->a->kind == ExprKind::Name ||
         (callee->a->kind == ExprKind::Member && callee->a->colon))) {
        Symbol* ns = nsChainSym(scope_, callee->a.get());
        if (ns && ns->kind == SymbolKind::Namespace && ns->scope) {
            std::vector<const Stmt*> cands;
            if (const std::vector<Symbol*>* v = ns->scope->localLookup(callee->text))
                for (Symbol* s : *v)
                    if (s->kind == SymbolKind::Function && s->decl) cands.push_back(s->decl);
            if (const Stmt* fn = resolve(cands, "function")) {
                call->resolved = fn;
                return genericReturn(nullptr, fn, unknown(), argTypes, e, &lambdaWalked);
            }
            if (!cands.empty()) return unknown();
        }
    }

    // --- method call: base.method(...) ---
    if (callee->kind == ExprKind::Member) {
        Type bt = typeOf(callee->a.get());
        bool addNone = false;
        if (callee->optChain && bt.kind == TKind::Union) {
            bt = unionMinus(bt, "None");
            addNone = true;
        } else if (!callee->optChain && bt.kind == TKind::Union) {
            return error(e->span, "narrow the union before calling members "
                                  "(use '!= None' or 'is')");
        }
        if (bt.canonical == "None")
            return error(e->span,
                         "'None' has no members (this branch narrowed it to None)");
        if (bt.kind == TKind::Class) {
            if (const Stmt* m = resolve(methodOverloads(bt.sym, callee->text), "method")) {
                // const.md §4: a `mutating` method writes `this` — i.e. writes
                // the value in the receiver's slot — so it needs the same
                // write view an assignment to the receiver would need.
                if (m->isMutating) {
                    BlockedWrite blocked = constBlockedWrite(callee->a.get());
                    if (!blocked.name.empty())
                        error(e->span, "cannot call mutating method '" +
                              std::string(callee->text) + "' on " +
                              (blocked.kind == BlockedWriteKind::Readonly ? "readonly" : "const") +
                              (blocked.isField ? " field '" : " '") + blocked.name + "'");
                }
                // designs/complete/techdesign-class-method-dispatch.md §2/§4.2 (S1): an
                // unqualified call dispatches on the receiver's RUNTIME slot
                // value whenever the candidate set is open — interface-typed
                // (as before) or a class-typed receiver whose resolved method
                // is overridden by some subclass. Leave `resolved` unset so
                // every engine falls back to dynamic dispatch by name
                // (findMethod / CallDyn); otherwise bind statically (byte-
                // identical to the pre-existing fast path). §5.4: loud error
                // if that runtime lookup would be ambiguous by arity alone.
                //
                // EXCEPTION — a base-qualified call `recv.Base::method(...)`
                // (info.md §6.9) is ALWAYS statically resolved to the named
                // base's method, bypassing any subclass override. Detected by
                // the `::` marker over a class-name source qualifier; without
                // this the override makes resolveDispatch return true, `resolved`
                // is nulled, and the call dynamically dispatches back to the
                // override — defeating the whole point of the idiom (bug.md #55).
                bool baseQualified = false;
                if (callee->colon && callee->a->kind == ExprKind::Member) {
                    Symbol* q = scope_ ? scope_->lookup(callee->a->text) : nullptr;
                    if (q && q->kind == SymbolKind::Class) baseQualified = true;
                }
                call->resolved = (!baseQualified && resolveDispatch(bt.sym, m, e->span))
                                     ? nullptr : m;
                Type r = genericReturn(bt.sym, m, bt, argTypes, e, &lambdaWalked);
                if (addNone && r.kind != TKind::Unknown) {
                    Type u{TKind::Union, nullptr, r.canonical + " | None", {}, nullptr, {}};
                    u.unionMembers = {r, primType("None")};
                    return u;
                }
                return r;
            }
        }
    }
    // --- value call: the callee is any other expression producing a callable
    // value — a call result (`f(x)(y)`, bug.md #47), an array/map index
    // (`fns[i]()`, bug.md #52), etc. Type the callee so the inner call RESOLVES
    // (leaving it untyped left it an unresolved by-name CallDyn, whose emit-C++
    // reachability over-marks every same-named method) and yield its function
    // type's return. The outer call itself stays a dynamic CallValue.
    if (callee->kind != ExprKind::Name && callee->kind != ExprKind::Member) {
        Type ct = typeOf(callee);
        if (ct.kind == TKind::FuncRef && ct.ret) return *ct.ret;
    }
    return unknown();
}

// struct-equality §6 (packet 06/07): a read of the one `float::NaN` language
// constant. typeOfMember resolves such a read to program_->floatNaNGlobal
// (stamping Expr::resolved); we match on that decl pointer, never on spelling,
// so an aliased read through a variable (`float n = float::NaN; ...`) is NOT
// the constant. `program_->floatNaNGlobal` is only materialized when the
// program references `float::NaN`, so it is null (and this is false) otherwise.
bool Checker::isFloatNaNConst(const Expr* e) const {
    return e && program_ && program_->floatNaNGlobal &&
           e->kind == ExprKind::Member && e->colon &&
           e->resolved == program_->floatNaNGlobal;
}

Type Checker::typeOfBinary(const Expr* e) {
    const bool assignment = e->op == TokenKind::Eq || isCompoundAssign(e->op);
    const bool savedMethodRefsAllowed = methodRefsAllowed_;
    if (assignment) methodRefsAllowed_ = false;
    // bug #42: a plain assignment's TARGET is typed against its DECLARED type,
    // not any if-narrowed read type. Narrowing governs reads, and a write is
    // about to invalidate it (info.md: "assignments invalidate narrowing"), so
    // the lazy-init idiom `if (x == None) { x = Node(); }` must check `Node`
    // against `x`'s declared `Node?`, not the branch-narrowed `None`. Suppress
    // the target path's narrowing fact only while typing the LHS, then restore
    // it so the RHS still sees narrowing (e.g. `x = x.foo()`).
    std::unordered_map<std::string, Type> savedNarrowLhs;
    bool restoreNarrowLhs = false;
    if (e->op == TokenKind::Eq) {
        std::string tp = pathOf(e->a.get());
        auto it = narrow_.find(tp);
        if (!tp.empty() && it != narrow_.end()) {
            savedNarrowLhs = narrow_;
            restoreNarrowLhs = true;
            narrow_.erase(it);
        }
    }
    // OQ1: a bare-Name assignment target is a write position, so suppress the
    // read-before-definite-assignment check while typing it — the DA block below
    // is the single place that decides a pending const's fate for BOTH `x = ...`
    // (closes the window) and `x += ...` (reads first, so it reports the
    // read-before-assign there, not here — this avoids a duplicate diagnostic).
    typingLhs_ = assignment && e->a->kind == ExprKind::Name;
    Type lt = typeOf(e->a.get());
    typingLhs_ = false;
    if (restoreNarrowLhs) narrow_ = savedNarrowLhs;
    methodRefsAllowed_ = savedMethodRefsAllowed;

    // Short-circuit forms are typed BEFORE the eager rhs pass so that lhs
    // narrowing facts govern the rhs ( x != None && x.length() ).
    if (e->op == TokenKind::AmpAmp) {
        std::vector<Fact> facts;
        analyzeCond(e->a.get(), facts, false);
        std::unordered_map<std::string, Type> saved;
        applyFacts(facts, true, saved);
        typeOf(e->b.get());
        narrow_ = saved;
        return primType("bool");
    }
    if (e->op == TokenKind::PipePipe) {
        typeOf(e->b.get());
        return primType("bool");
    }

    // LA-25 §2.2: an assignment's LHS type is the target for an overloaded
    // method-reference RHS (`b.fn = C::overloaded;`) — thread it through
    // before the ordinary typeOf pass would hit the ambiguous-without-target
    // error.
    Type rt;
    if (e->op == TokenKind::Eq && e->b->kind == ExprKind::Member) {
        bool isRef = false;
        rt = tryResolveMethodRef(const_cast<Expr*>(e->b.get()), &lt, isRef);
        if (!isRef) rt = typeOf(e->b.get());
    } else {
        rt = typeOf(e->b.get());
    }

    // Track 03 §1: char comparison target-typing — `c == 'a'` / `'a' == c` where
    // the other operand is char re-types the single-scalar literal to char (so it
    // compares by scalar, not as a string).
    if (isComparison(e->op)) {
        if (lt.canonical == "char" && isCharLiteral(e->b.get())) {
            markCharLiteral(e->b.get()); rt = primType("char");
        } else if (rt.canonical == "char" && isCharLiteral(e->a.get())) {
            markCharLiteral(e->a.get()); lt = primType("char");
        }
    }
    // bug #63: a plain assignment `charVar = 'X'` re-types the single-quoted
    // RHS literal to char against the LHS's declared char type — the same
    // by-expected-type retyping declarations do, extended to (re-)assignment,
    // which #50's stated scope didn't cover (`cannot assign 'string' to 'char'`).
    if (e->op == TokenKind::Eq && lt.canonical == "char" && isCharLiteral(e->b.get())) {
        markCharLiteral(e->b.get()); rt = primType("char");
    }

    // A value struct's non-mutating method may not write `this`'s fields (§9): the
    // receiver is a copy, so the write would be silently lost. Mark it `mutating`.
    if ((e->op == TokenKind::Eq || isCompoundAssign(e->op)) &&
        thisClass_ && thisClass_->isValue && curMember_ && !curMember_->isMutating &&
        writesThisField(e->a.get()))
        error(e->span, "cannot mutate field '" + fieldNameOf(e->a.get()) +
              "' in a non-mutating method of value type '" +
              std::string(thisClass_->name) + "'; mark the method `mutating`");

    // const.md OQ1: definite single assignment. A write to a still-open const
    // local (declared without an initializer, §2.2) is handled here BEFORE the
    // ordinary const write-error below, because the FIRST plain assignment on a
    // path is the permitted initialization that closes the window. Everything
    // after it — a second write, a compound write (which reads first), or a
    // write from inside a deeper loop/try than the declaration (§2.3) — is an
    // error; once closed, the name leaves constPending_ and falls through to the
    // ordinary "cannot assign to const" path.
    if ((e->op == TokenKind::Eq || isCompoundAssign(e->op)) &&
        e->a->kind == ExprKind::Name) {
        auto pc = constPending_.find(std::string(e->a->text));
        if (pc != constPending_.end()) {
            std::string nm(e->a->text);
            if (isCompoundAssign(e->op))
                return error(e->a->span, "const '" + nm +
                             "' is read before it is definitely assigned");
            if (loopDepth_ > pc->second.loopDepth)
                return error(e->a->span, "cannot assign to const '" + nm +
                             "' inside a loop body; a const's single-assignment "
                             "window cannot span a loop (const.md §2.3)");
            if (tryDepth_ > pc->second.tryDepth)
                return error(e->a->span, "cannot assign to const '" + nm +
                             "' inside a 'try' body; a const's single-assignment "
                             "window cannot span a 'try' (const.md §2.3)");
            if (!assignable(rt, lt))
                return error(e->span, "cannot assign '" + rt.canonical + "' to '" +
                             lt.canonical + "'");
            constPending_.erase(pc);            // the window closes on this path
            invalidatePath(pathOf(e->a.get()));
            return lt;
        }
    }

    // const.md: reject a write to a const slot up front, for both plain
    // (`x = v`) and compound (`x += v`) assignment — supersedes Bug 7's ad
    // hoc "no namespace-global writes at all" ban now that constness can be
    // declared: a non-const namespace global stays freely assignable.
    if (e->op == TokenKind::Eq || isCompoundAssign(e->op)) {
        BlockedWrite blocked = constBlockedWrite(e->a.get());
        if (!blocked.name.empty()) {
            if (blocked.kind == BlockedWriteKind::Readonly)
                return error(e->a->span, "cannot assign to readonly field '" +
                             blocked.name + "' outside its constructor");
            if (blocked.isField)
                return error(e->a->span, "cannot assign to const field '" + blocked.name +
                             "'; `const` fields are compile-time constants — use "
                             "`readonly` for a field assigned during construction");
            return error(e->a->span, "cannot assign to const '" + blocked.name + "'");
        }
    }

    if (e->op == TokenKind::Eq) {                    // assignment
        if (!assignable(rt, lt))
            return error(e->span, "cannot assign '" + rt.canonical + "' to '" +
                         lt.canonical + "'");
        invalidatePath(pathOf(e->a.get()));          // narrowing no longer holds
        return lt;
    }
    // §3.7 loudness: the operators a primitive actually implements. Anything
    // else (% & | ^ << >> on float, - on string, + on bool, ...) is a compile
    // error here — it used to type as the operand and silently produce a
    // runtime void (the `int x = 1 << 4;` silent-void bug). `<<`/`>>`/`^` are
    // int-only bitwise ops (Track 01 F1); no compound `<<=`/`>>=`/`^=` in v1
    // (they never reach this lambda — see the compound-assign switch below).
    auto primOpOk = [](std::string_view n, TokenKind op) {
        switch (op) {
            case TokenKind::Plus:
                return n == "int" || n == "float" || n == "string";
            case TokenKind::Minus: case TokenKind::Star: case TokenKind::Slash:
                return n == "int" || n == "float";
            case TokenKind::Percent: case TokenKind::Amp: case TokenKind::Pipe:
            case TokenKind::LtLt: case TokenKind::GtGt: case TokenKind::Caret:
                return n == "int";
            default:
                return false;
        }
    };
    switch (e->op) {                                 // compound assignment: a op= b
        case TokenKind::PlusEq: case TokenKind::MinusEq: case TokenKind::StarEq:
        case TokenKind::SlashEq: case TokenKind::PercentEq:
            if (lt.kind == TKind::Class && lt.sym && lt.sym->isPrimitive) {
                TokenKind base = e->op == TokenKind::PlusEq   ? TokenKind::Plus
                               : e->op == TokenKind::MinusEq  ? TokenKind::Minus
                               : e->op == TokenKind::StarEq   ? TokenKind::Star
                               : e->op == TokenKind::SlashEq  ? TokenKind::Slash
                                                              : TokenKind::Percent;
                if (!primOpOk(lt.sym->name, base))
                    return error(e->span, "no operator '" + std::string(opSymbol(base)) +
                                 "' on '" + std::string(lt.sym->name) + "'");
            }
            return lt;
        default: break;
    }
    if (e->op == TokenKind::QuestionQuestion) {      // a ?? b : default-when-None
        Type stripped = unionMinus(lt, "None");
        if (rt.kind != TKind::Unknown && stripped.kind != TKind::Unknown &&
            !assignable(rt, stripped))
            return error(e->span, "'??' default ('" + rt.canonical +
                         "') does not match '" + stripped.canonical + "'");
        return stripped;
    }

    // struct-equality §4 (packet 06): an OPERATOR compare against the
    // `float::NaN` constant is statically always-false (`==`) / always-true
    // (`!=`) under IEEE — a compile error with a fixit, never a silent
    // constant result. Match on the resolved decl pointer (typeOfMember stamped
    // it), not spelling: an aliased read through a variable (`float n =
    // float::NaN; x == n`) is NOT the constant and stays legal — honestly
    // IEEE-false at runtime, the documented escape hatch. `x != x` is a
    // different node shape and is untouched.
    if ((e->op == TokenKind::EqEq || e->op == TokenKind::BangEq) &&
        program_ && program_->floatNaNGlobal) {
        if (isFloatNaNConst(e->a.get()) || isFloatNaNConst(e->b.get())) {
            const bool eq = e->op == TokenKind::EqEq;
            return error(e->span, std::string("comparing against float::NaN with '") +
                         (eq ? "==" : "!=") + "' is always " + (eq ? "false" : "true") +
                         " (IEEE) — use x.isNaN() or a float::NaN match arm");
        }
    }

    // Primitives keep built-in operators (their object mask exposes methods, not
    // arithmetic): comparisons/logical -> bool, arithmetic/concat -> same type —
    // but ONLY the implemented set (primOpOk above); the rest error loudly.
    if (lt.kind == TKind::Class && lt.sym && lt.sym->isPrimitive) {
        if (e->op == TokenKind::AmpAmp || e->op == TokenKind::PipePipe ||
            isComparison(e->op))
            return primType("bool");
        if (!primOpOk(lt.sym->name, e->op))
            return error(e->span, "no operator '" + std::string(opSymbol(e->op)) +
                         "' on '" + std::string(lt.sym->name) + "'");
        return lt;
    }
    // Operators are methods: on a user class, resolve the (op) overload by rhs type.
    if (lt.kind == TKind::Class) {
        auto cands = methodOverloads(lt.sym, opSymbol(e->op));
        // (!=) derives automatically as !(==) when no (!=) is declared (§5).
        if (cands.empty() && e->op == TokenKind::BangEq) {
            cands = methodOverloads(lt.sym, "==");
            if (!cands.empty()) return primType("bool");
        }
        if (!cands.empty()) {
            bool ok = false;
            if (const Stmt* m = pickOverload(cands, {rt}, ok)) {
                const_cast<Expr*>(e)->resolved = m;
                return genericReturn(lt.sym, m, lt, {rt});
            }
        }
        // Design §5.1: value-struct ==/!= with no (==) — synthesized or explicit —
        // is the comparability gate firing. Name the first bad field, loudly.
        // (Name-match parity with enumDesugars above; see packet 03 warning.)
        if (lt.sym && lt.sym->isValue && program_ &&
            (e->op == TokenKind::EqEq || e->op == TokenKind::BangEq)) {
            for (const StructEqSynth& s : program_->structEqSynths)
                if (!s.synthesized && s.structName == lt.sym->name)
                    return error(e->span, "struct '" + std::string(lt.sym->name) +
                        "' has no '(==)': field '" + s.badField + "' (" +
                        s.badKindNote + ") is not comparable — define an explicit "
                        "'bool (==)(" + std::string(lt.sym->name) + " other)' to opt in");
        }
        // Reference classes have built-in identity equality. A user-declared
        // (==) still wins above; otherwise ==/!= compare object identity.
        if (lt.sym && !lt.sym->isValue &&
            (e->op == TokenKind::EqEq || e->op == TokenKind::BangEq) &&
            rt.kind == TKind::Class)
            return primType("bool");
        return unknown();
    }

    if (e->op == TokenKind::AmpAmp || e->op == TokenKind::PipePipe) return primType("bool");
    if (isComparison(e->op)) return primType("bool");
    if (lt.kind == TKind::Primitive) return lt;      // void etc.
    return unknown();
}

// ---------------------------------------------------------------------------
//  overload resolution (resolution by type, §1)
// ---------------------------------------------------------------------------

const Stmt* Checker::pickOverload(const std::vector<const Stmt*>& cands,
                                  const std::vector<Type>& args, bool& anyApplicable,
                                  const Expr* call) {
    anyApplicable = false;
    const Stmt* best = nullptr;
    int bestScore = -1;
    for (const Stmt* c : cands) {
        if (c->params.size() != args.size()) continue;      // arity must match
        int score = 0;
        bool ok = true;
        for (size_t i = 0; i < args.size(); ++i) {
            const TypeRef* p = c->params[i].type.get();
            if (mentionsTypeParam(p)) { score += 1; continue; }   // generic param: unifies
            Type pt = fromTypeRef(p);
            // bug.md #34: a deferred LAMBDA LITERAL argument is Unknown only
            // because its type is inferred from whichever parameter wins
            // (§9 "lambda-last" bidirectional inference) — it is compatible
            // ONLY with a function-typed parameter, unlike a genuinely
            // unknown/error-typed argument, which stays universally lenient.
            // Scoring it lenient against every parameter let a lambda tie
            // with (and lose to, by declaration order) a `string` overload
            // it could never actually satisfy.
            if (call && i < call->list.size() &&
                (call->list[i]->kind == ExprKind::Lambda ||
                 isDeferredMethodRefArg(call->list[i].get()))) {
                if (pt.kind == TKind::FuncRef) { score += 1; continue; }
                ok = false; break;
            }
            if (pt.kind == TKind::Unknown || args[i].kind == TKind::Unknown ||
                args[i].kind == TKind::Error) { score += 1; continue; }   // lenient
            if (pt.canonical == args[i].canonical) { score += 2; continue; }  // exact
            if (assignable(args[i], pt)) { score += 1; continue; }           // widening
            ok = false; break;                                                // inapplicable
        }
        if (!ok) continue;
        anyApplicable = true;
        if (score > bestScore) { bestScore = score; best = c; }   // ties: first-declared
    }
    return best;
}

std::vector<const Stmt*> Checker::methodOverloads(Symbol* cls, std::string_view name) {
    std::vector<const Stmt*> out;
    if (cls)
        for (const Slot& s : cls->shape.slots)
            if (s.isMethod && s.name == name) out.push_back(s.decl);
    return out;
}

std::vector<const Stmt*> Checker::ctorOverloads(Symbol* cls, std::string_view label) {
    std::vector<const Stmt*> out;
    if (cls && cls->decl)
        for (const StmtPtr& m : cls->decl->body)
            if (m->isCtor && m->name == label) out.push_back(m.get());
    return out;
}

std::vector<const Stmt*> Checker::functionOverloads(std::string_view name) {
    std::vector<const Stmt*> own;        // genuine declarations in the scope chain
    std::vector<const Stmt*> imported;   // names dumped in by a file-level `uses`/`use`
    for (const Scope* sc = scope_; sc; sc = sc->parent) {
        bool overlay = sema_.isFileOverlay(sc);
        if (const std::vector<Symbol*>* v = sc->localLookup(name))
            for (Symbol* s : *v)
                if (s->kind == SymbolKind::Function && s->decl)
                    (overlay ? imported : own).push_back(s->decl);
        // don't stop at first scope: a name can have overloads across enclosing scopes
    }
    // bug.md #78: a file's OWN declaration outranks a same-signature function
    // merely pulled in by a bulk `uses NS;`. The per-file import overlay sits
    // NEARER than `global` in the scope chain, so an imported name would
    // otherwise precede — and, since pickOverload breaks score ties by
    // first-in-list, silently shadow — the file's own top-level declaration.
    // Ranking own declarations first fixes that; genuinely different-signature
    // imports still participate as overloads (they win on arity/type score
    // regardless of position).
    own.insert(own.end(), imported.begin(), imported.end());
    return own;
}

// ---------------------------------------------------------------------------
//  bind / inject — dependency injection (§12.5)
// ---------------------------------------------------------------------------

// Enter a lexical scope: pre-scan its direct-child factory `bind`s so a bind is
// visible block-wide (not only after its textual position), with duplicate-in-
// scope detection. Object-install binds (`bind expr;`) are staged separately.
void Checker::pushLexicalScope(Scope* scope, const std::vector<StmtPtr>& items) {
    lexical_.pushScope(scope);
    for (const StmtPtr& s : items) {
        if (!s || s->kind != StmtKind::Bind || !s->type) continue;   // factory form only
        // bug.md #23: reject a `bind` on a VALUE type (struct/primitive). Injection
        // realizes a factory body through a fresh call frame that does not carry a
        // value type through — a struct bind silently arrives default-constructed
        // (even an inline `bind Cfg => Cfg(...)`), breaking the loudness rule (§16).
        // Value binds stay unsupported until a by-value-copy mechanism is designed;
        // such values must be passed as explicit arguments. (Registration itself —
        // into `scope->binds`, with duplicate-in-scope detection — is the Resolver's
        // job now, block-scoped-use §3.2; only this rule needs Checker type info.)
        Type bt = fromTypeRef(s->type.get());
        if (bt.sym && bt.sym->isValueType())
            error(s->span, "cannot 'bind' the value type '" + bt.canonical +
                  "' (a struct/primitive): dependency injection cannot carry a value "
                  "through — pass it as an explicit argument instead");
    }
    // system-binds.md §5.3 (Channel 1): activate each `use` the Resolver stamped
    // as resolving to a class/interface — installing its namespace's exported
    // bind for that type into THIS frame, exactly as if a `bind T => <that
    // factory>;` were textually present here. Rule 4: textual beats activated,
    // silently (a textual bind in `scope->binds` already claimed the key) — not
    // the duplicate-bind hard error, which is reserved for two textual claims.
    // Rule 5 (activated-vs-activated can't collide) falls out: a second `use` of
    // the same (NS, T) finds the key already filled by the first and skips.
    // Two distinct keys: `namespaceBinds_` is looked up by the type's OWN name
    // (how the bind is declared inside NS, §5.2's index key), but the activated
    // entry is registered under `s->name` — the LOCAL name this `use` bound (the
    // alias under `as`, else the same name) — because that's the spelling an
    // injection site in THIS scope's param types will canonicalize to. An alias
    // changes the name only (§5.1 rule 2): keying by anything else would make
    // `use std::IEnv as E;` fail to fill an `E`-typed parameter even though it's
    // the identical bind.
    auto& activated = lexical_.frames.back().activated;
    for (const StmtPtr& s : items) {
        if (!s || s->kind != StmtKind::Use || !s->useResolvedNs) continue;
        std::string localKey(s->name);
        if ((scope && scope->localBind(localKey)) || activated.count(localKey)) continue;
        auto nsIt = namespaceBinds_.find(s->useResolvedNs);
        if (nsIt == namespaceBinds_.end()) continue;
        auto bindIt = nsIt->second.find(std::string(s->useResolvedTypeKey));
        if (bindIt == nsIt->second.end()) continue;
        activated[localKey] = bindIt->second;
    }
}

// system-binds.md §5.2: index every namespace body's top-level factory binds,
// read-only — descending into `items`' Namespace statements only (a bind
// nested in a block/function inside the namespace is not exported, §5.1
// rule 1) and recursing into nested namespaces via their OWN resolved scope,
// the same navigation `walk` uses for a namespace body. Never descends into a
// factory body: this is a lookup structure, not a second checking pass.
void Checker::indexNamespaceBinds(const std::vector<StmtPtr>& items, Scope* scope) {
    for (const StmtPtr& s : items) {
        if (!s || s->kind != StmtKind::Namespace || !scope) continue;
        Symbol* ns = scope->lookup(s->name);
        if (!ns || !ns->scope) continue;
        auto& table = namespaceBinds_[ns];
        for (const StmtPtr& c : s->body) {
            if (!c || c->kind != StmtKind::Bind || !c->type) continue;  // factory form only
            std::string key = c->type->canonical.empty()
                ? std::string(c->type->name) : c->type->canonical;
            table.emplace(key, c.get());   // first-declared wins on a stray duplicate
        }
        indexNamespaceBinds(s->body, ns->scope);
    }
}

void Checker::buildNamespaceBindIndex(const Program* prelude, const Program& program) {
    if (namespaceBindsBuilt_) return;
    namespaceBindsBuilt_ = true;
    if (prelude) indexNamespaceBinds(prelude->items, sema_.global);
    indexNamespaceBinds(program.items, sema_.global);
}

void Checker::popLexicalScope() {
    lexical_.pop();
}

// Nearest-wins: search innermost frame outward (block-scoped-use §3.3).
const Stmt* Checker::lookupBind(const std::string& canonical) {
    return lexical_.lookupBind(canonical);
}

static ExprPtr cloneDefaultExpr(const Expr* e) {
    if (!e) return nullptr;
    auto out = std::make_unique<Expr>(e->kind);
    out->span = e->span; out->text = e->text; out->op = e->op;
    out->colon = e->colon; out->optChain = e->optChain;
    out->isRawSegment = e->isRawSegment; out->isQuasiPayload = e->isQuasiPayload;
    out->isRawString = e->isRawString;
    out->singleQuoted = e->singleQuoted;
    out->charLit = e->charLit;
    out->a = cloneDefaultExpr(e->a.get());
    out->b = cloneDefaultExpr(e->b.get());
    out->c = cloneDefaultExpr(e->c.get());
    for (const ExprPtr& x : e->list) out->list.push_back(cloneDefaultExpr(x.get()));
    if (e->type) out->type = copyTypeRef(e->type.get());
    return out;
}

const Stmt* Checker::pickInjecting(const std::vector<const Stmt*>& cands,
                                   std::vector<Type>& argTypes, Expr* call, bool& ok,
                                   bool& diagnosed) {
    diagnosed = false;
    // Re-entry sees the canonical full positional list and can use the ordinary
    // overload picker.  This is the idempotence guard for synthesized defaults
    // and injections.
    if (call->argsNormalized) return pickOverload(cands, argTypes, ok, call);

    struct Binding {
        const Stmt* candidate = nullptr;
        std::vector<int> supplied;          // param index -> raw argument index
        std::vector<const Stmt*> injections;
        int score = -1;
        int omitted = 0;
    } best;

    // LA-31 §2.2 / E3: per applicable candidate, which lambda-LITERAL argument
    // positions matched an `expr::Expr<Fn>` parameter vs a FuncRef parameter.
    struct CandTie { int score; std::vector<int> exprTargetArgs; std::vector<int> funcRefArgs; };
    std::vector<CandTie> ties;

    std::string soleFailure;
    for (const Stmt* c : cands) {
        Binding cur;
        cur.candidate = c;
        cur.supplied.assign(c->params.size(), -1);
        cur.injections.assign(c->params.size(), nullptr);
        bool applicable = true;
        std::string failure;
        size_t positional = 0;

        for (size_t ai = 0; ai < call->list.size() && applicable; ++ai) {
            const Expr* arg = call->list[ai].get();
            size_t pi = c->params.size();
            if (arg->argLabel.empty()) {
                pi = positional++;
                if (pi >= c->params.size()) applicable = false;
            } else {
                for (size_t j = 0; j < c->params.size(); ++j)
                    if (c->params[j].name == arg->argLabel) { pi = j; break; }
                if (pi == c->params.size()) {
                    failure = "no parameter named '" + std::string(arg->argLabel) + "'";
                    applicable = false;
                } else if (cur.supplied[pi] >= 0) {
                    failure = "parameter '" + std::string(arg->argLabel) +
                              "' is bound both positionally and by name";
                    applicable = false;
                }
            }
            if (applicable) cur.supplied[pi] = static_cast<int>(ai);
        }

        for (size_t pi = 0; pi < c->params.size() && applicable; ++pi) {
            if (cur.supplied[pi] >= 0) continue;
            ++cur.omitted;
            if (c->params[pi].defaultValue) continue;       // explicit default wins
            Type pt = fromTypeRef(c->params[pi].type.get());
            const Stmt* bind = pt.canonical.empty() ? nullptr : lookupBind(pt.canonical);
            if (bind) cur.injections[pi] = bind;
            else {
                failure = "missing required argument '" +
                          std::string(c->params[pi].name) + "'";
                applicable = false;
            }
        }

        int score = 0;
        std::vector<int> curExprTargetArgs, curFuncRefArgs;
        for (size_t pi = 0; pi < c->params.size() && applicable; ++pi) {
            int ai = cur.supplied[pi];
            if (ai < 0) continue;                            // omitted values do not score
            const TypeRef* p = c->params[pi].type.get();
            if (mentionsTypeParam(p)) { score += 1; continue; }
            Type pt = fromTypeRef(p);
            const Type& at = argTypes[static_cast<size_t>(ai)];
            // bug.md #34: a deferred LAMBDA LITERAL argument is Unknown only
            // because its type is inferred from whichever parameter wins
            // (§9 "lambda-last") — compatible ONLY with a function-typed
            // parameter, unlike a genuinely unknown/error type (universally
            // lenient below). Without this, a lambda argument tied with (and
            // lost to, by declaration order) a sibling `string` overload it
            // could never satisfy.
            if (call->list[static_cast<size_t>(ai)]->kind == ExprKind::Lambda ||
                isDeferredMethodRefArg(call->list[static_cast<size_t>(ai)].get())) {
                bool isLit = call->list[static_cast<size_t>(ai)]->kind == ExprKind::Lambda;
                if (pt.kind == TKind::FuncRef) {
                    if (isLit) curFuncRefArgs.push_back(ai);
                    score += 1; continue;
                }
                // LA-31 §2.2 (R2): a lambda LITERAL is applicable against an
                // `expr::Expr<Fn>` parameter at the same tier, if arities match.
                if (isLit)
                    if (const TypeRef* fn = exprTargetFn(p))
                        if (fn->funcParams.size() ==
                            call->list[static_cast<size_t>(ai)]->params.size()) {
                            curExprTargetArgs.push_back(ai);
                            score += 1; continue;
                        }
                applicable = false;
                continue;
            }
            if (pt.kind == TKind::Unknown || at.kind == TKind::Unknown ||
                at.kind == TKind::Error) { score += 1; continue; }
            if (pt.canonical == at.canonical) { score += 2; continue; }
            // bug #50: a bare single-quoted char literal re-types to `char` when
            // the parameter expects char — the same by-expected-type retyping
            // that declarations/comparisons/returns already apply (typeInitExpr).
            // Applied here in call-argument applicability so a lone `char`
            // overload isn't rejected outright; the literal is marked below only
            // for the WINNING candidate (never during a losing candidate's scan).
            if (pt.canonical == "char" &&
                isCharLiteral(call->list[static_cast<size_t>(ai)].get())) {
                score += 2; continue;
            }
            if (assignable(at, pt)) { score += 1; continue; }
            applicable = false;
        }
        if (!applicable) {
            if (cands.size() == 1) soleFailure = std::move(failure);
            continue;
        }
        cur.score = score;
        ties.push_back({score, std::move(curExprTargetArgs), std::move(curFuncRefArgs)});
        if (!best.candidate || cur.score > best.score ||
            (cur.score == best.score && cur.omitted < best.omitted))
            best = std::move(cur);                            // exact ties: first-declared
    }

    // LA-31 E3 (§2.2): a lambda literal that matched a FuncRef parameter in one
    // top-tier candidate and an `expr::Expr` parameter in another is ambiguous —
    // this overrides the declaration-order tiebreak. Post-hoc over ties only; the
    // scoring numbers themselves are untouched (bug #34 adjacency, H1).
    if (best.candidate) {
        for (const CandTie& t : ties) {
            if (t.score != best.score) continue;
            for (int ei : t.exprTargetArgs)
                for (const CandTie& u : ties) {
                    if (u.score != best.score) continue;
                    for (int fi : u.funcRefArgs)
                        if (fi == ei) {
                            error(call->list[static_cast<size_t>(ei)]->span,
                                  "ambiguous lambda argument: matches both a function "
                                  "parameter and an expr::Expr parameter; extract a typed "
                                  "local to select");
                            ok = false; diagnosed = true;
                            return nullptr;
                        }
                }
        }
    }

    // LA-31 E1 (§2.2/R16): a NON-literal argument that landed in an
    // `expr::Expr<Fn>` parameter of the chosen candidate — a lambda-typed value,
    // a `C::m` reference, etc. FuncRef is universally assignable, so scoring did
    // not reject it; name it rather than silently (mis)accepting a non-reifiable
    // value. Only a lambda literal can reify (R16).
    if (best.candidate)
        for (size_t pi = 0; pi < best.candidate->params.size(); ++pi) {
            int ai = best.supplied[pi];
            if (ai < 0) continue;
            if (!exprTargetFn(best.candidate->params[pi].type.get())) continue;
            const Expr* a = call->list[static_cast<size_t>(ai)].get();
            if (a->kind == ExprKind::Lambda) continue;   // a literal reifies
            // Only a lambda-typed VALUE is the error; passing an already-typed
            // `expr::Expr<F>` value through is an ordinary value pass.
            if (argTypes[static_cast<size_t>(ai)].kind != TKind::FuncRef) continue;
            error(a->span, "only a lambda literal can be reified to expr::Expr<F>");
            ok = false; diagnosed = true;
            return nullptr;
        }

    if (!best.candidate) {
        ok = false;
        if (!soleFailure.empty()) {
            error(call->span, soleFailure);
            diagnosed = true;
        }
        return nullptr;
    }

    std::vector<ExprPtr> raw = std::move(call->list);
    std::vector<Type> rawTypes = std::move(argTypes);
    std::vector<ExprPtr> ordered(best.candidate->params.size());
    std::vector<Type> orderedTypes(best.candidate->params.size());
    for (size_t pi = 0; pi < best.candidate->params.size(); ++pi) {
        int ai = best.supplied[pi];
        if (ai >= 0) {
            ordered[pi] = std::move(raw[static_cast<size_t>(ai)]);
            ordered[pi]->argLabel = {};
            orderedTypes[pi] = rawTypes[static_cast<size_t>(ai)];
            // bug #50: finalize a char-literal argument bound to a `char`
            // parameter of the chosen overload — flip it to a char value so the
            // engines emit a char, matching the retyping declarations already do.
            if (isCharLiteral(ordered[pi].get()) &&
                expectsChar(best.candidate->params[pi].type.get())) {
                markCharLiteral(ordered[pi].get());
                orderedTypes[pi] = primType("char");
            }
        } else if (best.candidate->params[pi].defaultValue) {
            ordered[pi] = cloneDefaultExpr(best.candidate->params[pi].defaultValue.get());
            orderedTypes[pi] = fromTypeRef(best.candidate->params[pi].type.get());
        } else {
            auto arg = std::make_unique<Expr>(ExprKind::Inject);
            arg->span = call->span;
            arg->type = copyTypeRef(best.candidate->params[pi].type.get());
            arg->resolved = best.injections[pi];
            orderedTypes[pi] = fromTypeRef(arg->type.get());
            ordered[pi] = std::move(arg);
        }
    }
    call->list = std::move(ordered);
    argTypes = std::move(orderedTypes);
    call->argsNormalized = true;
    ok = true;
    return best.candidate;
}

// ---------------------------------------------------------------------------
//  generic inference (§9)
// ---------------------------------------------------------------------------

// `cls`'s own direct base/interface TypeRef whose resolved symbol is `target`
// (e.g. `class ArrayIterator<T> : IIterator<T>` when target=IIterator) — its
// `.generics` are written in terms of `cls`'s OWN type parameters, ready to
// unify against a target instantiation's generics. One level only (deeper
// ancestry would need substitution composition through the intermediate
// class's own params — not needed by any shape this checker builds today;
// falling through to the existing raw/error handling below is correct and
// safe if a future caller needs it).
static const TypeRef* directBaseTypeRef(Symbol* cls, Symbol* target) {
    if (!cls || !cls->decl || !target) return nullptr;
    for (const TypeRefPtr& b : cls->decl->bases)
        if (b->resolvedSymbol == target) return b.get();
    return nullptr;
}

Type Checker::inferConstruction(Symbol* cls, const Stmt* ctor,
                                const std::vector<Type>& args, const TypeRef* expected,
                                SourceSpan span) {
    if (!cls || !cls->decl || cls->decl->generics.empty())
        return classType(cls);

    const std::vector<std::string_view>& params = cls->decl->generics;
    std::unordered_map<std::string_view, Type> map;

    // 1. from the chosen constructor's arguments — full structural unification
    // (the same unify() Track 05 built for lambda-last method inference), not
    // just a parameter typed AS a bare class type parameter (`T v`) but any
    // parameter whose declared type mentions one through a compound generic
    // shape (`Array<T> src`, `IIterator<T> s`, ...) — unify() already walks
    // both shapes correctly; a lambda-typed param is silently skipped (its
    // TypeKind::Function guard), same as before.
    if (ctor)
        for (size_t i = 0; i < ctor->params.size() && i < args.size(); ++i)
            unify(ctor->params[i].type.get(), args[i], map);
    // 2. fill remaining from the target type — either `cls` itself, or (the
    // common protocol-implementation shape: a constructor result immediately
    // used as one of `cls`'s own declared bases/interfaces, e.g.
    // `IIterator<T> iterator() => ArrayIterator(items);`) a direct base of
    // `cls`. In the latter case the base's generics (written in `cls`'s own
    // type-parameter names) unify against the expected instantiation's
    // generics to bind `cls`'s params — the same unify() Track 05 built for
    // lambda-last method inference, reused here for interface satisfaction.
    if (expected && expected->kind == TypeKind::Named) {
        if (expected->resolvedSymbol == cls && expected->generics.size() == params.size()) {
            for (size_t i = 0; i < params.size(); ++i)
                if (!map.count(params[i])) map[params[i]] = fromTypeRef(expected->generics[i].get());
        } else if (const TypeRef* baseRef = directBaseTypeRef(cls, expected->resolvedSymbol)) {
            for (size_t i = 0; i < baseRef->generics.size() && i < expected->generics.size(); ++i)
                unify(baseRef->generics[i].get(), fromTypeRef(expected->generics[i].get()), map);
        }
    }

    // Inside a generic class body and/or a generic method (thisClass_'s own
    // params, or the current member's own <U>-style params) a type parameter
    // is inherently symbolic — this whole file's convention is to check such
    // bodies once, leniently (§2.5's HKT rule; every other generic body reads
    // its own T as unknown() throughout, per fromTypeRef's Named case above).
    // A failed inference here isn't a user mistake to report — it's the same
    // "can't know yet, verify at the real call site instead" situation
    // substitute() already degrades gracefully (§ anyUnbound below); only a
    // genuinely concrete, non-generic context still owes the user a hard
    // "cannot infer" diagnostic.
    bool insideGenericScope = (thisClass_ && thisClass_->decl && !thisClass_->decl->generics.empty()) ||
                              (curMember_ && !curMember_->generics.empty());

    std::vector<Type> bound;
    bound.reserve(params.size());
    bool anyUnbound = false;
    for (std::string_view p : params) {
        auto it = map.find(p);
        if (it == map.end()) {
            // §9: inferred when recoverable, REQUIRED when not.
            if (!insideGenericScope)
                error(span, "cannot infer type argument '" + std::string(p) +
                      "' for '" + std::string(cls->name) +
                      "'; provide a target type or a type-bearing argument");
            anyUnbound = true;
            continue;
        }
        // A bound type-PARAMETER reference (e.g. constructing Foo<T> from
        // inside a generic body using the enclosing class's own, still-
        // abstract T — fromTypeRef has no Type for a bare type param and
        // returns unknown()) has no concrete Type to report yet. Degrade to
        // the raw head, the same fallback substitute() already uses for
        // this exact shape (§2.5's leniency), rather than building a
        // malformed 'Foo<>' that then fails every assignability check
        // downstream (raw compatibility requires no '<' in the canonical at
        // all — a stray empty bracket pair does not qualify).
        if (it->second.kind == TKind::Unknown) anyUnbound = true;
        else bound.push_back(it->second);
    }
    if (anyUnbound) return classType(cls);   // raw — assignable to any instantiation

    Type r{TKind::Class, cls, std::string(cls->name) + "<", {}, nullptr, {}};
    for (size_t i = 0; i < bound.size(); ++i) {
        if (i) r.canonical += ", ";
        r.args.push_back(bound[i]);
        r.canonical += bound[i].canonical;
    }
    r.canonical += ">";
    return r;
}

// Bind type-param names by matching a declared parameter TypeRef against an
// argument's actual Type (only where a type variable appears; lambdas are opaque).
void Checker::unify(const TypeRef* param, const Type& arg,
                    std::unordered_map<std::string_view, Type>& subst) {
    if (!param || param->kind != TypeKind::Named) return;
    if (param->resolvedSymbol && param->resolvedSymbol->kind == SymbolKind::TypeParam) {
        if (param->generics.empty()) {
            if (!subst.count(param->name)) subst[param->name] = arg;   // bind the type var
        } else if (arg.kind == TKind::Class && arg.sym) {
            // Higher-kinded: F<A> vs Array<int> — bind the constructor HEAD
            // (F = Array), then unify the type arguments (A = int).
            if (!subst.count(param->name))
                subst[param->name] =
                    Type{TKind::TypeValue, arg.sym, std::string(arg.sym->name), {}, nullptr, {}};
            for (size_t i = 0; i < param->generics.size() && i < arg.args.size(); ++i)
                unify(param->generics[i].get(), arg.args[i], subst);
        }
        return;
    }
    // Concrete generic (e.g. Array<U>) vs a matching instantiation (Array<Tag>).
    if (!param->generics.empty() && param->resolvedSymbol == arg.sym)
        for (size_t i = 0; i < param->generics.size() && i < arg.args.size(); ++i)
            unify(param->generics[i].get(), arg.args[i], subst);
}

// Rebuild a declared TypeRef with type params replaced by their bindings.
Type Checker::substitute(const TypeRef* t,
                         const std::unordered_map<std::string_view, Type>& subst) {
    if (!t) return unknown();
    // A generic-optional return (`T?` = `T | None`, Track 05 §2's `find`/
    // `firstOrNone` etc.): substitute each member so `T` resolves to the
    // receiver's bound type, same as any other declared-type position.
    if (t->kind == TypeKind::Union) {
        Type u{TKind::Union, nullptr, "", {}, nullptr, {}};
        for (const TypeRefPtr& m : t->members) {
            Type mt = substitute(m.get(), subst);
            if (!u.canonical.empty()) u.canonical += " | ";
            u.canonical += mt.canonical;
            u.unionMembers.push_back(std::move(mt));
        }
        return u;
    }
    if (t->kind == TypeKind::Named) {
        auto it = subst.find(t->name);
        if (it != subst.end()) {
            if (t->generics.empty()) return it->second;        // a bound type var
            // Higher-kinded application: F<B> with F bound to a constructor head.
            Symbol* head = it->second.sym;
            if (!head) return unknown();
            Type r{TKind::Class, head, std::string(head->name), {}, nullptr, {}};
            bool anyUnbound = false;
            for (const TypeRefPtr& g : t->generics) {
                Type at = substitute(g.get(), subst);
                if (at.kind == TKind::Unknown) anyUnbound = true;
                r.args.push_back(at);
            }
            if (anyUnbound) { r.args.clear(); return r; }      // raw head — lenient
            r.canonical += "<";
            for (size_t i = 0; i < r.args.size(); ++i) {
                if (i) r.canonical += ", ";
                r.canonical += r.args[i].canonical;
            }
            r.canonical += ">";
            return r;
        }
        Symbol* s = t->resolvedSymbol;
        if (s && s->kind == SymbolKind::Primitive) return primitive(t->canonical);
        if (s && s->kind == SymbolKind::Class) {
            Type r{TKind::Class, s, std::string(s->name), {}, nullptr, {}};
            bool anyUnbound = false;
            for (const TypeRefPtr& g : t->generics) {
                Type at = substitute(g.get(), subst);
                if (at.kind == TKind::Unknown) anyUnbound = true;
                r.args.push_back(std::move(at));
            }
            // An unbound var (e.g. U in map<U> when the lambda's return type
            // couldn't be inferred) degrades to the RAW head — assignable to
            // any instantiation (§2.5's leniency, same as the HKT path above)
            // — never the malformed hard-error form 'Array<>'.
            if (anyUnbound) { r.args.clear(); return r; }
            if (!r.args.empty()) {
                r.canonical += "<";
                for (size_t i = 0; i < r.args.size(); ++i) {
                    if (i) r.canonical += ", ";
                    r.canonical += r.args[i].canonical;
                }
                r.canonical += ">";
            }
            return r;
        }
        return unknown();   // an unbound type var, or unresolved
    }
    return fromTypeRef(t);
}

// Walk a lambda body with supplied parameter types (declared types win; missing
// entries fall back to unknown, the old behavior). Returns the inferred return
// type: an expression body's type, or a block body's uniform Return type —
// mixed or absent returns yield unknown, which substitute() degrades leniently.
Type Checker::checkLambdaBody(const Expr* lam, const std::vector<Type>& paramTypes) {
    env_.emplace_back();
    for (size_t i = 0; i < lam->params.size(); ++i) {
        const Param& p = lam->params[i];
        Type pt = p.type ? fromTypeRef(p.type.get())
                         : (i < paramTypes.size() ? paramTypes[i] : unknown());
        env_.back()[p.name] = {std::move(pt), p.isConst};
    }
    Type savedRt = returnType_; const TypeRef* savedRtr = returnTypeRef_;
    returnType_ = Type{}; returnTypeRef_ = nullptr;
    // techdesign-02 F1: a break/continue inside this lambda can never exit a
    // loop in the ENCLOSING function — the lambda body is its own loop scope.
    int savedLoopDepth = loopDepth_;
    loopDepth_ = 0;
    // techdesign-labeled-break-continue.md F3 (P4): a labeled break/continue
    // naming an ENCLOSING function's label must fail to resolve too.
    auto savedLabelStack = std::move(labelStack_);
    labelStack_.clear();
    std::vector<Type> rets;
    std::vector<Type>* savedLr = lambdaReturns_;
    Type ret = unknown();
    if (lam->block) {
        lambdaReturns_ = &rets;
        check(lam->block.get());
        lambdaReturns_ = savedLr;
        bool uniform = !rets.empty();
        for (const Type& t : rets)
            if (t.canonical.empty() || t.canonical != rets[0].canonical) uniform = false;
        if (uniform) ret = rets[0];
    } else if (lam->a) {
        lambdaReturns_ = nullptr;   // an expr body's nested lambdas own their returns
        ret = typeOf(lam->a.get());
        lambdaReturns_ = savedLr;
    }
    loopDepth_ = savedLoopDepth;
    labelStack_ = std::move(savedLabelStack);
    returnType_ = savedRt; returnTypeRef_ = savedRtr;
    exitEnvScope();
    return ret;
}

// ===========================================================================
//  LA-31 expression reification (designs/expr-reification/techdesign-02-reifier.md)
// ===========================================================================

// R6: the one whitelist register — drives both the accept path (§3.3 Call row)
// and the E2 "reifiable calls: …" allow-list line (generated, never hand-kept).
namespace {
struct ExprCallRow { const char* name; enum Recv { Str, ArrayT } recv; int arity; };
constexpr ExprCallRow kExprWhitelist[] = {
    {"like",       ExprCallRow::Str,    1},
    {"ilike",      ExprCallRow::Str,    1},
    {"startsWith", ExprCallRow::Str,    1},
    {"endsWith",   ExprCallRow::Str,    1},
    {"contains",   ExprCallRow::Str,    1},
    {"contains",   ExprCallRow::ArrayT, 1},
};
std::string whitelistAllowLine() {
    std::string s = "reifiable calls: ";
    for (size_t i = 0; i < std::size(kExprWhitelist); ++i) {
        const ExprCallRow& r = kExprWhitelist[i];
        if (i) s += ", ";
        s += (r.recv == ExprCallRow::Str ? "string." : "Array.");
        s += r.name; s += "/"; s += std::to_string(r.arity);
    }
    return s;
}
// The reify op set (§3.3): whitelisted binary/unary spellings. opSymbol above
// covers everything except the short-circuit boolean pair.
const char* reifyOp(TokenKind k) {
    if (k == TokenKind::AmpAmp) return "&&";
    if (k == TokenKind::PipePipe) return "||";
    return opSymbol(k);
}
bool reifyBinaryOk(TokenKind k) {
    switch (k) {
        case TokenKind::EqEq: case TokenKind::BangEq: case TokenKind::Lt:
        case TokenKind::Gt:   case TokenKind::Le:     case TokenKind::Ge:
        case TokenKind::AmpAmp: case TokenKind::PipePipe:
        case TokenKind::Plus: case TokenKind::Minus: case TokenKind::Star:
        case TokenKind::Slash: case TokenKind::Percent: return true;
        default: return false;
    }
}
const char* exprKindConstruct(ExprKind k) {
    switch (k) {
        case ExprKind::Await:   return "await";
        case ExprKind::Lambda:  return "nested lambda";
        case ExprKind::Is:      return "'is' expression";
        case ExprKind::Match:   return "'match' expression";
        case ExprKind::Index:   return "indexing";
        case ExprKind::Range:   return "range";
        case ExprKind::Ternary: return "conditional expression";
        default:                return nullptr;   // caller uses "unsupported construct"
    }
}
}  // namespace

// The reifier's per-site state (§3.1): the lambda's Field roots, the ordered
// bind table (canonical spelling -> slot; insertion order = slot order, R5),
// the set-shape flag, and the lambda site span for the E2 note.
struct Checker::ReifyCtx {
    std::vector<std::string> paramNames;
    struct BindEntry { std::string key; ExprPtr capture; bool storeNone; };
    std::vector<BindEntry> binds;
    bool setShaped = false;
    SourceSpan siteSpan;
    // canonical spelling -> slot; a repeat spelling shares the slot (R5).
    int findOrAddSlot(std::string key, ExprPtr capture, bool storeNone) {
        for (size_t i = 0; i < binds.size(); ++i)
            if (binds[i].key == key) return static_cast<int>(i);
        binds.push_back({std::move(key), std::move(capture), storeNone});
        return static_cast<int>(binds.size()) - 1;
    }
};

std::string_view Checker::ownText(std::string s) {
    progMut_->synthNames.push_back(std::move(s));
    return progMut_->synthNames.back();
}

Symbol* Checker::exprClass() {
    if (!exprLookedUp_) {
        exprLookedUp_ = true;
        Symbol* ns = sema_.global ? sema_.global->lookup("expr") : nullptr;
        if (ns && ns->kind == SymbolKind::Namespace && ns->scope) {
            exprNamespace_ = ns;
            if (const std::vector<Symbol*>* v = ns->scope->localLookup("Expr"))
                for (Symbol* s : *v)
                    if (s->kind == SymbolKind::Class) { exprClass_ = s; break; }
        }
    }
    return exprClass_;
}

Symbol* Checker::exprNodeClass(const char* name) {
    if (!exprClass()) return nullptr;            // also fills exprNamespace_
    if (!exprNamespace_ || !exprNamespace_->scope) return nullptr;
    if (const std::vector<Symbol*>* v = exprNamespace_->scope->localLookup(name))
        for (Symbol* s : *v) if (s->kind == SymbolKind::Class) return s;
    return nullptr;
}

// §2.1: an Expr-target is `expr::Expr<Fn>` with a single function-typed generic
// argument. Return Fn's (Function) TypeRef, else null.
const TypeRef* Checker::exprTargetFn(const TypeRef* pt) {
    if (!pt || pt->kind != TypeKind::Named) return nullptr;
    if (pt->generics.size() != 1) return nullptr;
    Symbol* ec = exprClass();
    if (!ec || pt->resolvedSymbol != ec) return nullptr;
    const TypeRef* fn = pt->generics[0].get();
    return (fn && fn->kind == TypeKind::Function) ? fn : nullptr;
}

// The resolved-Type form (overload scoring, §2.2): a Class of `expr::Expr` with
// one FuncRef generic argument.
bool Checker::isExprTargetType(const Type& t) {
    Symbol* ec = exprClass();
    return ec && t.kind == TKind::Class && t.sym == ec && t.args.size() == 1 &&
           t.args[0].kind == TKind::FuncRef;
}

bool Checker::isDbValueType(const Type& t) const {
    auto isBase = [](const std::string& c) {
        return c == "string" || c == "int" || c == "float" || c == "bool";
    };
    if (t.kind == TKind::Union) {
        for (const Type& m : t.unionMembers)
            if (!isBase(m.canonical) && m.canonical != "None") return false;
        return !t.unionMembers.empty();
    }
    return isBase(t.canonical);
}

// A span-carrying deep clone over the leaf grammar reification ever clones:
// Name/This, Member (`.`/`::`) chains, and *Lit leaves (H6). Anything else is
// copied structurally best-effort (never reached by a valid capture/literal).
ExprPtr Checker::cloneForReify(const Expr* e) const {
    if (!e) return nullptr;
    auto c = std::make_unique<Expr>(e->kind);
    c->span = e->span;
    c->text = e->text;
    c->op = e->op;
    c->colon = e->colon;
    c->optChain = e->optChain;
    c->isRawSegment = e->isRawSegment;
    c->isRawString = e->isRawString;
    c->isQuasiPayload = e->isQuasiPayload;
    c->singleQuoted = e->singleQuoted;
    c->charLit = e->charLit;
    c->resolved = e->resolved;
    c->resolvedClass = e->resolvedClass;
    c->a = cloneForReify(e->a.get());
    c->b = cloneForReify(e->b.get());
    c->c = cloneForReify(e->c.get());
    for (const ExprPtr& el : e->list) c->list.push_back(cloneForReify(el.get()));
    return c;
}

ExprPtr Checker::makeExprNode(const char* cls, std::vector<ExprPtr> args,
                              SourceSpan sp) {
    auto call = std::make_unique<Expr>(ExprKind::Call);
    call->span = sp;
    auto callee = std::make_unique<Expr>(ExprKind::Member);
    callee->span = sp; callee->colon = true; callee->text = cls;   // static literal
    auto ns = std::make_unique<Expr>(ExprKind::Name);
    ns->span = sp; ns->text = "expr";
    callee->a = std::move(ns);
    call->a = std::move(callee);
    call->list = std::move(args);
    return call;
}

// A string literal node carrying `content` (op spelling / member name / call
// name) as an ordinary quoted, escape-decodable literal.
static ExprPtr reifyStringLit(std::string_view content, SourceSpan sp,
                              std::deque<std::string>& arena) {
    std::string q = "\"";
    for (char ch : content) {
        if (ch == '\\' || ch == '"') q += '\\';
        q += ch;
    }
    q += "\"";
    arena.push_back(std::move(q));
    auto s = std::make_unique<Expr>(ExprKind::StringLit);
    s->span = sp; s->text = arena.back();
    return s;
}

// Build one `Array<Elem>`-valued argument out of `elems`, where each element's
// static type is a SUBTYPE of Elem: empty -> a raw `[]` literal (assignable to
// any Array); non-empty -> an immediately-invoked closure that appends each
// element into a declared `Array<Elem>` and returns it. A bare non-empty
// literal is invariantly un-assignable here (a uniform `[Lit]` is `Array<Lit>`,
// a uniform `[int]` is `Array<int>`); this is the one landed shape that widens.
// `elemType` is a fully-resolved TypeRef for `Elem`.
namespace {
TypeRefPtr cloneResolvedTypeRef(const TypeRef* t) {
    if (!t) return nullptr;
    auto r = std::make_unique<TypeRef>(t->kind);
    r->span = t->span;
    r->path = t->path;
    r->name = t->name;
    r->canonical = t->canonical;
    r->resolvedSymbol = t->resolvedSymbol;
    for (const TypeRefPtr& g : t->generics) r->generics.push_back(cloneResolvedTypeRef(g.get()));
    for (const TypeRefPtr& m : t->members) r->members.push_back(cloneResolvedTypeRef(m.get()));
    for (const TypeRefPtr& p : t->funcParams) r->funcParams.push_back(cloneResolvedTypeRef(p.get()));
    if (t->funcRet) r->funcRet = cloneResolvedTypeRef(t->funcRet.get());
    return r;
}
}  // namespace

// Build `Array<elemType>` (fully resolved) as a TypeRef for a synthesized decl.
static TypeRefPtr arrayOfTypeRef(TypeRefPtr elemType, Symbol* arraySym,
                                 SourceSpan sp) {
    auto arr = std::make_unique<TypeRef>(TypeKind::Named);
    arr->span = sp; arr->name = "Array";
    arr->resolvedSymbol = arraySym;
    arr->canonical = "Array<" + elemType->canonical + ">";
    arr->generics.push_back(std::move(elemType));
    return arr;
}

// §4 step 3 helper. `arrayTypeRef` is the fully-resolved `Array<Elem>` decl type.
static ExprPtr buildTypedArrayImpl(std::vector<ExprPtr> elems,
                                   TypeRefPtr arrayTypeRef, SourceSpan sp,
                                   std::deque<std::string>& arena) {
    if (elems.empty()) {
        auto arr = std::make_unique<Expr>(ExprKind::Array);   // raw [] — assignable
        arr->span = sp;
        return arr;
    }
    // A plain identifier (no `$`, which the lexer reserves for quasiquote holes)
    // so the printed IIFE re-lexes cleanly on the --expand round-trip. Each IIFE
    // is its own lambda scope, so reusing one name across them never collides.
    arena.push_back("__lvReifyArr");
    std::string_view slot = arena.back();
    auto block = std::make_unique<Stmt>(StmtKind::Block);
    block->span = sp;
    // Array<Elem> $reify$a = [];
    auto decl = std::make_unique<Stmt>(StmtKind::Var);
    decl->span = sp; decl->name = slot;
    decl->type = std::move(arrayTypeRef);
    { auto empty = std::make_unique<Expr>(ExprKind::Array); empty->span = sp;
      decl->init = std::move(empty); }
    block->body.push_back(std::move(decl));
    // $reify$a = $reify$a.add(<elem>);   (one per element)
    for (ExprPtr& el : elems) {
        auto assign = std::make_unique<Expr>(ExprKind::Binary);
        assign->span = sp; assign->op = TokenKind::Eq;
        auto lhs = std::make_unique<Expr>(ExprKind::Name); lhs->span = sp; lhs->text = slot;
        assign->a = std::move(lhs);
        auto call = std::make_unique<Expr>(ExprKind::Call); call->span = sp;
        auto callee = std::make_unique<Expr>(ExprKind::Member);
        callee->span = sp; callee->text = "add";
        auto recv = std::make_unique<Expr>(ExprKind::Name); recv->span = sp; recv->text = slot;
        callee->a = std::move(recv);
        call->a = std::move(callee);
        call->list.push_back(std::move(el));
        assign->b = std::move(call);
        auto st = std::make_unique<Stmt>(StmtKind::ExprStmt);
        st->span = sp; st->expr = std::move(assign);
        block->body.push_back(std::move(st));
    }
    // return $reify$a;
    auto ret = std::make_unique<Stmt>(StmtKind::Return);
    ret->span = sp;
    { auto r = std::make_unique<Expr>(ExprKind::Name); r->span = sp; r->text = slot;
      ret->expr = std::move(r); }
    block->body.push_back(std::move(ret));
    // (() => { <block> })()
    auto lam = std::make_unique<Expr>(ExprKind::Lambda);
    lam->span = sp; lam->block = std::move(block);
    auto iife = std::make_unique<Expr>(ExprKind::Call);
    iife->span = sp; iife->a = std::move(lam);
    (void)arena;
    return iife;
}

ExprPtr Checker::reifyReject(const char* construct, SourceSpan span, ReifyCtx& ctx) {
    sink_.error(span, std::string("cannot reify ") + construct +
                      ": outside the LA-31 reifiable subset");
    sink_.note(span, whitelistAllowLine());
    sink_.note(ctx.siteSpan, "in this lambda reified to expr::Expr<F>");
    return nullptr;
}

// The Array<T>.contains receiver (R17): a captured Array chain -> `Bind(slot)`
// whose binds value is the None marker (the DbValue union cannot carry an
// Array). Any other shape here is a reject.
ExprPtr Checker::reifyArrayReceiver(const Expr* e, ReifyCtx& ctx) {
    std::string key = pathOf(e);
    if (key.empty()) return reifyReject("capture of a non-value type", e->span, ctx);
    // A param-rooted chain cannot be an Array capture (params are Field roots,
    // never Bind); only a captured chain reaches here.
    int slot = ctx.findOrAddSlot(key, cloneForReify(e), /*storeNone=*/true);
    auto n = std::make_unique<Expr>(ExprKind::IntLit);
    n->span = e->span; n->text = ownText(std::to_string(slot));
    return makeExprNode("Bind", [&]{ std::vector<ExprPtr> v; v.push_back(std::move(n)); return v; }(), e->span);
}

ExprPtr Checker::reifyNode(const Expr* e, ReifyCtx& ctx) {
    if (!e) return nullptr;
    SourceSpan sp = e->span;
    auto one = [](ExprPtr x) { std::vector<ExprPtr> v; v.push_back(std::move(x)); return v; };
    switch (e->kind) {
        case ExprKind::IntLit:
        case ExprKind::FloatLit:
        case ExprKind::BoolLit:
            return makeExprNode("Lit", one(cloneForReify(e)), sp);
        case ExprKind::StringLit:
            // A plain literal (incl. the single-segment `isRawSegment` shape the
            // parser emits for any non-interpolated string) reifies as Lit. A
            // genuinely interpolated string is a Binary(+) of segments and a
            // `.toString()` call — that call is rejected by the whitelist below.
            return makeExprNode("Lit", one(cloneForReify(e)), sp);
        case ExprKind::Name: {
            if (e->text == "None") {
                auto none = std::make_unique<Expr>(ExprKind::Name);
                none->span = sp; none->text = "None";
                return makeExprNode("Lit", one(std::move(none)), sp);
            }
            for (const std::string& p : ctx.paramNames)
                if (e->text == p)                        // a bare Field root: the
                    return reifyReject("bare lambda parameter", sp, ctx);   // whole record isn't a value
            // A captured bare local/param-of-enclosing -> Bind.
            Type t = typeOf(e);
            if (!isDbValueType(t)) return reifyReject("capture of a non-value type", sp, ctx);
            int slot = ctx.findOrAddSlot(pathOf(e), cloneForReify(e), false);
            auto n = std::make_unique<Expr>(ExprKind::IntLit);
            n->span = sp; n->text = ownText(std::to_string(slot));
            return makeExprNode("Bind", one(std::move(n)), sp);
        }
        case ExprKind::This:
            return reifyReject("capture of a non-value type", sp, ctx);
        case ExprKind::Member: {
            // Determine chain shape: descend a-links.
            bool anyColon = false;
            const Expr* root = e;
            std::vector<std::string_view> segs;   // outer-most first
            for (const Expr* p = e; p && p->kind == ExprKind::Member; p = p->a.get()) {
                if (p->colon) anyColon = true;
                segs.push_back(p->text);
                root = p->a.get();
            }
            if (anyColon) {
                // Enum member `E::M` -> Lit(<carrier>): resolved to an enum-member
                // const global (Track 03 §2). Match on the stamped decl pointer.
                if (program_ && e->resolved)
                    for (const EnumDesugar& ed : program_->enumDesugars)
                        for (const EnumDesugar::Member& m : ed.members)
                            if (m.global == e->resolved) {
                                auto n = std::make_unique<Expr>(ExprKind::IntLit);
                                n->span = sp; n->text = ownText(std::to_string(m.carrier));
                                return makeExprNode("Lit", one(std::move(n)), sp);
                            }
                return reifyReject("unsupported construct 'qualified member'", sp, ctx);
            }
            std::reverse(segs.begin(), segs.end());   // root -> outer
            bool paramRooted = root && root->kind == ExprKind::Name &&
                [&]{ for (const std::string& p : ctx.paramNames) if (root->text == p) return true; return false; }();
            if (paramRooted) {
                std::vector<ExprPtr> path;
                for (std::string_view s : segs)
                    path.push_back(reifyStringLit(s, sp, progMut_->synthNames));
                auto arr = std::make_unique<Expr>(ExprKind::Array);
                arr->span = sp; arr->list = std::move(path);   // Array<string> — direct
                return makeExprNode("Field", one(std::move(arr)), sp);
            }
            // A captured chain (this.x, cfg.minAge, capturedObj.field) -> Bind.
            Type t = typeOf(e);
            if (!isDbValueType(t)) return reifyReject("capture of a non-value type", sp, ctx);
            int slot = ctx.findOrAddSlot(pathOf(e), cloneForReify(e), false);
            auto n = std::make_unique<Expr>(ExprKind::IntLit);
            n->span = sp; n->text = ownText(std::to_string(slot));
            return makeExprNode("Bind", one(std::move(n)), sp);
        }
        case ExprKind::Unary: {
            if (e->op != TokenKind::Bang && e->op != TokenKind::Minus)
                return reifyReject("unsupported construct 'unary'", sp, ctx);
            ExprPtr inner = reifyNode(e->a.get(), ctx);
            if (!inner) return nullptr;
            std::vector<ExprPtr> args;
            args.push_back(reifyStringLit(reifyOp(e->op), sp, progMut_->synthNames));
            args.push_back(std::move(inner));
            return makeExprNode("Un", std::move(args), sp);
        }
        case ExprKind::Binary: {
            if (e->op == TokenKind::Eq || isCompoundAssign(e->op))
                return reifyReject("assignment outside a set-shaped lambda", sp, ctx);
            if (!reifyBinaryOk(e->op))
                return reifyReject("unsupported construct 'binary'", sp, ctx);
            ExprPtr l = reifyNode(e->a.get(), ctx);
            if (!l) return nullptr;
            ExprPtr r = reifyNode(e->b.get(), ctx);
            if (!r) return nullptr;
            std::vector<ExprPtr> args;
            args.push_back(reifyStringLit(reifyOp(e->op), sp, progMut_->synthNames));
            args.push_back(std::move(l));
            args.push_back(std::move(r));
            return makeExprNode("Bin", std::move(args), sp);
        }
        case ExprKind::Call: {
            const Expr* callee = e->a.get();
            if (!callee || callee->kind != ExprKind::Member || callee->colon)
                return reifyReject("unsupported construct 'call'", sp, ctx);
            std::string_view name = callee->text;
            const Expr* recv = callee->a.get();
            Type rt = typeOf(recv);
            bool isStr = rt.canonical == "string";
            bool isArr = rt.kind == TKind::Class && rt.sym && rt.sym->name == "Array";
            const ExprCallRow* row = nullptr;
            for (const ExprCallRow& r : kExprWhitelist) {
                if (name != r.name) continue;
                if (r.arity != static_cast<int>(e->list.size())) continue;
                if (r.recv == ExprCallRow::Str && isStr) { row = &r; break; }
                if (r.recv == ExprCallRow::ArrayT && isArr) { row = &r; break; }
            }
            if (!row) {
                std::string what = "non-whitelisted call '" + std::string(name) + "'";
                return reifyReject(ownText(what).data(), sp, ctx);
            }
            ExprPtr recvNode = (row->recv == ExprCallRow::ArrayT)
                                   ? reifyArrayReceiver(recv, ctx)
                                   : reifyNode(recv, ctx);
            if (!recvNode) return nullptr;
            std::vector<ExprPtr> argNodes;
            for (const ExprPtr& a : e->list) {
                ExprPtr an = reifyNode(a.get(), ctx);
                if (!an) return nullptr;
                argNodes.push_back(std::move(an));
            }
            // Array<expr::Node> args (widened via the IIFE builder).
            Symbol* nodeSym = exprNodeClass("Node");
            auto nodeElem = std::make_unique<TypeRef>(TypeKind::Named);
            nodeElem->span = sp; nodeElem->name = "Node";
            nodeElem->path = {"expr"};
            nodeElem->resolvedSymbol = nodeSym;
            nodeElem->canonical = "expr::Node";
            TypeRefPtr arrTy = arrayOfTypeRef(std::move(nodeElem),
                                              scope_ ? scope_->lookup("Array") : nullptr, sp);
            ExprPtr argsArr = buildTypedArrayImpl(std::move(argNodes), std::move(arrTy),
                                                  sp, progMut_->synthNames);
            std::vector<ExprPtr> args;
            args.push_back(reifyStringLit(name, sp, progMut_->synthNames));
            args.push_back(std::move(recvNode));
            args.push_back(std::move(argsArr));
            return makeExprNode("Call", std::move(args), sp);
        }
        default: {
            const char* c = exprKindConstruct(e->kind);
            if (c) return reifyReject(c, sp, ctx);
            return reifyReject("unsupported construct", sp, ctx);
        }
    }
}

// §4 step 3: the binds array argument — a DbValue-union Array.
ExprPtr Checker::buildBindsExpr(ReifyCtx& ctx, SourceSpan sp) {
    std::vector<ExprPtr> elems;
    for (ReifyCtx::BindEntry& b : ctx.binds) {
        if (b.storeNone) {                       // Array-typed capture (R17)
            auto none = std::make_unique<Expr>(ExprKind::Name);
            none->span = sp; none->text = "None";
            elems.push_back(std::move(none));
        } else {
            elems.push_back(std::move(b.capture));
        }
    }
    if (elems.empty()) {
        auto arr = std::make_unique<Expr>(ExprKind::Array); arr->span = sp;
        return arr;
    }
    // Array<string|int|float|bool|None> element type, fully resolved.
    auto u = std::make_unique<TypeRef>(TypeKind::Union);
    u->span = sp;
    const char* members[] = {"string", "int", "float", "bool", "None"};
    for (const char* m : members) {
        auto mr = std::make_unique<TypeRef>(TypeKind::Named);
        mr->span = sp; mr->name = m;
        mr->resolvedSymbol = scope_ ? scope_->lookup(m) : nullptr;
        mr->canonical = m;
        u->members.push_back(std::move(mr));
    }
    u->canonical = "string | int | float | bool | None";
    TypeRefPtr arrTy = arrayOfTypeRef(std::move(u),
                                      scope_ ? scope_->lookup("Array") : nullptr, sp);
    return buildTypedArrayImpl(std::move(elems), std::move(arrTy), sp, progMut_->synthNames);
}

// §3/§4: reify `lambda` against `fnRef` (Fn) and rewrite it in place into the
// `expr::Expr(...)` construction. Returns the concrete `expr::Expr<Fn>` type or
// an Error type on a reject (already emitted).
Type Checker::reifyLambda(Expr* lambda, const TypeRef* fnRef,
                           const std::unordered_map<std::string_view, Type>* subst) {
    Symbol* ec = exprClass();
    if (!ec || !fnRef || fnRef->kind != TypeKind::Function || !progMut_)
        return unknown();                        // hook mis-fired; leave as-is
    if (reifiedLambdas_.count(lambda)) return unknown();   // idempotence (R1)

    // Fn param/return types. When this lambda arrives through a generic
    // method's parameter (e.g. `Query<E>.where(expr::Expr<(E)=>bool>)`),
    // `fnRef` is the method's raw, unsubstituted declaration TypeRef — `subst`
    // carries the caller's E->User binding so the lambda parameter and any
    // whitelisted-Call receiver typed through it resolve to the concrete type
    // instead of degrading to unknown() (bug.md #89).
    std::vector<Type> paramTypes;
    for (const TypeRefPtr& p : fnRef->funcParams)
        paramTypes.push_back(subst ? substitute(p.get(), *subst) : fromTypeRef(p.get()));
    Type fnRet = subst ? substitute(fnRef->funcRet.get(), *subst)
                        : fromTypeRef(fnRef->funcRet.get());
    if (lambda->params.size() != paramTypes.size())
        return error(lambda->span,
                     "lambda has " + std::to_string(lambda->params.size()) +
                     " parameter(s) but expr::Expr<F> expects " +
                     std::to_string(paramTypes.size()));

    // Bind params (Field roots) in a fresh body scope — the CLOSURE leg is
    // checked here, and the reify walk reads the same checked types.
    ReifyCtx ctx;
    ctx.siteSpan = lambda->span;
    ctx.setShaped = fnRet.canonical == "int";      // R10
    env_.emplace_back();
    for (size_t i = 0; i < lambda->params.size(); ++i) {
        const Param& p = lambda->params[i];
        Type pt = p.type ? fromTypeRef(p.type.get()) : paramTypes[i];
        ctx.paramNames.push_back(std::string(p.name));
        env_.back()[p.name] = {std::move(pt), p.isConst};
    }
    Type savedRt = returnType_; const TypeRef* savedRtr = returnTypeRef_;
    int savedLoop = loopDepth_;
    auto savedLabelStack = std::move(labelStack_);
    labelStack_.clear();
    std::vector<Type>* savedLr = lambdaReturns_;
    returnType_ = Type{}; returnTypeRef_ = nullptr; loopDepth_ = 0; lambdaReturns_ = nullptr;

    ExprPtr tree;
    bool blockBody = lambda->block != nullptr;
    if (blockBody) {
        reifyReject("block body", lambda->span, ctx);          // R9
    } else if (lambda->a) {
        Type bodyType = typeOf(lambda->a.get());               // closure leg check
        if (bodyType.kind == TKind::Error) {
            // The ordinary checker already rejected the body — no reify diag.
        } else if (ctx.setShaped && lambda->a->kind == ExprKind::Binary &&
                   lambda->a->op == TokenKind::Eq) {
            // R10 set-shape: `u.field = <expr>` as the entire body.
            const Expr* asn = lambda->a.get();
            const Expr* lhs = asn->a.get();
            // LHS must be a param-rooted field chain (-> Field).
            bool anyColon = false; const Expr* root = lhs;
            std::vector<std::string_view> segs;
            for (const Expr* p = lhs; p && p->kind == ExprKind::Member; p = p->a.get()) {
                if (p->colon) anyColon = true;
                segs.push_back(p->text); root = p->a.get();
            }
            bool paramRooted = !anyColon && lhs->kind == ExprKind::Member &&
                root && root->kind == ExprKind::Name &&
                [&]{ for (const std::string& pn : ctx.paramNames) if (root->text == pn) return true; return false; }();
            if (!paramRooted) {
                reifyReject("mutation of a capture", lhs->span, ctx);
            } else {
                std::reverse(segs.begin(), segs.end());
                std::vector<ExprPtr> path;
                for (std::string_view s : segs) path.push_back(reifyStringLit(s, lhs->span, progMut_->synthNames));
                auto arr = std::make_unique<Expr>(ExprKind::Array); arr->span = lhs->span;
                arr->list = std::move(path);
                ExprPtr target = makeExprNode("Field",
                    [&]{ std::vector<ExprPtr> v; v.push_back(std::move(arr)); return v; }(), lhs->span);
                ExprPtr rhs = reifyNode(asn->b.get(), ctx);
                if (rhs) {
                    std::vector<ExprPtr> args;
                    args.push_back(std::move(target));
                    args.push_back(std::move(rhs));
                    tree = makeExprNode("Assign", std::move(args), lambda->span);
                }
            }
        } else {
            tree = reifyNode(lambda->a.get(), ctx);
        }
    }

    returnType_ = savedRt; returnTypeRef_ = savedRtr;
    loopDepth_ = savedLoop; labelStack_ = std::move(savedLabelStack);
    lambdaReturns_ = savedLr;
    env_.pop_back();

    if (!tree) return Type{TKind::Error, nullptr, "", {}, nullptr, {}};

    int siteId = exprSiteCounter_++;             // R4
    SourceSpan sp = lambda->span;

    // §4: move the Lambda's fields into a fresh node (construction argument 1),
    // then rewrite the ORIGINAL node in place into the construction Call.
    auto lamNode = std::make_unique<Expr>(ExprKind::Lambda);
    lamNode->span = sp;
    lamNode->params = std::move(lambda->params);
    lamNode->a = std::move(lambda->a);
    lamNode->block = std::move(lambda->block);
    reifiedLambdas_.insert(lamNode.get());       // R1 idempotence guard

    ExprPtr bindsArr = buildBindsExpr(ctx, sp);
    auto siteLit = std::make_unique<Expr>(ExprKind::IntLit);
    siteLit->span = sp; siteLit->text = ownText(std::to_string(siteId));

    // Resolve the sub-node argument trees (stamps every construction's
    // resolved/resolvedClass for the engines); arg1's body was resolved above.
    typeOf(tree.get());
    typeOf(bindsArr.get());

    // Find the 4-arg Expr constructor for the outer construction's stamp.
    const Stmt* ctor = nullptr;
    if (ec->decl)
        for (const StmtPtr& m : ec->decl->body)
            if (m->kind == StmtKind::Member && m->isCtor && m->params.size() == 4) { ctor = m.get(); break; }

    // Rewrite `lambda` in place -> expr::Expr(<lambda>, <tree>, <binds>, <siteId>).
    auto callee = std::make_unique<Expr>(ExprKind::Member);
    callee->span = sp; callee->colon = true; callee->text = "Expr";
    { auto ns = std::make_unique<Expr>(ExprKind::Name); ns->span = sp; ns->text = "expr";
      callee->a = std::move(ns); }
    lambda->kind = ExprKind::Call;
    lambda->text = std::string_view();
    lambda->op = TokenKind::End;
    lambda->colon = false;
    lambda->params.clear();
    lambda->block = nullptr;
    lambda->a = std::move(callee);
    lambda->list.clear();
    lambda->list.push_back(std::move(lamNode));
    lambda->list.push_back(std::move(tree));
    lambda->list.push_back(std::move(bindsArr));
    lambda->list.push_back(std::move(siteLit));
    lambda->resolved = ctor;
    lambda->resolvedClass = ec;
    lambda->argsNormalized = true;

    // The concrete site type: expr::Expr<Fn>. Unsubstituted call sites keep the
    // exact prior behavior (fnRef's own canonical/FuncRef conversion); a
    // substituted call site rebuilds both from the resolved param/return types
    // so a generic-parameter site reports e.g. expr::Expr<(User)=>bool>, not
    // expr::Expr<(E)=>bool> (bug.md #89).
    if (subst) {
        std::string paramCanon;
        for (size_t i = 0; i < paramTypes.size(); ++i) {
            if (i) paramCanon += ", ";
            paramCanon += paramTypes[i].canonical;
        }
        std::string fnCanon = "(" + paramCanon + ") => " + fnRet.canonical;
        Type r{TKind::Class, ec, "expr::Expr<" + fnCanon + ">", {}, nullptr, {}};
        Type fnType{TKind::FuncRef, nullptr, fnCanon, {}, nullptr, {}};
        fnType.ret = std::make_shared<Type>(fnRet);
        r.args.push_back(std::move(fnType));
        return r;
    }
    Type r{TKind::Class, ec, "expr::Expr<" + std::string(fnRef->canonical) + ">",
           {}, nullptr, {}};
    r.args.push_back(fromTypeRef(fnRef));
    return r;
}

// The return Type of a (possibly generic) method/function call: bind class
// params from the receiver, method params from the arguments, then substitute.
// Lambda arguments run LAST (when `call`/`lambdaWalked` are given): value
// arguments have already bound what they can (T from the receiver, A from a
// seed), so each lambda's parameter types are known; its body then infers the
// return type, which binds the remaining vars (U in `map<U>((T) => U fn)`).
Type Checker::genericReturn(Symbol* cls, const Stmt* fn, const Type& receiver,
                            const std::vector<Type>& args,
                            const Expr* call, std::vector<char>* lambdaWalked) {
    std::unordered_map<std::string_view, Type> subst;
    if (cls && cls->decl)
        for (size_t i = 0; i < cls->decl->generics.size() && i < receiver.args.size(); ++i)
            subst[cls->decl->generics[i]] = receiver.args[i];
    for (size_t i = 0; i < fn->params.size() && i < args.size(); ++i)
        unify(fn->params[i].type.get(), args[i], subst);
    if (call && lambdaWalked) {
        for (size_t i = 0; i < fn->params.size() && i < call->list.size(); ++i) {
            Expr* a = call->list[i].get();
            if ((*lambdaWalked)[i]) continue;
            const TypeRef* pt = fn->params[i].type.get();
            // LA-25 §2.2/§6 (M3): an overloaded method reference argument
            // couldn't type without this call's chosen overload — resolve it
            // now against the (possibly generic-substituted) parameter type,
            // the same deferral point a Lambda argument already rides.
            if (a->kind == ExprKind::Member || a->kind == ExprKind::Name) {
                // bug #55: a bare free-function name argument is a callable
                // reference too, resolved against this parameter's function type.
                Type expected = substitute(pt, subst);
                bool isRef = false;
                tryResolveMethodRef(a, &expected, isRef);
                if (isRef) (*lambdaWalked)[i] = 1;
                continue;
            }
            if (a->kind != ExprKind::Lambda) continue;
            // LA-31 §2.2: an `expr::Expr<Fn>` parameter reifies the lambda in
            // place (this is the call-argument arrival path; the body is checked
            // once, here, with Fn's param types — the same lambda-last deferral).
            if (const TypeRef* fn = exprTargetFn(pt)) {
                reifyLambda(a, fn, &subst);
                (*lambdaWalked)[i] = 1;
                continue;
            }
            std::vector<Type> ptypes;
            if (pt && pt->kind == TypeKind::Function)
                for (const TypeRefPtr& fp : pt->funcParams)
                    ptypes.push_back(substitute(fp.get(), subst));
            Type ret = checkLambdaBody(a, ptypes);
            if (pt && pt->kind == TypeKind::Function && pt->funcRet)
                unify(pt->funcRet.get(), ret, subst);
            (*lambdaWalked)[i] = 1;
        }
    }
    recordSpecialization(fn, subst, call);
    return substitute(fn->type.get(), subst);
}

bool Checker::callableTypeParam(std::string_view name) const {
    if (!curMember_) return false;
    for (std::string_view p : curMember_->generics) if (p == name) return true;
    return false;
}

bool Checker::classTypeParam(std::string_view name) const {
    if (!thisClass_ || !thisClass_->decl) return false;
    for (std::string_view p : thisClass_->decl->generics) if (p == name) return true;
    return false;
}

void Checker::recordSpecialization(
    const Stmt* fn, const std::unordered_map<std::string_view, Type>& subst,
    const Expr* call) {
    if (!fn || !fn->specializationRequired || !call) return;

    std::vector<Type> tuple;
    bool abstract = false;
    for (std::string_view p : fn->generics) {
        auto it = subst.find(p);
        if (it == subst.end() || it->second.kind == TKind::Unknown ||
            it->second.kind == TKind::TypeValue || it->second.canonical.empty()) {
            abstract = true;
            break;
        }
        tuple.push_back(it->second);
    }
    if (abstract) {
        // Transitive case: `outer<A>` calls specialization-required `inner`
        // while A is still abstract. Specializing outer makes the inner tuple
        // concrete when the clone is checked.
        if (curMember_ && !curMember_->generics.empty()) {
            curMember_->specializationRequired = true;
            return;
        }
        error(call->span, "cannot determine a concrete type tuple for generic '" +
              std::string(fn->name) + "' at this call site");
        return;
    }
    int depth = activeSpecialization_ ? activeSpecializationDepth_ + 1 : 1;
    if (depth > 32) {
        error(call->span, "generic static-member specialization exceeded the v1 depth "
              "limit (32); this instantiation graph may be unbounded");
        return;
    }
    SpecializationDemand d;
    d.generic = const_cast<Stmt*>(fn); d.call = const_cast<Expr*>(call);
    d.tuple = std::move(tuple); d.scope = callableScopes_[fn];
    d.cls = callableClasses_[fn]; d.instantiationSpan = call->span; d.depth = depth;
    specializationDemands_.push_back(std::move(d));
}

Symbol* Checker::specializeValueStruct(Symbol* generic, const std::vector<Type>& args) {
    // Only user value structs with type parameters are candidates. Interfaces,
    // primitives, reference classes, and the native collections are never columnar.
    if (!generic || !generic->isValue || generic->isPrimitive || !generic->decl ||
        generic->isInterface())
        return nullptr;
    const std::vector<std::string_view>& params = generic->decl->generics;
    if (params.empty() || params.size() != args.size()) return nullptr;
    if (!generic->shape.built) return nullptr;

    // Memoize on (generic, arg-canonical tuple) so every eligible `Pair<int,int>`
    // resolves to ONE symbol (the §5 keyEquals identity invariant). Ineligible
    // instantiations cache a null so we don't re-test them.
    std::string key = std::to_string(reinterpret_cast<uintptr_t>(generic));
    for (const Type& a : args) key += "\x1f" + a.canonical;
    auto found = valueStructSpecs_.find(key);
    if (found != valueStructSpecs_.end()) return found->second;

    std::unordered_map<std::string_view, std::string> sub;
    for (size_t i = 0; i < params.size(); ++i) sub[params[i]] = args[i].canonical;

    // Build the substituted shape. Every non-method field slot must substitute to
    // a plain columnar scalar (int/float/bool/char) — that is exactly the
    // columnarEligibleStruct line, evaluated against the monomorphized shape. A
    // field whose type is a nested/heap type (e.g. `Array<A>`, or a param bound to
    // `string`) leaves the instantiation ineligible: keep the generic symbol.
    Shape shape;
    bool anyField = false;
    for (const Slot& s : generic->shape.slots) {
        Slot out = s;
        if (!s.isMethod) {
            std::string canon = s.canonical;
            auto it = sub.find(s.canonical);   // a field typed directly `A` (whole spelling)
            if (it != sub.end()) canon = it->second;
            if (columnarTypecodeOf(canon) == 0) { valueStructSpecs_[key] = nullptr; return nullptr; }
            out.canonical = canon;
            anyField = true;
        }
        shape.slots.push_back(std::move(out));
    }
    if (!anyField) { valueStructSpecs_[key] = nullptr; return nullptr; }

    std::string name = "$spec." + std::string(generic->name) + "<";
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) name += ", ";
        name += args[i].canonical;
    }
    name += ">";
    Symbol* spec = sema_.newLateSymbol(SymbolKind::Class, sema_.intern(name), generic->decl);
    spec->isValue = true;
    spec->isPrimitive = false;
    spec->scope = generic->scope;   // shared decl → same method/base lookups
    shape.built = true;
    spec->shape = std::move(shape);
    valueStructSpecs_[key] = spec;
    return spec;
}

void Checker::genericStaticMissing(const Expr* site, Symbol* concrete,
                                   std::string_view member, bool constructor) {
    std::string param = site && !site->genericStaticParam.empty()
        ? std::string(site->genericStaticParam) : "type parameter";
    const Stmt* origin = activeSpecialization_ && activeSpecialization_->specializationOf
        ? activeSpecialization_->specializationOf : activeSpecialization_;
    std::string callable = origin ? std::string(origin->name) : "generic callable";
    if (origin && !origin->generics.empty()) {
        callable += "<";
        for (size_t i = 0; i < origin->generics.size(); ++i) {
            if (i) callable += ", ";
            callable += origin->generics[i];
        }
        callable += ">";
    }
    std::string kind = constructor ? "labeled constructor" : "member";
    sink_.error(site ? site->span : SourceSpan{},
                "type '" + std::string(concrete ? concrete->name : "<unknown>") +
                "' (instantiating '" + param + "' of '" + callable +
                "') has no " + kind + " '" + std::string(member) + "'");
    if (activeSpecialization_)
        sink_.note(activeSpecialization_->instantiationSpan,
                   "generic static-member specialization was instantiated here");
}

// bug #54: overload-set sibling of genericStaticMissing. Fired when an
// overloaded free/namespace call inside a *specialized* generic body has no
// applicable overload for the concrete instantiation (e.g. `enc(item)` where
// the concrete `T` has no matching `enc`). Names the concrete argument types
// and the originating generic, and adds the instantiation-site second span.
void Checker::specializationOverloadMissing(const Expr* call, const char* what,
                                            const std::vector<Type>& args) {
    const Stmt* origin = activeSpecialization_ && activeSpecialization_->specializationOf
        ? activeSpecialization_->specializationOf : activeSpecialization_;
    std::string callable = origin ? std::string(origin->name) : "generic callable";
    if (origin && !origin->generics.empty()) {
        callable += "<";
        for (size_t i = 0; i < origin->generics.size(); ++i) {
            if (i) callable += ", ";
            callable += origin->generics[i];
        }
        callable += ">";
    }
    std::string argList;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) argList += ", ";
        argList += args[i].canonical.empty() ? "?" : args[i].canonical;
    }
    sink_.error(call ? call->span : SourceSpan{},
                std::string("no overload of '") + what +
                "' matches the arguments (" + argList +
                ") in specialization of '" + callable + "'");
    if (activeSpecialization_)
        sink_.note(activeSpecialization_->instantiationSpan,
                   "generic specialization was instantiated here");
}

Type Checker::typeInitExpr(const Expr* e, const TypeRef* expected) {
    // LA-31 §2.3: arrival path 2 — an `expr::Expr<Fn>` expected-type position
    // (local/global bind, `return`, class field initializer, R12). A lambda
    // literal reifies; a non-literal lambda-typed value is E1 (R16). Ordered
    // before construction/method-ref handling so a `C::m` reference falls to E1
    // naturally (it is not a lambda literal).
    if (e) if (const TypeRef* fn = exprTargetFn(expected)) {
        if (e->kind == ExprKind::Lambda)
            return reifyLambda(const_cast<Expr*>(e), fn);
        Type vt = typeOf(e);
        if (vt.kind == TKind::FuncRef)
            return error(e->span,
                         "only a lambda literal can be reified to expr::Expr<F>");
        // Otherwise fall through — an ordinary type-mismatch is reported by the
        // caller against `expected`.
    }
    // Track 03 §1: a single-scalar single-quoted literal at a `char`-expected
    // site (declared type on a var/field, a `char` return, or a `char?` union)
    // re-types to char.
    if (expectsChar(expected) && isCharLiteral(e)) {
        markCharLiteral(e);
        return primType("char");
    }
    // F3: a declared Array<(...)=>R> gives each callable-reference element
    // the target type needed to select an overloaded bound/unbound method.
    // The ordinary Array typer then sees the rewritten lambdas.
    if (e && e->kind == ExprKind::Array && expected &&
        expected->kind == TypeKind::Named && expected->name == "Array" &&
        expected->generics.size() == 1) {
        Type elementExpected = fromTypeRef(expected->generics[0].get());
        for (const ExprPtr& element : e->list) {
            if (element->kind != ExprKind::Member) continue;
            bool isRef = false;
            tryResolveMethodRef(element.get(), &elementExpected, isRef);
        }
    }
    // A construction at an initializer/return site: pick the ctor by args and
    // use the target type to fill any type args not fixed by the arguments.
    if (e && e->kind == ExprKind::Call) {
        const Expr* callee = e->a.get();
        Symbol* ctorClass = nullptr; std::string_view label;
        if (callee->kind == ExprKind::Name) {
            // Same alias-vs-own-name distinction as typeOfCall's construction
            // path above.
            if (Symbol* hs = hygienicClass(callee)) {
                ctorClass = hs; label = hs->name;
            } else if (Symbol* s = scope_ ? scope_->lookup(callee->text) : nullptr)
                if (s->kind == SymbolKind::Class) { ctorClass = s; label = s->name; }
        } else if (callee->kind == ExprKind::Member) {
            Type bt = typeOf(callee->a.get());
            // Track 03 §2: `Enum::fromCode(...)` is a free-function call, not
            // construction — leave ctorClass null so it falls through to
            // typeOf -> typeOfCallInner where the fromCode rule resolves it.
            bool enumFromCode = false;
            if (callee->colon && callee->text == "fromCode" &&
                bt.kind == TKind::TypeValue && bt.sym && program_)
                for (const EnumDesugar& ed : program_->enumDesugars)
                    if (ed.name == bt.sym->name) { enumFromCode = true; break; }
            // struct-equality design §8: `float::fromBits(...)` is a free-
            // function call (math::floatFromBits), NOT construction — same
            // shape as Enum::fromCode above. Leave ctorClass null so it falls
            // through to typeOf -> typeOfCallInner where the routing resolves it.
            if (callee->colon && callee->text == "fromBits" &&
                bt.kind == TKind::TypeValue && bt.sym && bt.sym->isPrimitive &&
                bt.sym->name == "float")
                enumFromCode = true;
            if (!enumFromCode && bt.kind == TKind::TypeValue) {
                ctorClass = bt.sym; label = callee->text;
            } else if (!enumFromCode && callee->a->kind == ExprKind::Name) {
                if (Symbol* cls = hygienicClass(callee->a.get())) {
                    ctorClass = cls; label = callee->text;
                } else if (Symbol* cls = visibleClass(callee->a->text)) {
                    ctorClass = cls; label = callee->text;
                }
            }
        }
        if (ctorClass) {
            // LA-25 §2.2/§6 (M3): defer an OVERLOADED method-reference argument
            // here too (this is the var-decl/return/field-init construction
            // path, a separate arg-typing pass from typeOfCallInner's) — typed
            // unknown for overload choice, then walked below against the
            // chosen constructor's declared parameter type, same as a Lambda.
            auto cands = ctorOverloads(ctorClass, label);
            // bug.md #43: thread the constructor parameter's declared type into
            // each argument as its TARGET type, so a nested generic construction
            // (`Foo(1, Map())`) infers its own type args from the parameter it
            // fills instead of erroring for lack of a target. ctorOverloads does
            // not consult argument types, so a UNIQUE constructor's parameters are
            // known before the arguments are typed; with overloads the target is
            // ambiguous, so fall back to bare typing (existing behavior).
            const Stmt* soleCtor = cands.size() == 1 ? cands.front() : nullptr;
            std::vector<Type> argTypes;
            for (size_t i = 0; i < e->list.size(); ++i) {
                const Expr* a = e->list[i].get();
                if (isDeferredMethodRefArg(a)) { argTypes.push_back(unknown()); continue; }
                if (soleCtor && i < soleCtor->params.size())
                    argTypes.push_back(typeInitExpr(a, soleCtor->params[i].type.get()));
                else
                    argTypes.push_back(typeOf(a));
            }
            if (cands.empty() && activeSpecialization_ && callee->colon &&
                (callee->genericStaticSite ||
                 (callee->a && callee->a->genericStaticSite))) {
                genericStaticMissing(callee, ctorClass, label, true);
                return Type{TKind::Error, nullptr, "", {}, nullptr, {}};
            }
            bool ok = false, diagnosed = false;
            // Constructors use the same named/default/injection binding and
            // positional normalization path as every other call.
            const Stmt* ctor = pickInjecting(cands, argTypes, const_cast<Expr*>(e),
                                             ok, diagnosed);
            if (!cands.empty() && !ok && !diagnosed)
                error(e->span, "no constructor matches the arguments");
            const_cast<Expr*>(e)->resolved = ctor;
            const_cast<Expr*>(e)->resolvedClass = ctorClass;
            if (ctor)
                for (size_t i = 0; i < ctor->params.size() && i < e->list.size(); ++i) {
                    Expr* a = e->list[i].get();
                    // bug #55: a bare free-function name constructor argument is a
                    // callable reference too, not only a `T::m`/`obj.m` member.
                    if (a->kind != ExprKind::Member && a->kind != ExprKind::Name) continue;
                    Type et = fromTypeRef(ctor->params[i].type.get());
                    bool isRef = false;
                    tryResolveMethodRef(a, &et, isRef);
                }
            return inferConstruction(ctorClass, ctor, argTypes, expected, e->span);
        }
    }
    // LA-25 §2.2: the declared-type slot's type is the target for an
    // overloaded method-reference initializer (var-decl, field init, return).
    if (e && e->kind == ExprKind::Member) {
        bool isRef = false;
        Type expectedType = expected ? fromTypeRef(expected) : unknown();
        Type t = tryResolveMethodRef(const_cast<Expr*>(e), expected ? &expectedType : nullptr,
                                     isRef);
        if (isRef) return t;
    }
    return typeOf(e);
}

// ---------------------------------------------------------------------------
//  statement checking
// ---------------------------------------------------------------------------

// techdesign-labeled-break-continue.md F3.
void Checker::pushLoopLabel(Stmt* s) {
    if (s->label.empty()) return;
    for (const LabelEntry& e : labelStack_)
        if (e.name == s->label) {
            error(s->span, "label '" + std::string(s->label) +
                  "' is already used by an enclosing loop");
            break;
        }
    labelStack_.push_back({s->label, s});
}

void Checker::popLoopLabel(Stmt* s) {
    if (s->label.empty()) return;
    labelStack_.pop_back();
}

void Checker::bindLoopLabel(Stmt* s) {
    const char* what = s->kind == StmtKind::Break ? "break" : "continue";
    if (s->label.empty()) {
        if (loopDepth_ == 0) error(s->span, std::string("'") + what + "' outside a loop");
        return;
    }
    for (auto it = labelStack_.rbegin(); it != labelStack_.rend(); ++it)
        if (it->name == s->label) { s->labelTarget = it->stmt; return; }
    error(s->span, "no enclosing loop is labeled '" + std::string(s->label) + "'");
}

void Checker::check(Stmt* s) {
    if (!s) return;
    // techdesign-02 F3: `using` is legal only as a direct statement of a block.
    // Read-then-clear so a nested check() (If branches, loop bodies, etc.)
    // never inherits an outer block's permission.
    bool usingOk = usingOkHere_;
    usingOkHere_ = false;
    switch (s->kind) {
        case StmtKind::Block: {
            // bug.md #8 / block-scoped-use §3.2: a block-level `use`/`uses`/`bind`
            // is visible for exactly this block. The Resolver put its imports and
            // binds in `s->blockScope`; one RAII guard swaps `scope_` to it for
            // names, pushes the locals frame, and pushes the bind frame — the four
            // hand-paired ops this replaces stay in lockstep by construction.
            BlockScopeGuard guard(*this, s);
            for (StmtPtr& c : s->body) { usingOkHere_ = true; check(c.get()); }
            break;
        }
        case StmtKind::Var: {
            invalidatePath(std::string(s->name));
            // OQ1: a bare `const T x;` LOCAL with a declared type is no longer a
            // hard error — its write view stays open until the flow engine proves
            // a single definite assignment (constPending_). Two things still
            // error: an inferred `const x;` (no type, no init — nothing to infer),
            // and any bare const outside a function body (globals/fields keep the
            // "needs an initializer" rule, enforced on their own decl paths).
            bool writeOpenConst = false;
            if (s->isConst && !s->init) {
                // A `using` decl is `const` too (Parser), but it manages a
                // block-scoped resource — a deferred definite assignment makes no
                // sense for it, so it keeps requiring an initializer outright.
                if (s->inferred || env_.empty() || s->isUsing)
                    error(s->span, "const '" + std::string(s->name) +
                          "' needs an initializer (its write view is the declaration, "
                          "once)");
                else
                    writeOpenConst = true;
            }
            Type initT = s->init
                ? typeInitExpr(s->init.get(), s->inferred ? nullptr : s->type.get())
                : unknown();
            Type declared = s->inferred ? initT : fromTypeRef(s->type.get());
            if (!s->inferred && s->init && !assignable(initT, declared))
                error(s->init->span, "cannot initialize '" + std::string(s->name) +
                      " : " + declared.canonical + "' with '" + initT.canonical + "'");
            if (s->isUsing) {
                if (!usingOk)
                    error(s->span, "a 'using' declaration must be a direct "
                          "statement of a block");
                Symbol* disposable = scope_ ? scope_->lookup("IDisposable") : nullptr;
                if (!disposable || declared.kind != TKind::Class || !declared.sym ||
                    declared.sym->isValue || !isSubclass(declared.sym, disposable))
                    error(s->span, "v1: 'using' requires a reference type "
                          "implementing IDisposable (got '" + declared.canonical + "')");
                else {
                    std::vector<const Stmt*> cands = methodOverloads(declared.sym, "close");
                    bool anyApplicable = false;
                    const Stmt* chosen = pickOverload(cands, {}, anyApplicable);
                    if (!chosen)
                        error(s->span, "'" + declared.canonical + "' has no zero-arg 'close()'");
                    s->usingClose = chosen;
                }
            }
            if (!env_.empty()) env_.back()[s->name] = {declared, s->isConst};
            if (writeOpenConst)
                constPending_[std::string(s->name)] = {loopDepth_, tryDepth_};
            break;
        }
        case StmtKind::Break:
            bindLoopLabel(s);
            break;
        case StmtKind::Continue:
            bindLoopLabel(s);
            break;
        case StmtKind::DoWhile: {
            pushLoopLabel(s);
            ++loopDepth_;
            check(s->thenBranch.get());
            --loopDepth_;
            popLoopLabel(s);
            typeOf(s->expr.get());
            break;
        }
        case StmtKind::Return:
            if (s->expr) {
                Type t = typeInitExpr(s->expr.get(), returnTypeRef_);
                if (lambdaReturns_) lambdaReturns_->push_back(t);
                if (returnType_.kind != TKind::Unknown && !assignable(t, returnType_))
                    error(s->expr->span, "cannot return '" + t.canonical + "' as '" +
                          returnType_.canonical + "'");
            }
            break;
        case StmtKind::If: {
            typeOf(s->expr.get());
            std::vector<Fact> facts;
            analyzeCond(s->expr.get(), facts, false);
            std::unordered_map<std::string, Type> saved;
            applyFacts(facts, true, saved);
            // OQ1: definite assignment across the branch. Each arm starts from
            // the pre-`if` open set; a const is definitely assigned after the
            // `if` iff it closed on BOTH arms, i.e. the post-join open set is the
            // UNION of the two arms' still-open sets. A missing `else` arm assigns
            // nothing, so `pendingBefore` stands in for it — nothing closes.
            std::unordered_map<std::string, PendingConst> pendingBefore = constPending_;
            check(s->thenBranch.get());
            std::unordered_map<std::string, PendingConst> pendingThen = constPending_;
            narrow_ = saved;
            constPending_ = pendingBefore;
            applyFacts(facts, false, saved);
            check(s->elseBranch.get());
            for (const auto& kv : pendingThen) constPending_.insert(kv);
            narrow_ = saved;
            break;
        }
        case StmtKind::While: {
            typeOf(s->expr.get());
            std::vector<Fact> facts;
            analyzeCond(s->expr.get(), facts, false);
            std::unordered_map<std::string, Type> saved;
            applyFacts(facts, true, saved);
            pushLoopLabel(s);
            ++loopDepth_;
            check(s->thenBranch.get());
            --loopDepth_;
            popLoopLabel(s);
            narrow_ = saved;
            break;
        }
        case StmtKind::For:
            env_.emplace_back();                       // scope the init variable
            check(s->forInit.get());
            if (s->expr) typeOf(s->expr.get());
            if (s->forStep) typeOf(s->forStep.get());
            pushLoopLabel(s);
            ++loopDepth_;
            check(s->thenBranch.get());
            --loopDepth_;
            popLoopLabel(s);
            exitEnvScope();
            break;
        case StmtKind::ForIn: {
            Type iter = typeOf(s->expr.get());
            Type elem = unknown();
            bool builtin = false;
            if (iter.kind == TKind::Class && iter.sym) {
                if (iter.sym->name == "Range") { elem = primType("int"); builtin = true; }
                else if (iter.sym->name == "Array" && !iter.args.empty()) { elem = iter.args[0]; builtin = true; }
                else if (iter.sym->name == "Map") {         // entries iterate as Pair<K,V>
                    if (Symbol* p = scope_ ? scope_->lookup("Pair") : nullptr) elem = classType(p);
                    builtin = true;
                }
            }
            // techdesign-07 §2: the iterator protocol. Dispatch order (contract
            // C5): the built-in fast paths above win; only when they don't apply
            // does a type implementing `IIterable<E>` iterate via `e.iterator()`
            // then `hasNext()`/`next()`. The element type is E from that
            // interface's own `iterator()` return (`IIterator<E>`); record the
            // chosen path on the Stmt so Eval/Lower never re-derive it.
            if (!builtin && iter.kind == TKind::Class && iter.sym) {
                Symbol* iterable = scope_ ? scope_->lookup("IIterable") : nullptr;
                if (iterable && isSubclass(iter.sym, iterable)) {
                    const Stmt* itMethod = nullptr;
                    for (const Stmt* c : methodOverloads(iter.sym, "iterator"))
                        if (c && c->params.empty()) { itMethod = c; break; }
                    if (itMethod) {
                        Type itType = genericReturn(iter.sym, itMethod, iter, {});
                        if (!itType.args.empty()) elem = itType.args[0];
                        s->forInProtocol = true;
                    }
                } else if (iter.sym->shape.built && !iter.sym->isInterface()) {
                    // A concrete, fully-resolved type that is neither a built-in
                    // collection nor an IIterable: name the protocol rather than
                    // silently looping zero times.
                    error(s->expr->span, "'" + iter.canonical + "' is not iterable — it "
                          "implements neither a built-in collection nor IIterable<T> "
                          "(add an 'iterator()' method returning IIterator<T>)");
                }
            }
            Type bound = s->inferred ? elem : fromTypeRef(s->type.get());
            env_.emplace_back();
            env_.back()[s->name] = {bound, s->isConst};
            pushLoopLabel(s);
            ++loopDepth_;
            check(s->thenBranch.get());
            --loopDepth_;
            popLoopLabel(s);
            exitEnvScope();
            break;
        }
        case StmtKind::ExprStmt:
            typeOf(s->expr.get());
            break;
        case StmtKind::Throw: {
            Type t = typeOf(s->expr.get());
            Symbol* iexc = scope_ ? scope_->lookup("IException") : nullptr;
            if (iexc && t.kind == TKind::Class && t.sym && !isSubclass(t.sym, iexc))
                error(s->expr->span, "thrown value must implement IException (got '" +
                      t.canonical + "')");
            break;
        }
        case StmtKind::Try:
            // OQ1 §2.3: a `try` body cannot host a const's single definite
            // assignment — a throw may precede the write, so the `catch`/post-
            // `try` join treats the try arm as NOT definitely-assigning. Marking
            // the body's try-depth makes an in-`try` write to an outer write-open
            // const an error (typeOfBinary), keeping the window from spanning it.
            ++tryDepth_;
            check(s->thenBranch.get());
            --tryDepth_;
            for (const CatchClause& c : s->catches) {
                env_.emplace_back();
                if (!c.name.empty()) env_.back()[c.name] = fromTypeRef(c.type.get());
                check(c.body.get());
                exitEnvScope();
            }
            break;
        case StmtKind::Bind: {
            // §12.5: a factory body is checked against the BOUND type, not the
            // enclosing function's return type (fixing the "cannot return X as
            // void" misfire). Registration happened in the Resolver's fillBinds.
            // techdesign-02 F1: a factory body is its own loop-nesting scope,
            // same as any other function body.
            int savedLoopDepth = loopDepth_;
            loopDepth_ = 0;
            auto savedLabelStack = std::move(labelStack_);
            labelStack_.clear();
            if (s->memberBody && s->type) {
                Type savedRt = returnType_;
                const TypeRef* savedRtr = returnTypeRef_;
                returnType_ = fromTypeRef(s->type.get());
                returnTypeRef_ = s->type.get();
                check(s->memberBody.get());
                returnType_ = savedRt;
                returnTypeRef_ = savedRtr;
            } else if (s->memberBody) {
                check(s->memberBody.get());
            }
            loopDepth_ = savedLoopDepth;
            labelStack_ = std::move(savedLabelStack);
            if (s->init) typeOf(s->init.get());
            break;
        }
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
//  walking
// ---------------------------------------------------------------------------

// Is `lhs` a write to a field of the current `this` (a bare field name that is
// not a local, or an explicit `this.field`)? Used for the `mutating` check.
bool Checker::writesThisField(Expr* lhs) {
    if (!thisClass_) return false;
    std::string_view name;
    if (lhs->kind == ExprKind::Name && !localLookup(lhs->text)) name = lhs->text;
    else if (lhs->kind == ExprKind::Member && lhs->a->kind == ExprKind::This) name = lhs->text;
    else return false;
    for (const Slot* s : slotsNamed(thisClass_->shape, name))
        if (!s->isMethod) return true;
    return false;
}
std::string Checker::fieldNameOf(Expr* lhs) {
    return std::string(lhs->kind == ExprKind::Member ? lhs->text : lhs->text);
}

// const.md: classify `e` (an assignment LHS, or a mutating-method-call
// receiver — both are "a write to whatever `e` names") as a write to a const
// slot currently outside its initialization window. Returns the slot's name
// for the diagnostic, or empty if the write is fine.
//
// Per slot kind (const.md §3): locals/params/for-in vars and namespace/
// top-level globals get their write view for the declaration's own
// initializer/binding ONLY — that happens inline in the Var/ForIn/param
// setup, never through this path, so any assignment reaching here targeting
// one is unconditionally outside the window. Fields additionally get the
// declaring class's own constructor bodies (subsumes Bug 7's namespace-global
// ad hoc ban, generalized).
BlockedWrite Checker::constBlockedWrite(Expr* e) {
    if (!e) return {};

    // Field write: bare name (not a local) or `this.name` — the same shape
    // the value-struct `mutating` check (writesThisField) already detects.
    if (thisClass_ && writesThisField(e)) {
        std::string_view name = e->text;
        for (const Slot* s : slotsNamed(thisClass_->shape, name)) {
            if (s->isMethod) continue;
            if (s->isConst)
                // techdesign-readonly §4.2: a const field's ONLY legal write is
                // its initializer — even a ctor assignment is now blocked.
                return {std::string(name), BlockedWriteKind::Const, true};
            if (s->isReadonly) {
                bool inWindow = curMember_ && curMember_->isCtor && s->source == thisClass_;
                if (!inWindow) return {std::string(name), BlockedWriteKind::Readonly, true};
            }
        }
        return {};
    }

    if (e->kind == ExprKind::Name) {
        if (LocalBinding* b = localBinding(e->text))
            return b->isConst ? BlockedWrite{std::string(e->text), BlockedWriteKind::Const}
                               : BlockedWrite{};
        // Not a local and not a this-field (checked above): an unqualified
        // global (top-level, or a namespace member reached from inside its
        // own namespace body).
        if (Symbol* sym = scope_ ? scope_->lookup(e->text) : nullptr)
            if (sym->kind == SymbolKind::Var && sym->decl && sym->decl->isConst)
                return {std::string(e->text), BlockedWriteKind::Const};
        return {};
    }

    if (e->kind == ExprKind::Member) {
        // `NS::name`
        if (e->a->kind == ExprKind::Name && scope_) {
            if (Symbol* ns = scope_->lookup(e->a->text);
                ns && ns->kind == SymbolKind::Namespace && ns->scope) {
                if (const std::vector<Symbol*>* v = ns->scope->localLookup(e->text))
                    for (Symbol* s : *v)
                        if (s->kind == SymbolKind::Var && s->decl && s->decl->isConst)
                            return {std::string(e->text), BlockedWriteKind::Const};
                return {};
            }
        }
        // `expr.field` through an arbitrary (non-`this`) receiver: a const OR
        // readonly field is never writable from outside the declaring class's
        // own ctor on `this` (C# readonly semantics) — not even from that same
        // class's own ctor acting on some OTHER instance, so this is always
        // blocked (there's no "window" reachable through a foreign receiver).
        Type bt = typeOf(e->a.get());
        if (bt.kind == TKind::Class && bt.sym)
            for (const Slot* s : slotsNamed(bt.sym->shape, e->text))
                if (!s->isMethod && (s->isConst || s->isReadonly))
                    return {std::string(e->text),
                            s->isConst ? BlockedWriteKind::Const : BlockedWriteKind::Readonly, true};
        return {};
    }

    if (e->kind == ExprKind::Index) {
        // Indexer sugar on a PURE array/map is a rebind of the base slot
        // (reference §3.6): `a[i] = v` on `const Array<T> a` writes `a`
        // itself. On a mutable object with a `([])` set accessor, the write
        // stays inside the object and never touches the slot.
        Type bt = typeOf(e->a.get());
        bool pureColl = bt.kind == TKind::Class && bt.sym &&
                        (bt.sym->name == "Array" || bt.sym->name == "Map");
        return pureColl ? constBlockedWrite(e->a.get()) : BlockedWrite{};
    }

    return {};
}

static bool defaultHasParamRef(const Expr* e, const std::vector<Param>& params) {
    if (!e) return false;
    if (e->kind == ExprKind::This) return true;
    if (e->kind == ExprKind::Name)
        for (const Param& p : params)
            if (e->text == p.name) return true;
    if (defaultHasParamRef(e->a.get(), params) ||
        defaultHasParamRef(e->b.get(), params) ||
        defaultHasParamRef(e->c.get(), params)) return true;
    for (const ExprPtr& x : e->list)
        if (defaultHasParamRef(x.get(), params)) return true;
    return false;
}

static bool isFoldedDefault(const Expr* e) {
    if (!e) return false;
    switch (e->kind) {
        case ExprKind::IntLit: case ExprKind::FloatLit:
        case ExprKind::StringLit: case ExprKind::BoolLit:
            return true;
        case ExprKind::Name:
            return e->text == "None";
        case ExprKind::Array:
            for (const ExprPtr& x : e->list)
                if (!isFoldedDefault(x.get())) return false;
            return true;
        default:
            return false;
    }
}

void Checker::markSpecializationSites(Program& program) {
    auto hasName = [](const std::vector<std::string_view>& xs, std::string_view n) {
        for (std::string_view x : xs) if (x == n) return true;
        return false;
    };
    std::function<bool(const TypeRef*, std::string_view)> hktUse =
        [&](const TypeRef* t, std::string_view p) -> bool {
            if (!t) return false;
            if (t->kind == TypeKind::Named) {
                if (t->name == p && !t->generics.empty()) return true;
                for (const TypeRefPtr& g : t->generics) if (hktUse(g.get(), p)) return true;
            } else if (t->kind == TypeKind::Union) {
                for (const TypeRefPtr& m : t->members) if (hktUse(m.get(), p)) return true;
            } else if (t->kind == TypeKind::Function) {
                for (const TypeRefPtr& a : t->funcParams) if (hktUse(a.get(), p)) return true;
                return hktUse(t->funcRet.get(), p);
            }
            return false;
        };

    // bug #54: does a declared type mention (syntactically, by spelling) any of
    // `gens` — the enclosing callable's type parameters? Recurses into Named
    // generics (`Array<T>`), Union members (`T?`), and function-type parts, the
    // same shapes `mentionsTypeParam` handles but keyed on the still-unresolved
    // parameter NAME (this pre-pass predates full type-param symbol binding, so
    // it stays syntactic exactly like the `hasName`/`hktUse` checks above).
    std::function<bool(const TypeRef*, const std::vector<std::string_view>&)> mentionsGen =
        [&](const TypeRef* t, const std::vector<std::string_view>& gens) -> bool {
            if (!t) return false;
            if (t->kind == TypeKind::Named) {
                for (std::string_view g : gens) if (t->name == g) return true;
                for (const TypeRefPtr& g : t->generics) if (mentionsGen(g.get(), gens)) return true;
            } else if (t->kind == TypeKind::Union) {
                for (const TypeRefPtr& m : t->members) if (mentionsGen(m.get(), gens)) return true;
            } else if (t->kind == TypeKind::Function) {
                for (const TypeRefPtr& a : t->funcParams) if (mentionsGen(a.get(), gens)) return true;
                return mentionsGen(t->funcRet.get(), gens);
            }
            return false;
        };

    // bug #54: enumerate the free/namespace function overload candidates a call
    // callee resolves to, from `scope` (the callable's lexical scope, recorded
    // in callableScopes_). Bare Name → the scope chain; `NS::fn`/`A::B::fn` →
    // the resolved namespace's own scope. Only used to decide "is this callee an
    // overload set (2+ arity-matching candidates)"; conservative and syntactic —
    // false positives cost only an extra (harmless) specialization.
    auto overloadCandidates = [&](const Expr* callee, Scope* scope) -> std::vector<const Stmt*> {
        std::vector<const Stmt*> out;
        if (!callee || !scope) return out;
        if (callee->kind == ExprKind::Name) {
            for (const Scope* sc = scope; sc; sc = sc->parent)
                if (const std::vector<Symbol*>* v = sc->localLookup(callee->text))
                    for (Symbol* s : *v)
                        if (s->kind == SymbolKind::Function && s->decl) out.push_back(s->decl);
        } else if (callee->kind == ExprKind::Member && callee->colon && callee->a &&
                   (callee->a->kind == ExprKind::Name ||
                    (callee->a->kind == ExprKind::Member && callee->a->colon))) {
            Symbol* ns = nsChainSym(scope, callee->a.get());
            if (ns && ns->kind == SymbolKind::Namespace && ns->scope)
                if (const std::vector<Symbol*>* v = ns->scope->localLookup(callee->text))
                    for (Symbol* s : *v)
                        if (s->kind == SymbolKind::Function && s->decl) out.push_back(s->decl);
        }
        return out;
    };

    // bug #54: the declared types of the enclosing callable's params + its
    // body-local `Var`/`for-in` bindings, so a Name argument in a call can be
    // typed back to `T`/`Array<T>`/… without a Scope. Rebuilt per callable.
    std::unordered_map<std::string_view, const TypeRef*> curLocals;
    std::function<void(Stmt*)> collectLocals = [&](Stmt* s) {
        if (!s) return;
        if ((s->kind == StmtKind::Var || s->kind == StmtKind::ForIn) &&
            !s->inferred && s->type && !s->name.empty())
            curLocals[s->name] = s->type.get();
        collectLocals(s->memberBody.get());
        for (StmtPtr& x : s->body) collectLocals(x.get());
        collectLocals(s->thenBranch.get()); collectLocals(s->elseBranch.get());
        collectLocals(s->forInit.get());
        for (CatchClause& c : s->catches) collectLocals(c.body.get());
    };
    auto buildLocals = [&](Stmt* fn) {
        curLocals.clear();
        for (const Param& p : fn->params)
            if (p.type && !p.name.empty()) curLocals[p.name] = p.type.get();
        collectLocals(fn->memberBody.get());
    };

    std::function<void(Expr*, Stmt*, Symbol*)> scanExpr;
    std::function<void(Stmt*, Stmt*, Symbol*)> scanStmt;
    scanExpr = [&](Expr* e, Stmt* fn, Symbol* cls) {
        if (!e) return;
        // bug #54: a call to an overloaded free/namespace function passing a
        // type-parameter-typed argument must resolve per concrete instantiation.
        // Widen LA-18's specialization demand to this shape so it rides the same
        // above-IR pipeline (mark → collect tuple → clone → re-resolve). Fires
        // only for FUNCTION-level type params (LA-18 v1 scope), matching the
        // `hasName(fn->generics, …)` distinction the `::` trigger below uses.
        if (e->kind == ExprKind::Call && fn && !fn->generics.empty() &&
            !fn->specializationRequired && e->a) {
            std::vector<const Stmt*> cands =
                overloadCandidates(e->a.get(), callableScopes_.count(fn) ? callableScopes_[fn] : nullptr);
            int arityMatch = 0;
            for (const Stmt* c : cands) if (c->params.size() == e->list.size()) ++arityMatch;
            if (arityMatch >= 2) {                    // a genuine overload set
                for (const ExprPtr& arg : e->list) {
                    if (!arg || arg->kind != ExprKind::Name) continue;
                    auto it = curLocals.find(arg->text);
                    if (it != curLocals.end() && mentionsGen(it->second, fn->generics)) {
                        fn->specializationRequired = true;
                        break;
                    }
                }
            }
        }
        if (e->kind == ExprKind::Member && e->colon && e->a &&
            e->a->kind == ExprKind::Name && fn) {
            std::string_view p = e->a->text;
            bool callable = hasName(fn->generics, p);
            bool classLevel = cls && cls->decl && hasName(cls->decl->generics, p);
            if (callable || classLevel) {
                e->genericStaticSite = true; e->genericStaticParam = p;
                e->a->genericStaticSite = true; e->a->genericStaticParam = p;
                if (callable) {
                    fn->specializationRequired = true;
                    bool hkt = hktUse(fn->type.get(), p);
                    for (const Param& a : fn->params) hkt = hkt || hktUse(a.type.get(), p);
                    if (hkt)
                        error(e->span, "'::' on type-constructor parameter '" +
                              std::string(p) + "' is not supported (v1)");
                }
            }
        }
        scanExpr(e->a.get(), fn, cls); scanExpr(e->b.get(), fn, cls);
        scanExpr(e->c.get(), fn, cls);
        for (ExprPtr& x : e->list) scanExpr(x.get(), fn, cls);
        for (Param& p : e->params) scanExpr(p.defaultValue.get(), fn, cls);
        scanStmt(e->block.get(), fn, cls);
        for (MatchArm& a : e->arms) {
            scanExpr(a.value.get(), fn, cls); scanExpr(a.bodyExpr.get(), fn, cls);
            scanStmt(a.bodyBlock.get(), fn, cls);
        }
    };
    scanStmt = [&](Stmt* s, Stmt* fn, Symbol* cls) {
        if (!s) return;
        scanExpr(s->init.get(), fn, cls); scanExpr(s->expr.get(), fn, cls);
        scanExpr(s->forStep.get(), fn, cls);
        scanStmt(s->memberBody.get(), fn, cls);
        for (StmtPtr& x : s->body) scanStmt(x.get(), fn, cls);
        scanStmt(s->thenBranch.get(), fn, cls); scanStmt(s->elseBranch.get(), fn, cls);
        scanStmt(s->forInit.get(), fn, cls);
        for (CatchClause& c : s->catches) scanStmt(c.body.get(), fn, cls);
    };

    std::function<void(std::vector<StmtPtr>&, Scope*)> visit =
        [&](std::vector<StmtPtr>& items, Scope* scope) {
            bool top = scope == sema_.global;
            for (StmtPtr& p : items) {
                Stmt* s = p.get();
                Scope* lex = top ? sema_.fileScopeFor(s->span.offset) : scope;
                if (s->kind == StmtKind::Namespace) {
                    Symbol* ns = scope->lookup(s->name);
                    if (ns && ns->scope) visit(s->body, ns->scope);
                } else if (s->kind == StmtKind::Class) {
                    Symbol* cls = nullptr;
                    if (const std::vector<Symbol*>* v = scope->localLookup(s->name))
                        for (Symbol* q : *v)
                            if (q->kind == SymbolKind::Class && q->decl == s) { cls = q; break; }
                    Scope* cs = cls && cls->scope ? cls->scope : lex;
                    for (StmtPtr& m : s->body) if (m->memberBody) {
                        callableScopes_[m.get()] = cs; callableClasses_[m.get()] = cls;
                        buildLocals(m.get());                       // bug #54: arg-type lookup
                        scanStmt(m->memberBody.get(), m.get(), cls);
                    }
                } else if (s->kind == StmtKind::Member && s->memberBody) {
                    callableScopes_[s] = lex; callableClasses_[s] = nullptr;
                    buildLocals(s);                                 // bug #54: arg-type lookup
                    scanStmt(s->memberBody.get(), s, nullptr);
                }
            }
        };
    visit(program.items, sema_.global);
}

void Checker::materializeSpecializations(Program& program) {
    for (size_t i = 0; i < specializationDemands_.size(); ++i) {
        SpecializationDemand d = specializationDemands_[i];
        std::string key = std::to_string(reinterpret_cast<uintptr_t>(d.generic));
        std::string tupleText;
        for (const Type& t : d.tuple) {
            key += "\x1f" + t.canonical;
            if (!tupleText.empty()) tupleText += ", ";
            tupleText += t.canonical;
        }
        auto found = specializationsByKey_.find(key);
        if (found != specializationsByKey_.end()) {
            d.call->resolved = found->second;
            continue;
        }

        std::unordered_map<std::string_view, Type> subst;
        for (size_t n = 0; n < d.generic->generics.size() && n < d.tuple.size(); ++n)
            subst[d.generic->generics[n]] = d.tuple[n];
        SpecializationCloner cloner{program, subst};
        StmtPtr owned = cloner.stmt(d.generic);
        Stmt* spec = owned.get();
        program.synthNames.push_back("$spec." + std::string(d.generic->name) +
                                     "<" + tupleText + ">");
        spec->name = program.synthNames.back();
        spec->generics.clear();
        spec->specializationRequired = false;
        spec->isSpecialization = true;
        spec->specializationOf = d.generic;
        spec->specializationClass = d.cls;
        spec->instantiationSpan = d.instantiationSpan;
        callableScopes_[spec] = d.scope;
        callableClasses_[spec] = d.cls;
        specializationsByKey_[key] = spec;
        d.call->resolved = spec;
        program.specializationReport.push_back(std::string(d.generic->name) + "<" +
                                                tupleText + "> -> " +
                                                std::string(spec->name));
        program.specializations.push_back(std::move(owned));

        Stmt* saved = activeSpecialization_;
        int savedDepth = activeSpecializationDepth_;
        auto savedBinds = lexical_.frames;
        auto bit = callableBindScopes_.find(d.generic);
        if (bit != callableBindScopes_.end()) lexical_.frames = bit->second;
        activeSpecialization_ = spec; activeSpecializationDepth_ = d.depth;
        checkFunction(spec, d.scope ? d.scope : sema_.global, d.cls);
        activeSpecialization_ = saved; activeSpecializationDepth_ = savedDepth;
        lexical_.frames = std::move(savedBinds);
    }
}

// techdesign-readonly §4.2/§8 OQ-1: `const` field initializer grammar. v1
// (conservative, widen on demand): literals/None/arrays-of-those (isFoldedDefault,
// P-2), plus references to other `const`/`comptime` values, plus arithmetic/
// bitwise operators (unary/binary) over constant operands — covers every known
// Atlantis constant (P-4: `const int FLAG = 0x01;`, `const int BOTH = A | B;`).
bool Checker::isCompileTimeConstant(const Expr* e) {
    if (!e) return false;
    if (isFoldedDefault(e)) return true;
    switch (e->kind) {
        case ExprKind::Name: {
            if (e->text == "None") return true;
            if (Symbol* sym = scope_ ? scope_->lookup(e->text) : nullptr)
                if (sym->kind == SymbolKind::Var && sym->decl &&
                    (sym->decl->isConst || sym->decl->isComptime))
                    return true;
            if (thisClass_)
                for (const Slot* s : slotsNamed(thisClass_->shape, e->text))
                    if (!s->isMethod && s->isConst) return true;
            return false;
        }
        case ExprKind::Member: {
            // `NS::name` referencing another const/comptime namespace global,
            // or `Class::name` referencing another const CLASS field.
            if (e->a && e->a->kind == ExprKind::Name && scope_) {
                if (Symbol* ns = scope_->lookup(e->a->text)) {
                    if (ns->kind == SymbolKind::Namespace && ns->scope) {
                        if (const std::vector<Symbol*>* v = ns->scope->localLookup(e->text))
                            for (Symbol* s : *v)
                                if (s->kind == SymbolKind::Var && s->decl &&
                                    (s->decl->isConst || s->decl->isComptime))
                                    return true;
                    } else if (ns->kind == SymbolKind::Class) {
                        for (const Slot* s : slotsNamed(ns->shape, e->text))
                            if (!s->isMethod && s->isConst) return true;
                    }
                }
            }
            return false;
        }
        case ExprKind::Unary:
            return isConstFoldableUnaryOp(e->op) && isCompileTimeConstant(e->a.get());
        case ExprKind::Binary:
            return isConstFoldableOp(e->op) &&
                   isCompileTimeConstant(e->a.get()) && isCompileTimeConstant(e->b.get());
        case ExprKind::Array:
            for (const ExprPtr& x : e->list)
                if (!isCompileTimeConstant(x.get())) return false;
            return true;
        default:
            return false;
    }
}

void Checker::checkFunction(Stmt* fn, Scope* scope, Symbol* thisClass) {
    callableScopes_[fn] = scope;
    callableClasses_[fn] = thisClass;
    callableBindScopes_[fn] = lexical_.frames;
    if (fn->specializationRequired && !fn->isSpecialization && thisClass) {
        buildOverrideIndex();
        bool participates = isOverriddenBelow(thisClass, fn);
        std::string ownCanonical;
        for (const Slot& s : thisClass->shape.slots)
            if (s.decl == fn) { ownCanonical = s.canonical; break; }
        walkProperAncestors(thisClass, [&](const Symbol* a) {
            for (const Slot& s : a->shape.slots)
                if (s.isMethod && s.name == fn->name && s.canonical == ownCanonical)
                    participates = true;
        });
        if (participates)
            error(fn->span, "generic static-member specialization for instance method '" +
                  std::string(fn->name) +
                  "' is not supported when the method overrides or is overridden (v1)");
    }
    scope_ = scope;
    thisClass_ = thisClass;
    curMember_ = fn;
    returnType_ = fromTypeRef(fn->type.get());
    returnTypeRef_ = fn->type.get();
    loopDepth_ = 0;   // techdesign-02 F1: a fresh function body is its own loop nesting
    labelStack_.clear();
    env_.emplace_back();
    for (const Param& p : fn->params) {
        if (!p.defaultValue) continue;
        if (mentionsTypeParam(p.type.get())) {
            error(p.span, "default parameter '" + std::string(p.name) +
                  "' cannot have a type-variable type");
            continue;
        }
        if (defaultHasParamRef(p.defaultValue.get(), fn->params)) {
            error(p.defaultValue->span, "default parameter '" +
                  std::string(p.name) +
                  "' cannot reference another parameter or 'this'");
            continue;
        }
        if (!isFoldedDefault(p.defaultValue.get())) {
            error(p.defaultValue->span,
                  "a default value must be a compile-time constant");
            continue;
        }
        Type dt = typeInitExpr(p.defaultValue.get(), p.type.get());
        Type pt = fromTypeRef(p.type.get());
        if (!assignable(dt, pt))
            error(p.defaultValue->span, "default value for parameter '" +
                  std::string(p.name) + "' is not assignable to '" +
                  pt.canonical + "'");
    }
    for (const Param& p : fn->params)
        env_.back()[p.name] = {fromTypeRef(p.type.get()), p.isConst};
    check(fn->memberBody.get());
    exitEnvScope();
    thisClass_ = nullptr;
    curMember_ = nullptr;
}

void Checker::walk(std::vector<StmtPtr>& items, Scope* scope) {
    // At the top level, each item resolves names through its file's import
    // overlay (bug.md #8) — the same per-file scope the Resolver used, so a
    // top-of-file `uses` is visible in that file only.
    bool top = (scope == sema_.global);
    for (StmtPtr& item : items) {
        Stmt* s = item.get();
        Scope* lex = top ? sema_.fileScopeFor(s->span.offset) : scope;
        switch (s->kind) {
            case StmtKind::Namespace: {
                Symbol* ns = scope->lookup(s->name);
                if (ns && ns->scope) {
                    pushLexicalScope(ns->scope, s->body);  // §12.5: binds scoped to this namespace body
                    walk(s->body, ns->scope);
                    popLexicalScope();
                }
                break;
            }
            case StmtKind::Bind:                 // check the factory body (registration
                if (s->memberBody && s->type) {  // already happened in the Resolver)
                    scope_ = lex;
                    returnType_ = fromTypeRef(s->type.get());
                    returnTypeRef_ = s->type.get();
                    loopDepth_ = 0;   // techdesign-02 F1: fresh function body
                    labelStack_.clear();
                    check(s->memberBody.get());
                    returnType_ = Type{}; returnTypeRef_ = nullptr;
                }
                break;
            case StmtKind::Class: {
                // Find the class where it is DECLARED (`scope`, not the import
                // overlay) so an imported same-named class can't shadow it. Its
                // scope was re-parented to the file overlay by the Resolver, so
                // `classScope` still reaches the file's imports for the body.
                Symbol* sym = scope->lookup(s->name);
                Scope* classScope = sym && sym->scope ? sym->scope : lex;
                for (StmtPtr& m : s->body) {
                    if (m->memberBody) {
                        checkFunction(m.get(), classScope, sym);
                    } else if (m->init) {              // field initializer
                        scope_ = classScope; thisClass_ = sym;
                        env_.emplace_back();
                        Type ft = fromTypeRef(m->type.get());
                        Type it = typeInitExpr(m->init.get(), m->type.get());
                        if (!assignable(it, ft))
                            error(m->init->span, "cannot initialize field '" +
                                  std::string(m->name) + " : " + ft.canonical +
                                  "' with '" + it.canonical + "'");
                        exitEnvScope();
                        thisClass_ = nullptr;
                    }
                }
                // techdesign-readonly §4.2/§4.5: `const`/`readonly` field
                // constraints — a `const` field must be a named compile-time
                // constant (no ctor write is possible either; that's enforced
                // by constBlockedWrite above, since a const field's write view
                // is now ALWAYS empty); a field can't be both modifiers.
                for (const StmtPtr& f : s->body) {
                    if (f->kind != StmtKind::Member || f->isCtor || f->isGet ||
                        f->isSet || f->callable)
                        continue;
                    if (f->isConst && f->isReadonly) {
                        error(f->span, "a field cannot be both `const` and `readonly`");
                        continue;
                    }
                    if (f->isWeak) {
                        if (s->isValue)
                            error(f->span, "weak fields are allowed on classes only, not value structs");
                        if (f->isConst)
                            error(f->span, "weak const is meaningless; use weak readonly or weak");
                        bool valid = false;
                        if (f->type && f->type->kind == TypeKind::Union &&
                            f->type->members.size() == 2) {
                            const TypeRef* target = nullptr;
                            bool none = false;
                            for (const TypeRefPtr& m : f->type->members) {
                                if (m->canonical == "None") none = true;
                                else target = m.get();
                            }
                            Symbol* ts = target ? target->resolvedSymbol : nullptr;
                            valid = none && ts && ts->kind == SymbolKind::Class &&
                                    !ts->isPrimitive && !ts->isValue &&
                                    ts->name != "Array" && ts->name != "Map" &&
                                    ts->name != "Block";
                        }
                        if (!valid)
                            error(f->span, "weak field must have optional class/interface type");
                    }
                    if (f->isConst) {
                        scope_ = classScope; thisClass_ = sym;
                        bool ok = f->init && isCompileTimeConstant(f->init.get());
                        thisClass_ = nullptr;
                        if (!ok)
                            error(f->span, "const field '" + std::string(f->name) +
                                  "' needs a compile-time-constant initializer; use "
                                  "`readonly` for a field assigned during construction");
                    }
                }
                // techdesign-readonly §4.4: readonly definite assignment +
                // write-once, v1 top-level-statements-only (sound, not
                // complete — §8 OQ-2). Walked per declaring class, over that
                // class's OWN field/ctor decls only (§4.4's "no extra logic
                // for base/derived": constBlockedWrite already confines a
                // readonly field's write window to its declaring class).
                {
                    std::vector<const Stmt*> ctors;
                    for (const StmtPtr& m : s->body)
                        if (m->kind == StmtKind::Member && m->isCtor) ctors.push_back(m.get());
                    for (const StmtPtr& f : s->body) {
                        if (f->kind != StmtKind::Member || f->isCtor || f->isGet ||
                            f->isSet || f->callable || !f->isReadonly)
                            continue;
                        std::string_view fname = f->name;
                        if (f->init) {
                            // Initializer form: the initializer IS the single write;
                            // a ctor assigning it too would be a second write.
                            for (const Stmt* ctor : ctors)
                                for (Stmt* st : topLevelStmts(ctor->memberBody.get()))
                                    if (st->kind == StmtKind::ExprStmt &&
                                        exprAssignsField(st->expr.get(), fname))
                                        error(st->span, "readonly field '" + std::string(fname) +
                                              "' already has an initializer; a constructor "
                                              "may not assign it again (write-once)");
                        } else if (ctors.empty()) {
                            error(f->span, "readonly field '" + std::string(fname) +
                                  "' is never assigned; give it an initializer or a "
                                  "constructor that assigns it");
                        } else {
                            for (const Stmt* ctor : ctors) {
                                int count = 0;
                                for (Stmt* st : topLevelStmts(ctor->memberBody.get()))
                                    if (st->kind == StmtKind::ExprStmt &&
                                        exprAssignsField(st->expr.get(), fname))
                                        ++count;
                                if (count == 0)
                                    error(ctor->span, "readonly field '" + std::string(fname) +
                                          "' is not assigned in constructor '" +
                                          std::string(ctor->name) + "' — every constructor "
                                          "must assign it exactly once");
                                else if (count > 1)
                                    error(ctor->span, "readonly field '" + std::string(fname) +
                                          "' is assigned more than once in constructor '" +
                                          std::string(ctor->name) + "' (write-once)");
                            }
                        }
                    }
                }
                // const.md §4 / techdesign-readonly §4.5: a `set` accessor is a
                // write view over its same-named backing slot; declaring one
                // over a const OR readonly field is a write view that would
                // outlive the write window either way.
                for (const StmtPtr& m : s->body) {
                    if (!m->isSet) continue;
                    for (const StmtPtr& f : s->body)
                        if (f->kind == StmtKind::Member && !f->isCtor && !f->isGet &&
                            !f->isSet && !f->callable && (f->isConst || f->isReadonly) &&
                            f->name == m->name)
                            error(m->span, "cannot declare a `set` accessor over " +
                                  std::string(f->isReadonly ? "readonly" : "const") +
                                  " field '" + std::string(m->name) + "'");
                }
                break;
            }
            case StmtKind::Member:                     // free function
                if (s->memberBody) checkFunction(s, lex, nullptr);
                break;
            case StmtKind::Var: {
                scope_ = lex;
                if (s->isConst && !s->init)
                    error(s->span, "const '" + std::string(s->name) +
                          "' needs an initializer (its write view is the declaration, "
                          "once)");
                if (s->init) {
                    // Same target-typed construction inference as the
                    // function-body Var case (check()'s StmtKind::Var, above)
                    // — a bare top-level `Box<int> p = Box();` was going
                    // through typeOf() with no expected type at all, so a
                    // generic class ctor that doesn't type-bear its args
                    // (e.g. any zero-arg constructor) could never infer its
                    // type arguments here even though the identical
                    // declaration inside a function works fine.
                    Type it = typeInitExpr(s->init.get(), s->inferred ? nullptr : s->type.get());
                    Type dt = s->inferred ? it : fromTypeRef(s->type.get());
                    if (!s->inferred && !assignable(it, dt))
                        error(s->init->span, "cannot initialize '" + std::string(s->name) +
                              "' with '" + it.canonical + "'");
                }
                break;
            }
            case StmtKind::ExprStmt:
                scope_ = lex;
                typeOf(s->expr.get());
                break;
            default:
                break;
        }
    }
}

void Checker::run(Program& program, const Program* prelude) {
    program_ = &program;            // Track 03 §2: enum metadata for `::` typing
    progMut_ = &program;            // LA-31: reified nodes' text lives in the AST arena
    markSpecializationSites(program);
    buildNamespaceBindIndex(prelude, program);   // system-binds.md §5.2
    pushLexicalScope(sema_.global, program.items);   // §12.5: file/top-level binds propagate to the body
    walk(program.items, sema_.global);
    popLexicalScope();
    materializeSpecializations(program);
}

// Metaprog Phase 4 §8: type a single comptime-root expression in isolation.
// Fresh walk state, `scope` as the lexical scope, no locals/this/loops — the
// caller guarantees a scope-complete position (root or namespace level).
void Checker::checkComptimeRoot(const Expr* e, Scope* scope) {
    scope_ = scope;
    thisClass_ = nullptr;
    curMember_ = nullptr;
    returnType_ = Type{};
    returnTypeRef_ = nullptr;
    loopDepth_ = 0;
    labelStack_.clear();
    lambdaReturns_ = nullptr;
    env_.clear();
    narrow_.clear();
    constPending_.clear();
    tryDepth_ = 0;
    comptimeRoot_ = true;   // item Q: `target::` is live in comptime roots
    typeOf(e);
    comptimeRoot_ = false;
}
