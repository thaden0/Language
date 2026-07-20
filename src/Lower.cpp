#include "Lower.hpp"
#include <cstdlib>
#include <functional>
#include <unordered_set>

// bug #3: collect every identifier a lambda body references (a conservative
// SUPERSET of its free variables). Used to capture only the referenced enclosing
// locals into a closure, instead of every visible local — so an unreferenced
// non-flattenable local (Worker/Timer/Block/nested closure) merely in lexical
// scope no longer rides into a spawn body's flatten and force a reject.
static void lwrCollectStmtNames(const Stmt* s, std::unordered_set<std::string_view>& names);
static void lwrCollectExprNames(const Expr* e, std::unordered_set<std::string_view>& names) {
    if (!e) return;
    if (e->kind == ExprKind::Name || e->kind == ExprKind::Member)
        if (!e->text.empty()) names.insert(e->text);
    lwrCollectExprNames(e->a.get(), names);
    lwrCollectExprNames(e->b.get(), names);
    lwrCollectExprNames(e->c.get(), names);
    for (const ExprPtr& x : e->list) lwrCollectExprNames(x.get(), names);
    for (const MatchArm& arm : e->arms) {
        lwrCollectExprNames(arm.value.get(), names);
        lwrCollectExprNames(arm.bodyExpr.get(), names);
        lwrCollectStmtNames(arm.bodyBlock.get(), names);
    }
    for (const Param& p : e->params) lwrCollectExprNames(p.defaultValue.get(), names);
    lwrCollectStmtNames(e->block.get(), names);
}
static void lwrCollectStmtNames(const Stmt* s, std::unordered_set<std::string_view>& names) {
    if (!s) return;
    lwrCollectExprNames(s->expr.get(), names);
    lwrCollectExprNames(s->init.get(), names);
    lwrCollectExprNames(s->forStep.get(), names);
    for (const StmtPtr& b : s->body) lwrCollectStmtNames(b.get(), names);
    lwrCollectStmtNames(s->thenBranch.get(), names);
    lwrCollectStmtNames(s->elseBranch.get(), names);
    lwrCollectStmtNames(s->forInit.get(), names);
    lwrCollectStmtNames(s->memberBody.get(), names);
    for (const CatchClause& c : s->catches) lwrCollectStmtNames(c.body.get(), names);
}

// ---------------------------------------------------------------------------
//  plumbing
// ---------------------------------------------------------------------------

void Lowerer::fail(SourceSpan span, const std::string& what) {
    if (ok_) sink_.error(span, "IR: not yet lowerable: " + what);
    ok_ = false;
}

int Lowerer::emit(Op op, int a, int b, int c, int d) {
    Inst in; in.op = op; in.a = a; in.b = b; in.c = c; in.d = d;
    F().code.push_back(std::move(in));
    return (int)F().code.size() - 1;
}

int Lowerer::addConst(Value v) {
    F().consts.push_back(std::move(v));
    return (int)F().consts.size() - 1;
}

int* Lowerer::findLocal(std::string_view name) {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto f = it->find(name);
        if (f != it->end()) return &f->second;
    }
    return nullptr;
}

int Lowerer::thisReg() {
    if (int* r = findLocal("this")) return *r;   // lambda body: captured receiver
    return curClass_ ? 0 : -1;                   // member body: implicit r0
}

bool Lowerer::classHasMember(Symbol* cls, std::string_view name) {
    if (!cls) return false;
    for (const Slot& s : cls->shape.slots)
        if (s.name == name) return true;
    if (!cls->decl) return false;
    for (const StmtPtr& m : cls->decl->body)
        if ((m->isGet || m->isSet) && m->name == name) return true;
    return false;
}

bool Lowerer::isBaseOrSelf(Symbol* derived, Symbol* base) {
    if (!derived || !base) return false;
    if (derived == base) return true;
    if (!derived->decl) return false;
    for (const TypeRefPtr& b : derived->decl->bases)
        if (isBaseOrSelf(b->resolvedSymbol, base)) return true;
    return false;
}

// The 0-based packed slot of a plain field `key` in `cls`'s shape, or -1. Mirrors
// the fieldKeys() numbering in every backend: non-method slots in shape order.
// `distinct` fields (same name+type from two bases, kept apart) still occupy a
// slot but are reached only by qualified key, so they never fast-path by name.
int Lowerer::fieldSlotOf(Symbol* cls, std::string_view key) {
    if (!cls) return -1;
    int idx = 0;
    for (const Slot& s : cls->shape.slots) {
        if (s.isMethod) continue;
        if (!s.distinct && s.name == key) return idx;
        ++idx;
    }
    return -1;
}

// A field access `recv.key`, where `recv` is statically typed `cls`, may lower to
// a direct slot offset iff — over `cls` and every subclass (the runtime types
// `recv` can hold) — `key` sits at the SAME packed slot, and (when we're
// replacing the accessor-aware member path) no class in that set intercepts
// `key` with a get/set accessor. Returns slot+1 (0 = use the runtime path).
// This is the general rule; $init is its degenerate case (the type set is {cls}).
int Lowerer::packedSlot(Symbol* cls, std::string_view key, bool requireNoAccessor) {
    int s = fieldSlotOf(cls, key);
    if (s < 0) return 0;
    for (const auto& sp : sema_.symbols) {
        Symbol* sub = sp.get();
        if (sub->kind != SymbolKind::Class || !isBaseOrSelf(sub, cls)) continue;
        if (fieldSlotOf(sub, key) != s) return 0;          // slot shifts under this subclass
        if (requireNoAccessor && sub->decl)
            for (const StmtPtr& m : sub->decl->body)
                if ((m->isGet || m->isSet) && m->name == key) return 0;  // accessor intercepts
    }
    return s + 1;
}

// ---------------------------------------------------------------------------
//  collection: functions, methods, ctors, accessors — user program + prelude
// ---------------------------------------------------------------------------

void Lowerer::collectClass(Stmt* cls) {
    Symbol* sym = nullptr;
    // find the class symbol via the global scope walk (classes are gathered)
    if (const std::vector<Symbol*>* v = sema_.global->localLookup(cls->name)) {
        for (Symbol* s : *v)
            if (s->kind == SymbolKind::Class && s->decl == cls) sym = s;
    }
    if (!sym) {   // namespace-scoped class: search all namespace scopes
        for (const auto& sp : sema_.symbols)
            if (sp->kind == SymbolKind::Class && sp->decl == cls) { sym = sp.get(); break; }
    }
    if (!sym) return;
    for (StmtPtr& m : cls->body) {
        if (m->kind != StmtKind::Member || !m->callable || !m->memberBody) continue;
        if (m->specializationRequired && !m->isSpecialization) continue; // LA-18 erased body
        if (m->memberBody->kind == StmtKind::Empty) continue;    // native intrinsic
        int idx = (int)mod_->functions.size();
        mod_->functions.emplace_back();
        IrFunction& fn = mod_->functions.back();
        fn.name = std::string(sym->name) + "." + std::string(m->name);
        fn.hasThis = true;
        fn.nparams = 1 + (int)m->params.size();
        mod_->byDecl[m.get()] = idx;
        pending_.push_back({m.get(), sym, idx});
    }
}

// §12.5: a factory `bind Type => e;` lowers as a zero-arg function returning the
// injected value. Binds can appear anywhere a statement can, so scan the whole
// statement tree (bodies included) and register each one in byDecl + pending.
void Lowerer::collectBinds(Stmt* s) {
    if (!s) return;
    switch (s->kind) {
        case StmtKind::Bind:
            if (s->memberBody && s->type) {
                // A `bind T => expr;` factory body is now INLINED at each inject
                // site (bug.md #56), so it needs no standalone zero-arg $bind
                // function — and one referencing an enclosing local could not be
                // lowered as a separate function anyway. Only a `{ block }` body
                // (no single return expression) still gets a $bind function that
                // inject falls back to calling.
                if (s->memberBody->kind == StmtKind::Return && s->memberBody->expr)
                    break;
                int idx = (int)mod_->functions.size();
                mod_->functions.emplace_back();
                IrFunction& fn = mod_->functions.back();
                fn.name = std::string("$bind.") +
                          (s->type->canonical.empty() ? std::string(s->type->name)
                                                       : s->type->canonical);
                fn.nparams = 0;
                mod_->byDecl[s] = idx;
                pending_.push_back({s, nullptr, idx});
            }
            break;
        case StmtKind::Namespace:
        case StmtKind::Block:
            for (StmtPtr& c : s->body) collectBinds(c.get());
            break;
        case StmtKind::Class:
            for (StmtPtr& m : s->body) if (m->memberBody) collectBinds(m->memberBody.get());
            break;
        case StmtKind::Member:
            if (s->memberBody) collectBinds(s->memberBody.get());
            break;
        case StmtKind::If:
            collectBinds(s->thenBranch.get()); collectBinds(s->elseBranch.get());
            break;
        case StmtKind::While:
        case StmtKind::DoWhile:
        case StmtKind::ForIn:
            collectBinds(s->thenBranch.get());
            break;
        case StmtKind::For:
            collectBinds(s->forInit.get()); collectBinds(s->thenBranch.get());
            break;
        case StmtKind::Try:
            collectBinds(s->thenBranch.get());
            for (CatchClause& c : s->catches) collectBinds(c.body.get());
            break;
        default:
            break;
    }
}

void Lowerer::collect(std::vector<StmtPtr>& items) {
    for (StmtPtr& item : items) {
        if (item->kind == StmtKind::Member && item->callable && item->memberBody &&
            item->memberBody->kind != StmtKind::Empty) {
            if (item->specializationRequired && !item->isSpecialization) continue; // LA-18
            int idx = (int)mod_->functions.size();
            mod_->functions.emplace_back();
            IrFunction& fn = mod_->functions.back();
            fn.name = std::string(item->name);
            fn.nparams = (int)item->params.size();
            mod_->byDecl[item.get()] = idx;
            pending_.push_back({item.get(), nullptr, idx});
        } else if (item->kind == StmtKind::Namespace) {
            collect(item->body);
        } else if (item->kind == StmtKind::Class) {
            collectClass(item.get());
        }
    }
}

// $init: default-or-initialize every field slot of the class's shape.
int Lowerer::synthesizeInit(Symbol* cls) {
    auto it = mod_->initByClass.find(cls);
    if (it != mod_->initByClass.end()) return it->second;

    int idx = (int)mod_->functions.size();
    mod_->functions.emplace_back();
    mod_->functions[idx].name = std::string(cls->name) + ".$init";
    mod_->functions[idx].hasThis = true;
    mod_->functions[idx].nparams = 1;
    mod_->initByClass[cls] = idx;

    int savedCur = cur_; Symbol* savedClass = curClass_;
    auto savedScopes = std::move(scopes_);
    auto savedFresh = std::move(freshStructRegs_);   // §15: reg marks are per-function
    freshStructRegs_.clear();
    cur_ = idx; curClass_ = cls;
    scopes_.clear(); scopes_.emplace_back();
    newReg();   // r0 = this

    int fieldIdx = 0;
    for (const Slot& s : cls->shape.slots) {
        if (s.isMethod) continue;
        std::string key = (s.distinct && s.source)
            ? std::string(s.source->name) + "::" + std::string(s.name)
            : std::string(s.name);
        Symbol* fcls = s.decl && s.decl->type ? s.decl->type->resolvedSymbol : nullptr;
        bool valueField = fcls && fcls->kind == SymbolKind::Class && fcls->isValue &&
                          !fcls->isPrimitive && fcls->decl && !fcls->decl->isInterface;
        int v;
        if (s.decl && s.decl->init) {
            v = lowerExpr(s.decl->init.get());
            // §15: a value-struct field owns its own standalone copy (freed with
            // the object by recursiveFree's vfree) — never alias the initializer.
            if (valueField) {
                int c = newReg(); emit(Op::CopyVal, c, v); last().c = 1; v = c;
            }
        } else if (bareFieldSuppliedByCtor(cls, s)) {
            // A constructor definite-first-assigns this bare reference field, so
            // §3's throwaway default (a NewObject + discarded nullary ctor, whose
            // side effects can throw — Sonar's single-App rule) is elided: the
            // slot takes Op::Default (null/None) and the ctor's own store binds
            // the real value, the same slot state onConstructionCycle fields ride.
            v = newReg();
            emit(Op::Default, v);
            last().sname = s.canonical;
        } else if (bareFieldAutoConstructs(fcls)) {
            // §3: a bare constructable-class field auto-constructs — there is no
            // null/unbound state. A VALUE STRUCT co-allocates in `this`'s tier
            // (in.c=1: arena outer -> arena inner, dying together; heap outer ->
            // heap inner, vfree'd by recursiveFree). A REFERENCE CLASS is
            // refcounted like any object (in.c=0): the NewObject + nullary-ctor +
            // RawSet sequence is exactly what a `Field f = Field();` initializer
            // already lowers to, so the field slot's +1 and its release ride the
            // same ARC path. Self-referential reference types (onConstructionCycle)
            // have no finite default and fall through to Op::Default below.
            int initFn = synthesizeInit(fcls);
            v = newReg();
            emit(Op::NewObject, v, initFn, fcls->isValue ? 1 : 0);
            last().sym = fcls;
            if (const Stmt* ctor = nullaryCtor(fcls)) {
                auto itc = mod_->byDecl.find(ctor);
                if (itc != mod_->byDecl.end()) {
                    int base = F().nregs;
                    { int rr = newReg(); emit(Op::Move, rr, v); }   // this
                    int dst = newReg();
                    emit(Op::Call, dst, itc->second, base, 1);
                }
            }
        } else {
            v = newReg();
            emit(Op::Default, v);
            last().sname = s.canonical;
        }
        emit(s.isWeak ? Op::RawSetWeak : Op::RawSet, v, 0);
        last().sname = key;
        last().d = fieldIdx + 1;      // §7: compile-time packed slot index (0 = none, i = slot i-1)
        ++fieldIdx;
    }
    emit(Op::RetVoid);

    cur_ = savedCur; curClass_ = savedClass; scopes_ = std::move(savedScopes);
    freshStructRegs_ = std::move(savedFresh);
    return idx;
}

