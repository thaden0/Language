// Part of the refactor_1 Checker.cpp split (designs/complete/refactor_1/techdesign-03-checker-split-sonnet.md).
#include "sema/CheckerInternal.hpp"


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
    // Attribute class symbols only name types behind `@` — prefer a real class
    // when both are visible (e.g. @Row vs Atlantis::Data::Row), falling back
    // to the attribute hit only when nothing else matches.
    Symbol* attrHit = nullptr;
    for (const Scope* sc = scope_; sc; sc = sc->parent)
        if (const std::vector<Symbol*>* syms = sc->localLookup(name))
            for (Symbol* s : *syms)
                if (s->kind == SymbolKind::Class) {
                    if (s->decl && s->decl->isAttribute) {
                        if (!attrHit) attrHit = s;
                    } else {
                        return s;
                    }
                }
    return attrHit;
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
            if (checkerDetail::mentionsTypeParam(p)) { score += 1; continue; }   // generic param: unifies
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


bool Checker::filterExplicitCandidates(
    const Expr* call, std::vector<const Stmt*>& cands,
    const std::vector<std::string_view>* constructionParams,
    std::string_view ownerName) {
    if (!call || call->explicitTypeArgs.empty()) return true;
    const size_t got = call->explicitTypeArgs.size();
    auto arguments = [](size_t n) {
        return n == 1 ? "argument" : "arguments";
    };

    if (constructionParams) {
        const size_t expected = constructionParams->size();
        if (got == expected) return true;
        error(call->span, "construction of '" + std::string(ownerName) +
              "' expects " + std::to_string(expected) + " explicit type " +
              arguments(expected) + ", got " + std::to_string(got));
        return false;
    }

    std::vector<const Stmt*> matching;
    for (const Stmt* c : cands)
        if (c && c->generics.size() == got) matching.push_back(c);
    if (!matching.empty()) {
        cands = std::move(matching);
        return true;
    }

    if (cands.size() == 1) {
        const size_t expected = cands.front()->generics.size();
        error(call->span, "call to '" + std::string(ownerName) + "' expects " +
              std::to_string(expected) + " explicit type " + arguments(expected) +
              ", got " + std::to_string(got));
    } else {
        error(call->span, "no overload of '" + std::string(ownerName) + "' accepts " +
              std::to_string(got) + " explicit type " + arguments(got));
    }
    cands.clear();
    return false;
}


std::unordered_map<std::string_view, Type> Checker::callGenericSeed(
    const Expr* call, const Stmt* candidate,
    const std::vector<std::string_view>* explicitParams,
    Symbol* receiverClass, const Type* receiver) const {
    std::unordered_map<std::string_view, Type> seed;
    if (receiverClass && receiverClass->decl && receiver)
        for (size_t i = 0;
             i < receiverClass->decl->generics.size() && i < receiver->args.size(); ++i)
            seed[receiverClass->decl->generics[i]] = receiver->args[i];

    if (!call || call->explicitTypeArgs.empty()) return seed;
    const std::vector<std::string_view>* params = explicitParams;
    if (!params && candidate) params = &candidate->generics;
    if (!params) return seed;
    for (size_t i = 0; i < params->size() && i < call->explicitTypeArgs.size(); ++i)
        seed[(*params)[i]] = fromTypeRef(call->explicitTypeArgs[i].get());
    return seed;
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
    for (const TypeRefPtr& t : e->explicitTypeArgs)
        out->explicitTypeArgs.push_back(checkerDetail::copyTypeRef(t.get()));
    if (e->type) out->type = checkerDetail::copyTypeRef(e->type.get());
    return out;
}


