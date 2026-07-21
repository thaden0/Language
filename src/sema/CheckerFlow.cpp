// Part of the refactor_1 Checker.cpp split (designs/complete/refactor_1/techdesign-03-checker-split-sonnet.md).
#include "sema/CheckerInternal.hpp"


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
        checkerDetail::resolveExprType(const_cast<TypeRef*>(cond->type.get()), scope_);
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