Symbol* Lowerer::classToAutoConstruct(const TypeRef* t) {
    if (!t) return nullptr;
    Symbol* cls = t->resolvedSymbol;
    if (cls && cls->kind == SymbolKind::Class && !cls->isPrimitive &&
        !cls->decl->isInterface && cls->name != "Array" && cls->name != "Map")
        return cls;
    return nullptr;
}

const Stmt* Lowerer::nullaryCtor(Symbol* cls) {
    if (!cls || !cls->decl) return nullptr;
    for (const StmtPtr& m : cls->decl->body)
        if (m->isCtor && m->params.empty()) return m.get();
    return nullptr;
}

void Lowerer::lowerPending(const Pending& p) {
    cur_ = p.index;
    curClass_ = p.cls;
    curNamespace_ = p.cls ? nullptr : p.decl->enclosingNs;   // free functions only
    curIsCtor_ = p.decl->isCtor;
    curAccessor_ = (p.decl->isGet || p.decl->isSet) ? std::string(p.decl->name) : "";
    curIsLambda_ = false;
    scopes_.clear();
    scopes_.emplace_back();
    freshStructRegs_.clear();                            // §15: per-function reg marks
    loops_.clear();                                      // techdesign-02 F1: fresh function
    usings_.clear();                                     // techdesign-02 F3: fresh function
    chainRetReg_ = -1; chainRetIsVoid_ = false;
    if (p.cls) newReg();                                 // r0 = this
    for (const Param& prm : p.decl->params) scopes_.back()[prm.name] = newReg();
    lowerStmt(p.decl->memberBody.get());
    emit(Op::RetVoid);
}

bool Lowerer::lower(Program& program, Program& prelude, IrModule& out) {
    mod_ = &out;
    out.columnar = columnar_;   // techdesign-columnar: mode travels into the IR
    mod_->sema = &sema_;
    ok_ = true;

    // Console member decls: checker-resolved calls to these lower to Print
    // even when the receiver is an alias, not the `console` name itself.
    consoleMembers_.clear();
    if (const std::vector<Symbol*>* v = sema_.global->localLookup("Console"))
        for (Symbol* s : *v)
            if (s->kind == SymbolKind::Class && s->decl)
                for (const StmtPtr& m : s->decl->body)
                    if (m->kind == StmtKind::Member && m->callable &&
                        m->memberBody && m->memberBody->kind == StmtKind::Empty)
                        consoleMembers_.insert(m.get());

    collect(prelude.items);
    collect(program.items);
    // LA-18 concrete copies live outside Program::items so @main never sees
    // them. Register them as ordinary functions; specialized instance methods
    // keep their receiver in r0.
    for (StmtPtr& s : program.specializations) {
        if (!s->memberBody || s->memberBody->kind == StmtKind::Empty) continue;
        int idx = (int)mod_->functions.size();
        mod_->functions.emplace_back();
        IrFunction& fn = mod_->functions.back();
        fn.name = std::string(s->name);
        fn.hasThis = s->specializationClass != nullptr;
        fn.nparams = (fn.hasThis ? 1 : 0) + (int)s->params.size();
        mod_->byDecl[s.get()] = idx;
        pending_.push_back({s.get(), s->specializationClass, idx});
    }
    for (StmtPtr& item : prelude.items) collectBinds(item.get());   // §12.5 factory binds
    for (StmtPtr& item : program.items) collectBinds(item.get());
    for (StmtPtr& item : program.specializations) collectBinds(item.get());

    // Prelude namespace globals (std::read etc.): assign indices, then lower
    // their initializers into a synthetic @ginit. The IR interpreter runs it
    // before the entry; native backends skip it (globals arrive natively with
    // object coverage).
    {
        std::vector<Stmt*> globalVars;        // prelude + user-ns vars WITH init — the init loop
        std::vector<Stmt*> userDefaultVars;   // every USER namespace var (bare + init) — Phase A
        // `collectTop` collects Vars at the current level. The prelude has no
        // @main, so ALL its Var-with-init decls (top-level and namespace-nested)
        // are globals. The user program's top-level Vars are locals executed in
        // @main, so only its namespace-nested Vars (bug 1) become globals —
        // recursion into any namespace always collects. `userNs` marks that we
        // are inside the USER program's namespaces (not the prelude), so those
        // globals also join Phase A default-construction (known_bugs #75/#76).
        std::function<void(std::vector<StmtPtr>&, bool, bool)> scan =
            [&](std::vector<StmtPtr>& items, bool collectTop, bool userNs) {
                for (StmtPtr& item : items) {
                    if (item->kind == StmtKind::Namespace) scan(item->body, true, userNs);
                    else if (collectTop && item->kind == StmtKind::Var) {
                        if (item->init) globalVars.push_back(item.get());
                        if (userNs) userDefaultVars.push_back(item.get());
                    }
                }
            };
        scan(prelude.items, /*collectTop=*/true,  /*userNs=*/false);
        scan(program.items, /*collectTop=*/false, /*userNs=*/true);
        for (Stmt* v : globalVars)
            mod_->globalIndex[std::string(v->name)] = mod_->nglobals++;
        // known_bugs #75: a BARE user-namespace global (no init) was never given
        // a slot at all (globalVars requires an initializer), so any function
        // reading/writing it failed to lower ("not yet lowerable"). Give it one.
        for (Stmt* v : userDefaultVars)
            if (!mod_->globalIndex.count(std::string(v->name)))
                mod_->globalIndex[std::string(v->name)] = mod_->nglobals++;
        // bug.md #2: the user program's bare TOP-LEVEL Vars are also globals —
        // a function reading `G` lowers separately from @main and can never
        // see a @main-local. They get slots here but keep their initializers
        // in @main (statement order preserved; @ginit would run user side
        // effects before the program's own top-level sequence). A name that
        // is already a global (prelude/namespace collision) is skipped: it
        // keeps today's @main-local behavior rather than clobbering the
        // existing slot (globalIndex is name-keyed). Each also joins Phase A.
        for (StmtPtr& item : program.items)
            if (item->kind == StmtKind::Var &&
                !mod_->globalIndex.count(std::string(item->name))) {
                mod_->globalIndex[std::string(item->name)] = mod_->nglobals++;
                topLevelGlobals_.insert(item.get());
                userDefaultVars.push_back(item.get());
            }
        int gi = (int)mod_->functions.size();
        mod_->functions.emplace_back();
        mod_->functions[gi].name = "@ginit";
        mod_->ginit = gi;
        cur_ = gi;
        curClass_ = nullptr; curAccessor_.clear(); curIsCtor_ = false;
        scopes_.clear(); scopes_.emplace_back();
        freshStructRegs_.clear();
        // Phase A (known_bugs #75/#76/#79/#80): auto-construct every USER global
        // up front — before the namespace-scoped initializers below and before
        // @main — so an initializer or top-level statement always sees a real
        // §3-constructed slot, never an absent/zero one (info.md §3: there is no
        // null/unbound state on declaration). A BARE pre-constructed TOP-LEVEL
        // global is recorded so @main does not re-construct (and clobber a
        // namespace initializer's mutation). Two sub-passes: pure-default types
        // (primitive/string/Array/Map/union/None) first, then class types, so a
        // class ctor run here can read the already-constructed pure defaults.
        auto isClassType = [](const Stmt* v) {
            Symbol* c = v->type ? v->type->resolvedSymbol : nullptr;
            return c && c->kind == SymbolKind::Class && !c->isPrimitive &&
                   c->name != "Array" && c->name != "Map";
        };
        for (Stmt* v : userDefaultVars) {
            if (isClassType(v)) continue;
            int r = newReg();
            emit(Op::Default, r);
            last().sname = v->type ? v->type->canonical : "";
            emit(Op::StoreGlobal, r, mod_->globalIndex[std::string(v->name)]);
            if (topLevelGlobals_.count(v)) preDefaultedGlobals_.insert(v);
        }
        for (Stmt* v : userDefaultVars) {
            if (!isClassType(v)) continue;
            if (v->init) continue;             // explicit initializer constructs it once,
                                               // in its normal phase — never auto-construct
                                               // too (would double-run ctor side effects)
            Symbol* cls = classToAutoConstruct(v->type.get());
            if (!cls) continue;                // interface / non-constructable: leave null
            int r = newReg();
            int initFn = synthesizeInit(cls);   // memoized; safe mid-@ginit
            emit(Op::NewObject, r, initFn);
            last().sym = cls;
            if (const Stmt* ctor = nullaryCtor(cls)) {
                auto it = mod_->byDecl.find(ctor);   // ctor slots exist (collect ran first)
                if (it != mod_->byDecl.end()) {
                    int base = F().nregs;
                    { int rr = newReg(); emit(Op::Move, rr, r); }   // this
                    int dst = newReg();
                    emit(Op::Call, dst, it->second, base, 1);
                }
            }
            emit(Op::StoreGlobal, r, mod_->globalIndex[std::string(v->name)]);
            if (topLevelGlobals_.count(v)) preDefaultedGlobals_.insert(v);
        }
        // Phase B: prelude + user-namespace initializers, in source order. Each
        // overwrites its Phase-A default; a namespace initializer now sees every
        // other global's auto-constructed value instead of an absent slot.
        for (Stmt* v : globalVars) {
            int r = lowerExpr(v->init.get());
            emit(Op::StoreGlobal, r, mod_->globalIndex[std::string(v->name)]);
        }
        emit(Op::RetVoid);
    }
    // Synthesize $init for every class up front (ctor lowering references them).
    for (const auto& sp : sema_.symbols)
        if (sp->kind == SymbolKind::Class && sp->decl && !sp->decl->isInterface &&
            !sp->isPrimitive)
            synthesizeInit(sp.get());

    for (const Pending& p : pending_) lowerPending(p);

    // @main from top-level executable statements.
    int mainIdx = (int)mod_->functions.size();
    mod_->functions.emplace_back();
    mod_->functions[mainIdx].name = "@main";
    mod_->entry = mainIdx;
    cur_ = mainIdx;
    curClass_ = nullptr; curAccessor_.clear(); curIsCtor_ = false;
    scopes_.clear(); scopes_.emplace_back();
    freshStructRegs_.clear();
    for (StmtPtr& item : program.items) {
        switch (item->kind) {
            case StmtKind::Member:
            case StmtKind::Class:
            case StmtKind::Namespace:
                break;
            default:
                lowerStmt(item.get());
        }
    }
    emit(Op::RetVoid);
    return ok_;
}

// ---------------------------------------------------------------------------
//  statements
// ---------------------------------------------------------------------------

// techdesign-02 F3: the exact shape a nullary member call (e.g. `f.close()`)
// takes through lowerCall's dynamic-dispatch path (mirrors ExprKind::Extract's
// `stream >>` template) — dynamic dispatch via CallDyn-by-name so an
// interface-typed `using` slot resolves to whatever the runtime object
// actually is, byte-identical to user-written `f.close()`.
void Lowerer::emitCloseCall(int slotReg, const Stmt* closeDecl) {
    int base = F().nregs;
    { int r = newReg(); emit(Op::Move, r, slotReg); }   // receiver
    int dst = newReg();
    emit(Op::CallDyn, dst, 0, base, 1);
    last().sname = "close";
    // Dispatch close() on the runtime object. If closeDecl is an interface's
    // bodyless requirement (`using IDisposable d = ...`), it is only a hint to
    // the empty requirement — passing it makes CallDyn call the no-op instead
    // of the concrete override, so teardown is silently skipped. Drop the hint
    // there and let CallDyn resolve `close` by name on the actual class.
    bool bodyless = closeDecl && closeDecl->memberBody &&
                    closeDecl->memberBody->kind == StmtKind::Empty;
    last().decl = bodyless ? nullptr : closeDecl;
}

// techdesign-labeled-break-continue.md F5: find-or-create `target`'s chain.
// A using crossed by only one target's traffic (the label-free common case,
// and every labeled case that stays within one loop) gets exactly one entry
// — the pre-existing single-chain shape.
Lowerer::ExitChain& Lowerer::chainFor(std::vector<ExitChain>& chains, const Stmt* target) {
    for (ExitChain& ch : chains)
        if (ch.targetLoop == target) return ch;
    chains.push_back({});
    chains.back().targetLoop = target;
    return chains.back();
}

// The post-order argument (F5 item 4 / problem #5): a using's cleanup group
// emits at the end of the block that declared it, which is (transitively)
// inside its crossing break/continue's TARGET loop's body — the checker's
// lexical-enclosure validation guarantees it — so that loop's own case is
// still on the C++ call stack, and its LoopCtx still on `loops_`, whenever
// this is consulted. A miss means that argument had a hole; callers treat it
// as an internal-error STOP condition, never a workaround.
Lowerer::LoopCtx* Lowerer::loopCtxFor(const Stmt* target) {
    for (LoopCtx& lc : loops_)
        if (lc.stmt == target) return &lc;
    return nullptr;
}

