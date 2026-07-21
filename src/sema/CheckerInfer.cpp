// Part of the refactor_1 Checker.cpp split (designs/complete/refactor_1/techdesign-03-checker-split-sonnet.md).
#include "sema/CheckerInternal.hpp"

namespace {

Type typeValue(Symbol* s) { return Type{TKind::TypeValue, s, std::string(s->name), {}, nullptr, {}}; }

Type funcRef(Symbol* s) { return Type{TKind::FuncRef, s, std::string(s->name), {}, nullptr, {}}; }


bool isComparison(TokenKind k) {
    switch (k) {
        case TokenKind::EqEq: case TokenKind::BangEq: case TokenKind::Lt:
        case TokenKind::Gt:   case TokenKind::Le:     case TokenKind::Ge: return true;
        default: return false;
    }
}


bool allSameType(const std::vector<const Slot*>& slots) {
    for (size_t i = 1; i < slots.size(); ++i)
        if (slots[i]->canonical != slots[0]->canonical) return false;
    return true;
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
    if (!e) return checkerDetail::unknown();
    switch (e->kind) {
        case ExprKind::IntLit:    return primType("int");
        case ExprKind::FloatLit:  return primType("float");
        case ExprKind::StringLit: return primType("string");
        case ExprKind::BoolLit:   return primType("bool");
        case ExprKind::This:
            return thisClass_ ? checkerDetail::classType(thisClass_) : checkerDetail::unknown();

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
                auto fields = checkerDetail::slotsNamed(thisClass_->shape, e->text);
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
                if (data.size() > 1) return checkerDetail::unknown();   // diff types: needs context (phase 2)
                if (!fields.empty()) return checkerDetail::unknown();    // a method referenced bare
            }
            // globals
            if (Symbol* sym = scope_ ? scope_->lookup(e->text) : nullptr) {
                if (sym->kind == SymbolKind::Class) return typeValue(sym);
                if (sym->kind == SymbolKind::Function) {
                    // LA-32 §4.6: a GENERIC function in value position needs a
                    // concrete type tuple. A turbofish supplies it (pinned →
                    // eta-expand to a concrete closure); an unpinned reference is
                    // the LA-25 §8.6 error, now suggesting the turbofish. Non-
                    // generic names keep the plain funcRef. This value-position
                    // typeOf is never reached for a call callee (typeOfCallInner
                    // resolves those), so it is exactly the reference position.
                    if (sym->decl && !sym->decl->generics.empty() && methodRefsAllowed_) {
                        bool isRef = false;
                        Type t = tryResolveMethodRef(const_cast<Expr*>(e), nullptr, isRef);
                        if (isRef) return t;
                    }
                    return funcRef(sym);
                }
                if (sym->kind == SymbolKind::Var && sym->decl) {
                    // Record the declaration so a read through a `use ... as`
                    // alias (imports.md §4: "the alias names the same slot")
                    // still reaches the same runtime global — the evaluator's
                    // and lowerer's global storage is keyed by the var's own
                    // declared name, not whatever alias text read it here.
                    const_cast<Expr*>(e)->resolved = sym->decl;
                    return fromTypeRef(sym->decl->type.get());
                }
                return checkerDetail::unknown();   // known name of another kind (namespace, type param)
            }
            if (e->text == "System")
                return checkerDetail::unknown();   // builtin global not yet modeled
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
            return checkerDetail::unknown();
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
                if (e->op == TokenKind::Bang) return checkerDetail::primitive("bool");
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
            if (f.canonical == "char" && checkerDetail::isCharLiteral(e->b.get())) {
                checkerDetail::markCharLiteral(e->b.get()); t = primType("char");
            } else if (t.canonical == "char" && checkerDetail::isCharLiteral(e->c.get())) {
                checkerDetail::markCharLiteral(e->c.get()); f = primType("char");
            }
            return t.canonical == f.canonical ? t : checkerDetail::unknown();
        }
        case ExprKind::Inject: {                 // `inject Type` — explicit selector (§12.5)
            checkerDetail::resolveExprType(const_cast<TypeRef*>(e->type.get()), scope_);
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
            return t.kind == TKind::Unknown ? checkerDetail::unknown() : t;
        }
        case ExprKind::Array: {
            // Infer Array<Elem> when all elements share a type; else raw Array.
            Type elem = checkerDetail::unknown();
            bool uniform = true;
            for (size_t i = 0; i < e->list.size(); ++i) {
                Type t = typeOf(e->list[i].get());
                if (e->list[i]->kind == ExprKind::Range) t = primType("int");  // [1..5] spreads ints
                if (i == 0) elem = t;
                else if (t.canonical != elem.canonical) uniform = false;
            }
            Symbol* arrSym = scope_ ? scope_->lookup("Array") : nullptr;
            if (!arrSym) return checkerDetail::unknown();
            if (e->list.empty() || !uniform || elem.canonical.empty())
                return checkerDetail::classType(arrSym);                    // raw Array
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
            return checkerDetail::unknown();
        case ExprKind::Is: {
            typeOf(e->a.get());
            checkerDetail::resolveExprType(const_cast<TypeRef*>(e->type.get()), scope_);
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
                    checkerDetail::resolveExprType(const_cast<TypeRef*>(arm.type.get()), scope_);
                    covered.push_back(arm.type->canonical);
                    if (!path.empty()) narrow_[path] = fromTypeRef(arm.type.get());
                } else if (arm.value) {           // VALUE / range pattern
                    // Track 03 §1: char-subject match — `match (c) { 'a' => ... }`
                    // re-types a single-scalar arm literal to char (compares by
                    // scalar). Done before typeOf so the pattern types as char.
                    if (subj.canonical == "char" && checkerDetail::isCharLiteral(arm.value.get()))
                        checkerDetail::markCharLiteral(arm.value.get());
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
                    ? (check(const_cast<Stmt*>(arm.bodyBlock.get())), checkerDetail::unknown())
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
            return haveResult && uniform ? result : checkerDetail::unknown();
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
            return checkerDetail::unknown();
        }
        case ExprKind::Range: {
            typeOf(e->a.get()); typeOf(e->b.get());
            Symbol* r = scope_ ? scope_->lookup("Range") : nullptr;
            return r ? checkerDetail::classType(r) : checkerDetail::unknown();
        }
        default:                 return checkerDetail::unknown();
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
        return checkerDetail::unknown();
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
                return checkerDetail::classType(nameSym);
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
                        return checkerDetail::classType(bt.sym);
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
            if (!checkerDetail::slotsNamed(bt.sym->shape, name).empty()) return bt;
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
        auto slots = checkerDetail::slotsNamed(bt.sym->shape, name);
        if (slots.empty()) return wrap(checkerDetail::unknown());           // unmodeled: lenient
        if (slots.size() == 1) {
            const Slot* s = slots[0];
            const_cast<Expr*>(e)->weakField = s->isWeak;
            const_cast<Expr*>(e)->weakDirect = s->isWeak &&
                !findAccessorDecl(bt.sym, name, true) &&
                !findAccessorDecl(bt.sym, name, false);
            return s->isMethod ? wrap(checkerDetail::unknown())
                               : wrap(fromTypeRef(s->decl->type.get()));
        }
        if (allSameType(slots))
            return error(e->span, "ambiguous read of '" + std::string(name) + " : " +
                         slots[0]->canonical +
                         "' (distinct on multiple bases); qualify with '::'");
        return checkerDetail::unknown();   // different types: resolution needs a target (phase 2)
    }
    return checkerDetail::unknown();
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
            argTypes.push_back(checkerDetail::unknown());
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
    bool explicitSelectionFailed = false;
    bool declaredCallAttempted = false;
    auto resolve = [&](std::vector<const Stmt*> cands, const char* what,
                       bool allowInject = true, Symbol* constructionOwner = nullptr,
                       Symbol* receiverClass = nullptr,
                       const Type* receiver = nullptr) -> const Stmt* {
        if (cands.empty() && !constructionOwner) return nullptr;
        declaredCallAttempted = true;
        const std::vector<std::string_view>* explicitParams =
            constructionOwner && constructionOwner->decl
                ? &constructionOwner->decl->generics : nullptr;
        const std::string_view ownerName = constructionOwner
            ? constructionOwner->name : callee->text;
        if (!filterExplicitCandidates(call, cands, explicitParams, ownerName)) {
            explicitSelectionFailed = true;
            return nullptr;
        }
        if (cands.empty()) return nullptr;
        bool ok = false, diagnosed = false;
        const Stmt* picked = allowInject
            ? pickInjecting(cands, argTypes, call, ok, diagnosed, explicitParams,
                            receiverClass, receiver)
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
                    if (!e->explicitTypeArgs.empty()) {
                        std::vector<const Stmt*> one{ed.fromCode};
                        if (!filterExplicitCandidates(e, one, nullptr, callee->text))
                            return Type{TKind::Error, nullptr, "", {}, nullptr, {}};
                    }
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
                            if (!e->explicitTypeArgs.empty()) {
                                std::vector<const Stmt*> one{s->decl};
                                if (!filterExplicitCandidates(e, one, nullptr, callee->text))
                                    return Type{TKind::Error, nullptr, "", {}, nullptr, {}};
                            }
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
            if (Symbol* ns = checkerDetail::nsChainSym(scope_, callee->a.get()))
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
                                   /*allowInject=*/true, ctorClass);
        if (explicitSelectionFailed)
            return Type{TKind::Error, nullptr, "", {}, nullptr, {}};
        if (!ctor && !e->explicitTypeArgs.empty() && !ctorCands.empty())
            return Type{TKind::Error, nullptr, "", {}, nullptr, {}};
        call->resolved = ctor;
        call->resolvedClass = ctorClass;
        const std::vector<std::string_view>* ctorGenericParams =
            ctorClass->decl ? &ctorClass->decl->generics : nullptr;
        const auto ctorSeed = callGenericSeed(e, ctor, ctorGenericParams);
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
                        reifyLambda(a, fn, e->explicitTypeArgs.empty() ? nullptr : &ctorSeed);
                        lambdaWalked[i] = 1;
                        continue;
                    }
                    std::vector<Type> ptypes;
                    if (pt && pt->kind == TypeKind::Function)
                        for (const TypeRefPtr& fp : pt->funcParams)
                            ptypes.push_back(e->explicitTypeArgs.empty()
                                ? fromTypeRef(fp.get()) : substitute(fp.get(), ctorSeed));
                    checkLambdaBody(a, ptypes);
                    lambdaWalked[i] = 1;
                } else if (a->kind == ExprKind::Member) {
                    // LA-25 §2.2/§6 (M3): an overloaded method reference passed
                    // as a constructor argument (the route-table shape) —
                    // disambiguate against the declared parameter's function type.
                    Type expected = e->explicitTypeArgs.empty()
                        ? fromTypeRef(pt) : substitute(pt, ctorSeed);
                    bool isRef = false;
                    tryResolveMethodRef(a, &expected, isRef);
                    if (isRef) lambdaWalked[i] = 1;
                }
            }
        return inferConstruction(ctorClass, ctor, argTypes, e, nullptr, e->span);
    }

    // --- free function (overloaded) ---
    if (callee->kind == ExprKind::Name) {
        if (Type* loc = localLookup(callee->text)) {              // function-typed local
            if (!e->explicitTypeArgs.empty())
                return error(e->span,
                    "explicit type arguments require a declared function, method, or constructor");
            for (const ExprPtr& arg : e->list)
                if (!arg->argLabel.empty())
                    return error(e->span,
                        "named arguments require a declared function, method, "
                        "constructor, or attribute parameter name");
            if (loc->kind == TKind::FuncRef && loc->ret) return *loc->ret;
            return checkerDetail::unknown();                                     // callable-ish local
        }
        auto cands = functionOverloads(callee->text);
        if (!cands.empty()) {
            if (const Stmt* fn = resolve(cands, "function")) {
                call->resolved = fn;
                return genericReturn(nullptr, fn, checkerDetail::unknown(), argTypes, e, &lambdaWalked);
            }
            return checkerDetail::unknown();   // overloads existed but none applied (already errored)
        }
        // a method of `this` called unqualified?
        if (thisClass_ && !checkerDetail::slotsNamed(thisClass_->shape, callee->text).empty()) {
            Type thisType = checkerDetail::classType(thisClass_);
            if (const Stmt* m = resolve(methodOverloads(thisClass_, callee->text), "method",
                                        true, nullptr, thisClass_, &thisType)) {
                // S2 (§4.2): a bare this-call inside an inherited method — `this`
                // flowing through a base method is not necessarily the
                // most-derived class, so the same override-openness test applies.
                call->resolved = resolveDispatch(thisClass_, m, e->span) ? nullptr : m;
                return genericReturn(thisClass_, m, thisType, argTypes, e, &lambdaWalked);
            }
            return checkerDetail::unknown();
        }
        if (callee->text != "System") {
            Type err = error(e->span, "unknown function '" + std::string(callee->text) + "'");
            noteBlockScopedImport(callee->text);   // S5: block-confined import hint
            return err;
        }
        if (!e->explicitTypeArgs.empty())
            return error(e->span,
                "explicit type arguments require a declared function, method, or constructor");
        return checkerDetail::unknown();
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
        Symbol* ns = checkerDetail::nsChainSym(scope_, callee->a.get());
        if (ns && ns->kind == SymbolKind::Namespace && ns->scope) {
            std::vector<const Stmt*> cands;
            if (const std::vector<Symbol*>* v = ns->scope->localLookup(callee->text))
                for (Symbol* s : *v)
                    if (s->kind == SymbolKind::Function && s->decl) cands.push_back(s->decl);
            if (const Stmt* fn = resolve(cands, "function")) {
                call->resolved = fn;
                return genericReturn(nullptr, fn, checkerDetail::unknown(), argTypes, e, &lambdaWalked);
            }
            if (!cands.empty()) return checkerDetail::unknown();
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
            if (const Stmt* m = resolve(methodOverloads(bt.sym, callee->text), "method",
                                        true, nullptr, bt.sym, &bt)) {
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
    if (!e->explicitTypeArgs.empty() && declaredCallAttempted)
        return Type{TKind::Error, nullptr, "", {}, nullptr, {}};
    if (!e->explicitTypeArgs.empty())
        return error(e->span,
            "explicit type arguments require a declared function, method, or constructor");
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
    return checkerDetail::unknown();
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
    const bool assignment = e->op == TokenKind::Eq || checkerDetail::isCompoundAssign(e->op);
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
        if (lt.canonical == "char" && checkerDetail::isCharLiteral(e->b.get())) {
            checkerDetail::markCharLiteral(e->b.get()); rt = primType("char");
        } else if (rt.canonical == "char" && checkerDetail::isCharLiteral(e->a.get())) {
            checkerDetail::markCharLiteral(e->a.get()); lt = primType("char");
        }
    }
    // bug #63: a plain assignment `charVar = 'X'` re-types the single-quoted
    // RHS literal to char against the LHS's declared char type — the same
    // by-expected-type retyping declarations do, extended to (re-)assignment,
    // which #50's stated scope didn't cover (`cannot assign 'string' to 'char'`).
    if (e->op == TokenKind::Eq && lt.canonical == "char" && checkerDetail::isCharLiteral(e->b.get())) {
        checkerDetail::markCharLiteral(e->b.get()); rt = primType("char");
    }

    // A value struct's non-mutating method may not write `this`'s fields (§9): the
    // receiver is a copy, so the write would be silently lost. Mark it `mutating`.
    if ((e->op == TokenKind::Eq || checkerDetail::isCompoundAssign(e->op)) &&
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
    if ((e->op == TokenKind::Eq || checkerDetail::isCompoundAssign(e->op)) &&
        e->a->kind == ExprKind::Name) {
        auto pc = constPending_.find(std::string(e->a->text));
        if (pc != constPending_.end()) {
            std::string nm(e->a->text);
            if (checkerDetail::isCompoundAssign(e->op))
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
    if (e->op == TokenKind::Eq || checkerDetail::isCompoundAssign(e->op)) {
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
                    return error(e->span, "no operator '" + std::string(checkerDetail::opSymbol(base)) +
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
            return error(e->span, "no operator '" + std::string(checkerDetail::opSymbol(e->op)) +
                         "' on '" + std::string(lt.sym->name) + "'");
        return lt;
    }
    // Operators are methods: on a user class, resolve the (op) overload by rhs type.
    if (lt.kind == TKind::Class) {
        auto cands = methodOverloads(lt.sym, checkerDetail::opSymbol(e->op));
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
        return checkerDetail::unknown();
    }

    if (e->op == TokenKind::AmpAmp || e->op == TokenKind::PipePipe) return primType("bool");
    if (isComparison(e->op)) return primType("bool");
    if (lt.kind == TKind::Primitive) return lt;      // void etc.
    return checkerDetail::unknown();
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
    for (const Slot* s : checkerDetail::slotsNamed(thisClass_->shape, name))
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
        for (const Slot* s : checkerDetail::slotsNamed(thisClass_->shape, name)) {
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
            for (const Slot* s : checkerDetail::slotsNamed(bt.sym->shape, e->text))
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
