#include "sema/CheckerInternal.hpp"
#include <algorithm>
#include <functional>
#include <iterator>

namespace {


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
    if (e->op != TokenKind::Eq && !checkerDetail::isCompoundAssign(e->op)) return false;
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


// Rebuild a syntax TypeRef from a semantic type when a synthesized expression
// needs to carry the concrete substituted spelling (notably an Inject node).
static TypeRefPtr typeRefFromType(const Type& t, SourceSpan span) {
    if (t.kind == TKind::Union) {
        auto r = std::make_unique<TypeRef>(TypeKind::Union);
        r->span = span; r->canonical = t.canonical;
        for (const Type& m : t.unionMembers)
            r->members.push_back(typeRefFromType(m, span));
        return r;
    }
    if (t.kind == TKind::FuncRef) {
        // Type currently retains a function's return but not its parameter
        // tuple. This fallback is used only when no authored TypeRef survives;
        // ordinary explicit bindings are cloned structurally below.
        auto r = std::make_unique<TypeRef>(TypeKind::Function);
        r->span = span; r->canonical = t.canonical;
        if (t.ret) r->funcRet = typeRefFromType(*t.ret, span);
        return r;
    }
    auto r = std::make_unique<TypeRef>(TypeKind::Named);
    r->span = span; r->canonical = t.canonical; r->resolvedSymbol = t.sym;
    if (t.sym) r->name = t.sym->name;
    else {
        // `canonical` is owned by the TypeRef, so this view remains stable.
        r->name = r->canonical;
    }
    for (const Type& a : t.args) r->generics.push_back(typeRefFromType(a, span));
    return r;
}

}  // namespace

// refactor_1 session 03: helpers whose call sites land in more than one
// split translation unit. Each is defined exactly once, here, per the
// split's helper-placement rule (no static may be duplicated); every
// cross-TU call site is qualified checkerDetail::.
namespace checkerDetail {


Type unknown() { return Type{TKind::Unknown, nullptr, "", {}, nullptr, {}}; }

Type primitive(std::string c) { return Type{TKind::Primitive, nullptr, std::move(c), {}, nullptr, {}}; }

Type classType(Symbol* s) { return Type{TKind::Class, s, std::string(s->name), {}, nullptr, {}}; }


// bug #37/#46: resolve a namespace qualifier that may be a NESTED chain
// (`A::B::…`) to its innermost namespace Symbol. The root is looked up through
// the ordinary scope chain (honoring `uses`/file overlays); each further
// segment descends into that namespace's own scope.
Symbol* nsChainSym(Scope* scope, const Expr* e) {
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


bool isCompoundAssign(TokenKind k) {
    switch (k) {
        case TokenKind::PlusEq: case TokenKind::MinusEq: case TokenKind::StarEq:
        case TokenKind::SlashEq: case TokenKind::PercentEq: return true;
        default: return false;
    }
}


// Slots in a shape with a given name.
std::vector<const Slot*> slotsNamed(const Shape& sh, std::string_view name) {
    std::vector<const Slot*> out;
    for (const Slot& s : sh.slots)
        if (s.name == name) out.push_back(&s);
    return out;
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


// Structural TypeRef substitution for synthesized AST. Unlike semantic
// substitute(), this preserves function/union syntax and printer fidelity.
TypeRefPtr copyTypeRefWithSubst(
    const TypeRef* t,
    const std::unordered_map<std::string_view, Type>& subst) {
    if (!t) return nullptr;
    if (t->kind == TypeKind::Named && t->resolvedSymbol &&
        t->resolvedSymbol->kind == SymbolKind::TypeParam && t->generics.empty()) {
        auto it = subst.find(t->name);
        if (it != subst.end() && it->second.kind != TKind::Unknown)
            return typeRefFromType(it->second, t->span);
    }
    auto r = std::make_unique<TypeRef>(t->kind);
    r->span = t->span; r->path = t->path; r->name = t->name;
    r->canonical = t->canonical; r->resolvedSymbol = t->resolvedSymbol;
    for (const TypeRefPtr& g : t->generics)
        r->generics.push_back(copyTypeRefWithSubst(g.get(), subst));
    for (const TypeRefPtr& m : t->members)
        r->members.push_back(copyTypeRefWithSubst(m.get(), subst));
    for (const TypeRefPtr& p : t->funcParams)
        r->funcParams.push_back(copyTypeRefWithSubst(p.get(), subst));
    r->funcRet = copyTypeRefWithSubst(t->funcRet.get(), subst);
    return r;
}

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


// ---------------------------------------------------------------------------
//  flow narrowing
// ---------------------------------------------------------------------------

// Types embedded in EXPRESSIONS (`x is T`) aren't walked by the resolver;
// resolve them here, in place, against the current scope.
void resolveExprType(TypeRef* t, Scope* scope) {
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


// ---------------------------------------------------------------------------
//  expression typing
// ---------------------------------------------------------------------------

// Track 03 §1: is `e` a single-quoted, single-scalar string literal? Such a
// literal re-types to `char` at a char-expected site (declared type, char
// comparison, char match arm). Double-quoted / multi-scalar / empty never.
bool isCharLiteral(const Expr* e) {
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
void markCharLiteral(const Expr* e) {
    Expr* x = const_cast<Expr*>(e);
    x->charLit = true;
    x->evalKind = 0;
}

}  // namespace checkerDetail


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
    return (s && s->kind == SymbolKind::Class) ? checkerDetail::classType(s) : checkerDetail::primitive(name);
}


Type Checker::fromTypeRef(const TypeRef* t) const {
    if (!t) return checkerDetail::unknown();
    switch (t->kind) {
        case TypeKind::Inferred:
            return checkerDetail::unknown();
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
            if (s && s->kind == SymbolKind::Primitive) return checkerDetail::primitive(t->canonical);
            if (s && s->kind == SymbolKind::Class) {
                Type r{TKind::Class, s, t->canonical, {}, nullptr, {}};
                for (const TypeRefPtr& g : t->generics) r.args.push_back(fromTypeRef(g.get()));
                return r;
            }
            return checkerDetail::unknown();   // type param or unresolved
        }
    }
    return checkerDetail::unknown();
}


bool Checker::isSubclass(Symbol* a, Symbol* b) const {
    if (!a || !b) return false;
    if (a == b) return true;
    if (!a->decl) return false;
    for (const TypeRefPtr& base : a->decl->bases)
        if (isSubclass(base->resolvedSymbol, b)) return true;
    return false;
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
        auto slots = checkerDetail::slotsNamed(bt.sym->shape, name);
        for (const Slot* s : slots) if (!s->isMethod) return false;
        return methodOverloads(bt.sym, name).size() > 1;
    }
    return false;
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
                : checkerDetail::unknown();
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
            Type elem = checkerDetail::unknown();
            bool builtin = false;
            if (iter.kind == TKind::Class && iter.sym) {
                if (iter.sym->name == "Range") { elem = primType("int"); builtin = true; }
                else if (iter.sym->name == "Array" && !iter.args.empty()) { elem = iter.args[0]; builtin = true; }
                else if (iter.sym->name == "Map") {         // entries iterate as Pair<K,V>
                    if (Symbol* p = scope_ ? scope_->lookup("Pair") : nullptr) elem = checkerDetail::classType(p);
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
                for (const Slot* s : checkerDetail::slotsNamed(thisClass_->shape, e->text))
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
                        for (const Slot* s : checkerDetail::slotsNamed(ns->shape, e->text))
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
        checkerDetail::walkProperAncestors(thisClass, [&](const Symbol* a) {
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
        if (checkerDetail::mentionsTypeParam(p.type.get())) {
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