// techdesign-02 F3: the normative cleanup-group layout. For usings declared
// directly in one block, in reverse declaration order, each gets ONE group:
// a fallthrough head (close; Jump <next group / "out">), a landing pad
// (close; Throw <bound value> — the handler for THIS resource's own range,
// which ends exactly where its group begins, never inside it), and lazily an
// return/break/continue stub per crossing edge actually used. A resource's
// entire group sits inside every ENCLOSING resource's range (this block's
// earlier usings, or an outer block's) but never inside its own — that
// ordering is what prevents a close()-throw from re-entering its own pad
// (problem #8: the rev-1 double-close bug this layout replaces).
void Lowerer::lowerUsingCleanupGroups(size_t watermark) {
    Symbol* iexc = sema_.global->lookup("IException");
    int pendingFallthroughJump = -1;
    for (size_t i = usings_.size(); i-- > watermark; ) {
        UsingCtx& ctx = usings_[i];
        int groupStart = (int)F().code.size();     // this resource's OWN range ends here
        if (pendingFallthroughJump >= 0) F().code[pendingFallthroughJump].a = groupStart;

        // Fallthrough head: close, then continue to the next (outer-in-this-
        // block) group's head, or to "out" once the outermost group patches it.
        emitCloseCall(ctx.slotReg, ctx.closeDecl);
        pendingFallthroughJump = emit(Op::Jump, 0);

        // Landing pad: close, rethrow. Registered as the handler for exactly
        // this resource's own range [rangeStart, groupStart) — never a range
        // that includes this group's own code.
        int padPc = (int)F().code.size();
        int bindReg = newReg();
        emitCloseCall(ctx.slotReg, ctx.closeDecl);
        emit(Op::Throw, bindReg);
        Handler h;
        h.start = ctx.rangeStart; h.end = groupStart; h.handlerPc = padPc;
        h.bindReg = bindReg; h.type = iexc;
        F().handlers.push_back(h);

        // Return stub: close, then hop to the next-outer using's return stub
        // (any index in usings_, regardless of block — nesting order IS
        // vector order), or emit the real Ret/RetVoid if this was outermost.
        if (ctx.needRet) {
            int retPc = (int)F().code.size();
            emitCloseCall(ctx.slotReg, ctx.closeDecl);
            for (int j : ctx.retJumps) F().code[j].a = retPc;
            if (i > 0) {
                usings_[i - 1].retJumps.push_back(emit(Op::Jump, 0));
                usings_[i - 1].needRet = true;
            } else if (chainRetIsVoid_) {
                emit(Op::RetVoid);
            } else {
                emit(Op::Ret, chainRetReg_);
            }
        }
        // Break/Continue stubs (techdesign-labeled-break-continue.md F5 item
        // 4): one stub per ExitChain actually recorded — one per DISTINCT
        // target loop this using is crossed for, not just one per using.
        // Each stub closes, then hops to the next-outer using's SAME-target
        // chain only if that using is also inside the chain's own target
        // loop (index at or above THAT target's usingsFloor — never
        // loops_.back()'s, the one-line trap this design exists to name);
        // otherwise this using is the outermost one that particular
        // break/continue crosses, so land directly in the target loop's own
        // F1 jump list, where ordinary loop-end/continue-point patching
        // takes over.
        for (ExitChain& ch : ctx.brkChains) {
            int brkPc = (int)F().code.size();
            emitCloseCall(ctx.slotReg, ctx.closeDecl);
            for (int j : ch.jumps) F().code[j].a = brkPc;
            LoopCtx* target = loopCtxFor(ch.targetLoop);
            if (!target) { fail(SourceSpan{}, "internal: labeled break target not on the loop stack"); continue; }
            if (i > 0 && i - 1 >= target->usingsFloor) {
                chainFor(usings_[i - 1].brkChains, ch.targetLoop).jumps.push_back(emit(Op::Jump, 0));
            } else {
                target->breakJumps.push_back(emit(Op::Jump, 0));
            }
        }
        for (ExitChain& ch : ctx.cntChains) {
            int cntPc = (int)F().code.size();
            emitCloseCall(ctx.slotReg, ctx.closeDecl);
            for (int j : ch.jumps) F().code[j].a = cntPc;
            LoopCtx* target = loopCtxFor(ch.targetLoop);
            if (!target) { fail(SourceSpan{}, "internal: labeled continue target not on the loop stack"); continue; }
            if (i > 0 && i - 1 >= target->usingsFloor) {
                chainFor(usings_[i - 1].cntChains, ch.targetLoop).jumps.push_back(emit(Op::Jump, 0));
            } else {
                target->continueJumps.push_back(emit(Op::Jump, 0));
            }
        }
    }
    // The outermost-in-this-block group's fallthrough jump lands here — pure
    // block-local: it never chains into an enclosing block's using (that
    // would close a resource whose scope hasn't actually ended).
    if (pendingFallthroughJump >= 0) F().code[pendingFallthroughJump].a = (int)F().code.size();
    usings_.resize(watermark);
}

void Lowerer::lowerStmt(Stmt* s) {
    if (!s || !ok_) return;
    switch (s->kind) {
        case StmtKind::Block: {
            scopes_.emplace_back();
            if (s->blockScope) lexical_.pushScope(s->blockScope);
            size_t usingWatermark = usings_.size();
            for (StmtPtr& c : s->body) lowerStmt(c.get());
            if (usings_.size() > usingWatermark) lowerUsingCleanupGroups(usingWatermark);
            if (s->blockScope) lexical_.pop();
            scopes_.pop_back();
            break;
        }
        case StmtKind::Var: {
            // bug.md #2: a bare top-level Var initializes its GLOBAL slot (in
            // order, here in @main) instead of binding a @main-local — every
            // read, @main's included, then goes through LoadGlobal, so writes
            // from functions and top level observe one storage location.
            bool topGlobal = topLevelGlobals_.count(s) != 0;
            // known_bugs #75/#76/#79/#80: a BARE pure-default top-level global was
            // already auto-constructed in @ginit (Phase A), before any namespace
            // initializer ran. Re-defaulting it here would clobber a mutation such
            // an initializer performed, so skip it — the slot already holds the
            // correct value. A top-level global WITH an explicit initializer still
            // runs it here, in source order (bug.md #2), overwriting the default.
            if (topGlobal && !s->init && preDefaultedGlobals_.count(s)) break;
            int r = newReg();
            if (!topGlobal) scopes_.back()[s->name] = r;
            if (s->init) {
                int rv = lowerExpr(s->init.get());               // value structs copy on bind
                ExprKind k = s->init->kind;
                // A fresh literal is uniquely owned -> move it in (no retain), so a
                // subsequent `a = a.add(...)` can mutate the buffer in place. A Name
                // or call result may be shared, so retain (plain Move).
                Op op = s->init->mayBeValueStruct ? Op::CopyVal
                      : (k == ExprKind::Array || k == ExprKind::Range) ? Op::MoveClear
                      : Op::Move;
                emit(op, r, rv);
                // The init expr's flag misses a constructor call (`Point()` isn't
                // typed via typeOf), so fall back to the DECLARED type `Point p`.
                if (op == Op::CopyVal &&
                    (s->init->definiteValueStruct ||
                     (s->type && s->type->resolvedSymbol && s->type->resolvedSymbol->isValue &&
                      !s->type->resolvedSymbol->isInterface())))
                    last().c = 1;
                maybeVFree(rv);            // §15: fresh struct result copied into the local
            } else if (Symbol* cls = classToAutoConstruct(s->type.get())) {
                // §3: bare declaration of a class type auto-constructs (run $init
                // + any nullary ctor) — no null/unbound state.
                int initFn = synthesizeInit(cls);
                emit(Op::NewObject, r, initFn);
                last().sym = cls;
                if (const Stmt* ctor = nullaryCtor(cls)) {
                    auto it = mod_->byDecl.find(ctor);
                    if (it != mod_->byDecl.end()) {
                        int base = F().nregs;
                        { int rr = newReg(); emit(Op::Move, rr, r); }   // this
                        int dst = newReg();
                        emit(Op::Call, dst, it->second, base, 1);
                    }
                }
            } else {
                emit(Op::Default, r);
                last().sname = s->type ? s->type->canonical : "";
            }
            if (topGlobal) {
                auto g = mod_->globalIndex.find(std::string(s->name));
                if (g != mod_->globalIndex.end()) emit(Op::StoreGlobal, r, g->second);
            }
            if (s->isUsing) {
                UsingCtx ctx;
                ctx.slotReg = r;
                ctx.closeDecl = s->usingClose;
                ctx.rangeStart = (int)F().code.size();
                usings_.push_back(std::move(ctx));
            }
            break;
        }
        case StmtKind::ExprStmt: {
            int r = lowerExpr(s->expr.get());
            // §15: a discarded fresh struct result (bare call statement, or the
            // rhs temp of an assignment statement — already copied by
            // lowerAssign) is dead here. Chained expression uses never reach a
            // statement boundary with the mark still set, so this stays safe.
            maybeVFree(r);
            break;
        }
        case StmtKind::Return:
            if (usings_.empty()) {
                // byte-identical to pre-F3 lowering when no using is active.
                if (s->expr) {
                    int rv = lowerExpr(s->expr.get());
                    if (s->expr->mayBeValueStruct) {    // return by value for value structs
                        int rc = newReg();
                        emit(Op::CopyVal, rc, rv);
                        if (s->expr->definiteValueStruct) last().c = 1;
                        maybeVFree(rv);    // §15: chained struct call copied into the return
                        emit(Op::Ret, rc);
                    } else emit(Op::Ret, rv);
                } else emit(Op::RetVoid);
            } else {
                // techdesign-02 F3: a using is active — compute the value as
                // usual, funnel it into the per-function chain register, then
                // jump into the innermost active using's cleanup-group stub
                // chain instead of returning directly. The chain's outermost
                // stub emits the real Ret/RetVoid once every using has closed.
                if (chainRetReg_ < 0) {
                    chainRetReg_ = newReg();
                    chainRetIsVoid_ = !s->expr;
                }
                if (s->expr) {
                    int rv = lowerExpr(s->expr.get());
                    if (s->expr->mayBeValueStruct) {
                        int rc = newReg();
                        emit(Op::CopyVal, rc, rv);
                        if (s->expr->definiteValueStruct) last().c = 1;
                        maybeVFree(rv);
                        emit(Op::Move, chainRetReg_, rc);
                    } else emit(Op::Move, chainRetReg_, rv);
                }
                usings_.back().retJumps.push_back(emit(Op::Jump, 0));
                usings_.back().needRet = true;
            }
            break;
        case StmtKind::Throw:
            emit(Op::Throw, lowerExpr(s->expr.get()));
            break;
        case StmtKind::Try: {
            int start = (int)F().code.size();
            lowerStmt(s->thenBranch.get());
            int end = (int)F().code.size();
            std::vector<int> exits;
            exits.push_back(emit(Op::Jump, 0));
            for (const CatchClause& c : s->catches) {
                Handler h;
                h.start = start; h.end = end;
                h.handlerPc = (int)F().code.size();
                h.bindReg = newReg();
                h.type = c.type ? c.type->resolvedSymbol : nullptr;
                F().handlers.push_back(h);
                scopes_.emplace_back();
                if (!c.name.empty()) scopes_.back()[c.name] = h.bindReg;
                lowerStmt(c.body.get());
                scopes_.pop_back();
                exits.push_back(emit(Op::Jump, 0));
            }
            for (int j : exits) F().code[j].a = (int)F().code.size();
            break;
        }
        case StmtKind::If: {
            int c = lowerExpr(s->expr.get());
            int jElse = emit(Op::JumpIfFalse, c, 0);
            lowerStmt(s->thenBranch.get());
            if (s->elseBranch) {
                int jEnd = emit(Op::Jump, 0);
                F().code[jElse].b = (int)F().code.size();
                lowerStmt(s->elseBranch.get());
                F().code[jEnd].a = (int)F().code.size();
            } else {
                F().code[jElse].b = (int)F().code.size();
            }
            break;
        }
        case StmtKind::While: {
            int top = (int)F().code.size();
            int c = lowerExpr(s->expr.get());
            int jEnd = emit(Op::JumpIfFalse, c, 0);
            loops_.push_back({}); loops_.back().usingsFloor = usings_.size(); loops_.back().stmt = s;
            lowerStmt(s->thenBranch.get());
            emit(Op::Jump, top);
            int endPos = (int)F().code.size();
            F().code[jEnd].b = endPos;
            for (int j : loops_.back().continueJumps) F().code[j].a = top;
            for (int j : loops_.back().breakJumps) F().code[j].a = endPos;
            loops_.pop_back();
            break;
        }
        case StmtKind::DoWhile: {
            int top = (int)F().code.size();
            loops_.push_back({}); loops_.back().usingsFloor = usings_.size(); loops_.back().stmt = s;
            lowerStmt(s->thenBranch.get());
            int contPos = (int)F().code.size();
            for (int j : loops_.back().continueJumps) F().code[j].a = contPos;
            int c = lowerExpr(s->expr.get());
            emit(Op::JumpIfTrue, c, top);
            int endPos = (int)F().code.size();
            for (int j : loops_.back().breakJumps) F().code[j].a = endPos;
            loops_.pop_back();
            break;
        }
        case StmtKind::For: {
            scopes_.emplace_back();
            lowerStmt(s->forInit.get());
            int top = (int)F().code.size();
            int jEnd = -1;
            if (s->expr) jEnd = emit(Op::JumpIfFalse, lowerExpr(s->expr.get()), 0);
            loops_.push_back({}); loops_.back().usingsFloor = usings_.size(); loops_.back().stmt = s;
            lowerStmt(s->thenBranch.get());
            int stepPos = (int)F().code.size();
            for (int j : loops_.back().continueJumps) F().code[j].a = stepPos;
            if (s->forStep) lowerExpr(s->forStep.get());
            emit(Op::Jump, top);
            int endPos = (int)F().code.size();
            if (jEnd >= 0) F().code[jEnd].b = endPos;
            for (int j : loops_.back().breakJumps) F().code[j].a = endPos;
            loops_.pop_back();
            scopes_.pop_back();
            break;
        }
        case StmtKind::ForIn: {
            scopes_.emplace_back();
            if (s->expr->kind == ExprKind::Range) {
                // counted loop, no Range object
                int i = newReg();
                scopes_.back()[s->name] = i;
                int lo = lowerExpr(s->expr->a.get());
                int hi = lowerExpr(s->expr->b.get());
                int hiC = newReg();
                emit(Op::Move, i, lo);
                emit(Op::Move, hiC, hi);
                int top = (int)F().code.size();
                int cond = newReg();
                emit(Op::Arith, cond, i, hiC); last().tk = TokenKind::Le;
                int jEnd = emit(Op::JumpIfFalse, cond, 0);
                loops_.push_back({}); loops_.back().usingsFloor = usings_.size(); loops_.back().stmt = s;
                lowerStmt(s->thenBranch.get());
                int incPos = (int)F().code.size();
                for (int j : loops_.back().continueJumps) F().code[j].a = incPos;
                int one = newReg();
                emit(Op::LoadConst, one, addConst(vint(1)));
                emit(Op::Arith, i, i, one); last().tk = TokenKind::Plus;
                emit(Op::Jump, top);
                int endPos = (int)F().code.size();
                F().code[jEnd].b = endPos;
                for (int j : loops_.back().breakJumps) F().code[j].a = endPos;
                loops_.pop_back();
            } else if (s->forInProtocol) {
                // techdesign-07 §2: the iterator protocol, lowered to ordinary
                // CallDyn (no new IR op; zero backend work). The desugar is
                //   var __it = e.iterator();
                //   while (__it.hasNext()) { x = __it.next(); <body> }
                // Each call is a name-dispatched CallDyn (decl left null) so it
                // resolves on the runtime object's class, exactly like a
                // user-written `__it.hasNext()`. Reuses the Track-02 loop-ctx for
                // break/continue.
                auto callNullary = [&](const char* method, int recv, int dst) {
                    int base = F().nregs;
                    { int r = newReg(); emit(Op::Move, r, recv); }   // window[0] = receiver
                    emit(Op::CallDyn, dst, 0, base, 1);
                    last().sname = method;
                };
                int iterable = lowerExpr(s->expr.get());
                int itReg = newReg();
                callNullary("iterator", iterable, itReg);
                int elem = newReg();
                scopes_.back()[s->name] = elem;
                int top = (int)F().code.size();
                int cond = newReg();
                callNullary("hasNext", itReg, cond);
                int jEnd = emit(Op::JumpIfFalse, cond, 0);
                callNullary("next", itReg, elem);
                loops_.push_back({}); loops_.back().usingsFloor = usings_.size(); loops_.back().stmt = s;
                lowerStmt(s->thenBranch.get());
                // continue re-checks hasNext (there is no separate step slot):
                // route continue jumps straight back to the loop head.
                for (int j : loops_.back().continueJumps) F().code[j].a = top;
                emit(Op::Jump, top);
                int endPos = (int)F().code.size();
                F().code[jEnd].b = endPos;
                for (int j : loops_.back().breakJumps) F().code[j].a = endPos;
                loops_.pop_back();
            } else {
                // general: iterate arrays/maps/range values by index
                int iter = lowerExpr(s->expr.get());
                int len = newReg();
                emit(Op::IterLen, len, iter);
                int idx = newReg();
                emit(Op::LoadConst, idx, addConst(vint(0)));
                int elem = newReg();
                scopes_.back()[s->name] = elem;
                // techdesign-columnar §5.5: iterating a columnar array GATHERS a
                // fresh rc=0 record into `elem` each step (IterAt), so — unlike the
                // dense record-pointer alias — each must be reclaimed or the loop
                // leaks O(N). Reclaim the PREVIOUS gather at the top of each step
                // (void on the first step; correct across break/continue since the
                // free is before IterAt) plus the final one after the loop. Gated
                // on a columnar-eligible declared loop-var type so the dense path
                // (whose IterAt aliases the buffer) is never VFree'd. (Non-flat
                // value structs live boxed but IterAt ALIASES them too — borrowed,
                // never VFree'd — per the #66 runtime fix, so no extra case here.)
                bool colElem = columnar_ && s->type && s->type->resolvedSymbol &&
                               columnarEligibleStruct(s->type->resolvedSymbol);
                int top = (int)F().code.size();
                int cond = newReg();
                emit(Op::Arith, cond, idx, len); last().tk = TokenKind::Lt;
                int jEnd = emit(Op::JumpIfFalse, cond, 0);
                if (colElem) emit(Op::VFree, elem);   // free the prior step's gather
                emit(Op::IterAt, elem, iter, idx);
                loops_.push_back({}); loops_.back().usingsFloor = usings_.size(); loops_.back().stmt = s;
                lowerStmt(s->thenBranch.get());
                int incPos = (int)F().code.size();
                for (int j : loops_.back().continueJumps) F().code[j].a = incPos;
                int one = newReg();
                emit(Op::LoadConst, one, addConst(vint(1)));
                emit(Op::Arith, idx, idx, one); last().tk = TokenKind::Plus;
                emit(Op::Jump, top);
                int endPos = (int)F().code.size();
                F().code[jEnd].b = endPos;
                for (int j : loops_.back().breakJumps) F().code[j].a = endPos;
                loops_.pop_back();
                if (colElem) emit(Op::VFree, elem);   // reclaim the final gather
                // bug #66: a VALUE-STRUCT loop variable ALIASES the array element
                // (a dense buffer record, or a boxed element separately alloc'd),
                // it does not own it — the array does. Clear the register after
                // the loop so the stale alias is not released at function-scope
                // exit: once the array is reassigned/freed (e.g. `items = next`
                // inside the same method), releasing that alias reads a freed
                // value struct's classId (a use-after-free — harmless no-op while
                // arrays were always dense, a crash now that a heap-field struct's
                // array is boxed and its element is individually freed).
                else if (s->type && s->type->resolvedSymbol &&
                         s->type->resolvedSymbol->isValue)
                    emit(Op::LoadConst, elem, addConst(vvoid()));
            }
            scopes_.pop_back();
            break;
        }
        case StmtKind::Break: {
            // techdesign-labeled-break-continue.md F5: resolve the target
            // LoopCtx — the checker-resolved label target if labeled,
            // otherwise the innermost loop (loops_.back(), unchanged).
            LoopCtx* target = nullptr;
            if (s->labelTarget) {
                target = loopCtxFor(s->labelTarget);
                if (!target) { fail(s->span, "internal: labeled break target not on the loop stack"); break; }
            } else {
                if (loops_.empty()) { fail(s->span, "internal: 'break' outside a loop"); break; }
                target = &loops_.back();
            }
            // techdesign-02 F3: a using declared inside the TARGET loop (at
            // or above ITS usingsFloor — never loops_.back()'s, the one-line
            // trap this design exists to name) must close before the jump —
            // route through its cleanup-group stub instead of jumping directly.
            if (usings_.size() > target->usingsFloor) {
                chainFor(usings_.back().brkChains, target->stmt).jumps.push_back(emit(Op::Jump, 0));
            } else {
                target->breakJumps.push_back(emit(Op::Jump, 0));
            }
            break;
        }
        case StmtKind::Continue: {
            LoopCtx* target = nullptr;
            if (s->labelTarget) {
                target = loopCtxFor(s->labelTarget);
                if (!target) { fail(s->span, "internal: labeled continue target not on the loop stack"); break; }
            } else {
                if (loops_.empty()) { fail(s->span, "internal: 'continue' outside a loop"); break; }
                target = &loops_.back();
            }
            if (usings_.size() > target->usingsFloor) {
                chainFor(usings_.back().cntChains, target->stmt).jumps.push_back(emit(Op::Jump, 0));
            } else {
                target->continueJumps.push_back(emit(Op::Jump, 0));
            }
            break;
        }
        case StmtKind::Empty:
        case StmtKind::Bind:
        case StmtKind::Use:
        case StmtKind::UsesImport:
            break;
        default:
            fail(s->span, "this statement form");
    }
}

