// Part of the refactor_1 Checker.cpp split (designs/complete/refactor_1/techdesign-03-checker-split-sonnet.md).
#include "sema/CheckerInternal.hpp"
#include <functional>

namespace {


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

}  // namespace


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
            checkerDetail::walkProperAncestors(D, [&](const Symbol* T) {
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
                                 const Type& retType,
                                 const std::unordered_map<std::string_view, Type>* subst) {
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
        // LA-32 §4.6: a turbofish-pinned reference renders concrete parameter
        // types (`int`, not `T`) through the seeded substitution.
        sig += subst ? substitute(target->params[i].type.get(), *subst).canonical
                     : fromTypeRef(target->params[i].type.get()).canonical;
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


// LA-32 §4.6: a turbofish-pinned reference to a generic callable in value
// position. Reuses the same explicit-arg machinery the call path uses —
// filterExplicitCandidates for the arity check and callGenericSeed for the
// substitution — then hands the pinned callable to the eta-expansion rewrite so
// it becomes ONE concrete closure (the erasure posture holds: every engine
// consumes it with no new op). The tuple LA-25 §8.6 lacked is now supplied.
Type Checker::pinnedGenericRef(Expr* e, const Stmt* fn, Symbol* recvClass,
                               Symbol* ctorClass, const std::string& recvCanon) {
    std::vector<const Stmt*> one{fn};
    if (!filterExplicitCandidates(e, one, nullptr, fn->name))
        return Type{TKind::Error, nullptr, "", {}, nullptr, {}};   // arity already diagnosed
    auto seed = callGenericSeed(e, fn, &fn->generics, nullptr, nullptr);
    return rewriteAsMethodRef(e, fn, recvClass, ctorClass, recvCanon,
                              substitute(fn->type.get(), seed), &seed);
}


Type Checker::tryResolveMethodRef(Expr* e, const Type* expected, bool& isRef) {
    isRef = false;
    if (!e) return checkerDetail::unknown();
    // bug #55: a BARE free-function name in value position (`add`, not `NS::add`
    // or `obj.add`) is a callable reference too — rewrite it to its eta-expansion
    // lambda `(p0,…) => add(p0,…)`, exactly as the NS::fn / obj.method cases
    // below do. Without this, the name typed as a plain FuncRef but no engine
    // reified it: the oracle read it as void, the IR lowerer errored ("not yet
    // lowerable: name"), so storing it in a function-typed field and calling
    // back through the field failed while a hand-written lambda wrapper worked.
    if (e->kind == ExprKind::Name) {
        if (localLookup(e->text)) return checkerDetail::unknown();   // a local/param shadows: not a fn ref
        const std::vector<Symbol*>* v = nullptr;
        for (const Scope* s = scope_; s; s = s->parent)
            if (const std::vector<Symbol*>* found = s->localLookup(e->text)) { v = found; break; }
        if (!v) return checkerDetail::unknown();
        std::vector<const Stmt*> cands;
        for (Symbol* s : *v)
            if (s->kind == SymbolKind::Function && s->decl) cands.push_back(s->decl);
        if (cands.empty()) return checkerDetail::unknown();
        isRef = true;
        bool ambiguous = false;
        const Stmt* fn = pickMethodRefOverload(cands, "", "", expected, ambiguous);
        if (!fn) {
            if (ambiguous)
                return error(e->span, "ambiguous function reference '" +
                             std::string(e->text) + "'; annotate the target function type");
            return checkerDetail::unknown();
        }
        if (!fn->generics.empty()) {
            if (!e->explicitTypeArgs.empty())            // LA-32 §4.6: pinned reference
                return pinnedGenericRef(e, fn, nullptr, nullptr, "");
            return error(e->span, "cannot reference generic function '" +
                         std::string(e->text) +
                         "' in value position — supply explicit type arguments, e.g. '" +
                         std::string(e->text) + "::<...>'");
        }
        return rewriteAsMethodRef(e, fn, nullptr, nullptr, "", fromTypeRef(fn->type.get()));
    }
    if (e->kind != ExprKind::Member) return checkerDetail::unknown();
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
                        return checkerDetail::unknown();
                    }
                    if (!fn->generics.empty()) {
                        if (!e->explicitTypeArgs.empty())   // LA-32 §4.6: pinned NS::fn::<T>
                            return pinnedGenericRef(e, fn, nullptr, nullptr, "");
                        return error(e->span, "cannot reference generic function '" +
                                    std::string(name) +
                                    "' in value position — supply explicit type arguments, e.g. '" +
                                    std::string(name) + "::<...>'");
                    }
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
                return checkerDetail::unknown();
            }
            return rewriteAsMethodRef(e, ctor, nullptr, bt.sym, "", checkerDetail::classType(bt.sym));
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
                return checkerDetail::unknown();
            }
            if (!m->generics.empty()) {
                if (!e->explicitTypeArgs.empty())   // LA-32 §4.6: pinned Type::method::<U>
                    return pinnedGenericRef(e, m, bt.sym, nullptr, bt.canonical);
                return error(e->span, "cannot reference generic function '" +
                            std::string(name) +
                            "' in value position — supply explicit type arguments, e.g. '" +
                            std::string(name) + "::<...>'");
            }
            return rewriteAsMethodRef(e, m, bt.sym, nullptr, bt.canonical,
                                      fromTypeRef(m->type.get()));
        }
    }

    // F3: `obj.method` in value position binds `obj`. A data slot of the same
    // name keeps ordinary field-read precedence; only a resolved method slot
    // is eligible for this rewrite.
    if (!e->colon && bt.kind == TKind::Class && bt.sym) {
        auto slots = checkerDetail::slotsNamed(bt.sym->shape, name);
        for (const Slot* s : slots)
            if (!s->isMethod) return checkerDetail::unknown();
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
                return checkerDetail::unknown();
            }
            if (!m->generics.empty()) {
                if (!e->explicitTypeArgs.empty()) {   // LA-32 §4.6: pinned obj.method::<U>
                    // The bound rewrite needs its receiver check to run first, so
                    // fall through to it below with the seed applied there.
                } else {
                    diagnosedMethodRefs_.insert(e);
                    return error(e->span, "cannot reference generic function '" +
                                std::string(name) +
                                "' in value position — supply explicit type arguments, e.g. '" +
                                std::string(name) + "::<...>'");
                }
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
            if (!m->generics.empty() && !e->explicitTypeArgs.empty())  // LA-32 §4.6: pinned
                return pinnedGenericRef(e, m, bt.sym, nullptr, "");
            return rewriteAsMethodRef(e, m, bt.sym, nullptr, "",
                                      fromTypeRef(m->type.get()));
        }
    }
    return checkerDetail::unknown();
}
