// Part of the refactor_1 Checker.cpp split (designs/complete/refactor_1/techdesign-03-checker-split-sonnet.md).
#include "sema/CheckerInternal.hpp"
#include <deque>

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
    return checkerDetail::opSymbol(k);
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
    for (const TypeRefPtr& t : e->explicitTypeArgs)
        c->explicitTypeArgs.push_back(checkerDetail::copyTypeRef(t.get()));
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
            if (e->op == TokenKind::Eq || checkerDetail::isCompoundAssign(e->op))
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
        return checkerDetail::unknown();                        // hook mis-fired; leave as-is
    if (reifiedLambdas_.count(lambda)) return checkerDetail::unknown();   // idempotence (R1)

    // Fn param/return types. When this lambda arrives through a generic
    // method's parameter (e.g. `Query<E>.where(expr::Expr<(E)=>bool>)`),
    // `fnRef` is the method's raw, unsubstituted declaration TypeRef — `subst`
    // carries the caller's E->User binding so the lambda parameter and any
    // whitelisted-Call receiver typed through it resolve to the concrete type
    // instead of degrading to checkerDetail::unknown() (bug.md #89).
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

    // Rewrite `lambda` in place ->
    // expr::Expr::<Fn>(<lambda>, <tree>, <binds>, <siteId>).
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
    lambda->explicitTypeArgs.clear();
    lambda->explicitTypeArgs.push_back(subst
        ? checkerDetail::copyTypeRefWithSubst(fnRef, *subst) : checkerDetail::copyTypeRef(fnRef));
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