// ---------------------------------------------------------------------------
//  member keys, assignment
// ---------------------------------------------------------------------------

// this.A::v  /  obj.name — compute base register + "name" or "Source::name".
bool Lowerer::memberKey(Expr* e, int& baseReg, std::string& key) {
    if (e->kind != ExprKind::Member) return false;
    Expr* base = e->a.get();
    // base-view narrowing: base is Member{X, ClassName} where ClassName is a class
    if (base->kind == ExprKind::Member) {
        if (const std::vector<Symbol*>* v = sema_.global->localLookup(base->text)) {
            for (Symbol* s : *v)
                if (s->kind == SymbolKind::Class) {
                    baseReg = lowerExpr(base->a.get());
                    key = std::string(base->text) + "::" + std::string(e->text);
                    return true;
                }
        }
    }
    baseReg = lowerExpr(base);
    key = std::string(e->text);
    return true;
}

// Copy each argument into the call window — a value-struct arg copies (the
// callee's parameter is its own value), non-struct args just move (§9 gate).
void Lowerer::emitArgCopies(const std::vector<int>& argRegs, const std::vector<ExprPtr>& args) {
    for (size_t k = 0; k < argRegs.size(); ++k) {
        bool mayCopy = k >= args.size() || !args[k] || args[k]->mayBeValueStruct;
        emit(mayCopy ? Op::CopyVal : Op::Move, newReg(), argRegs[k]);
        if (mayCopy && k < args.size() && args[k] && args[k]->definiteValueStruct)
            last().c = 1;
        maybeVFree(argRegs[k]);            // §15: a fresh struct arg is copied out — dead
    }
}

// §15: mark a call result that is a FRESH standalone value-struct copy — the
// callee's return-site CopyVal, which escaped to the heap by being returned.
// Only in-language calls qualify (their Ret always deep-copies): Op::Call,
// Op::CallValue (closures), and CallDyn resolved to a lowered method. Native
// dispatch is excluded — e.g. Array<Struct>.at returns a pointer INTO the dense
// buffer, not a standalone copy; freeing that would corrupt the buffer. The
// optChain path is excluded too (its Moves are the last writer, and the None
// short-circuit may skip the call entirely).
void Lowerer::noteFreshStructResult(const Expr* e, int reg) {
    if (!e || !e->definiteValueStruct || !ok_) return;
    for (int i = (int)F().code.size() - 1; i >= 0; --i) {
        const Inst& in = F().code[i];
        if (in.a != reg) continue;                       // find reg's last writer
        bool fresh = in.op == Op::Call || in.op == Op::CallValue ||
                     (in.op == Op::CallDyn && in.decl && mod_->byDecl.count(in.decl));
        if (fresh) freshStructRegs_.insert(reg);
        return;
    }
}

// §15: if reg holds a fresh struct copy that has just been consumed (copied out),
// free it. Unconsumed marks simply expire with the function (a leak, never a
// premature free — the safe direction).
void Lowerer::maybeVFree(int reg) {
    auto it = freshStructRegs_.find(reg);
    if (it == freshStructRegs_.end()) return;
    freshStructRegs_.erase(it);
    emit(Op::VFree, reg);
}

// The namespace symbol `name` resolves to at `offset` (bug.md #1): nearest
// block import overlay first (a block-scoped `uses NS;` / `use NS::Sub as X;`
// binds the namespace in Stmt::blockScope, which sema_.global and the file
// overlay never see), then the call site's file overlay chain, then global.
// A nearer NON-namespace binding shadows (same break-on-first-hit rule the
// file-overlay scan always had). Null = not a namespace here.
Symbol* Lowerer::namespaceSym(std::string_view name, uint32_t offset) {
    Symbol* fromBlocks = nullptr;
    if (lexical_.namespaceInFrames(name, fromBlocks))
        return fromBlocks;                          // block binding wins (null = shadowed)
    for (const Scope* sc = sema_.fileScopeFor(offset); sc; sc = sc->parent)
        if (const std::vector<Symbol*>* v = sc->localLookup(name)) {
            for (Symbol* s : *v)
                if (s->kind == SymbolKind::Namespace) return s;
            return nullptr;
        }
    return nullptr;
}

Symbol* Lowerer::namespaceChainSym(const Expr* e, uint32_t offset) {
    if (!e) return nullptr;
    if (e->kind == ExprKind::Name) return namespaceSym(e->text, offset);
    if (e->kind == ExprKind::Member && e->colon) {
        Symbol* base = namespaceChainSym(e->a.get(), offset);
        if (base && base->scope)
            if (const std::vector<Symbol*>* v = base->scope->localLookup(e->text))
                for (Symbol* s : *v)
                    if (s->kind == SymbolKind::Namespace) return s;
    }
    return nullptr;
}

void Lowerer::lowerAssign(Expr* lhs, int valueReg, const Expr* rhs) {
    // value structs copy into their slot; a provably non-struct rhs just moves
    const bool weakTarget = lhs && lhs->weakField;
    Op bind = (rhs && !rhs->mayBeValueStruct) ? Op::Move : Op::CopyVal;
    if (!weakTarget) {
        int c = newReg(); emit(bind, c, valueReg);
        if (bind == Op::CopyVal && rhs && rhs->definiteValueStruct)
            last().c = 1;                  // §15: definite copy = its own alloc site
        valueReg = c;
    }
    auto dropWeakTemporary = [&] {
        // A call/construction used only as the RHS has no source binding. The
        // weak slot must not extend its register lifetime to function exit.
        if (weakTarget && rhs && rhs->kind == ExprKind::Call)
            emit(Op::LoadConst, valueReg, addConst(vnone()));
    };
    if (lhs->kind == ExprKind::Name) {
        if (int* r = findLocal(lhs->text)) { emit(Op::Move, *r, valueReg); return; }
        if (curClass_ && classHasMember(curClass_, lhs->text)) {
            // implicit member of `this`; raw inside the field's own accessor, or
            // when the slot is stable and no accessor intercepts it anywhere.
            bool selfAcc = (lhs->text == curAccessor_);
            int slot = packedSlot(curClass_, lhs->text, /*requireNoAccessor=*/!selfAcc);
            Op op = (lhs->weakDirect || (lhs->weakField && selfAcc)) ? Op::RawSetWeak
                                   : ((selfAcc || slot > 0) ? Op::RawSet : Op::SetMember);
            emit(op, valueReg, thisReg());
            last().sname = std::string(lhs->text);
            if (op == Op::RawSet || op == Op::RawSetWeak) last().d = slot;
            dropWeakTemporary();
            return;
        }
        // bug.md #2: a bare top-level global (or a non-const namespace global,
        // which the checker permits writing) stores through its global slot.
        // Gated on "no receiver" to mirror the oracle's assignment precedence
        // (a method's implicit-this write wins there).
        if (thisReg() < 0) {
            std::string key = lhs->resolved ? std::string(lhs->resolved->name)
                                             : std::string(lhs->text);
            auto g = mod_->globalIndex.find(key);
            if (g != mod_->globalIndex.end()) {
                emit(Op::StoreGlobal, valueReg, g->second);
                return;
            }
        }
        fail(lhs->span, "assignment to '" + std::string(lhs->text) + "'");
        return;
    }
    if (lhs->kind == ExprKind::Member) {
        int baseReg; std::string key;
        if (memberKey(lhs, baseReg, key)) {
            // this.field with a stable, accessor-free slot → direct store.
            int slot = (lhs->a->kind == ExprKind::This && curClass_)
                           ? packedSlot(curClass_, key, /*requireNoAccessor=*/true) : 0;
            emit(lhs->weakDirect ? Op::RawSetWeak : (slot > 0 ? Op::RawSet : Op::SetMember),
                 valueReg, baseReg);
            last().sname = key;
            if (slot > 0) last().d = slot;
            dropWeakTemporary();
            return;
        }
    }
    if (lhs->kind == ExprKind::Index) {
        int baseReg = lowerExpr(lhs->a.get());
        int idxReg = lowerExpr(lhs->b.get());
        int nb = newReg();
        emit(Op::IndexStore, nb, baseReg, idxReg, valueReg);
        lowerAssign(lhs->a.get(), nb);    // write back (identity for objects)
        return;
    }
    fail(lhs->span, "this assignment target");
}

