// Part of the refactor_1 Checker.cpp split (designs/complete/refactor_1/techdesign-03-checker-split-sonnet.md).
#include "CheckerInternal.hpp"

namespace {


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
        for (const TypeRefPtr& t : e->explicitTypeArgs)
            out->explicitTypeArgs.push_back(type(t.get()));
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

}  // namespace


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
            Symbol* ns = checkerDetail::nsChainSym(scope, callee->a.get());
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