const Stmt* Checker::pickInjecting(const std::vector<const Stmt*>& cands,
                                   std::vector<Type>& argTypes, Expr* call, bool& ok,
                                   bool& diagnosed,
                                   const std::vector<std::string_view>* explicitParams,
                                   Symbol* receiverClass, const Type* receiver) {
    diagnosed = false;
    // Re-entry sees the canonical full positional list and can use the ordinary
    // overload picker.  This is the idempotence guard for synthesized defaults
    // and injections.
    // Explicit calls must rebuild their authoritative seed on every pass, so
    // only the legacy no-list path takes the short cut.
    if (call->argsNormalized && call->explicitTypeArgs.empty())
        return pickOverload(cands, argTypes, ok, call);

    struct Binding {
        const Stmt* candidate = nullptr;
        std::vector<int> supplied;          // param index -> raw argument index
        std::vector<const Stmt*> injections;
        std::vector<Type> paramTypes;
        std::unordered_map<std::string_view, Type> subst;
        int score = -1;
        int omitted = 0;
    } best;

    // LA-31 §2.2 / E3: per applicable candidate, which lambda-LITERAL argument
    // positions matched an `expr::Expr<Fn>` parameter vs a FuncRef parameter.
    struct CandTie { int score; std::vector<int> exprTargetArgs; std::vector<int> funcRefArgs; };
    std::vector<CandTie> ties;

    std::string soleFailure;
    SourceSpan soleFailureSpan = call->span;
    const bool hasExplicit = !call->explicitTypeArgs.empty();
    for (const Stmt* c : cands) {
        Binding cur;
        cur.candidate = c;
        cur.supplied.assign(c->params.size(), -1);
        cur.injections.assign(c->params.size(), nullptr);
        cur.paramTypes.reserve(c->params.size());
        if (hasExplicit)
            cur.subst = callGenericSeed(call, c, explicitParams,
                                        receiverClass, receiver);
        for (const Param& p : c->params)
            cur.paramTypes.push_back(hasExplicit
                ? substitute(p.type.get(), cur.subst)
                : fromTypeRef(p.type.get()));
        bool applicable = true;
        std::string failure;
        SourceSpan failureSpan = call->span;
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
            const Type& pt = cur.paramTypes[pi];
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
            if (!hasExplicit && checkerDetail::mentionsTypeParam(p)) { score += 1; continue; }
            const Type& pt = cur.paramTypes[pi];
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
            // Scored BELOW the exact-match tier (1, not 2): a literal's default
            // type is `string`, so an `f(string)` sibling already scores 2 here
            // via the exact-match branch above. Track 03 §5 problem #1's
            // back-compat rule ("`string` wins when both f(char)/f(string)
            // exist") must hold regardless of declaration order, which a tied
            // score would leave to first-declared-wins — scoring the char match
            // strictly lower makes `string` win by score, not by luck of order.
            if (pt.canonical == "char" &&
                checkerDetail::isCharLiteral(call->list[static_cast<size_t>(ai)].get())) {
                score += 1; continue;
            }
            if (assignable(at, pt)) { score += 1; continue; }
            if (hasExplicit) {
                failure = "argument for parameter '" +
                          std::string(c->params[pi].name) + "' has type '" +
                          at.canonical + "', expected '" + pt.canonical + "'";
                failureSpan = call->list[static_cast<size_t>(ai)]->span;
            }
            applicable = false;
        }
        if (!applicable) {
            if (cands.size() == 1) {
                soleFailure = std::move(failure);
                soleFailureSpan = failureSpan;
            }
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
            error(soleFailureSpan, soleFailure);
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
            if (checkerDetail::isCharLiteral(ordered[pi].get()) &&
                (hasExplicit ? best.paramTypes[pi].canonical == "char"
                             : expectsChar(best.candidate->params[pi].type.get()))) {
                checkerDetail::markCharLiteral(ordered[pi].get());
                orderedTypes[pi] = primType("char");
            }
        } else if (best.candidate->params[pi].defaultValue) {
            ordered[pi] = cloneDefaultExpr(best.candidate->params[pi].defaultValue.get());
            orderedTypes[pi] = best.paramTypes[pi];
        } else {
            auto arg = std::make_unique<Expr>(ExprKind::Inject);
            arg->span = call->span;
            arg->type = hasExplicit
                ? checkerDetail::copyTypeRefWithSubst(best.candidate->params[pi].type.get(), best.subst)
                : checkerDetail::copyTypeRef(best.candidate->params[pi].type.get());
            arg->resolved = best.injections[pi];
            orderedTypes[pi] = best.paramTypes[pi];
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
                                const std::vector<Type>& args, const Expr* call,
                                const TypeRef* expected, SourceSpan span) {
    // Implicit memberwise construction (bug #38): a class that declares NO
    // constructor populates its data fields positionally, in slot order. That
    // path performed no arity check at all, so surplus arguments were silently
    // dropped at runtime — `int(3.9)`/`string(5)`/`bool(1)` (primitives have
    // zero data fields) and `Foo(1, 2, 3)` (one field) all compiled and then
    // yielded a default/garbage value that diverged across engines (the IR
    // backend zero-inited an int to "0" while the tree-walker / LLVM / emit-C++
    // left an empty object stringifying to ""). Reject a construction that
    // supplies more arguments than the class has assignable data slots — the
    // same "too many arguments" a declared constructor's arity check already
    // gives. Gated on "declares no constructor": a class that DOES declare one
    // but matched none has already been diagnosed by the overload picker
    // ("no constructor matches the arguments"), so this never double-reports.
    // `ctor` null + zero-or-fewer args than fields (partial memberwise) and the
    // canonical `FieldError("email", "required")` (2 fields, 2 args) stay valid.
    if (!ctor && cls && call && !call->list.empty()) {
        bool declaresAnyCtor = false;
        if (cls->decl)
            for (const StmtPtr& m : cls->decl->body)
                if (m->isCtor) { declaresAnyCtor = true; break; }
        if (!declaresAnyCtor) {
            size_t dataSlots = 0;
            for (const Slot& s : cls->shape.slots)
                if (!s.isMethod) ++dataSlots;
            if (call->list.size() > dataSlots)
                return error(span,
                    "too many arguments to construct '" + std::string(cls->name) +
                    "': it has " + std::to_string(dataSlots) +
                    (dataSlots == 1 ? " assignable field but " : " assignable fields but ") +
                    std::to_string(call->list.size()) + " were given");
        }
    }
    if (!cls || !cls->decl || cls->decl->generics.empty())
        return checkerDetail::classType(cls);

    const std::vector<std::string_view>& params = cls->decl->generics;
    // Explicit class arguments are authoritative occupied slots. unify() is
    // first-binding-wins, so later value/target inference can fill only gaps.
    std::unordered_map<std::string_view, Type> map =
        callGenericSeed(call, ctor, &params);

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
    // its own T as checkerDetail::unknown() throughout, per fromTypeRef's Named case above).
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
        // returns checkerDetail::unknown()) has no concrete Type to report yet. Degrade to
        // the raw head, the same fallback substitute() already uses for
        // this exact shape (§2.5's leniency), rather than building a
        // malformed 'Foo<>' that then fails every assignability check
        // downstream (raw compatibility requires no '<' in the canonical at
        // all — a stray empty bracket pair does not qualify).
        if (it->second.kind == TKind::Unknown) anyUnbound = true;
        else bound.push_back(it->second);
    }
    if (anyUnbound) return checkerDetail::classType(cls);   // raw — assignable to any instantiation

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
    if (!t) return checkerDetail::unknown();
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
            if (!head) return checkerDetail::unknown();
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
        if (s && s->kind == SymbolKind::Primitive) return checkerDetail::primitive(t->canonical);
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
        return checkerDetail::unknown();   // an unbound type var, or unresolved
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
                         : (i < paramTypes.size() ? paramTypes[i] : checkerDetail::unknown());
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
    Type ret = checkerDetail::unknown();
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


// The return Type of a (possibly generic) method/function call: bind class
// params from the receiver, method params from the arguments, then substitute.
// Lambda arguments run LAST (when `call`/`lambdaWalked` are given): value
// arguments have already bound what they can (T from the receiver, A from a
// seed), so each lambda's parameter types are known; its body then infers the
// return type, which binds the remaining vars (U in `map<U>((T) => U fn)`).
Type Checker::genericReturn(Symbol* cls, const Stmt* fn, const Type& receiver,
                            const std::vector<Type>& args,
                            const Expr* call, std::vector<char>* lambdaWalked) {
    std::unordered_map<std::string_view, Type> subst =
        callGenericSeed(call, fn, nullptr, cls, &receiver);
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
            // bug #101 (LA-18): a lambda argument's VALUE type was deferred to
            // unknown() during arg collection, so the value-type unify above saw
            // no evidence for a generic parameter that appears only in the
            // lambda's parameter position (`(A) => R` matched by `(Left a) => a`
            // binds R from the body below, but nothing bound A). Ordinary
            // generic checking tolerates this because a `specializationRequired`
            // callable's return type here (`() => string`) doesn't mention A —
            // but LA-18's specialization-set collector needs the full A/R tuple.
            // Consult the lambda's OWN declared parameter types, matching them
            // against the function parameter's declared param types, before the
            // body is checked. This is the reuse §4.1 point 4 of
            // designs/complete/techdesign-generic-static-members.md specifies.
            if (pt && pt->kind == TypeKind::Function)
                for (size_t j = 0; j < pt->funcParams.size() && j < a->params.size(); ++j)
                    if (a->params[j].type)
                        unify(pt->funcParams[j].get(),
                              fromTypeRef(a->params[j].type.get()), subst);
            std::vector<Type> ptypes;
            if (pt && pt->kind == TypeKind::Function)
                for (const TypeRefPtr& fp : pt->funcParams)
                    ptypes.push_back(substitute(fp.get(), subst));
            Type ret = checkLambdaBody(a, ptypes);
            if (pt && pt->kind == TypeKind::Function && pt->funcRet) {
                if (call && !call->explicitTypeArgs.empty()) {
                    Type expectedRet = substitute(pt->funcRet.get(), subst);
                    if (ret.kind != TKind::Unknown && ret.kind != TKind::Error &&
                        expectedRet.kind != TKind::Unknown &&
                        !assignable(ret, expectedRet))
                        error(a->span, "lambda body has type '" + ret.canonical +
                              "', expected '" + expectedRet.canonical + "'");
                }
                unify(pt->funcRet.get(), ret, subst);
            }
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
    if (expectsChar(expected) && checkerDetail::isCharLiteral(e)) {
        checkerDetail::markCharLiteral(e);
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
            const std::vector<std::string_view>* explicitParams =
                ctorClass->decl ? &ctorClass->decl->generics : nullptr;
            // LA-25 §2.2/§6 (M3): defer an OVERLOADED method-reference argument
            // here too (this is the var-decl/return/field-init construction
            // path, a separate arg-typing pass from typeOfCallInner's) — typed
            // unknown for overload choice, then walked below against the
            // chosen constructor's declared parameter type, same as a Lambda.
            auto cands = ctorOverloads(ctorClass, label);
            if (!filterExplicitCandidates(e, cands, explicitParams, ctorClass->name))
                return Type{TKind::Error, nullptr, "", {}, nullptr, {}};
            // bug.md #43: thread the constructor parameter's declared type into
            // each argument as its TARGET type, so a nested generic construction
            // (`Foo(1, Map())`) infers its own type args from the parameter it
            // fills instead of erroring for lack of a target. ctorOverloads does
            // not consult argument types, so a UNIQUE constructor's parameters are
            // known before the arguments are typed; with overloads the target is
            // ambiguous, so fall back to bare typing (existing behavior).
            const Stmt* soleCtor = cands.size() == 1 ? cands.front() : nullptr;
            const auto explicitSeed = callGenericSeed(e, soleCtor, explicitParams);
            std::vector<Type> argTypes;
            for (size_t i = 0; i < e->list.size(); ++i) {
                const Expr* a = e->list[i].get();
                if (isDeferredMethodRefArg(a)) { argTypes.push_back(checkerDetail::unknown()); continue; }
                if (soleCtor && i < soleCtor->params.size()) {
                    TypeRefPtr target = e->explicitTypeArgs.empty()
                        ? checkerDetail::copyTypeRef(soleCtor->params[i].type.get())
                        : checkerDetail::copyTypeRefWithSubst(soleCtor->params[i].type.get(), explicitSeed);
                    argTypes.push_back(typeInitExpr(a, target.get()));
                } else
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
                                             ok, diagnosed, explicitParams);
            if (!cands.empty() && !ok && !diagnosed)
                error(e->span, "no constructor matches the arguments");
            if (!ctor && !e->explicitTypeArgs.empty() && !cands.empty())
                return Type{TKind::Error, nullptr, "", {}, nullptr, {}};
            const_cast<Expr*>(e)->resolved = ctor;
            const_cast<Expr*>(e)->resolvedClass = ctorClass;
            if (ctor)
                for (size_t i = 0; i < ctor->params.size() && i < e->list.size(); ++i) {
                    Expr* a = e->list[i].get();
                    // bug #55: a bare free-function name constructor argument is a
                    // callable reference too, not only a `T::m`/`obj.m` member.
                    if (a->kind != ExprKind::Member && a->kind != ExprKind::Name) continue;
                    Type et = e->explicitTypeArgs.empty()
                        ? fromTypeRef(ctor->params[i].type.get())
                        : substitute(ctor->params[i].type.get(), explicitSeed);
                    bool isRef = false;
                    tryResolveMethodRef(a, &et, isRef);
                }
            return inferConstruction(ctorClass, ctor, argTypes, e, expected, e->span);
        }
    }
    // LA-25 §2.2: the declared-type slot's type is the target for an
    // overloaded method-reference initializer (var-decl, field init, return).
    if (e && e->kind == ExprKind::Member) {
        bool isRef = false;
        Type expectedType = expected ? fromTypeRef(expected) : checkerDetail::unknown();
        Type t = tryResolveMethodRef(const_cast<Expr*>(e), expected ? &expectedType : nullptr,
                                     isRef);
        if (isRef) return t;
    }
    return typeOf(e);
}