// ---------------------------------------------------------------------------
//  calls
// ---------------------------------------------------------------------------

int Lowerer::lowerCall(Expr* e) {
    Expr* callee = e->a.get();

    // console.write / console.writeln — by receiver name, or by checker
    // resolution to a Console native (aliased receivers lower to Print too).
    bool isConsole = callee->kind == ExprKind::Member &&
                     callee->a->kind == ExprKind::Name &&
                     callee->a->text == "console" && !findLocal("console") &&
                     (callee->text == "write" || callee->text == "writeln");
    if (!isConsole && callee->kind == ExprKind::Member && e->resolved &&
        consoleMembers_.count(e->resolved)) {
        // keep receiver side effects when it isn't just a bare name
        if (callee->a->kind != ExprKind::Name) lowerExpr(callee->a.get());
        isConsole = true;
    }
    if (isConsole) {
        for (const ExprPtr& arg : e->list) {
            emit(Op::Print, lowerExpr(arg.get()));
            last().b = arg->evalKind;          // codegen hint (bool/string/float)
        }
        if (callee->text == "writeln") emit(Op::PrintNl);
        return newReg();
    }

    // Construction: checker-resolved, or (prelude bodies) statically detectable.
    // Track 03 §2: a call the checker resolved to a callable non-ctor (e.g.
    // `Enum::fromCode`) is NOT construction — don't let the name-based ctor
    // fallback below claim `Enum::fromCode(...)` as `Enum(...)` construction.
    bool resolvedToFreeFn = e->resolved && e->resolved->kind == StmtKind::Member &&
                            e->resolved->callable && !e->resolved->isCtor &&
                            !e->resolvedClass;
    if (resolvedToFreeFn) {
        auto it = mod_->byDecl.find(e->resolved);
        if (it != mod_->byDecl.end() && !mod_->functions[it->second].hasThis) {
            std::vector<int> argRegs;
            for (const ExprPtr& arg : e->list) argRegs.push_back(lowerExpr(arg.get()));
            int base = F().nregs;
            emitArgCopies(argRegs, e->list);
            int dst = newReg();
            emit(Op::Call, dst, it->second, base, (int)argRegs.size());
            return dst;
        }
    }
    Symbol* ctorClass = e->resolvedClass;
    std::string label;
    if (ctorClass) {
        label = std::string(callee->kind == ExprKind::Member ? callee->text : callee->text);
    } else if (!resolvedToFreeFn) {
        Expr* nameExpr = nullptr;
        if (callee->kind == ExprKind::Name) { nameExpr = callee; }
        else if (callee->kind == ExprKind::Member && callee->a->kind == ExprKind::Name)
            nameExpr = callee->a.get();
        if (nameExpr)
            if (const std::vector<Symbol*>* v = sema_.global->localLookup(nameExpr->text))
                for (Symbol* s : *v)
                    if (s->kind == SymbolKind::Class) {
                        ctorClass = s;
                        label = std::string(callee->kind == ExprKind::Member
                                                ? callee->text : callee->text);
                        break;
                    }
        // bug.md #32: `ns::Class(...)` — base is a namespace, not a class;
        // descend into its scope the same way the function-call path does
        // (namespaceSym honors block import overlays, matching bug #1).
        if (!ctorClass && callee->kind == ExprKind::Member && callee->colon &&
            (callee->a->kind == ExprKind::Name ||
             (callee->a->kind == ExprKind::Member && callee->a->colon))) {
            // bug #37/#46: `A::B::T(...)` — the qualifier is a nested chain.
            Symbol* nsSym = callee->a->kind == ExprKind::Name
                ? namespaceSym(callee->a->text, e->span.offset)
                : namespaceChainSym(callee->a.get(), e->span.offset);
            if (nsSym && nsSym->scope)
                if (const std::vector<Symbol*>* v = nsSym->scope->localLookup(callee->text))
                    for (Symbol* s : *v)
                        if (s->kind == SymbolKind::Class) {
                            ctorClass = s;
                            label = std::string(callee->text);
                            break;
                        }
        }
        // bug.md #32 M2: bare `Class(...)` inside its own namespace, unchecked
        // prelude code — descend into the CURRENTLY LOWERING free function's
        // enclosing namespace (curNamespace_, from Stmt::enclosingNs) the same
        // way the qualified form above descends via an explicit `ns::` name.
        if (!ctorClass && callee->kind == ExprKind::Name && curNamespace_ &&
            curNamespace_->scope) {
            if (const std::vector<Symbol*>* v =
                    curNamespace_->scope->localLookup(callee->text))
                for (Symbol* s : *v)
                    if (s->kind == SymbolKind::Class) {
                        ctorClass = s;
                        label = std::string(callee->text);
                        break;
                    }
        }
    }
    if (ctorClass) {
        // find the ctor decl: checker-resolved, else by label + arity
        const Stmt* ctor = e->resolved;
        if (!ctor && ctorClass->decl)
            for (const StmtPtr& m : ctorClass->decl->body)
                if (m->isCtor && m->name == label && m->params.size() == e->list.size())
                    { ctor = m.get(); break; }

        // base-ctor call inside a constructor: applies to `this`, no allocation
        if (curIsCtor_ && curClass_ && isBaseOrSelf(curClass_, ctorClass)) {
            if (!ctor) return newReg();          // default base ctor: nothing to run
            auto it = mod_->byDecl.find(ctor);
            if (it == mod_->byDecl.end()) { fail(e->span, "base constructor"); return 0; }
            std::vector<int> argRegs;
            for (const ExprPtr& arg : e->list) argRegs.push_back(lowerExpr(arg.get()));
            int base = F().nregs;
            { int r = newReg(); emit(Op::Move, r, 0); }          // this
            emitArgCopies(argRegs, e->list);
            int dst = newReg();
            emit(Op::Call, dst, it->second, base, 1 + (int)argRegs.size());
            return dst;
        }

        if (ctorClass->name == "Array") {
            std::vector<int> argRegs;
            for (const ExprPtr& arg : e->list) argRegs.push_back(lowerExpr(arg.get()));
            int base = F().nregs;
            emitArgCopies(argRegs, e->list);
            int dst = newReg();
            emit(Op::NewArraySized, dst, 0, base, (int)argRegs.size());
            return dst;
        }
        if (ctorClass->name == "Map") { int dst = newReg(); emit(Op::NewMap, dst); return dst; }
        // Track 03 §3: Block(n) / Block::fromString(s) construct a byte buffer.
        if (ctorClass->name == "Block") {
            int arg = e->list.empty() ? -1 : lowerExpr(e->list[0].get());
            int dst = newReg();
            emit(label == "fromString" ? Op::NewBlockStr : Op::NewBlock, dst, 0, arg);
            return dst;
        }

        // allocate + $init + ctor body (if any); synthesize the $init on
        // demand (declaration order in the prelude must not matter)
        int obj = newReg();
        int initFn = -1;
        if (ctorClass->decl && !ctorClass->decl->isInterface && !ctorClass->isPrimitive)
            initFn = synthesizeInit(ctorClass);
        emit(Op::NewObject, obj, initFn);
        // techdesign-generic-value-struct-columnar: stamp the monomorphized class
        // symbol (its own classId) for an eligible generic value-struct instantiation
        // so `Array<Pair<int,int>>` flips columnar; `valueClass` is `ctorClass`
        // itself for non-generic / ineligible structs, so this is a no-op there.
        last().sym = (e->valueClass && e->valueClass->isValue) ? e->valueClass : ctorClass;
        if (ctor) {
            auto it = mod_->byDecl.find(ctor);
            if (it == mod_->byDecl.end()) { fail(e->span, "constructor body"); return obj; }
            std::vector<int> argRegs;
            for (const ExprPtr& arg : e->list) argRegs.push_back(lowerExpr(arg.get()));
            int base = F().nregs;
            { int r = newReg(); emit(Op::Move, r, obj); }
            emitArgCopies(argRegs, e->list);
            int dst = newReg();
            emit(Op::Call, dst, it->second, base, 1 + (int)argRegs.size());
        } else if (!e->list.empty()) {
            // bug #38: no explicit constructor, but positional args — populate
            // the declared data fields positionally in slot (declaration) order,
            // mirroring the oracle's runCtor fallback and synthesizeInit's slot
            // walk. NewObject already seeded field defaults, so each RawSet
            // overwrites its field's default (RawSet does the slot's ARC).
            std::vector<int> argRegs;
            for (const ExprPtr& arg : e->list) argRegs.push_back(lowerExpr(arg.get()));
            int fieldIdx = 0;
            size_t ai = 0;
            for (const Slot& s : ctorClass->shape.slots) {
                if (s.isMethod) continue;
                if (ai < argRegs.size()) {
                    Symbol* fcls = s.decl && s.decl->type ? s.decl->type->resolvedSymbol : nullptr;
                    bool valueField = fcls && fcls->kind == SymbolKind::Class && fcls->isValue &&
                                      !fcls->isPrimitive && fcls->decl && !fcls->decl->isInterface;
                    int v = argRegs[ai];
                    if (valueField) { int c = newReg(); emit(Op::CopyVal, c, v); last().c = 1; v = c; }
                    std::string key = (s.distinct && s.source)
                        ? std::string(s.source->name) + "::" + std::string(s.name)
                        : std::string(s.name);
                    emit(Op::RawSet, v, obj);
                    last().sname = key;
                    last().d = fieldIdx + 1;
                    ++ai;
                }
                ++fieldIdx;
            }
        }
        return obj;
    }

    // Name callee: closure local, resolved free function, or self-method
    if (callee->kind == ExprKind::Name) {
        if (int* r = findLocal(callee->text)) {
            // callable value (function-typed param / stored closure)
            std::vector<int> argRegs;
            for (const ExprPtr& arg : e->list) argRegs.push_back(lowerExpr(arg.get()));
            int base = F().nregs;
            { int rr = newReg(); emit(Op::Move, rr, *r); }
            for (int rr : argRegs) emit(Op::Move, newReg(), rr);
            int dst = newReg();
            emit(Op::CallValue, dst, 0, base, 1 + (int)argRegs.size());
            return dst;
        }
        if (e->resolved && e->resolved->memberBody &&
            e->resolved->memberBody->kind == StmtKind::Empty) {
            std::vector<int> argRegs;
            for (const ExprPtr& arg : e->list) argRegs.push_back(lowerExpr(arg.get()));
            int base = F().nregs;
            emitArgCopies(argRegs, e->list);
            int dst = newReg();
            emit(Op::CallNativeFn, dst, 0, base, (int)argRegs.size());
            last().sname = std::string(callee->text);
            return dst;
        }
        if (e->resolved) {
            auto it = mod_->byDecl.find(e->resolved);
            if (it != mod_->byDecl.end()) {
                bool needThis = mod_->functions[it->second].hasThis;
                std::vector<int> argRegs;
                for (const ExprPtr& arg : e->list) argRegs.push_back(lowerExpr(arg.get()));
                int base = F().nregs;
                // bug.md #53: the implicit receiver of a bare self-method call is
                // `this`, which is r0 in a member body but the *captured* receiver
                // inside a lambda body (r0 there is the closure). thisReg() picks
                // the right one; a hardcoded 0 silently called on the closure.
                if (needThis) { int r = newReg(); emit(Op::Move, r, thisReg()); }
                emitArgCopies(argRegs, e->list);
                int dst = newReg();
                emit(Op::Call, dst, it->second, base,
                     (needThis ? 1 : 0) + (int)argRegs.size());
                return dst;
            }
        }
        // unqualified FREE-function call in an unchecked prelude body: resolve
        // by name through the global scope (matches the oracle's fallback).
        if (!e->resolved) {
            const Stmt* fnDecl = nullptr;
            for (const Scope* sc = sema_.global; sc && !fnDecl; sc = sc->parent)
                if (const std::vector<Symbol*>* v = sc->localLookup(callee->text))
                    for (Symbol* s : *v)
                        if (s->kind == SymbolKind::Function && s->decl) {
                            fnDecl = s->decl;
                            break;
                        }
            if (fnDecl && !(curClass_ && classHasMember(curClass_, callee->text))) {
                std::vector<int> argRegs;
                for (const ExprPtr& arg : e->list) argRegs.push_back(lowerExpr(arg.get()));
                int base = F().nregs;
                emitArgCopies(argRegs, e->list);
                int dst = newReg();
                if (fnDecl->memberBody && fnDecl->memberBody->kind == StmtKind::Empty) {
                    emit(Op::CallNativeFn, dst, 0, base, (int)argRegs.size());
                    last().sname = std::string(callee->text);
                } else {
                    auto it = mod_->byDecl.find(fnDecl);
                    if (it == mod_->byDecl.end()) { fail(e->span, "prelude function"); return 0; }
                    emit(Op::Call, dst, it->second, base, (int)argRegs.size());
                }
                return dst;
            }
        }
        // unqualified self-method call (incl. prelude bodies, unresolved)
        if (curClass_ && classHasMember(curClass_, callee->text)) {
            std::vector<int> argRegs;
            for (const ExprPtr& arg : e->list) argRegs.push_back(lowerExpr(arg.get()));
            int base = F().nregs;
            { int r = newReg(); emit(Op::Move, r, thisReg()); }  // this
            emitArgCopies(argRegs, e->list);
            int dst = newReg();
            emit(Op::CallDyn, dst, 0, base, 1 + (int)argRegs.size());
            last().sname = std::string(callee->text);
            last().decl = e->resolved;
            return dst;
        }
        fail(e->span, "call to '" + std::string(callee->text) + "'");
        return 0;
    }

    // Member callee: namespaced function (resolved or by-name) or a method call
    if (callee->kind == ExprKind::Member) {
        // bug #37/#46: a checker-RESOLVED namespaced function through a `::`
        // qualifier — `A::B::fn(...)` (nested) or a sibling `B::C::fn(...)` whose
        // root resolves through the enclosing namespace. The checker pinned the
        // decl; emit a direct call to it, which uniformly covers nested and
        // sibling paths that namespaceSym (file/block scopes only) can't re-walk.
        if (callee->colon && e->resolved &&
            (callee->a->kind == ExprKind::Member ||
             namespaceSym(callee->a->text, e->span.offset))) {
            auto it = mod_->byDecl.find(e->resolved);
            if (it != mod_->byDecl.end() && !mod_->functions[it->second].hasThis) {
                std::vector<int> argRegs;
                for (const ExprPtr& arg : e->list) argRegs.push_back(lowerExpr(arg.get()));
                int base = F().nregs;
                emitArgCopies(argRegs, e->list);
                int dst = newReg();
                emit(Op::Call, dst, it->second, base, (int)argRegs.size());
                return dst;
            }
            if (e->resolved->memberBody &&
                e->resolved->memberBody->kind == StmtKind::Empty) {
                std::vector<int> argRegs;
                for (const ExprPtr& arg : e->list) argRegs.push_back(lowerExpr(arg.get()));
                int base = F().nregs;
                emitArgCopies(argRegs, e->list);
                int dst = newReg();
                emit(Op::CallNativeFn, dst, 0, base, (int)argRegs.size());
                last().sname = std::string(callee->text);
                return dst;
            }
        }
        // Track 03 §2: `Enum::fromCode(...)` — a checker-resolved free function
        // reached via `::` on a type (not a namespace), so there is no receiver.
        if (e->resolved && callee->a->kind == ExprKind::Name &&
            !namespaceSym(callee->a->text, e->span.offset)) {
            auto it = mod_->byDecl.find(e->resolved);
            if (it != mod_->byDecl.end() && !mod_->functions[it->second].hasThis) {
                std::vector<int> argRegs;
                for (const ExprPtr& arg : e->list) argRegs.push_back(lowerExpr(arg.get()));
                int base = F().nregs;
                emitArgCopies(argRegs, e->list);
                int dst = newReg();
                emit(Op::Call, dst, it->second, base, (int)argRegs.size());
                return dst;
            }
            // struct-equality design §8: `float::fromBits(...)` resolves to a
            // NATIVE free function (math::floatFromBits, empty body) — no
            // lowered body in byDecl, so dispatch by native name. The native
            // key is the resolved decl's own name ("floatFromBits"), NOT the
            // `::` selector text ("fromBits"). MUST be gated on `::` (colon):
            // an INSTANCE-method native reached via `.` (e.g. `arr.length()`,
            // whose checker-resolved decl is also an empty-body prelude native)
            // must fall through to the CallDyn method-dispatch path below —
            // routing it as a free CallNativeFn misdispatches it (a `length`
            // free-native does not exist).
            if (callee->colon && e->resolved->memberBody &&
                e->resolved->memberBody->kind == StmtKind::Empty) {
                std::vector<int> argRegs;
                for (const ExprPtr& arg : e->list) argRegs.push_back(lowerExpr(arg.get()));
                int base = F().nregs;
                emitArgCopies(argRegs, e->list);
                int dst = newReg();
                emit(Op::CallNativeFn, dst, 0, base, (int)argRegs.size());
                last().sname = std::string(e->resolved->name);
                return dst;
            }
        }
        // NS::fn — resolve through the namespace scope (prelude bodies are
        // unchecked, so fall back to by-name lookup like the oracle does).
        // namespaceSym walks block import overlays -> file overlay -> global,
        // so a block-scoped `uses Lib;` / `use Lib::Inner as In;` qualifier
        // resolves here too (bug.md #1).
        // bug.md #70: this fallback exists for the prelude's genuinely-unchecked
        // `e->resolved == nullptr` case, but the checker ALSO leaves `resolved`
        // null for a checked, dynamically-dispatched interface-typed receiver
        // (info.md §3.4a) — so a local/parameter whose name shadows a namespace
        // (`env`, `math`, ...) walked into this branch and silently called the
        // NAMESPACE function instead of dispatching on the real receiver. Same
        // precedent as the `console` shadow guard above: a local/parameter of
        // this exact name always wins over the namespace fallback.
        if ((callee->a->kind == ExprKind::Name && !findLocal(callee->a->text)) ||
            (callee->a->kind == ExprKind::Member && callee->a->colon)) {
            // bug #37/#46: the qualifier may be a nested chain (`A::B::fn()`),
            // not only a bare `A::fn()` — resolve the whole chain to a namespace.
            Symbol* nsSym = callee->a->kind == ExprKind::Name
                ? namespaceSym(callee->a->text, e->span.offset)
                : namespaceChainSym(callee->a.get(), e->span.offset);
            if (nsSym && nsSym->scope) {
                const Stmt* fnDecl = e->resolved;
                if (!fnDecl) {
                    if (const std::vector<Symbol*>* v =
                            nsSym->scope->localLookup(callee->text))
                        for (Symbol* s : *v)
                            if (s->kind == SymbolKind::Function && s->decl) {
                                fnDecl = s->decl;
                                break;
                            }
                }
                if (fnDecl) {
                    std::vector<int> argRegs;
                    for (const ExprPtr& arg : e->list)
                        argRegs.push_back(lowerExpr(arg.get()));
                    int base = F().nregs;
                    emitArgCopies(argRegs, e->list);
                    int dst = newReg();
                    if (fnDecl->memberBody &&
                        fnDecl->memberBody->kind == StmtKind::Empty) {
                        emit(Op::CallNativeFn, dst, 0, base, (int)argRegs.size());
                        last().sname = std::string(callee->text);
                    } else {
                        auto it = mod_->byDecl.find(fnDecl);
                        if (it == mod_->byDecl.end()) {
                            fail(e->span, "namespaced function");
                            return 0;
                        }
                        emit(Op::Call, dst, it->second, base, (int)argRegs.size());
                    }
                    return dst;
                }
            }
        }
        // (legacy resolved-path below handles shadowed cases)
        if (e->resolved && callee->a->kind == ExprKind::Name) {
            // namespaceSym searches the block import overlays first (bug.md
            // #1), then this call's file overlay (`use NS as Alias;`, bug.md
            // #8/imports.md), then global — `sema_.global` alone sees none of
            // the overlay layers.
            bool isNamespace = namespaceSym(callee->a->text, e->span.offset) != nullptr;
            if (isNamespace && e->resolved->memberBody &&
                e->resolved->memberBody->kind == StmtKind::Empty) {
                std::vector<int> argRegs;
                for (const ExprPtr& arg : e->list) argRegs.push_back(lowerExpr(arg.get()));
                int base = F().nregs;
                emitArgCopies(argRegs, e->list);
                int dst = newReg();
                emit(Op::CallNativeFn, dst, 0, base, (int)argRegs.size());
                last().sname = std::string(callee->text);
                return dst;
            }
            if (isNamespace) {
                auto it = mod_->byDecl.find(e->resolved);
                if (it != mod_->byDecl.end()) {
                    std::vector<int> argRegs;
                    for (const ExprPtr& arg : e->list) argRegs.push_back(lowerExpr(arg.get()));
                    int base = F().nregs;
                    emitArgCopies(argRegs, e->list);
                    int dst = newReg();
                    emit(Op::Call, dst, it->second, base, (int)argRegs.size());
                    return dst;
                }
            }
        }
        // a?.m(...): receiver first; None short-circuits around the whole call
        if (callee->optChain) {
            int recv = lowerExpr(callee->a.get());
            int r = newReg();
            int t = newReg();
            emit(Op::IsType, t, recv);
            last().sname = "None";
            int jCall = emit(Op::JumpIfFalse, t, 0);
            emit(Op::LoadConst, r, addConst(vnone()));
            int jEnd = emit(Op::Jump, 0);
            F().code[jCall].b = (int)F().code.size();
            std::vector<int> argRegs;
            for (const ExprPtr& arg : e->list) argRegs.push_back(lowerExpr(arg.get()));
            int base = F().nregs;
            { int rr = newReg(); emit(Op::Move, rr, recv); }
            for (int rr : argRegs) emit(Op::Move, newReg(), rr);
            int dst = newReg();
            auto sit = e->resolved && e->resolved->isSpecialization
                ? mod_->byDecl.find(e->resolved) : mod_->byDecl.end();
            if (sit != mod_->byDecl.end())
                emit(Op::Call, dst, sit->second, base, 1 + (int)argRegs.size());
            else {
                emit(Op::CallDyn, dst, 0, base, 1 + (int)argRegs.size());
                last().sname = std::string(callee->text);
                last().decl = e->resolved;
            }
            emit(Op::Move, r, dst);
            F().code[jEnd].a = (int)F().code.size();
            return r;
        }
        // `recv.Base::method(...)` — a base-qualified call. The inner `.Base` is
        // a SOURCE QUALIFIER naming a base class, not a field read, and dispatch
        // is STATIC to the checker-resolved base method (info.md §6.9), run on
        // the real receiver `recv` (callee->a->a). Without this, lowerExpr(callee->a)
        // below lowers a nonexistent "Base" member read as the receiver -> the
        // call runs against a detached value (silently blank on --ir, null-`this`
        // segfault on the oracle/LLVM) (bug.md #55).
        //
        // The base qualifier may be NAMESPACED (e.g. `Sonar::App` reached as
        // `this.App::renderFrame()`), so this must NOT gate on a bare global class
        // lookup of callee->a->text — `sema_.global->lookup` misses every
        // namespaced base, the guard is skipped, and the call drops to the
        // field-read receiver below with a null `this` (the SonarApp live-loop
        // segfault: the first frame's `this.App::renderFrame()` ran on a null
        // receiver). The free-function / namespace forms of `X::fn()` were all
        // handled above and resolve to a `!hasThis` function; reaching here with
        // `callee->colon`, a Member qualifier, and an instance method (hasThis)
        // is unambiguously a base-qualified instance call on `recv`.
        if (callee->colon && callee->a->kind == ExprKind::Member && e->resolved) {
            auto it = mod_->byDecl.find(e->resolved);
            if (it != mod_->byDecl.end() && mod_->functions[it->second].hasThis) {
                int recv = lowerExpr(callee->a->a.get());
                std::vector<int> argRegs;
                for (const ExprPtr& arg : e->list) argRegs.push_back(lowerExpr(arg.get()));
                int base = F().nregs;
                { int r = newReg(); emit(Op::Move, r, recv); }
                emitArgCopies(argRegs, e->list);
                int dst = newReg();
                emit(Op::Call, dst, it->second, base, 1 + (int)argRegs.size());
                return dst;
            }
        }
        // method call: receiver.method(args) — dynamic dispatch by name/decl
        bool clearRecv = moveRecvClear_; moveRecvClear_ = false;   // consume before args
        // Track 04 M4: capture-and-reset a pending field-self-append clear at
        // the SAME instant as clearRecv above — before the receiver or any
        // argument is lowered, so a nested call reached while lowering an
        // argument (e.g. `buf.subStr(1, buf.length()-1)`, where the 2nd arg
        // is itself a call on the same field) can't see or consume it.
        PendingFieldClear fieldClear = pendingFieldClear_;
        pendingFieldClear_.pending = false;
        int recv = lowerExpr(callee->a.get());
        std::vector<int> argRegs;
        for (const ExprPtr& arg : e->list) argRegs.push_back(lowerExpr(arg.get()));
        // Only NOW — after every argument is lowered, so one reading the same
        // field sees its real value — release the field's own reference (a
        // cheap void placeholder) so `recv` becomes the sole owner before the
        // marshaling MoveClear/CallDyn treats it as one.
        if (fieldClear.pending) {
            int voidReg = newReg();
            emit(Op::LoadConst, voidReg, addConst(vvoid()));
            emit(Op::RawSet, voidReg, thisReg());
            last().sname = fieldClear.name;
            last().d = fieldClear.slot;
        }
        int base = F().nregs;
        int recvWin = newReg();
        emit(clearRecv ? Op::MoveClear : Op::Move, recvWin, recv);
        emitArgCopies(argRegs, e->list);
        // bug.md #95: a value-struct receiver rides the window as a BARE ALIAS
        // (the Move's wrap retain no-ops on value classes), and the window
        // register otherwise survives to frame exit. #66 cleared the for-in
        // loop VARIABLE for exactly this shape, but a method call on it copies
        // the alias into this second register, which #66's clear cannot see —
        // once the aliased element's array dies inside the same frame (e.g.
        // Router.finalize's `this.routeList = rebuilt`), the frame-exit release
        // reads the freed block's classId (garbage), the value-class skip
        // fails, and the "release" decrements a freelist word: heap corruption
        // surfacing far from the site. Void the slot after the call, same
        // shape as #66's loop-var clear. Consumed receivers (clearRecv) are
        // containers, and the backend's CallDyn tail already voids their slot.
        auto clearStructRecvWin = [&] {
            if (!clearRecv && callee->a->definiteValueStruct)
                emit(Op::LoadConst, recvWin, addConst(vvoid()));
        };
        int dst = newReg();
        if (e->resolved && e->resolved->isSpecialization) {
            auto it = mod_->byDecl.find(e->resolved);
            if (it == mod_->byDecl.end()) { fail(e->span, "specialized method"); return dst; }
            emit(Op::Call, dst, it->second, base, 1 + (int)argRegs.size());
            clearStructRecvWin();
            return dst;
        }
        // in.b = 1 marks a CONSUMED receiver (COW self-append): the callee owns
        // the receiver buffer's fate, so the backend clears the receiver window
        // slot after the call WITHOUT releasing it (§15).
        emit(Op::CallDyn, dst, clearRecv ? 1 : 0, base, 1 + (int)argRegs.size());
        last().sname = std::string(callee->text);
        last().decl = e->resolved;
        clearStructRecvWin();
        return dst;
    }

    // The callee is any other VALUE-producing expression that yields a callable:
    // a call result (`f(x)(y)`, bug.md #47), an array/map index (`fns[i]()`,
    // bug.md #52), an immediately-invoked lambda `(() => {...})()` (bug.md #57,
    // the shape Sonar's `sonar!` template layer expands to), a parenthesized
    // closure, etc. Lower it to a register and invoke through CallValue, exactly
    // like a Name that resolves to a stored closure — the IR lowerer previously
    // special-cased only a handful of syntactic call shapes and bailed on the
    // rest (mirroring the oracle's own eval(callee) -> callClosure fallback).
    {
        int fnReg = lowerExpr(callee);
        std::vector<int> argRegs;
        for (const ExprPtr& arg : e->list) argRegs.push_back(lowerExpr(arg.get()));
        int base = F().nregs;
        { int rr = newReg(); emit(Op::Move, rr, fnReg); }
        for (int rr : argRegs) emit(Op::Move, newReg(), rr);
        int dst = newReg();
        emit(Op::CallValue, dst, 0, base, 1 + (int)argRegs.size());
        return dst;
    }
}

// ---------------------------------------------------------------------------
//  lambdas: closure conversion — capture visible locals by snapshot
// ---------------------------------------------------------------------------

int Lowerer::lowerLambda(Expr* e) {
    // create the lambda's IR function
    int fnIdx = (int)mod_->functions.size();
    mod_->functions.emplace_back();
    {
        IrFunction& fn = mod_->functions[fnIdx];
        fn.name = "@lambda";
        fn.hasThis = true;                        // r0 = the closure itself
        fn.nparams = 1 + (int)e->params.size();
    }

    // capture set = the visible locals the body actually REFERENCES (snapshot
    // semantics). bug #3: an unreferenced local — notably a non-flattenable
    // Worker/Timer/Block/closure merely in lexical scope — must NOT be captured,
    // or a spawn body that names nothing rejects at the thread boundary purely
    // because such a local is nearby. "this" is always kept below (implicit
    // member access never spells it). Matches the oracle's own free-var capture.
    std::unordered_set<std::string_view> lamFree;
    lwrCollectExprNames(e, lamFree);
    std::vector<std::pair<std::string_view, int>> captures;
    for (const auto& scope : scopes_)
        for (const auto& [name, reg] : scope)
            if (name == "this" || lamFree.count(name)) captures.push_back({name, reg});
    // The receiver is not a named local: a lambda born inside a member snapshots
    // it under the keyword name too, so its body can lower `this`, bare member
    // reads/writes, and self-method calls against the captured object. (Inside
    // a nested lambda the outer pre-load already put "this" in scopes_, so the
    // loop above re-captured it — don't add it twice.)
    int enclosingThis = thisReg();
    if (enclosingThis >= 0 && !findLocal("this"))
        captures.push_back({"this", enclosingThis});

    // lower the body in a fresh context
    int savedCur = cur_;
    Symbol* savedClass = curClass_;
    std::string savedAcc = curAccessor_;
    bool savedCtor = curIsCtor_, savedLam = curIsLambda_;
    auto savedScopes = std::move(scopes_);
    auto savedFresh = std::move(freshStructRegs_);   // §15: reg marks are per-function
    freshStructRegs_.clear();
    // techdesign-02 F1/F4: a lambda body is its own loop nesting — a bare
    // break/continue in the body must never resolve against the ENCLOSING
    // function's loop stack (the checker already rejects it statically; this
    // keeps Lower's stack empty to match, same precedent as freshStructRegs_).
    auto savedLoops = std::move(loops_);
    loops_.clear();
    // techdesign-02 F3: same precedent — a lambda body is its own function
    // for return/cleanup-chaining purposes too.
    auto savedUsings = std::move(usings_);
    usings_.clear();
    int savedChainRetReg = chainRetReg_;
    bool savedChainRetIsVoid = chainRetIsVoid_;
    chainRetReg_ = -1; chainRetIsVoid_ = false;

    cur_ = fnIdx;
    // curClass_ survives into the body: member-name recognition must keep
    // working there, with the captured "this" local standing in for r0 (every
    // implicit-receiver site goes through thisReg()). Accessor raw-access
    // context does NOT survive — a lambda body reads members like any
    // non-accessor member context (dispatching, never raw).
    curAccessor_.clear(); curIsCtor_ = false; curIsLambda_ = true;
    scopes_.clear(); scopes_.emplace_back();
    newReg();                                     // r0 = closure
    for (const Param& p : e->params) scopes_.back()[p.name] = newReg();
    // captured names resolve via LoadCapture: pre-load them into registers
    for (const auto& [name, _] : captures) {
        if (findLocal(name)) continue;            // params shadow captures
        int r = newReg();
        emit(Op::LoadCapture, r);
        last().sname = std::string(name);
        scopes_.back()[name] = r;
    }
    if (e->block) {                               // statement-block body
        lowerStmt(e->block.get());
        emit(Op::RetVoid);                         // fall-off returns void
    } else {
        int result = lowerExpr(e->a.get());       // expression body
        emit(Op::Ret, result);
    }

    cur_ = savedCur; curClass_ = savedClass; curAccessor_ = savedAcc;
    curIsCtor_ = savedCtor; curIsLambda_ = savedLam;
    scopes_ = std::move(savedScopes);
    freshStructRegs_ = std::move(savedFresh);
    loops_ = std::move(savedLoops);
    usings_ = std::move(savedUsings);
    chainRetReg_ = savedChainRetReg; chainRetIsVoid_ = savedChainRetIsVoid;

    // build the closure value + captures
    int dst = newReg();
    emit(Op::MakeClosure, dst, fnIdx);
    for (const auto& [name, reg] : captures) {
        emit(Op::CaptureVar, dst, reg);
        last().sname = std::string(name);
    }
    return dst;
}

// ---------------------------------------------------------------------------
//  expressions
// ---------------------------------------------------------------------------

// Emit an IsType test of a value against a type (the shared `is`/`match` core).
int Lowerer::emitIsType(int v, const TypeRef* t) {
    int r = newReg();
    emit(Op::IsType, r, v);
    std::string canon = t ? t->canonical : "";
    if (canon.empty() && t) {
        if (t->kind == TypeKind::Named) canon = std::string(t->name);
        else if (t->kind == TypeKind::Union)
            for (const TypeRefPtr& m : t->members)
                canon += (canon.empty() ? "" : " | ") + std::string(m->name);
    }
    last().sname = canon;
    Symbol* sym = t ? t->resolvedSymbol : nullptr;
    if (!sym && t && t->kind == TypeKind::Named) sym = sema_.global->lookup(t->name);
    last().sym = sym;
    return r;
}

int Lowerer::lowerExpr(Expr* e) {
    if (!e || !ok_) return 0;
    switch (e->kind) {
        case ExprKind::IntLit: {
            int r = newReg();
            emit(Op::LoadConst, r, addConst(vint(parseIntLiteral(e->text))));
            return r;
        }
        case ExprKind::FloatLit: {
            int r = newReg();
            emit(Op::LoadConst, r, addConst(vfloat(parseFloatLiteral(e->text))));
            return r;
        }
        case ExprKind::StringLit: {
            int r = newReg();
            // F4: an interpolation segment's text is already bare content
            // (no quotes to strip — see Expr::isRawSegment). request-string-
            // literal-tail: a raw string's text is already bare content too,
            // but byte-exact (no escape decoding at all — see Expr::isRawString).
            std::string decoded = e->isRawString ? std::string(e->text)
                                  : e->isRawSegment ? decodeEscapes(e->text)
                                                    : decodeStringLiteral(e->text);
            // Track 03 §1: the checker flipped this single-scalar literal to char.
            if (e->charLit) {
                size_t len; bool boundary;
                emit(Op::LoadConst, r, addConst(vchar(utf8DecodeAt(decoded, 0, len, boundary))));
                return r;
            }
            emit(Op::LoadConst, r, addConst(vstr(decoded)));
            return r;
        }
        case ExprKind::BoolLit: {
            int r = newReg();
            emit(Op::LoadConst, r, addConst(vbool(e->text == "true")));
            return r;
        }
        case ExprKind::This: {
            int tr = thisReg();                        // r0, or the captured local
            if (tr >= 0) return tr;
            fail(e->span, "'this' outside a member");
            return 0;
        }
        case ExprKind::Name: {
            if (e->text == "None") {
                int r = newReg();
                emit(Op::LoadConst, r, addConst(vnone()));
                return r;
            }
            if (int* r = findLocal(e->text)) return *r;
            if (curClass_ && classHasMember(curClass_, e->text)) {
                int r = newReg();
                // Inside the field's own accessor, bare name is the raw field (no
                // re-dispatch). Otherwise a plain field with a stable slot and no
                // accessor anywhere in the hierarchy can also skip the member path.
                bool selfAcc = (e->text == curAccessor_);
                int slot = packedSlot(curClass_, e->text, /*requireNoAccessor=*/!selfAcc);
                Op op = (e->weakDirect || (e->weakField && selfAcc)) ? Op::RawGetWeak
                                     : ((selfAcc || slot > 0) ? Op::RawGet : Op::GetMember);
                emit(op, r, thisReg());
                last().sname = std::string(e->text);
                if (op == Op::RawGet || op == Op::RawGetWeak) last().d = slot;
                return r;
            }
            {   // A namespaced value global, possibly reached through a
                // `use ... as` alias (imports.md §4). globalIndex is keyed by
                // the declaration's OWN name, so resolve through the
                // checker-recorded decl (e->resolved) when the read used an
                // alias — same fix as Eval.cpp's ExprKind::Name case.
                std::string key = e->resolved ? std::string(e->resolved->name)
                                               : std::string(e->text);
                auto g = mod_->globalIndex.find(key);
                if (g != mod_->globalIndex.end()) {
                    int r = newReg();
                    emit(Op::LoadGlobal, r, g->second);
                    return r;
                }
            }
            fail(e->span, "name '" + std::string(e->text) + "'");
            return 0;
        }
        case ExprKind::Member: {
            // techdesign-columnar §5.4: FUSE `arr[i].field` into one ColGet when
            // columnar mode is on and arr's element type is a columnar-eligible
            // value struct — reads column `slotK` directly, no gather. Guarded to
            // a plain `.field` on a syntactic index read with no accessor
            // intercepting the field (else fall through to gather + GetMember).
            if (columnar_ && !e->colon && !e->optChain && e->a &&
                e->a->kind == ExprKind::Index && e->a->definiteValueStruct &&
                columnarEligibleStruct(e->a->valueClass)) {
                int slot1 = packedSlot(e->a->valueClass, e->text, /*requireNoAccessor=*/true);
                if (slot1 > 0) {
                    int arrReg = lowerExpr(e->a->a.get());
                    int idxReg = lowerExpr(e->a->b.get());
                    int r = newReg();
                    emit(Op::ColGet, r, arrReg, idxReg, slot1 - 1);
                    last().sname = std::string(e->text);
                    last().sym = e->a->valueClass;
                    return r;
                }
            }
            // Track 03 §2: a checker-resolved enum member read (`Method::GET`)
            // loads the mangled const global the desugar initialized.
            if (e->resolved && e->resolved->kind == StmtKind::Var) {
                auto g = mod_->globalIndex.find(std::string(e->resolved->name));
                if (g != mod_->globalIndex.end()) {
                    int r = newReg();
                    emit(Op::LoadGlobal, r, g->second);
                    return r;
                }
            }
            if (e->optChain) {                            // a?.b — None short-circuits
                int b = lowerExpr(e->a.get());
                int r = newReg();
                int t = newReg();
                emit(Op::IsType, t, b);
                last().sname = "None";
                int jRead = emit(Op::JumpIfFalse, t, 0);  // not None -> read member
                emit(Op::LoadConst, r, addConst(vnone()));
                int jEnd = emit(Op::Jump, 0);
                F().code[jRead].b = (int)F().code.size();
                emit(e->weakDirect ? Op::RawGetWeak : Op::GetMember, r, b);
                last().sname = std::string(e->text);
                F().code[jEnd].a = (int)F().code.size();
                return r;
            }
            {                                             // NS::global (std::read)
                // through block/file import overlays too (bug.md #1). A
                // bare-Name-only check here reached just one hop, so a const
                // in a NESTED namespace read via its fully-qualified path
                // (`NS::Inner::member`) fell through to "not yet lowerable"
                // — namespaceChainSym is the same nested-chain resolver
                // already used for `NS::Inner::fn()` calls above.
                if (namespaceChainSym(e->a.get(), e->span.offset)) {
                    auto g = mod_->globalIndex.find(std::string(e->text));
                    if (g != mod_->globalIndex.end()) {
                        int r = newReg();
                        emit(Op::LoadGlobal, r, g->second);
                        return r;
                    }
                }
            }
            int baseReg; std::string key;
            if (memberKey(e, baseReg, key)) {
                int r = newReg();
                int slot = (e->a->kind == ExprKind::This && curClass_)
                               ? packedSlot(curClass_, key, /*requireNoAccessor=*/true) : 0;
                emit(e->weakDirect ? Op::RawGetWeak : (slot > 0 ? Op::RawGet : Op::GetMember),
                     r, baseReg);
                last().sname = key;
                if (slot > 0) last().d = slot;
                return r;
            }
            fail(e->span, "this member access");
            return 0;
        }
        case ExprKind::Index: {
            int b = lowerExpr(e->a.get());
            int i = lowerExpr(e->b.get());
            int r = newReg();
            emit(Op::GetIndex, r, b, i);
            // techdesign-columnar §5.3: an unfused columnar-eligible element read
            // GATHERS a fresh standalone rc=0 record (not a buffer alias), so a
            // consuming site must reclaim it — mark it fresh, like a struct-
            // returning call (noteFreshStructResult excludes native dispatch
            // precisely because the DENSE `.at` aliases; columnar does not).
            if (columnar_ && e->definiteValueStruct && columnarEligibleStruct(e->valueClass))
                freshStructRegs_.insert(r);
            return r;
        }
        case ExprKind::Array: {
            std::vector<int> elems;
            for (const ExprPtr& el : e->list) elems.push_back(lowerExpr(el.get()));
            int base = F().nregs;
            for (int r : elems) emit(Op::Move, newReg(), r);
            int dst = newReg();
            emit(Op::NewArray, dst, 0, base, (int)elems.size());
            return dst;
        }
        case ExprKind::Range: {
            int lo = lowerExpr(e->a.get());
            int hi = lowerExpr(e->b.get());
            int r = newReg();
            emit(Op::MakeRange, r, lo, hi);
            if (Symbol* rs = sema_.global->lookup("Range")) last().sym = rs;
            return r;
        }
        case ExprKind::Await: {
            int p = lowerExpr(e->a.get());
            int r = newReg();
            emit(Op::Await, r, p);
            return r;
        }
        case ExprKind::Is:
            return emitIsType(lowerExpr(e->a.get()), e->type.get());
        case ExprKind::Match: {
            // Desugar to a first-match-wins chain of IsType / equality tests +
            // branches — the SAME machinery as `is`, so every backend gets it.
            int result = newReg();
            emit(Op::Default, result); last().sname = "";      // void if nothing matches
            int subj = lowerExpr(e->a.get());
            std::vector<int> endJumps;
            for (const MatchArm& arm : e->arms) {
                std::vector<int> toNext;
                if (arm.isElse) {
                    // no test — always taken
                } else if (arm.type) {
                    int t = emitIsType(subj, arm.type.get());
                    toNext.push_back(emit(Op::JumpIfFalse, t, 0));
                } else if (arm.value && arm.value->kind == ExprKind::Range) {
                    int ge = newReg();
                    emit(Op::Arith, ge, subj, lowerExpr(arm.value->a.get()));
                    last().tk = TokenKind::Ge; last().sname = "ii";
                    toNext.push_back(emit(Op::JumpIfFalse, ge, 0));
                    int le = newReg();
                    emit(Op::Arith, le, subj, lowerExpr(arm.value->b.get()));
                    last().tk = TokenKind::Le; last().sname = "ii";
                    toNext.push_back(emit(Op::JumpIfFalse, le, 0));
                } else if (arm.value && e->a->evalKind == 3 &&
                           arm.value->evalKind == 3) {
                    // struct-equality §6 (packet 07): a float scrutinee classifies
                    // each value arm by the CANONICAL relation, not IEEE `==` — so
                    // `float::NaN =>` is a reachable arm and ±0.0 collapse. Lower
                    // the test as the `canonEq` native (packet 05) on the subject
                    // with the pattern as argument, exactly the method-call shape
                    // (CallDyn by name) the synthesized `(==)` body emits for float
                    // fields — a decl-less CallDyn dispatches through each engine's
                    // ONE canon (findMethodByName/callm/emitNativeRows), the same
                    // symbol keyEq uses (hash-consistency law §3.3). A hand-rolled
                    // bit compare would diverge from that symbol. Canonical ≡ IEEE
                    // except NaN, so no existing float match changes behavior.
                    // Mixed int/float arms fall to the scalar Arith path below (an
                    // int pattern is never NaN, so the two relations agree; and
                    // canonEq's float arg would misread an int payload).
                    // optimization deferred: canon-once (design §6) — the simple
                    // per-arm canonEq form is observably identical (canon is
                    // idempotent) and keeps the integer canon inside the native.
                    int pv = lowerExpr(arm.value.get());
                    int base = F().nregs;
                    { int r = newReg(); emit(Op::Move, r, subj); }
                    { int r = newReg(); emit(Op::Move, r, pv); }
                    int eq = newReg();
                    emit(Op::CallDyn, eq, 0, base, 2);
                    last().sname = "canonEq";
                    toNext.push_back(emit(Op::JumpIfFalse, eq, 0));
                } else if (arm.value) {
                    int eq = newReg();
                    emit(Op::Arith, eq, subj, lowerExpr(arm.value.get()));
                    last().tk = TokenKind::EqEq;
                    auto kc = [](int k) { return k==1?'b':k==2?'s':k==3?'f':'i'; };
                    last().sname = std::string(1, kc(e->a->evalKind)) +
                                   kc(arm.value->evalKind);
                    toNext.push_back(emit(Op::JumpIfFalse, eq, 0));
                }
                if (arm.bodyExpr) emit(Op::Move, result, lowerExpr(arm.bodyExpr.get()));
                else if (arm.bodyBlock) lowerStmt(arm.bodyBlock.get());
                endJumps.push_back(emit(Op::Jump, 0));
                int nextLabel = (int)F().code.size();
                for (int j : toNext) F().code[j].b = nextLabel;
            }
            int endLabel = (int)F().code.size();
            for (int j : endJumps) F().code[j].a = endLabel;
            return result;
        }
        case ExprKind::Extract: {                     // `stream >>` == stream.pull()
            int recv = lowerExpr(e->a.get());
            int base = F().nregs;
            { int r = newReg(); emit(Op::Move, r, recv); }
            int dst = newReg();
            emit(Op::CallDyn, dst, 0, base, 1);
            last().sname = "pull";
            last().decl = e->resolved;
            return dst;
        }
        case ExprKind::Lambda:
            return lowerLambda(e);
        case ExprKind::Inject: {                      // `inject Type` (§12.5)
            if (e->resolved) {
                // Inline the resolved bind's `=> expr` body AT THE INJECT SITE, in
                // the current scope: a block-scope `bind IFace => local;` then
                // reads the enclosing local (bug.md #56 — the separate zero-arg
                // $bind function can't see it), a top-level bind reads its global,
                // and `=> Ctor()` constructs FRESH per injection (design §2.2), all
                // uniformly. Only a `{ block }` factory body (no single return
                // expression) keeps the zero-arg $bind-function call.
                const Stmt* body = e->resolved->memberBody.get();
                if (body && body->kind == StmtKind::Return && body->expr)
                    return lowerExpr(body->expr.get());
                auto it = mod_->byDecl.find(e->resolved);
                if (it != mod_->byDecl.end()) {
                    int base = F().nregs;
                    int dst = newReg();
                    emit(Op::Call, dst, it->second, base, 0);
                    return dst;
                }
            }
            fail(e->span, "inject binding");
            return 0;
        }
        case ExprKind::Call: {
            int r = lowerCall(e);
            noteFreshStructResult(e, r);   // §15: fresh struct copy? mark for VFree
            return r;
        }
        case ExprKind::Unary: {
            int v = lowerExpr(e->a.get());
            if (e->op == TokenKind::Tilde) {
                // ~x lowers as x ^ (-1) — one less op for every backend to learn.
                int m1 = newReg();
                emit(Op::LoadConst, m1, addConst(vint(-1)));
                int r = newReg();
                emit(Op::Arith, r, v, m1); last().tk = TokenKind::Caret;
                return r;
            }
            int r = newReg();
            emit(e->op == TokenKind::Bang ? Op::Not : Op::Neg, r, v);
            return r;
        }
        case ExprKind::Ternary: {
            int r = newReg();
            int c = lowerExpr(e->a.get());
            int jElse = emit(Op::JumpIfFalse, c, 0);
            emit(Op::Move, r, lowerExpr(e->b.get()));
            int jEnd = emit(Op::Jump, 0);
            F().code[jElse].b = (int)F().code.size();
            emit(Op::Move, r, lowerExpr(e->c.get()));
            F().code[jEnd].a = (int)F().code.size();
            return r;
        }
        case ExprKind::Binary: {
            TokenKind op = e->op;
            if (op == TokenKind::Eq) {
                // `x = x.method(...)`: the old x is replaced by the result, so we can
                // hand x's buffer to the method to mutate in place when it is uniquely
                // owned (COW). Only fires when the receiver is exactly the target local.
                Expr* rhs = e->b.get();
                bool sameCallShape =
                    e->a->kind == ExprKind::Name && rhs->kind == ExprKind::Call &&
                    rhs->a && rhs->a->kind == ExprKind::Member && rhs->a->a &&
                    rhs->a->a->kind == ExprKind::Name && rhs->a->a->text == e->a->text;
                bool selfAppend = sameCallShape && findLocal(e->a->text);
                // Field analogue (bug.md problem #2, StringBuilder): only for
                // a plain accessor-free slot (packedSlot requireNoAccessor) —
                // a custom `set` would otherwise fire twice (once for the
                // transient clear, once for the real result).
                int fieldSlot = (!selfAppend && sameCallShape && curClass_ &&
                                 classHasMember(curClass_, e->a->text))
                    ? packedSlot(curClass_, e->a->text, /*requireNoAccessor=*/true) : 0;
                bool selfAppendField = fieldSlot > 0;
                if (selfAppend) moveRecvClear_ = true;
                if (selfAppendField) {
                    // Once the field is read-and-cleared, its register is the
                    // sole owner exactly like a local's — so the call's own
                    // receiver-marshaling step also needs moveRecvClear_
                    // (MoveClear, not Move) or it would retain an extra copy
                    // right back in, undoing the clear. The actual clear-write
                    // is armed here but performed inside the method-call
                    // lowering itself, AFTER its arguments are lowered (not
                    // as soon as the field is read) — see pendingFieldClear_.
                    moveRecvClear_ = true;
                    pendingFieldClear_ = {true, std::string(e->a->text), fieldSlot};
                }
                int v = lowerExpr(rhs);
                moveRecvClear_ = false;
                pendingFieldClear_.pending = false;
                if (selfAppend) {
                    // move the result straight back into x, clearing the call-result
                    // temp — otherwise it keeps the buffer live and COW sees it shared
                    int* aReg = findLocal(e->a->text);
                    emit(Op::MoveClear, *aReg, v);
                    return *aReg;
                }
                if (selfAppendField) {
                    // write the result back into the (already-cleared) field slot
                    emit(Op::RawSet, v, thisReg());
                    last().sname = std::string(e->a->text);
                    last().d = fieldSlot;
                    return v;
                }
                lowerAssign(e->a.get(), v, e->b.get());
                return v;
            }
            switch (op) {
                case TokenKind::PlusEq: case TokenKind::MinusEq: case TokenKind::StarEq:
                case TokenKind::SlashEq: case TokenKind::PercentEq: {
                    TokenKind base =
                        op == TokenKind::PlusEq ? TokenKind::Plus :
                        op == TokenKind::MinusEq ? TokenKind::Minus :
                        op == TokenKind::StarEq ? TokenKind::Star :
                        op == TokenKind::SlashEq ? TokenKind::Slash : TokenKind::Percent;
                    int cur = lowerExpr(e->a.get());
                    int rhs = lowerExpr(e->b.get());
                    int r = newReg();
                    emit(Op::Arith, r, cur, rhs); last().tk = base;
                    auto kc = [](int k) { return k==1?'b':k==2?'s':k==3?'f':'i'; };
                    last().sname = std::string(1, kc(e->a->evalKind)) + kc(e->b->evalKind);
                    lowerAssign(e->a.get(), r, e->a.get());
                    return r;
                }
                default: break;
            }
            if (op == TokenKind::QuestionQuestion) {
                int r = newReg();
                emit(Op::Move, r, lowerExpr(e->a.get()));
                int t = newReg();
                emit(Op::IsType, t, r);
                last().sname = "None";
                int jSkip = emit(Op::JumpIfFalse, t, 0);   // not None -> keep lhs
                emit(Op::Move, r, lowerExpr(e->b.get()));
                F().code[jSkip].b = (int)F().code.size();
                return r;
            }
            if (op == TokenKind::AmpAmp || op == TokenKind::PipePipe) {
                int r = newReg();
                emit(Op::Move, r, lowerExpr(e->a.get()));
                int jSkip = emit(op == TokenKind::AmpAmp ? Op::JumpIfFalse : Op::JumpIfTrue,
                                 r, 0);
                emit(Op::Move, r, lowerExpr(e->b.get()));
                F().code[jSkip].b = (int)F().code.size();
                return r;
            }
            int l = lowerExpr(e->a.get());
            int rr = lowerExpr(e->b.get());
            int r = newReg();
            emit(Op::Arith, r, l, rr);
            last().tk = op;
            last().decl = e->resolved;          // resolved operator method, if any
            // operand-kind hints for the native-elf backend (i/b/s/f), so it can
            // pick string concat vs integer arithmetic without runtime tags.
            auto kc = [](int k) { return k == 1 ? 'b' : k == 2 ? 's' : k == 3 ? 'f' : 'i'; };
            last().sname = std::string(1, kc(e->a->evalKind)) + kc(e->b->evalKind);
            return r;
        }
        default:
            fail(e->span, "this expression form");
            return 0;
    }
}
