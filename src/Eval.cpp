#include "Eval.hpp"
#include "RuntimeLoop.hpp"
#include "lv_task.h"   // LA-30: stackful-task substrate (techdesign-03)
#include <cassert>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <unordered_set>

namespace {

const char* opSymbol(TokenKind k) {
    switch (k) {
        case TokenKind::Plus: return "+";   case TokenKind::Minus: return "-";
        case TokenKind::Star: return "*";   case TokenKind::Slash: return "/";
        case TokenKind::Percent: return "%"; case TokenKind::EqEq: return "==";
        case TokenKind::BangEq: return "!="; case TokenKind::Lt: return "<";
        case TokenKind::Gt: return ">";     case TokenKind::Le: return "<=";
        case TokenKind::Ge: return ">=";    case TokenKind::LtLt: return "<<";
        case TokenKind::GtGt: return ">>";  default: return "?";
    }
}

bool isCompoundAssign(TokenKind k) {
    return k == TokenKind::PlusEq || k == TokenKind::MinusEq || k == TokenKind::StarEq ||
           k == TokenKind::SlashEq || k == TokenKind::PercentEq;
}
TokenKind compoundBase(TokenKind k) {
    switch (k) {
        case TokenKind::PlusEq:    return TokenKind::Plus;
        case TokenKind::MinusEq:   return TokenKind::Minus;
        case TokenKind::StarEq:    return TokenKind::Star;
        case TokenKind::SlashEq:   return TokenKind::Slash;
        case TokenKind::PercentEq: return TokenKind::Percent;
        default:                   return k;
    }
}

// LA-20: the type-name spelling used by every "got 'X'" diagnostic elsewhere
// in this file (evalCall's primitive-dispatch switch).
const char* valueKindName(const Value& v) {
    switch (v.kind) {
        case VKind::Int:     return "int";
        case VKind::Char:    return "char";
        case VKind::String:  return "string";
        case VKind::Bool:    return "bool";
        case VKind::Float:   return "float";
        case VKind::Array:   return "Array";
        case VKind::Map:     return "Map";
        case VKind::Block:   return "Block";
        case VKind::Ast:     return "Ast";
        case VKind::None:    return "None";
        case VKind::Object:  return "object";
        case VKind::Closure: return "closure";
        case VKind::Void:    return "void";
    }
    return "?";
}

// LA-20 §4: paths are plain relative paths with '/' separators — no leading
// '/', no '\', no empty path, no '.'/'..' segment. Lexical, checked before
// any filesystem contact: an escape-shaped path is refused outright rather
// than normalized (the same stance as the bare read of a collided member).
bool lexicallyValidImportPath(const std::string& p) {
    if (p.empty() || p.front() == '/') return false;
    if (p.find('\\') != std::string::npos) return false;
    size_t start = 0;
    while (start <= p.size()) {
        size_t slash = p.find('/', start);
        size_t end = slash == std::string::npos ? p.size() : slash;
        std::string seg = p.substr(start, end - start);
        if (seg.empty() || seg == "." || seg == "..") return false;
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return true;
}

bool readWholeFileForImport(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
//  helpers
// ---------------------------------------------------------------------------

Value* Evaluator::localLookup(std::string_view name) {
    for (auto it = env_.rbegin(); it != env_.rend(); ++it) {
        auto f = it->find(name);
        if (f != it->end()) return &f->second;
    }
    return nullptr;
}

// LA-30 B2 (doc 06 §2): raise a NAMED prelude exception class — throwRuntime
// parameterized so the cancellation carrier (CancelledException) rides the
// identical construct-and-flag machinery; catch matches by valueIsType, the
// same resolution-by-type test as everywhere else.
Value Evaluator::throwClass(const std::string& clsName, const std::string& message) {
    Symbol* cls = sema_.global->lookup(clsName);
    if (!cls) cls = sema_.global->lookup("RuntimeException");   // belt-and-braces
    if (cls) {
        std::vector<Value> args{vstr(message)};
        thrownValue_ = construct(cls, std::string(cls->name), args);
    } else {
        thrownValue_ = vstr(message);
    }
    throwing_ = true;
    return vvoid();
}

Value Evaluator::throwRuntime(const std::string& message) {
    return throwClass("RuntimeException", message);
}

Symbol* Evaluator::classOfValue(const Value& v) {
    switch (v.kind) {
        case VKind::Object: return v.obj ? v.obj->cls : nullptr;
        case VKind::Array:  return sema_.global->lookup("Array");
        case VKind::Map:    return sema_.global->lookup("Map");
        case VKind::Block:  return sema_.global->lookup("Block");
        case VKind::Ast:    return sema_.global->lookup("Ast");
        case VKind::Int:    return sema_.global->lookup("int");
        case VKind::Char:   return sema_.global->lookup("char");
        case VKind::String: return sema_.global->lookup("string");
        case VKind::Bool:   return sema_.global->lookup("bool");
        case VKind::Float:  return sema_.global->lookup("float");
        default:            return nullptr;
    }
}

// A value / range match-arm pattern: `subj == pattern`, or `lo <= subj <= hi`.
bool Evaluator::matchesValue(const Value& subj, Expr* pat) {
    if (pat->kind == ExprKind::Range) {
        Value lo = eval(pat->a.get()), hi = eval(pat->b.get());
        return subj.kind == VKind::Int && subj.i >= lo.i && subj.i <= hi.i;
    }
    Value pv = eval(pat);
    // struct-equality §6 (packet 07): a float value arm classifies by the
    // CANONICAL relation, not IEEE `==` — so `float::NaN =>` is a reachable arm
    // (canon(NaN)==canon(NaN)) and ±0.0 collapse to one arm. Canonical ≡ IEEE
    // except NaN, so no existing float match changes behavior. Route through the
    // ONE canon symbol (lv_canon, RuntimeValue.hpp — hash-consistency law §3.3);
    // the same symbol the `canonEq` native the other three engines lower to
    // uses. Mixed int/float arms keep the `combine` promotion path below (an int
    // pattern is never NaN, so canon vs IEEE agree there anyway).
    if (subj.kind == VKind::Float && pv.kind == VKind::Float)
        return lv_canon(subj.f) == lv_canon(pv.f);
    return combine(TokenKind::EqEq, subj, pv, nullptr).b;
}

bool Evaluator::valueIsType(const Value& v, const TypeRef* t) {
    if (!t) return false;
    if (t->kind == TypeKind::Union) {
        for (const TypeRefPtr& m : t->members)
            if (valueIsType(v, m.get())) return true;
        return false;
    }
    if (t->kind != TypeKind::Named) return false;
    std::string_view n = t->name;
    if (n == "None")   return v.kind == VKind::None;
    if (n == "int")    return v.kind == VKind::Int;
    if (n == "char")   return v.kind == VKind::Char;
    if (n == "string") return v.kind == VKind::String;
    if (n == "bool")   return v.kind == VKind::Bool;
    if (n == "float")  return v.kind == VKind::Float;
    if (n == "Array")  return v.kind == VKind::Array;
    if (n == "Map")    return v.kind == VKind::Map;
    if (n == "Block")  return v.kind == VKind::Block;
    if (n == "Ast")    return v.kind == VKind::Ast;
    if (v.kind == VKind::Object && v.obj && t->resolvedSymbol)
        return isSubclassOrSelf(v.obj->cls, t->resolvedSymbol);
    return false;
}

bool Evaluator::isSubclassOrSelf(Symbol* a, Symbol* b) {
    if (!a || !b) return false;
    if (a == b) return true;
    if (!a->decl) return false;
    for (const TypeRefPtr& base : a->decl->bases)
        if (isSubclassOrSelf(base->resolvedSymbol, b)) return true;
    return false;
}

// If `callee` names a constructor, set cls+label and return true.
//   Name C          -> (C, "C")           default/class-named ctor
//   C::Label        -> (C, "Label")
bool Evaluator::ctorTarget(Expr* callee, Symbol*& cls, std::string& label) {
    if (callee->kind == ExprKind::Name) {
        Symbol* s = sema_.global->lookup(callee->text);
        if (s && s->kind == SymbolKind::Class) { cls = s; label = std::string(callee->text); return true; }
        // bug.md #32 M2: bare `Class(...)` inside its own namespace, unchecked
        // prelude code — descend into the CURRENTLY EXECUTING free function's
        // enclosing namespace (curNamespace_) the same way the qualified form
        // below descends via an explicit `ns::` name.
        if (curNamespace_ && curNamespace_->scope) {
            const auto* v = curNamespace_->scope->localLookup(callee->text);
            if (v)
                for (Symbol* sym : *v)
                    if (sym->kind == SymbolKind::Class) {
                        cls = sym; label = std::string(callee->text); return true;
                    }
        }
    } else if (callee->kind == ExprKind::Member && callee->a->kind == ExprKind::Name) {
        Symbol* s = sema_.global->lookup(callee->a->text);
        if (s && s->kind == SymbolKind::Class) { cls = s; label = std::string(callee->text); return true; }
        // bug.md #32: `ns::Class(...)` — base is a namespace, not a class;
        // descend into its scope for a class matching the member name, the
        // same shape as resolveFunction's namespace descent below.
        if (s && s->kind == SymbolKind::Namespace && s->scope) {
            const auto* v = s->scope->localLookup(callee->text);
            if (v)
                for (Symbol* sym : *v)
                    if (sym->kind == SymbolKind::Class) { cls = sym; label = std::string(callee->text); return true; }
        }
    }
    return false;
}

// bug #37/#46: resolve a namespace qualifier expression to its Symbol. Handles
// both a bare `A` (Name) and a NESTED chain `A::B::…` (a Member-colon chain of
// namespaces), so a fully-qualified `A::B::member` reaches the innermost scope
// instead of the single-hop-only path the resolver used to have.
static Symbol* resolveNsChain(Scope* global, const Expr* e) {
    if (!e) return nullptr;
    if (e->kind == ExprKind::Name) {
        Symbol* s = global->lookup(e->text);
        return (s && s->kind == SymbolKind::Namespace) ? s : nullptr;
    }
    if (e->kind == ExprKind::Member && e->colon) {
        Symbol* base = resolveNsChain(global, e->a.get());
        if (base && base->scope)
            if (const auto* v = base->scope->localLookup(e->text))
                for (Symbol* s : *v)
                    if (s->kind == SymbolKind::Namespace) return s;
    }
    return nullptr;
}

const Stmt* Evaluator::resolveFunction(Expr* callee) {
    if (callee->kind == ExprKind::Name) {
        Symbol* s = sema_.global->lookup(callee->text);
        if (s && s->kind == SymbolKind::Function) return s->decl;
    } else if (callee->kind == ExprKind::Member) {
        // Accept BOTH `NS::fn` and the dot spelling `NS.fn` a namespace singleton
        // uses (e.g. `term.enableRaw()`): resolveNsChain resolves the qualifier
        // to a namespace (bare Name, dot or `::`) or a nested `A::B` chain; a
        // non-namespace base (an object receiver) yields null and falls through
        // to ordinary method dispatch. Requiring `::` here regressed the
        // dot-on-namespace form the pre-#37 resolver accepted.
        if (Symbol* ns = resolveNsChain(sema_.global, callee->a.get()))
            if (ns->scope)
                if (const auto* v = ns->scope->localLookup(callee->text))
                    for (Symbol* s : *v)
                        if (s->kind == SymbolKind::Function) return s->decl;
    }
    return nullptr;
}

bool Evaluator::classHasMember(Symbol* cls, std::string_view name) {
    if (!cls) return false;
    for (const Slot& s : cls->shape.slots)
        if (s.name == name) return true;
    return false;
}

const Stmt* Evaluator::findMethod(Symbol* cls, const std::string& name, int argc) {
    if (!cls) return nullptr;
    // bug.md #13: when the checker left an overloaded same-class call unresolved
    // (it never walks prelude class bodies, so `Expr::resolved` is null for every
    // bare self-call inside them), this by-name fallback must not pick the first
    // name-match arity-blind — that silently ran the 1-arg `indexOf` for a 2-arg
    // `indexOf(sub, from)` call and hung. When the caller passes the argument
    // count, prefer the unique method whose parameter count matches. A tie
    // (same arity, different param types) or no arity match keeps the legacy
    // first-found choice rather than guess; a single non-overloaded method is
    // unaffected either way.
    const Stmt* first = nullptr;
    const Stmt* arityHit = nullptr;
    int arityHits = 0;
    for (const Slot& s : cls->shape.slots) {
        if (!s.isMethod || s.name != name) continue;
        if (!first) first = s.decl;
        if (argc >= 0 && s.decl && (int)s.decl->params.size() == argc) {
            arityHit = s.decl;
            ++arityHits;
        }
    }
    if (arityHits == 1) return arityHit;
    return first;
}

// Is `decl` one of `cls`'s (flattened, inheritance-inclusive) method slots?
// Used to tell a pre-resolved same-class call apart from a free function.
bool Evaluator::isMethodOf(Symbol* cls, const Stmt* decl) {
    if (!cls || !decl) return false;
    for (const Slot& s : cls->shape.slots)
        if (s.isMethod && s.decl == decl) return true;
    return false;
}

const Stmt* Evaluator::findAccessor(Symbol* cls, const std::string& name, bool wantGet) {
    if (!cls || !cls->decl) return nullptr;
    for (const StmtPtr& m : cls->decl->body)
        if (((wantGet && m->isGet) || (!wantGet && m->isSet)) && m->name == name)
            return m.get();
    for (const TypeRefPtr& base : cls->decl->bases)
        if (const Stmt* a = findAccessor(base->resolvedSymbol, name, wantGet)) return a;
    return nullptr;
}

std::string Evaluator::keyFor(Symbol* cls, const std::string& name, const std::string& source) {
    if (cls)
        for (const Slot& s : cls->shape.slots) {
            if (s.isMethod || s.name != name) continue;
            if (!source.empty() && (!s.source || s.source->name != source)) continue;
            if (s.distinct && s.source) return std::string(s.source->name) + "::" + name;
            return name;
        }
    if (!source.empty()) return source + "::" + name;
    return name;
}

// ---------------------------------------------------------------------------
//  objects
// ---------------------------------------------------------------------------

void Evaluator::initFields(Object* obj, Symbol* cls) {
    for (const Slot& s : cls->shape.slots) {
        if (s.isMethod) continue;
        std::string key = (s.distinct && s.source)
            ? std::string(s.source->name) + "::" + std::string(s.name)
            : std::string(s.name);
        Value v;
        Symbol* fcls = s.decl && s.decl->type ? s.decl->type->resolvedSymbol : nullptr;
        bool valueField = fcls && fcls->kind == SymbolKind::Class && fcls->isValue &&
                          !fcls->isPrimitive && fcls->decl && !fcls->decl->isInterface;
        if (s.decl && s.decl->init) {
            auto savedThis = thisObj_; Symbol* savedCls = thisClass_;
            thisObj_ = nullptr; thisClass_ = cls;      // field inits are constants here
            v = eval(s.decl->init.get());
            thisObj_ = savedThis; thisClass_ = savedCls;
            // §15: a value-struct field owns its own copy — never alias the init
            if (valueField) v = copyValue(v);
        } else if (bareFieldSuppliedByCtor(cls, s)) {
            // A constructor definite-first-assigns this bare reference field, so
            // §3's throwaway default would only run a discarded ctor (whose side
            // effects can throw — Sonar's single-App rule). Leave the plain void
            // default; the ctor supplies the real value, exactly as a
            // construction-cycle field already relies on it to.
            v = defaultForCanonical(s.canonical);
        } else if (bareFieldAutoConstructs(fcls)) {
            // §3: a bare constructable-class field auto-constructs — there is no
            // null/unbound state. Value structs always do (a finite DAG — they
            // cannot self-reference by value); a reference class does too, unless
            // it sits on a construction cycle (onConstructionCycle), in which case
            // it has no finite default and keeps the void default below. The
            // predicate is static and shared with the lowerer, so field defaults
            // stay byte-identical across engines.
            std::vector<Value> none;
            v = construct(fcls, std::string(fcls->name), none);
        } else {
            v = defaultForCanonical(s.canonical);
        }
        obj->fields[key] = v;
    }
}

void Evaluator::runCtor(Symbol* cls, const std::string& label, std::vector<Value>& args,
                        std::shared_ptr<Object> self, const Stmt* ctorOverride) {
    const Stmt* ctor = ctorOverride;   // the checker's chosen overload, if any
    if (!ctor && cls->decl)
        for (const StmtPtr& m : cls->decl->body)
            if (m->isCtor && m->name == label && m->params.size() == args.size()) { ctor = m.get(); break; }
    if (!ctor) {
        // bug #38: a field-only struct/class with no explicit constructor but
        // positional args populates its declared data fields positionally, in
        // declaration (slot) order — the natural reading the Atlantis design's
        // `FieldError("email","required")` relies on. No args keeps the field
        // initializers (ordinary default construction), unchanged.
        if (!args.empty() && cls) {
            size_t ai = 0;
            for (const Slot& s : cls->shape.slots) {
                if (s.isMethod || ai >= args.size()) continue;
                std::string key = (s.distinct && s.source)
                    ? std::string(s.source->name) + "::" + std::string(s.name)
                    : std::string(s.name);
                self->fields[key] = copyValue(args[ai]);
                ++ai;
            }
        }
        return;   // no matching ctor (e.g. default) — fields already initialized
    }

    auto savedThis = thisObj_; Symbol* savedCls = thisClass_;
    bool savedInCtor = inCtor_; bool savedRet = returning_; Value savedRv = returnValue_;
    bool savedBrk = breaking_; bool savedCnt = continuing_;
    const Stmt* savedBrkT = breakTarget_; const Stmt* savedCntT = continueTarget_;
    // bug.md #32 M2: a ctor body is never itself a namespace-level free
    // function (it's gathered via gatherClass, so Stmt::enclosingNs is never
    // set on it) — don't let a caller's curNamespace_ leak into it.
    Symbol* savedNs = curNamespace_;
    curNamespace_ = nullptr;
    // Bug 21: the body runs on an ISOLATED stack (params frame only) — the
    // caller's still-live locals must be unreachable, or a bare-name field
    // write/read (implicit `this.`) binds to a same-named caller local.
    // Globals stay visible through globals_. Same swap as callClosure.
    auto savedEnv = std::move(env_);
    env_.clear();
    env_.emplace_back();
    for (size_t i = 0; i < ctor->params.size(); ++i) env_.back()[ctor->params[i].name] = copyValue(args[i]);
    thisObj_ = self; thisClass_ = cls; inCtor_ = true; returning_ = false;
    breaking_ = false; continuing_ = false;
    breakTarget_ = nullptr; continueTarget_ = nullptr;
    exec(ctor->memberBody.get());
    env_ = std::move(savedEnv);
    thisObj_ = savedThis; thisClass_ = savedCls; inCtor_ = savedInCtor;
    returning_ = savedRet; returnValue_ = savedRv;
    breaking_ = savedBrk; continuing_ = savedCnt;
    breakTarget_ = savedBrkT; continueTarget_ = savedCntT;
    curNamespace_ = savedNs;
}

Value Evaluator::construct(Symbol* cls, const std::string& label, std::vector<Value>& args,
                           const Stmt* ctorOverride) {
    // Native-backed collections construct to their value form, not a field object.
    if (cls && cls->name == "Array") {
        auto vec = std::make_shared<std::vector<Value>>();
        if (args.size() == 2) {                        // Array(n, fill)
            long long n = args[0].i;
            for (long long k = 0; k < n; ++k) vec->push_back(args[1]);
        }
        Value v; v.kind = VKind::Array; v.arr = vec; return v;
    }
    if (cls && cls->name == "Map") {
        Value v; v.kind = VKind::Map;
        v.map = std::make_shared<std::vector<std::pair<Value, Value>>>();
        return v;
    }
    // Track 03 §3: Block(n) -> a zeroed root buffer; Block::fromString(s) -> a
    // copy of the string's bytes. Reference value, not a field object.
    if (cls && cls->name == "Block") {
        if (label == "fromString") {
            const std::string& s = args.empty() ? std::string() : args[0].s;
            auto bd = std::make_shared<BlockData>();
            bd->bytes = std::make_shared<std::vector<uint8_t>>(s.begin(), s.end());
            bd->off = 0; bd->len = bd->bytes->size();
            return vblock(bd);
        }
        return vblockNew(args.empty() ? 0 : args[0].i);      // Block(n)
    }
    auto obj = std::make_shared<Object>();
    obj->cls = cls;
    initFields(obj.get(), cls);
    runCtor(cls, label, args, obj, ctorOverride);
    Value v; v.kind = VKind::Object; v.obj = obj;
    return v;
}

// Console output lands in the captured stream (out_); comptime evaluation
// (§16.5) redirects it to the real stdout instead — compile-time console
// output is emitted during compilation, not into the program's output.
void Evaluator::emitConsole(const std::string& text) {
    if (comptime_) {
        std::fwrite(text.data(), 1, text.size(), stdout);
        std::fflush(stdout);
        return;
    }
    interpEmitStdout(out_, text);   // captured; write-through once raw mode is on
}

// One step of comptime evaluation budget. Exhaustion raises an UNCATCHABLE
// pending throw (a runaway comptime loop must not be try-swallowed).
bool Evaluator::spendStep() {
    if (budgetExhausted_) return false;
    if (--steps_ > 0) return true;
    budgetExhausted_ = true;
    throwing_ = true;
    thrownValue_ = vstr("comptime step budget exceeded — runaway loop? "
                        "(--comptime-budget raises the limit)");
    return false;
}

// Item Q (techdesign-target-predicate.md): derive the `target::` constants
// from a --target triple, or from the host the compiler was built for when no
// triple was given. Values reflect the TARGET (portable pivot: cross emission
// must fold the destination's branch, never the build host's uname).
static void deriveTargetInfo(const std::string& triple, std::string& os,
                             std::string& arch, std::string& tripleOut) {
    if (triple.empty()) {
#if defined(_WIN32)
        os = "windows";
#elif defined(__APPLE__)
        os = "macos";
#elif defined(__linux__)
        os = "linux";
#else
        os = "unknown";
#endif
#if defined(__x86_64__) || defined(_M_X64)
        arch = "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
        arch = "aarch64";
#else
        arch = "unknown";
#endif
        tripleOut = arch + (os == "windows" ? "-pc-windows-gnu"
                          : os == "macos"   ? "-apple-darwin"
                          : os == "linux"   ? "-pc-linux-gnu"
                                            : "-unknown-unknown");
        return;
    }
    tripleOut = triple;
    arch = triple.substr(0, triple.find('-'));
    if (arch == "arm64") arch = "aarch64";
    if (arch.rfind("wasm", 0) == 0 || triple.find("wasi") != std::string::npos)
        os = "wasm";
    else if (triple.find("windows") != std::string::npos ||
             triple.find("mingw") != std::string::npos)
        os = "windows";
    else if (triple.find("darwin") != std::string::npos ||
             triple.find("macos") != std::string::npos ||
             triple.find("apple") != std::string::npos)
        os = "macos";
    else if (triple.find("linux") != std::string::npos)
        os = "linux";
    else
        os = "unknown";
}

void Evaluator::setComptime(const ComptimeOptions& o) {
    comptime_ = true;
    hermetic_ = o.hermetic;
    steps_ = o.stepBudget;
    budgetExhausted_ = false;
    deriveTargetInfo(o.targetTriple, targetOs_, targetArch_, targetTriple_);
}

void Evaluator::defineGlobal(const std::string& name, const Value& v) {
    globals_[name] = v;
}

// Evaluate one expression under the comptime gates. Failures (throws, budget,
// hermeticity) come back through `err` — never a crash, never a swallow.
Value Evaluator::finishComptime(Value v, std::string& err, bool& failed) {
    failed = false;
    if (budgetExhausted_) {                // sticky across sites (shared pool)
        failed = true;
        err = "comptime step budget exceeded — runaway loop? "
              "(--comptime-budget raises the limit)";
        throwing_ = false;
        returning_ = false;
        return vvoid();
    }
    if (throwing_) {
        failed = true;
        if (thrownValue_.kind == VKind::Object && thrownValue_.obj) {
            auto it = thrownValue_.obj->fields.find("message");
            err = it != thrownValue_.obj->fields.end() ? it->second.s
                                                       : "uncaught exception";
        } else {
            err = valueToString(thrownValue_);
        }
        throwing_ = false;                 // reset for the next comptime site
        returning_ = false;
    }
    return v;
}

Value Evaluator::evalComptime(Expr* e, std::string& err, bool& failed) {
    err.clear();
    env_.emplace_back();
    Value v = eval(e);
    env_.pop_back();
    return finishComptime(v, err, failed);
}

Value Evaluator::evalComptime(Expr* e, const std::unordered_map<std::string, Value>& locals,
                              std::string& err, bool& failed) {
    err.clear();
    env_.emplace_back();
    for (const auto& [name, val] : locals) env_.back()[std::string_view(name)] = val;
    Value v = eval(e);
    env_.pop_back();
    return finishComptime(v, err, failed);
}

Value Evaluator::evalComptimeBody(
    std::string_view macroName, std::string_view paramName, Stmt* body,
    const std::unordered_map<std::string, Value>& locals,
    std::string& err, bool& failed) {
    err.clear();
    auto savedEnv = std::move(env_);
    bool savedRet = returning_, savedBrk = breaking_, savedCnt = continuing_;
    const Stmt* savedBrkT = breakTarget_; const Stmt* savedCntT = continueTarget_;
    Value savedRv = returnValue_;
    SourceSpan savedReturnSpan = lastReturnSpan_;
    std::string_view savedMacro = comptimeMacroName_, savedParam = comptimeMacroParam_;
    Stmt* savedBody = comptimeMacroBody_;
    comptimeMacroName_ = macroName; comptimeMacroParam_ = paramName;
    comptimeMacroBody_ = body;
    env_.clear();
    env_.emplace_back();
    for (const auto& [name, val] : locals)
        env_.back()[std::string_view(name)] = copyValue(val);
    returning_ = breaking_ = continuing_ = false;
    breakTarget_ = nullptr; continueTarget_ = nullptr;
    returnValue_ = vvoid();
    lastReturnSpan_ = {};
    exec(body);
    comptimeReturnSpan_ = lastReturnSpan_;
    Value result = (returning_ && !throwing_) ? returnValue_ : vvoid();
    env_ = std::move(savedEnv);
    returning_ = savedRet; returnValue_ = savedRv;
    breaking_ = savedBrk; continuing_ = savedCnt;
    breakTarget_ = savedBrkT; continueTarget_ = savedCntT;
    lastReturnSpan_ = savedReturnSpan;
    comptimeMacroName_ = savedMacro; comptimeMacroParam_ = savedParam;
    comptimeMacroBody_ = savedBody;
    return finishComptime(result, err, failed);
}

Value Evaluator::callComptimeMacro(const Value& arg) {
    auto savedEnv = std::move(env_);
    bool savedRet = returning_, savedBrk = breaking_, savedCnt = continuing_;
    const Stmt* savedBrkT = breakTarget_; const Stmt* savedCntT = continueTarget_;
    Value savedRv = returnValue_;
    env_.clear(); env_.emplace_back();
    env_.back()[comptimeMacroParam_] = copyValue(arg);
    returning_ = breaking_ = continuing_ = false; returnValue_ = vvoid();
    breakTarget_ = nullptr; continueTarget_ = nullptr;
    exec(comptimeMacroBody_);
    Value result = (returning_ && !throwing_) ? returnValue_ : vvoid();
    env_ = std::move(savedEnv);
    returning_ = savedRet; returnValue_ = savedRv;
    breaking_ = savedBrk; continuing_ = savedCnt;
    breakTarget_ = savedBrkT; continueTarget_ = savedCntT;
    return result;
}

// LA-20: `import(path)` — a declared build input read at compile time (§6).
// Intercepted by callFunction below via Stmt* identity, before the prelude's
// throwing body ever runs. I01/I02 are lexical (checked before any file
// access); I03/I05 (plan builds) / I04 (single-file) come from resolution.
Value Evaluator::comptimeImport(std::vector<Value>& args) {
    if (args.empty() || args[0].kind != VKind::String)
        return throwRuntime("import: path must be a string (got '" +
                            std::string(args.empty() ? "nothing" : valueKindName(args[0])) +
                            "')");
    const std::string rel = args[0].s;
    if (!lexicallyValidImportPath(rel))
        return throwRuntime("import: path must be a plain project-relative path ('" +
                            rel + "' — no '..', no leading '/', '/' separators)");

    std::string abs;
    if (importCtx_.assets) {
        // Plan build (§5): resolve against the importing file's own module's
        // declared asset table — plain string equality, no path arithmetic.
        auto modIt = importCtx_.assets->find(importCtx_.currentModule);
        auto relIt = modIt != importCtx_.assets->end() ? modIt->second.find(rel)
                                                        : std::map<std::string, std::string>::const_iterator{};
        if (modIt == importCtx_.assets->end() || relIt == modIt->second.end()) {
            std::string note;
            for (const auto& [mod, table] : *importCtx_.assets) {
                if (mod == importCtx_.currentModule) continue;
                if (table.count(rel)) {
                    note = " (note: '" + rel + "' is an asset of module \"" + mod +
                          "\" — assets do not cross module boundaries)";
                    break;
                }
            }
            return throwRuntime("import: '" + rel + "' is not a declared asset of "
                                "this module — add it to assets = [...] in "
                                "trident.toml" + note);
        }
        abs = relIt->second;
    } else {
        // Single-file build (§5): root = the source file's own directory.
        abs = importCtx_.rootDir.empty() ? rel : importCtx_.rootDir + "/" + rel;
    }

    auto cached = importCache_.find(abs);
    if (cached != importCache_.end()) return vstr(cached->second);

    std::string content;
    if (!readWholeFileForImport(abs, content)) {
        if (importCtx_.assets)
            return throwRuntime("import: declared asset '" + rel + "' is unreadable "
                                "— the build plan is stale (re-run trident)");
        return throwRuntime("import: cannot read '" + rel + "' (relative to " +
                            (importCtx_.rootDir.empty() ? "." : importCtx_.rootDir) + ")");
    }
    importCache_[abs] = content;
    importedAssets_.push_back({rel, abs, importCtx_.currentModule, content.size()});
    return vstr(content);
}

// bug #35 (reject route A): collect a spawn body's bare Name references and the
// names BOUND within it (lambda params at any nesting, local Var/ForIn binders,
// catch binders). A referenced name that is NOT bound within the body and IS a
// program global is a bare-global reference — the shape that bypasses the
// capture flatten. Member names (field/method) are deliberately NOT collected:
// they never name a global variable, and collecting them would falsely reject a
// body merely touching a member that shares a global's name. Depth matches the
// LLVM/IR guard (computeFnGlobalRefs): nested lambda bodies ARE part of the
// body; a called top-level function's body is not (its name resolves to a
// function, never a globals_ entry — so it is harmless even if collected).
static void collectSpawnRefsStmt(const Stmt* s,
    std::unordered_set<std::string_view>& refs,
    std::unordered_set<std::string_view>& binders);
static void collectSpawnRefsExpr(const Expr* e,
    std::unordered_set<std::string_view>& refs,
    std::unordered_set<std::string_view>& binders) {
    if (!e) return;
    if (e->kind == ExprKind::Name) {
        if (!e->text.empty()) refs.insert(e->text);
        return;
    }
    if (e->kind == ExprKind::Member) {          // skip member name; walk the base
        collectSpawnRefsExpr(e->a.get(), refs, binders);
        collectSpawnRefsExpr(e->b.get(), refs, binders);
        collectSpawnRefsExpr(e->c.get(), refs, binders);
        for (const ExprPtr& x : e->list) collectSpawnRefsExpr(x.get(), refs, binders);
        return;
    }
    if (e->kind == ExprKind::Lambda)
        for (const Param& p : e->params) binders.insert(p.name);
    collectSpawnRefsExpr(e->a.get(), refs, binders);
    collectSpawnRefsExpr(e->b.get(), refs, binders);
    collectSpawnRefsExpr(e->c.get(), refs, binders);
    for (const ExprPtr& x : e->list) collectSpawnRefsExpr(x.get(), refs, binders);
    for (const MatchArm& arm : e->arms) {
        collectSpawnRefsExpr(arm.value.get(), refs, binders);
        collectSpawnRefsExpr(arm.bodyExpr.get(), refs, binders);
        collectSpawnRefsStmt(arm.bodyBlock.get(), refs, binders);
    }
    for (const Param& p : e->params) collectSpawnRefsExpr(p.defaultValue.get(), refs, binders);
    collectSpawnRefsStmt(e->block.get(), refs, binders);
}
static void collectSpawnRefsStmt(const Stmt* s,
    std::unordered_set<std::string_view>& refs,
    std::unordered_set<std::string_view>& binders) {
    if (!s) return;
    if ((s->kind == StmtKind::Var || s->kind == StmtKind::ForIn) && !s->name.empty())
        binders.insert(s->name);
    collectSpawnRefsExpr(s->expr.get(), refs, binders);
    collectSpawnRefsExpr(s->init.get(), refs, binders);
    collectSpawnRefsExpr(s->forStep.get(), refs, binders);
    for (const StmtPtr& b : s->body) collectSpawnRefsStmt(b.get(), refs, binders);
    collectSpawnRefsStmt(s->thenBranch.get(), refs, binders);
    collectSpawnRefsStmt(s->elseBranch.get(), refs, binders);
    collectSpawnRefsStmt(s->forInit.get(), refs, binders);
    collectSpawnRefsStmt(s->memberBody.get(), refs, binders);
    for (const CatchClause& c : s->catches) {
        if (!c.name.empty()) binders.insert(c.name);
        collectSpawnRefsStmt(c.body.get(), refs, binders);
    }
}
// True (err set) if the spawn `body` references a program global whose current
// value reaches a Promise A-1 forbids across a thread boundary. The tree-walk
// oracle keeps the source-AST lambda on the closure, so the free-name analysis
// above is available and sound here; the IR interpreter (lambda == null) uses
// its own slot-based check, byte-identical.
static bool spawnBodyGlobalPromise(const Value& body,
    const std::unordered_map<std::string, Value>& globals, std::string& err) {
    if (body.kind != VKind::Closure || !body.closure || !body.closure->lambda)
        return false;
    std::unordered_set<std::string_view> refs, binders;
    collectSpawnRefsExpr(body.closure->lambda, refs, binders);
    for (std::string_view nm : refs) {
        if (binders.count(nm)) continue;                 // bound within the body
        auto g = globals.find(std::string(nm));
        if (g == globals.end()) continue;                // not a program global
        if (lvThreadValueHasPromise(g->second, err)) return true;
    }
    return false;
}

Value Evaluator::callFunction(const Stmt* fn, std::vector<Value>& args,
                              std::shared_ptr<Object> self, Symbol* selfClass) {
    // LA-20: the comptime import() intercept, checked before the body ever
    // runs (the prelude body is an ordinary `throw`, for runtime misuse).
    // Symbol identity, not name, so a user's own `import` is never hijacked.
    if (comptime_ && importCtx_.importFn && fn == importCtx_.importFn)
        return comptimeImport(args);
    // Empty-bodied FREE function: a std::sys native (sysWrite/sysReadLine).
    if (!self && fn->memberBody && fn->memberBody->kind == StmtKind::Empty) {
        std::string name(fn->name);
        // Hermetic comptime (§16.5): the syscall floor is denied — compile-time
        // evaluation may not read files, sockets, stdin, clocks, or timers.
        // The one allowance is writing to stdout/stderr (console/compile logs).
        if (comptime_ && hermetic_ && name.rfind("sys", 0) == 0) {
            bool stdoutWrite = name == "sysWrite" && !args.empty() &&
                               (args[0].i == 1 || args[0].i == 2);
            if (!stdoutWrite)
                return throwRuntime("comptime code may not perform I/O ('" +
                                    name + "')");
            std::string data = args.size() > 1 ? args[1].s : "";
            emitConsole(data);
            return vint((long long)data.size());
        }
        // LA-30 B2 (doc 06 §4): the engine-level task natives — they spawn
        // language closures into tasks or PARK with the S3/G7 save/restore
        // discipline, so they live here, not in the shared RuntimeNatives.
        // (sys*-prefixed, so the hermetic-comptime deny above fired first —
        // G14 holds: comptime evaluation never reaches the scheduler.)
        {
            Value out;
            if (taskNative(name, args, out)) return out;
        }
        // bug #35 (reject route A): a spawn body reaching a Promise through a
        // bare GLOBAL bypasses the capture flatten (sysThreadTransfer walks only
        // captures). Re-apply A-1's reject at the spawn call — before the
        // cooperative task starts — byte-identical to the captured-Promise
        // reject and to the LLVM/IR guards. sysThreadStart is the spawn-only
        // path; TaskGroup's same-thread sysTaskRun never reaches here.
        if (name == "sysThreadStart" && !args.empty()) {
            std::string gerr;
            if (spawnBodyGlobalPromise(args[0], globals_, gerr)) return throwRuntime(gerr);
        }
        Value out; std::string err;
        if (nativeFreeCall(name, args, out, err, &out_)) {
            if (!err.empty()) return throwRuntime(err);
            // env.exit (designs/exit-codes.md §4): the native recorded a code +
            // requested exit. Ride the throwing_ unwind (every `if (throwing_)`
            // guard stops), but flag it as a clean exit so run() reports no
            // "Uncaught" and threads out the code, not 1.
            if (interpExitRequested() && !exiting_) { exiting_ = true; throwing_ = true; }
            return out;
        }
        return vvoid();
    }
    // Console natives (write/writeln): stringify-and-emit. This is the path
    // for aliased receivers (`Console c = console; c.writeln(x)`) — the
    // by-name fast path in evalCall covers the common spelling.
    if (self && selfClass && selfClass->name == "Console" &&
        fn->memberBody && fn->memberBody->kind == StmtKind::Empty) {
        std::string line;
        for (const Value& a : args) line += valueToString(a);
        if (fn->name == "writeln") line += "\n";
        emitConsole(line);
        return vvoid();
    }
    auto savedThis = thisObj_; Symbol* savedCls = thisClass_;
    bool savedInCtor = inCtor_; bool savedRet = returning_; Value savedRv = returnValue_;
    bool savedBrk = breaking_; bool savedCnt = continuing_;
    const Stmt* savedBrkT = breakTarget_; const Stmt* savedCntT = continueTarget_;
    // bug.md #32 M2: a free (non-method) function's enclosing namespace, so an
    // unchecked prelude body can resolve a bare same-namespace `Class(...)`
    // construction (ctorTarget) the same way it already resolves a bare
    // same-namespace function call (classHasMember/self-method path aside).
    Symbol* savedNs = curNamespace_;
    if (!self) curNamespace_ = fn->enclosingNs;
    // Bug 21: isolated stack — see runCtor.
    auto savedEnv = std::move(env_);
    env_.clear();
    env_.emplace_back();
    for (size_t i = 0; i < fn->params.size() && i < args.size(); ++i)
        env_.back()[fn->params[i].name] = copyValue(args[i]);
    thisObj_ = self; thisClass_ = selfClass; inCtor_ = false; returning_ = false;
    breaking_ = false; continuing_ = false;
    breakTarget_ = nullptr; continueTarget_ = nullptr;
    returnValue_ = vvoid();
    exec(fn->memberBody.get());
    Value result = (returning_ && !throwing_) ? returnValue_ : vvoid();
    env_ = std::move(savedEnv);
    thisObj_ = savedThis; thisClass_ = savedCls; inCtor_ = savedInCtor;
    returning_ = savedRet; returnValue_ = savedRv;
    breaking_ = savedBrk; continuing_ = savedCnt;
    breakTarget_ = savedBrkT; continueTarget_ = savedCntT;
    curNamespace_ = savedNs;
    return result;
}

Value Evaluator::callClosure(std::shared_ptr<Closure> cl, std::vector<Value>& args) {
    auto savedEnv = std::move(env_);
    auto savedThis = thisObj_; Symbol* savedCls = thisClass_;
    bool savedRet = returning_; Value savedRv = returnValue_;
    bool savedBrk = breaking_; bool savedCnt = continuing_;
    const Stmt* savedBrkT = breakTarget_; const Stmt* savedCntT = continueTarget_;

    env_ = cl->env;                       // the captured environment
    env_.emplace_back();
    for (size_t i = 0; i < cl->lambda->params.size() && i < args.size(); ++i)
        env_.back()[cl->lambda->params[i].name] = copyValue(args[i]);
    thisObj_ = cl->thisObj; thisClass_ = cl->thisClass;
    returning_ = false; returnValue_ = vvoid();
    breaking_ = false; continuing_ = false;
    breakTarget_ = nullptr; continueTarget_ = nullptr;
    Value r;
    if (cl->lambda->block) {                 // statement-block body
        exec(cl->lambda->block.get());
        r = returning_ ? returnValue_ : vvoid();
    } else {
        r = eval(cl->lambda->a.get());       // expression body
    }

    env_ = std::move(savedEnv);
    thisObj_ = savedThis; thisClass_ = savedCls;
    returning_ = savedRet; returnValue_ = savedRv;
    breaking_ = savedBrk; continuing_ = savedCnt;
    breakTarget_ = savedBrkT; continueTarget_ = savedCntT;
    return r;
}

Value Evaluator::callPrimMethod(const Stmt* fn, const Value& self,
                                std::vector<Value>& args, std::string_view className) {
    // Empty body => native intrinsic.
    if (!fn->memberBody || fn->memberBody->kind == StmtKind::Empty) {
        Value out;
        if (nativeCall(std::string(className), std::string(fn->name), self, args, out))
            return out;
        return vvoid();
    }
    // In-language body, with `this` = the primitive/array value.
    auto savedThis = thisObj_; Symbol* savedCls = thisClass_;
    Value savedPrim = primThis_; bool savedHas = hasPrimThis_;
    bool savedRet = returning_; Value savedRv = returnValue_;
    bool savedBrk = breaking_; bool savedCnt = continuing_;
    const Stmt* savedBrkT = breakTarget_; const Stmt* savedCntT = continueTarget_;
    // Bug 21: isolated stack — see runCtor.
    auto savedEnv = std::move(env_);
    env_.clear();
    env_.emplace_back();
    for (size_t i = 0; i < fn->params.size() && i < args.size(); ++i)
        env_.back()[fn->params[i].name] = copyValue(args[i]);
    thisObj_ = nullptr; thisClass_ = sema_.global->lookup(className);
    primThis_ = self; hasPrimThis_ = true; returning_ = false;
    breaking_ = false; continuing_ = false;
    breakTarget_ = nullptr; continueTarget_ = nullptr;
    exec(fn->memberBody.get());
    Value result = returning_ ? returnValue_ : vvoid();
    env_ = std::move(savedEnv);
    thisObj_ = savedThis; thisClass_ = savedCls;
    primThis_ = savedPrim; hasPrimThis_ = savedHas;
    returning_ = savedRet; returnValue_ = savedRv;
    breaking_ = savedBrk; continuing_ = savedCnt;
    breakTarget_ = savedBrkT; continueTarget_ = savedCntT;
    return result;
}

bool Evaluator::nativeCall(const std::string& cls, const std::string& m,
                           const Value& self, std::vector<Value>& args, Value& out) {
    std::string err;
    if (!::nativeCall(cls, m, self, args, out, err)) return false;
    if (!err.empty()) { out = throwRuntime(err); }
    return true;
}

// ---------------------------------------------------------------------------
//  member access
// ---------------------------------------------------------------------------

bool Evaluator::memberTarget(Expr* e, std::shared_ptr<Object>& obj,
                             std::string& name, std::string& source) {
    name = std::string(e->text);
    Expr* base = e->a.get();
    // `base.Ancestor::member`: the inner `.Ancestor` is a source qualifier.
    if (base->kind == ExprKind::Member) {
        Symbol* maybeClass = sema_.global->lookup(base->text);
        if (maybeClass && maybeClass->kind == SymbolKind::Class) {
            Value bv = eval(base->a.get());
            if (bv.kind != VKind::Object) return false;
            obj = bv.obj; source = std::string(base->text);
            return true;
        }
    }
    Value bv = eval(base);
    if (bv.kind != VKind::Object) return false;
    obj = bv.obj; source = "";
    return true;
}

Value Evaluator::memberRead(std::shared_ptr<Object> obj, const std::string& name,
                            const std::string& source) {
    if (rawField_ != name)
        if (const Stmt* getter = findAccessor(obj->cls, name, /*wantGet*/ true)) {
            std::string saved = rawField_; rawField_ = name;
            std::vector<Value> none;
            Value r = callFunction(getter, none, obj, obj->cls);
            rawField_ = saved;
            return r;
        }
    std::string key = keyFor(obj->cls, name, source);
    for (const Slot& s : obj->cls->shape.slots)
        if (!s.isMethod && s.isWeak &&
            (key == s.name || (s.source && key == std::string(s.source->name) + "::" + std::string(s.name))))
            return weakObjectRead(obj, key);
    auto it = obj->fields.find(key);
    return it != obj->fields.end() ? it->second : vvoid();
}

void Evaluator::memberWrite(std::shared_ptr<Object> obj, const std::string& name,
                            const std::string& source, const Value& v) {
    if (rawField_ != name)
        if (const Stmt* setter = findAccessor(obj->cls, name, /*wantGet*/ false)) {
            std::string saved = rawField_; rawField_ = name;
            std::vector<Value> args{v};
            callFunction(setter, args, obj, obj->cls);
            rawField_ = saved;
            return;
        }
    std::string key = keyFor(obj->cls, name, source);
    for (const Slot& s : obj->cls->shape.slots)
        if (!s.isMethod && s.isWeak &&
            (key == s.name || (s.source && key == std::string(s.source->name) + "::" + std::string(s.name)))) {
            weakObjectWrite(obj, key, v); return;
        }
    obj->fields[key] = v;
}

// ---------------------------------------------------------------------------
//  expressions
// ---------------------------------------------------------------------------

Value Evaluator::evalAssign(Expr* lhs, const Value& vin) {
    Value v = copyValue(vin);          // value structs copy into their slot (§9)
    if (lhs->kind == ExprKind::Name) {
        if (Value* local = localLookup(lhs->text)) { *local = v; return v; }
        if (thisObj_) { memberWrite(thisObj_, std::string(lhs->text), "", v); return v; }
        {   // bug.md #2: bare top-level / namespace globals store in globals_
            // (keyed by the declaration's own name when reached via an alias,
            // same as the read path). Before this, a free function's write to
            // a global was silently discarded.
            std::string key = lhs->resolved ? std::string(lhs->resolved->name)
                                             : std::string(lhs->text);
            auto g = globals_.find(key);
            if (g != globals_.end()) { g->second = v; return v; }
        }
    } else if (lhs->kind == ExprKind::Member) {
        std::shared_ptr<Object> obj; std::string name, source;
        if (memberTarget(lhs, obj, name, source)) memberWrite(obj, name, source, v);
    } else if (lhs->kind == ExprKind::Index) {
        Value base = eval(lhs->a.get());
        Value idx = eval(lhs->b.get());
        if (base.kind == VKind::Object && base.obj->cls) {
            // mutable object: dispatch to its ([]) set accessor (mutates in place)
            if (const Stmt* s = findAccessor(base.obj->cls, "[]", /*wantGet*/ false)) {
                std::vector<Value> args{idx, v};
                callFunction(s, args, base.obj, base.obj->cls);
            }
        } else if (base.kind == VKind::Array) {
            // pure array: produce a new array and rebind the base lvalue (§11)
            auto nv = std::make_shared<std::vector<Value>>(base.arr ? *base.arr
                                                                     : std::vector<Value>{});
            long long i = idx.i;
            if (i >= 0 && i < (long long)nv->size()) (*nv)[(size_t)i] = v;
            Value newArr; newArr.kind = VKind::Array; newArr.arr = nv;
            evalAssign(lhs->a.get(), newArr);
        } else if (base.kind == VKind::Map) {
            // pure map: m[k] = v  ==  m = m.with(k, v)  (rebind)
            auto nv = std::make_shared<std::vector<std::pair<Value, Value>>>(
                base.map ? *base.map : std::vector<std::pair<Value, Value>>{});
            bool replaced = false;
            for (auto& [k, val] : *nv)
                if (keyEquals(k, idx)) { val = v; replaced = true; break; }
            if (!replaced) nv->push_back({idx, v});
            Value newMap; newMap.kind = VKind::Map; newMap.map = nv;
            evalAssign(lhs->a.get(), newMap);
        }
    }
    return v;
}

// bug.md #58: at comptime the checker's expected-type char retype has not run
// yet (macro expansion precedes checking), so a single-quoted single-scalar
// literal (`'<'`) is still evaluated as a 1-char string. Detect such a literal
// so a comparison against a genuine char can decode it to a char here, matching
// the runtime behavior where the checker retyped it.
static bool comptimeCharLit(const Expr* e, long long& scalarOut) {
    if (!e || e->kind != ExprKind::StringLit || !e->singleQuoted || e->charLit) return false;
    std::string c = decodeEscapes(e->text);
    if (c.empty()) return false;
    size_t len; bool boundary;
    long long sc = utf8DecodeAt(c, 0, len, boundary);
    if (len == 0 || len != c.size()) return false;   // exactly one Unicode scalar
    scalarOut = sc;
    return true;
}

Value Evaluator::evalBinary(Expr* e) {
    if (throwing_) return vvoid();
    if (e->op == TokenKind::Eq) return evalAssign(e->a.get(), eval(e->b.get()));

    if (e->op == TokenKind::QuestionQuestion) {   // a ?? b — default when None
        Value l = eval(e->a.get());
        if (throwing_) return vvoid();
        return l.kind == VKind::None ? eval(e->b.get()) : l;
    }

    // compound assignment: a op= b  ==  a = a op b, using the SAME operator
    // machinery as a plain binary op (so obj += x dispatches the (+) method).
    if (isCompoundAssign(e->op)) {
        Value cur = eval(e->a.get());
        Value rhs = eval(e->b.get());
        if (throwing_) return vvoid();
        Value nv = combine(compoundBase(e->op), cur, rhs, nullptr);
        if (throwing_) return vvoid();
        evalAssign(e->a.get(), nv);
        return nv;
    }

    Value l = eval(e->a.get());
    if (throwing_) return vvoid();
    if (e->op == TokenKind::AmpAmp) return vbool(l.b && eval(e->b.get()).b);
    if (e->op == TokenKind::PipePipe) return vbool(l.b || eval(e->b.get()).b);

    Value r = eval(e->b.get());
    if (throwing_) return vvoid();
    // bug.md #58: comptime char-literal retype at a comparison site — a genuine
    // char compared against an un-retyped single-quoted literal (still a string)
    // would always differ; decode the literal to a char so `c == '<'` behaves at
    // comptime exactly as it does at runtime.
    if (comptime_ && (e->op == TokenKind::EqEq || e->op == TokenKind::BangEq ||
                      e->op == TokenKind::Lt || e->op == TokenKind::Gt ||
                      e->op == TokenKind::Le || e->op == TokenKind::Ge)) {
        long long sc;
        if (l.kind == VKind::Char && r.kind == VKind::String && comptimeCharLit(e->b.get(), sc))
            r = vchar(sc);
        else if (r.kind == VKind::Char && l.kind == VKind::String && comptimeCharLit(e->a.get(), sc))
            l = vchar(sc);
    }
    return combine(e->op, l, r, e->resolved);
}

// Apply a binary operator to two already-evaluated values: None tag-compare,
// object operator dispatch (checker-chosen or by symbol, with (!=) derivation),
// else primitive arithmetic. Shared by binary ops and compound assignment.
Value Evaluator::combine(TokenKind op, const Value& l, const Value& r,
                         const Stmt* resolved) {
    if (op == TokenKind::EqEq || op == TokenKind::BangEq)
        if (l.kind == VKind::None || r.kind == VKind::None)
            return arithPrim(op, l, r);
    if (l.kind == VKind::Object && l.obj->cls && l.obj->cls->name != "Range") {
        const Stmt* m = resolved ? resolved : findMethod(l.obj->cls, opSymbol(op));
        if (m) {
            std::vector<Value> args{r};
            return callFunction(m, args, l.obj, l.obj->cls);
        }
        if (op == TokenKind::BangEq)
            if (const Stmt* eq = findMethod(l.obj->cls, "==")) {
                std::vector<Value> args{r};
                return vbool(!callFunction(eq, args, l.obj, l.obj->cls).b);
            }
        if ((op == TokenKind::EqEq || op == TokenKind::BangEq) && !l.obj->cls->isValue) {
            // a class with no (==) compares by reference identity (design §5.2).
            // A value struct instead gets a synthesized field-wise (==) at
            // resolve time (designs/struct-equality/, §5.5) and never lands
            // here from checked code; falling through makes any hole loud.
            bool same = r.kind == VKind::Object && l.obj == r.obj;
            return vbool(op == TokenKind::EqEq ? same : !same);
        }
        return throwRuntime(std::string("no operator '") + opSymbol(op) + "' on '" +
                            std::string(l.obj->cls->name) + "'");
    }
    std::string err;
    Value v = arithPrim(op, l, r, &err);
    if (!err.empty()) return throwRuntime(err);
    return v;
}

Value Evaluator::evalCall(Expr* e) {
    Expr* callee = e->a.get();

    // console.writeln / console.write fast path (the Console class's natives,
    // reached through the global's name; aliased receivers go through
    // callFunction's Console-native path instead).
    if (callee->kind == ExprKind::Member && callee->a->kind == ExprKind::Name &&
        callee->a->text == "console" && !localLookup(callee->a->text)) {
        std::string line;
        for (const ExprPtr& a : e->list) {
            Value av = eval(a.get());
            if (throwing_) return vvoid();     // an arg threw: emit nothing
            line += valueToString(av);
        }
        if (callee->text == "writeln") line += "\n";
        emitConsole(line);
        return vvoid();
    }

    // a?.m(...): evaluate the receiver first; None short-circuits (args unevaluated)
    if (callee->kind == ExprKind::Member && callee->optChain) {
        Value bv = eval(callee->a.get());
        if (throwing_) return vvoid();
        if (bv.kind == VKind::None) return vnone();
        std::vector<Value> args;
        for (const ExprPtr& a : e->list) {
            args.push_back(eval(a.get()));
            if (throwing_) return vvoid();
        }
        std::string name(callee->text);
        if (bv.kind == VKind::Object && bv.obj) {
            const Stmt* m = e->resolved ? e->resolved
                                        : findMethod(bv.obj->cls, name, (int)args.size());
            if (m) return callFunction(m, args, bv.obj, bv.obj->cls);
        }
        Symbol* rc = classOfValue(bv);
        if (rc)
            if (const Stmt* m = e->resolved ? e->resolved
                                            : findMethod(rc, name, (int)args.size()))
                return callPrimMethod(m, bv, args, rc->name);
        return throwRuntime("cannot resolve call target '" + name + "'");
    }

    std::vector<Value> args;
    for (const ExprPtr& a : e->list) {
        args.push_back(eval(a.get()));
        if (throwing_) return vvoid();
    }

    // F4: the parser stays behind one callback owned by RuleEngine. At
    // runtime these calls are not intercepted and their prelude bodies throw.
    if (comptime_ && parseHook_ && callee->kind == ExprKind::Member && callee->colon &&
        callee->a && callee->a->kind == ExprKind::Name && callee->a->text == "meta" &&
        (callee->text == "parseExpr" || callee->text == "parseStmts")) {
        if (args.size() != 1 || args[0].kind != VKind::String)
            return throwRuntime("meta::" + std::string(callee->text) +
                                " expects one string argument");
        std::string parseErr;
        Value parsed = parseHook_(callee->text == "parseExpr", args[0].s, e->span, parseErr);
        if (!parseErr.empty()) return throwRuntime(parseErr);
        return parsed;
    }
    // A procedural macro is also an ordinary comptime function inside its own
    // body. This is recursion, not macro-expansion re-entry; the shared step
    // budget remains the termination guard.
    if (comptime_ && comptimeMacroBody_ && callee->kind == ExprKind::Name &&
        callee->text == comptimeMacroName_) {
        if (args.size() != 1 || args[0].kind != VKind::String)
            return throwRuntime("recursive procedural macro call expects one string argument");
        return callComptimeMacro(args[0]);
    }

    // closure stored in a local (e.g. a function-typed parameter)
    if (callee->kind == ExprKind::Name)
        if (Value* l = localLookup(callee->text))
            if (l->kind == VKind::Closure) return callClosure(l->closure, args);

    // constructor?  (checker-resolved class first; global-name fallback)
    // Track 03 §2: if the checker already resolved the call to a callable non-
    // ctor (e.g. `Enum::fromCode`), it is NOT construction — don't let the
    // by-name ctorTarget heuristic hijack `Enum::fromCode` as a ctor label.
    bool resolvedToFn = e->resolved && e->resolved->kind == StmtKind::Member &&
                        e->resolved->callable && !e->resolved->isCtor;
    Symbol* cls = e->resolvedClass; std::string label;
    if (cls) label = std::string(callee->kind == ExprKind::Member ? callee->text
                                                                   : callee->text);
    if (!resolvedToFn && (cls || ctorTarget(callee, cls, label))) {
        if (thisObj_ && inCtor_ && isSubclassOrSelf(thisClass_, cls)) {
            runCtor(cls, label, args, thisObj_, e->resolved);   // base init on `this`
            return vvoid();
        }
        return construct(cls, label, args, e->resolved);
    }

    // Name callee: resolved function overload, else by-name function, else self-method
    if (callee->kind == ExprKind::Name) {
        if (e->resolved) {
            // A bare same-class call (`show()` with no `this.`) that the checker
            // pre-resolved must still dispatch on the enclosing receiver — passing
            // nullptr here silently drops `this` (bug 4). Only reuse the receiver
            // when the resolved target is actually an instance method of it; a
            // free/namespaced function keeps the nullptr receiver.
            if (thisObj_ && isMethodOf(thisObj_->cls, e->resolved))
                return callFunction(e->resolved, args, thisObj_, thisObj_->cls);
            if (hasPrimThis_ && thisClass_ && isMethodOf(thisClass_, e->resolved))
                return callPrimMethod(e->resolved, primThis_, args, thisClass_->name);
            return callFunction(e->resolved, args, nullptr, nullptr);
        }
        // bug.md #29: unchecked bodies (notably the prelude) have no resolved
        // target. A bare call there must prefer a member on the enclosing
        // receiver over a same-named user global, matching Lower.cpp's
        // classHasMember guard. The check is deliberately name-only so the
        // oracle and compiled engines also agree on arity-mismatch failures.
        Symbol* selfCls = thisObj_ ? thisObj_->cls
                        : (hasPrimThis_ ? thisClass_ : nullptr);
        bool shadowedBySelf = selfCls && classHasMember(selfCls, callee->text);
        if (!shadowedBySelf)
            if (const Stmt* fn = resolveFunction(callee))
                return callFunction(fn, args, nullptr, nullptr);
        std::string name(callee->text);
        if (thisObj_) {
            if (const Stmt* m = findMethod(thisObj_->cls, name, (int)args.size()))
                return callFunction(m, args, thisObj_, thisObj_->cls);
        } else if (hasPrimThis_ && thisClass_) {
            if (const Stmt* m = findMethod(thisClass_, name, (int)args.size()))
                return callPrimMethod(m, primThis_, args, thisClass_->name);
        }
    }

    // Member callee: method on an object/primitive/array, or a namespaced function
    if (callee->kind == ExprKind::Member) {
        // `recv.Base::method(...)` — the inner `.Base` is a SOURCE QUALIFIER
        // naming a base class (§4), not a field read on `recv`. The receiver is
        // `recv` and dispatch is static to the checker-resolved base method.
        // Without this, `eval(callee->a)` reads a nonexistent "Base" member,
        // yields void, and the call below runs against a nullptr receiver —
        // silently wrong on --ir, a null-`this` segfault here (bug.md #55).
        Expr* recvExpr = callee->a.get();
        if (callee->colon && recvExpr->kind == ExprKind::Member && e->resolved) {
            // `recv.Base::method(...)` — the inner `.Base` is a SOURCE QUALIFIER
            // naming a base class (§4), not a field read on `recv`. Evaluate the
            // real receiver (recvExpr->a) and dispatch statically to the
            // checker-resolved base method.
            //
            // The base qualifier may be NAMESPACED (e.g. `Sonar::App` reached as
            // `this.App::renderFrame()`), so this must NOT gate on a bare global
            // class lookup of recvExpr->text — `sema_.global->lookup` misses
            // every namespaced base, the guard is skipped, and the call falls
            // through to the NS::fn path below where `bv` is Void and the base
            // method runs with a nullptr `this` (the SonarApp live-loop crash:
            // the first frame's `this.App::renderFrame()` ran on a null receiver,
            // segfaulting the traversal). A genuine namespace qualifier
            // (`Ns::fn()`) has recvExpr->kind == Name (not Member) and never
            // reaches here; a Member receiver that evaluates to a real object is
            // unambiguously a base-qualified instance call.
            Value rv = eval(recvExpr->a.get());
            if (throwing_) return vvoid();
            if (rv.kind == VKind::Object && rv.obj)
                return callFunction(e->resolved, args, rv.obj, rv.obj->cls);
        }
        Value bv = eval(callee->a.get());
        if (throwing_) return vvoid();
        if (bv.kind == VKind::Void && e->resolved)            // NS::fn (checker-resolved)
            return callFunction(e->resolved, args, nullptr, nullptr);
        std::string name(callee->text);
        if (bv.kind == VKind::Object && !bv.obj) {
            // A receiver tagged Object but carrying a null pointer is an
            // uninitialized / dangling object reference (e.g. a never-assigned
            // non-optional class/interface field surfacing during a traversal —
            // not a None, which is VKind::None and narrows normally). Reading
            // bv.obj->cls below would be a hard segfault; raise a catchable
            // runtime error instead so user code can never crash the engine
            // (same defensive posture as the null-`this` guards above, #55).
            return throwRuntime("call of method '" + name +
                                "' on a null object reference");
        }
        if (bv.kind == VKind::Object) {
            const Stmt* m = e->resolved ? e->resolved
                                        : findMethod(bv.obj->cls, name, (int)args.size());
            if (m) return callFunction(m, args, bv.obj, bv.obj->cls);
        } else {
            const char* cn = nullptr;
            switch (bv.kind) {
                case VKind::Int:    cn = "int"; break;
                case VKind::Char:   cn = "char"; break;
                case VKind::String: cn = "string"; break;
                case VKind::Bool:   cn = "bool"; break;
                case VKind::Float:  cn = "float"; break;
                case VKind::Array:  cn = "Array"; break;
                case VKind::Map:    cn = "Map"; break;
                case VKind::Block:  cn = "Block"; break;
                case VKind::Ast:    cn = "Ast"; break;
                default: break;
            }
            if (cn)
                if (Symbol* s = sema_.global->lookup(cn)) {
                    const Stmt* m = e->resolved ? e->resolved
                                                : findMethod(s, name, (int)args.size());
                    if (m) return callPrimMethod(m, bv, args, cn);
                }
        }
        if (const Stmt* fn = resolveFunction(callee))   // namespaced function (NS::fn)
            return callFunction(fn, args, nullptr, nullptr);
    }

    // fallback: callee evaluates to a closure (returned or stored elsewhere)
    Value cv = eval(callee);
    if (cv.kind == VKind::Closure) return callClosure(cv.closure, args);
    if (throwing_) return vvoid();
    return throwRuntime("cannot resolve call target" +
                        (callee->text.empty() ? std::string()
                                              : " '" + std::string(callee->text) + "'"));
}

Value Evaluator::eval(Expr* e) {
    if (!e) return vvoid();
    if (comptime_ && !spendStep()) return vvoid();
    switch (e->kind) {
        case ExprKind::IntLit:    return vint(parseIntLiteral(e->text));
        case ExprKind::FloatLit:  return vfloat(parseFloatLiteral(e->text));
        case ExprKind::StringLit: {
            // F4: an interpolation segment's text is already bare content
            // (no quotes to strip — see Expr::isRawSegment).
            std::string decoded = (e->isQuasiPayload || e->isRawString) ? std::string(e->text)
                                  : e->isRawSegment ? decodeEscapes(e->text)
                                                   : decodeStringLiteral(e->text);
            // Track 03 §1: the checker flipped this single-scalar literal to char.
            if (e->charLit) {
                size_t len; bool boundary;
                return vchar(utf8DecodeAt(decoded, 0, len, boundary));
            }
            return vstr(decoded);
        }
        case ExprKind::BoolLit:   return vbool(e->text == "true");
        case ExprKind::This: {
            if (hasPrimThis_) return primThis_;
            Value v; v.kind = VKind::Object; v.obj = thisObj_; return v;
        }
        case ExprKind::Name: {
            if (e->text == "None") return vnone();
            if (rawField_ == std::string(e->text) && thisObj_) {
                auto it = thisObj_->fields.find(keyFor(thisObj_->cls, std::string(e->text), ""));
                return it != thisObj_->fields.end() ? it->second : vvoid();
            }
            if (Value* local = localLookup(e->text)) return *local;
            if (thisObj_) {
                for (const Slot& s : thisClass_->shape.slots)
                    if (!s.isMethod && s.name == e->text)
                        return memberRead(thisObj_, std::string(e->text), "");
            }
            {   // A namespaced value global (e.g. `read` via the implicit std
                // import), possibly reached through a `use ... as` alias
                // (imports.md §4). `globals_` is keyed by the declaration's
                // OWN name, so resolve through the checker-recorded decl
                // (e->resolved) when the read used an alias.
                std::string key = e->resolved ? std::string(e->resolved->name)
                                               : std::string(e->text);
                auto g = globals_.find(key);
                if (g != globals_.end()) return g->second;
            }
            return vvoid();   // function/class/console name used as a value
        }
        case ExprKind::Inject: {                    // `inject Type` (§12.5)
            if (e->resolved) {
                // Evaluate the bind's `=> expr` body IN THE CURRENT scope, so a
                // block-scope `bind IFace => local;` reads the enclosing local
                // (bug.md #56); `=> Ctor()` still constructs fresh per injection.
                // A `{ block }` factory body runs through the bind function.
                const Stmt* body = e->resolved->memberBody.get();
                if (body && body->kind == StmtKind::Return && body->expr)
                    return eval(body->expr.get());
                std::vector<Value> none;
                return callFunction(e->resolved, none, nullptr, nullptr);
            }
            return vvoid();
        }
        case ExprKind::Is:
            return vbool(valueIsType(eval(e->a.get()), e->type.get()));
        case ExprKind::Match: {                     // first-match-wins by type/value
            Value subj = eval(e->a.get());
            if (throwing_) return vvoid();
            for (const MatchArm& arm : e->arms) {
                bool hit = arm.isElse ||
                           (arm.type && valueIsType(subj, arm.type.get())) ||
                           (arm.value && matchesValue(subj, arm.value.get()));
                if (!hit) continue;
                if (arm.bodyBlock) { exec(arm.bodyBlock.get()); return vvoid(); }
                return eval(arm.bodyExpr.get());
            }
            return vvoid();                          // no arm matched (checker requires else)
        }
        case ExprKind::Member: {
            // Item Q: the reserved comptime namespace `target` (family with
            // `meta`). Locals shadow; runtime positions are never intercepted
            // (no `target` symbol exists there — ordinary resolution applies).
            // An unknown member is loud: pre-Q it evaluated to void and a
            // `comptime if` condition folded silently false.
            if (comptime_ && e->colon && e->a && e->a->kind == ExprKind::Name &&
                e->a->text == "target" && !localLookup(e->a->text)) {
                if (e->text == "os")     return vstr(targetOs_);
                if (e->text == "arch")   return vstr(targetArch_);
                if (e->text == "triple") return vstr(targetTriple_);
                return throwRuntime("unknown target:: constant '" +
                                    std::string(e->text) +
                                    "' (target::os, target::arch, target::triple)");
            }
            // Track 03 §2: a checker-resolved enum member read (`Method::GET`)
            // resolves to the mangled const global the desugar initialized.
            if (e->resolved && e->resolved->kind == StmtKind::Var) {
                auto g = globals_.find(std::string(e->resolved->name));
                if (g != globals_.end()) return g->second;
            }
            if (e->optChain) {                      // a?.b — None short-circuits
                Value bv = eval(e->a.get());
                if (throwing_) return vvoid();
                if (bv.kind == VKind::None) return vnone();
                if (bv.kind == VKind::Object && bv.obj)
                    return memberRead(bv.obj, std::string(e->text), "");
                return vnone();
            }
            // namespace-qualified global: std::read. A bare-Name check here
            // only reached a single hop (`NS::member`), so a const in a
            // NESTED namespace read via its fully-qualified path
            // (`NS::Inner::member`) fell through to memberTarget/vvoid()
            // below — the same single-hop gap resolveFunction/ctorTarget
            // already closed for calls/construction (bug #37/#46) via
            // resolveNsChain.
            if (resolveNsChain(sema_.global, e->a.get())) {
                auto g = globals_.find(std::string(e->text));
                if (g != globals_.end()) return g->second;
            }
            std::shared_ptr<Object> obj; std::string name, source;
            if (memberTarget(e, obj, name, source)) return memberRead(obj, name, source);
            return vvoid();
        }
        case ExprKind::Call:    return evalCall(e);
        case ExprKind::Binary:  return evalBinary(e);
        case ExprKind::Unary:
            if (e->op == TokenKind::Bang) return vbool(!eval(e->a.get()).b);
            if (e->op == TokenKind::Tilde) return vint(~eval(e->a.get()).i);
            {   // unary minus: kind-aware (a float negates its .f payload)
                Value v = eval(e->a.get());
                return v.kind == VKind::Float ? vfloat(-v.f) : vint(-v.i);
            }
        case ExprKind::Ternary:
            return eval(e->a.get()).b ? eval(e->b.get()) : eval(e->c.get());
        case ExprKind::Lambda: {
            auto cl = std::make_shared<Closure>();
            cl->lambda = e; cl->env = env_;
            cl->thisObj = thisObj_; cl->thisClass = thisClass_;
            Value v; v.kind = VKind::Closure; v.closure = cl; return v;
        }
        case ExprKind::Await: {          // LA-30: park the task (pump until M6)
            if (comptime_ && hermetic_)
                return throwRuntime("comptime code may not await "
                                    "(compile-time evaluation is hermetic)");
            Value p = eval(e->a.get());
            if (throwing_) return vvoid();
            if (p.kind != VKind::Object || !p.obj) return p;   // await non-promise = id
            if (lv_tasks_enabled()) {
                // LA-30 S2 (techdesign-03 §2): ready → proceed WITHOUT yielding
                // (doc 5 C4 — no scheduling point; the pump's first ready-check,
                // preserved exactly). Not ready → park on the promise object;
                // the frame's own `p` holds the reference, so the borrowed key
                // cannot dangle while parked (G12).
                if (!(taskPollPromise(p.obj.get()) & 1)) {
                    // G14: comptime evaluation never reaches the scheduler —
                    // the hermetic error above fires first; belt-and-braces.
                    assert(!comptime_ && "G14: comptime evaluation reached a task park");
                    // S3/G7: other tasks run (and overwrite the engine's
                    // volatile members) while this fiber is parked. Save the
                    // set into THIS frame, restore on resume — park-time view
                    // == resume-time view, whatever ran in between.
                    TaskState saved = saveTaskState();
                    int r = lv_task_park_on(p.obj.get());
                    restoreTaskState(std::move(saved));
                    // env.exit fired while we were parked: resume by riding
                    // the SAME throwing_ unwind the pump rode (exit-codes §4,
                    // "all guards stop cleanly") — an exit stops every task,
                    // parked ones included. A plain callback throw
                    // deliberately does NOT re-raise here (doc 5 C2: never
                    // delivered to an unrelated await); those parks drain
                    // below per G2 parity / C3.
                    if (taskTermStashed_ && taskTermExiting_) {
                        exiting_ = true;
                        throwing_ = true;
                        return vvoid();
                    }
                    // LV_PARK_DRAINED → C3, live since the M5 flip (doc 5 §5,
                    // S5): the loop drained with the promise still pending —
                    // fabricating the default value is the disease None exists
                    // to cure (info.md §9). Loud, local, catchable; inside a
                    // spawn body it lands in spawn's existing catch → reject →
                    // rethrows at the spawner's await (doc 2 §7, no new
                    // plumbing). LANG_PUMP=1 keeps the old silent fall-through
                    // via the pump branch below; deleted together at M6.
                    if (r == LV_PARK_DRAINED)
                        return throwRuntime(
                            "await: event loop drained with promise unresolved");
                    // B2 (doc 06 §2): cancellation is an exception delivered
                    // at park points, and only there — the same rethrow-point
                    // machinery as Worker-failed and C3.
                    if (r == LV_PARK_CANCELLED)
                        return throwClass("CancelledException", "task cancelled");
                }
            } else {
                // today's pump, verbatim behind the flag — deleted at M6
                RuntimeLoop& loop = RuntimeLoop::instance();
                while (!throwing_) {
                    auto r = p.obj->fields.find("ready");
                    if (r != p.obj->fields.end() && r->second.b) break;
                    if (!loop.hasWork()) break;                 // nothing will resolve it
                    for (LoopCallback& job : loop.nextBatch()) {
                        if (job.callback.kind == VKind::Closure && job.callback.closure) {
                            std::vector<Value> a{job.argument};
                            callClosure(job.callback.closure, a);
                        }
                        if (throwing_) break;
                    }
                }
            }
            // techdesign-threads-3 §3.3: a Worker whose body threw sets `failed`;
            // `await` is a rethrow point (§14.6), so surface it as a catchable
            // RuntimeException carrying the worker's failMessage. Plain Promises
            // have no `failed` field (absent = not failed) — zero change for them.
            if (!throwing_) {
                auto f = p.obj->fields.find("failed");
                if (f != p.obj->fields.end() && f->second.b) {
                    auto m = p.obj->fields.find("failMessage");
                    return throwRuntime(m != p.obj->fields.end() && m->second.kind == VKind::String
                                            ? m->second.s : "worker failed");
                }
            }
            auto v = p.obj->fields.find("value");
            return v != p.obj->fields.end() ? v->second : vvoid();
        }
        case ExprKind::Extract: {                     // `stream >>` == stream.pull()
            Value bv = eval(e->a.get());
            if (throwing_) return vvoid();
            if (bv.kind == VKind::Object && bv.obj && bv.obj->cls) {
                const Stmt* m = e->resolved ? e->resolved
                                            : findMethod(bv.obj->cls, "pull", 0);
                if (m) {
                    std::vector<Value> none;
                    return callFunction(m, none, bv.obj, bv.obj->cls);
                }
            }
            return throwRuntime("cannot extract from this value");
        }
        case ExprKind::Range: {
            Value lo = eval(e->a.get()), hi = eval(e->b.get());
            if (Symbol* r = sema_.global->lookup("Range")) {
                std::vector<Value> args{lo, hi};
                return construct(r, "Range", args);
            }
            return vvoid();
        }
        case ExprKind::Array: {
            auto vec = std::make_shared<std::vector<Value>>();
            for (const ExprPtr& el : e->list) {
                Value ev = eval(el.get());
                // A Range element spreads into its expansion: [1..5] -> [1,2,3,4,5]
                if (ev.kind == VKind::Object && ev.obj->cls && ev.obj->cls->name == "Range") {
                    long long lo = ev.obj->fields["start"].i, hi = ev.obj->fields["end"].i;
                    for (long long k = lo; k <= hi; ++k) vec->push_back(vint(k));
                } else {
                    vec->push_back(ev);
                }
            }
            Value v; v.kind = VKind::Array; v.arr = vec; return v;
        }
        case ExprKind::Index: {                       // a[i] -> ([]) get accessor
            Value base = eval(e->a.get());
            Value idx = eval(e->b.get());
            Symbol* cls = classOfValue(base);
            if (cls)
                if (const Stmt* g = findAccessor(cls, "[]", /*wantGet*/ true)) {
                    std::vector<Value> args{idx};
                    return base.kind == VKind::Object ? callFunction(g, args, base.obj, cls)
                                                      : callPrimMethod(g, base, args, cls->name);
                }
            if (base.kind == VKind::Array && base.arr) {          // native fallback
                long long i = idx.i;
                if (i < 0 || i >= (long long)base.arr->size())
                    return throwRuntime("index " + std::to_string(i) +
                                        " out of bounds (length " +
                                        std::to_string(base.arr->size()) + ")");
                return (*base.arr)[(size_t)i];
            }
            return vvoid();
        }
        default:
            return vvoid();   // streams/await: out of scope here
    }
}

// ---------------------------------------------------------------------------
//  statements
// ---------------------------------------------------------------------------

void Evaluator::exec(Stmt* s) {
    if (!s || returning_ || throwing_ || breaking_ || continuing_) return;
    if (comptime_ && !spendStep()) return;
    switch (s->kind) {
        case StmtKind::Block: {
            env_.emplace_back();
            // techdesign-02 F3: usings declared directly in this block close,
            // in reverse declaration order, on every exit edge (fallthrough,
            // return, throw, break, continue). Collected as they successfully
            // execute — a throw in an earlier statement must not close a
            // later `using` that never ran (registration is acquisition).
            std::vector<std::pair<Value, const Stmt*>> usingCleanups;
            for (StmtPtr& c : s->body) {
                exec(c.get());
                if (c->kind == StmtKind::Var && c->isUsing && !throwing_) {
                    auto it = env_.back().find(c->name);
                    if (it != env_.back().end())
                        usingCleanups.emplace_back(it->second, c->usingClose);
                }
                if (returning_ || throwing_ || breaking_ || continuing_) break;
            }
            if (!budgetExhausted_) {
                for (auto it = usingCleanups.rbegin(); it != usingCleanups.rend(); ++it) {
                    const Value& v = it->first;
                    const Stmt* closeDecl = it->second;
                    // Flag-neutral call: close() runs with a clean signal
                    // slate. If it returns normally, whatever was pending
                    // before (return/break/continue/throw) resumes exactly
                    // as it was. If it throws, that new exception REPLACES
                    // the pending one — the uniform "close-throw wins" rule.
                    bool sRet = returning_, sBrk = breaking_, sCnt = continuing_;
                    const Stmt* sBrkT = breakTarget_; const Stmt* sCntT = continueTarget_;
                    bool sThr = throwing_;
                    Value sRv = returnValue_, sTv = thrownValue_;
                    returning_ = breaking_ = continuing_ = throwing_ = false;
                    breakTarget_ = nullptr; continueTarget_ = nullptr;
                    if (v.kind == VKind::Object && v.obj && closeDecl) {
                        std::vector<Value> noArgs;
                        // Dispatch close() on the RUNTIME class. For an
                        // interface-typed `using` binding (`using IDisposable d
                        // = ...`), closeDecl is the interface's bodyless
                        // requirement — calling it directly is a silent no-op, so
                        // the concrete class's close() would never run (teardown
                        // silently skipped). Look up the runtime override.
                        const Stmt* rc = v.obj->cls
                            ? findMethod(v.obj->cls, "close", 0) : nullptr;
                        callFunction(rc ? rc : closeDecl, noArgs, v.obj, v.obj->cls);
                    }
                    if (!throwing_) {
                        returning_ = sRet; breaking_ = sBrk; continuing_ = sCnt;
                        breakTarget_ = sBrkT; continueTarget_ = sCntT;
                        throwing_ = sThr; returnValue_ = sRv; thrownValue_ = sTv;
                    }
                }
            }
            env_.pop_back();
            break;
        }
        case StmtKind::Break:
            breaking_ = true;
            breakTarget_ = s->labelTarget;
            break;
        case StmtKind::Continue:
            continuing_ = true;
            continueTarget_ = s->labelTarget;
            break;
        case StmtKind::Var: {
            Value v;
            if (s->init) {
                v = eval(s->init.get());
            } else {
                // §3: bare declaration AUTO-CONSTRUCTS — there is no null/unbound
                // state. A class type runs $init (+ any nullary ctor); primitives
                // and pure collections take their value default.
                Symbol* cls = s->type ? s->type->resolvedSymbol : nullptr;
                if (cls && cls->kind == SymbolKind::Class && !cls->isPrimitive &&
                    cls->name != "Array" && cls->name != "Map") {
                    std::vector<Value> none;
                    v = construct(cls, std::string(cls->name), none);
                } else {
                    v = defaultForCanonical(s->type ? s->type->canonical : "");
                }
            }
            if (!env_.empty()) env_.back()[s->name] = copyValue(v);
            break;
        }
        case StmtKind::Return: {
            Value rv = s->expr ? eval(s->expr.get()) : vvoid();
            // If evaluating the return expression threw (e.g. `return a / b;` with
            // b == 0, or `return arr[i];` out of bounds), the throw must propagate
            // to an enclosing try/catch — do NOT convert it into a normal (void)
            // return. Previously `returning_` was set unconditionally, masking the
            // exception so the oracle silently returned void where every other
            // engine caught it (found via bug.md #10's div-by-zero catch test).
            if (throwing_) break;
            returnValue_ = copyValue(rv);
            if (comptime_) lastReturnSpan_ = s->span;
            returning_ = true;
            break;
        }
        case StmtKind::If:
            if (eval(s->expr.get()).b) exec(s->thenBranch.get());
            else exec(s->elseBranch.get());
            break;
        case StmtKind::While:
            while (!returning_ && !throwing_) {
                if (!eval(s->expr.get()).b || throwing_) break;
                exec(s->thenBranch.get());
                if (continuing_) {
                    if (continueTarget_ && continueTarget_ != s) break;
                    continuing_ = false; continueTarget_ = nullptr;
                }
                if (breaking_) {
                    if (!breakTarget_ || breakTarget_ == s) { breaking_ = false; breakTarget_ = nullptr; }
                    break;
                }
            }
            break;
        case StmtKind::DoWhile:
            do {
                exec(s->thenBranch.get());
                if (continuing_) {
                    if (continueTarget_ && continueTarget_ != s) break;
                    continuing_ = false; continueTarget_ = nullptr;
                }
                if (breaking_) {
                    if (!breakTarget_ || breakTarget_ == s) { breaking_ = false; breakTarget_ = nullptr; }
                    break;
                }
            } while (!returning_ && !throwing_ && eval(s->expr.get()).b);
            break;
        case StmtKind::ForIn: {
            Value iter = eval(s->expr.get());
            env_.emplace_back();
            if (iter.kind == VKind::Object && iter.obj->cls &&
                iter.obj->cls->name == "Range") {
                long long lo = iter.obj->fields["start"].i;
                long long hi = iter.obj->fields["end"].i;
                for (long long k = lo; k <= hi && !returning_ && !throwing_; ++k) {
                    env_.back()[s->name] = vint(k);
                    exec(s->thenBranch.get());
                    if (continuing_) {
                        if (continueTarget_ && continueTarget_ != s) break;
                        continuing_ = false; continueTarget_ = nullptr;
                    }
                    if (breaking_) {
                        if (!breakTarget_ || breakTarget_ == s) { breaking_ = false; breakTarget_ = nullptr; }
                        break;
                    }
                }
            } else if (iter.kind == VKind::Array && iter.arr) {
                for (const Value& el : *iter.arr) {
                    if (returning_ || throwing_) break;
                    env_.back()[s->name] = el;
                    exec(s->thenBranch.get());
                    if (continuing_) {
                        if (continueTarget_ && continueTarget_ != s) break;
                        continuing_ = false; continueTarget_ = nullptr;
                    }
                    if (breaking_) {
                        if (!breakTarget_ || breakTarget_ == s) { breaking_ = false; breakTarget_ = nullptr; }
                        break;
                    }
                }
            } else if (iter.kind == VKind::Map && iter.map) {
                // iterate entries as Pair<K, V>
                Symbol* pairSym = sema_.global->lookup("Pair");
                for (const auto& [k, v] : *iter.map) {
                    if (returning_ || throwing_) break;
                    std::vector<Value> pargs{k, v};
                    env_.back()[s->name] = pairSym ? construct(pairSym, "Of", pargs) : vvoid();
                    exec(s->thenBranch.get());
                    if (continuing_) {
                        if (continueTarget_ && continueTarget_ != s) break;
                        continuing_ = false; continueTarget_ = nullptr;
                    }
                    if (breaking_) {
                        if (!breakTarget_ || breakTarget_ == s) { breaking_ = false; breakTarget_ = nullptr; }
                        break;
                    }
                }
            } else if (s->forInProtocol && iter.kind == VKind::Object && iter.obj &&
                       iter.obj->cls) {
                // techdesign-07 §2: the iterator protocol. Desugar to
                //   var __it = e.iterator();
                //   while (__it.hasNext()) { x = __it.next(); <body> }
                // driven through the same dynamic-dispatch machinery every other
                // method call uses; break/continue/return/throw honored exactly
                // as the array branch (Track 02 loop flags).
                std::vector<Value> noArgs;
                const Stmt* itM = findMethod(iter.obj->cls, "iterator");
                Value it = itM ? callFunction(itM, noArgs, iter.obj, iter.obj->cls) : vvoid();
                if (!throwing_ && it.kind == VKind::Object && it.obj && it.obj->cls) {
                    const Stmt* hasNextM = findMethod(it.obj->cls, "hasNext");
                    const Stmt* nextM = findMethod(it.obj->cls, "next");
                    while (!returning_ && !throwing_) {
                        std::vector<Value> hnArgs;
                        Value hn = hasNextM ? callFunction(hasNextM, hnArgs, it.obj, it.obj->cls)
                                            : vbool(false);
                        if (throwing_ || hn.kind != VKind::Bool || !hn.b) break;
                        std::vector<Value> nxArgs;
                        Value nx = nextM ? callFunction(nextM, nxArgs, it.obj, it.obj->cls)
                                         : vvoid();
                        if (throwing_) break;
                        env_.back()[s->name] = nx;
                        exec(s->thenBranch.get());
                        if (continuing_) {
                            if (continueTarget_ && continueTarget_ != s) break;
                            continuing_ = false; continueTarget_ = nullptr;
                        }
                        if (breaking_) {
                            if (!breakTarget_ || breakTarget_ == s) { breaking_ = false; breakTarget_ = nullptr; }
                            break;
                        }
                    }
                }
            }
            env_.pop_back();
            break;
        }
        case StmtKind::For:
            env_.emplace_back();
            for (exec(s->forInit.get());
                 !returning_ && !throwing_ && (!s->expr || eval(s->expr.get()).b);
                 eval(s->forStep.get())) {
                exec(s->thenBranch.get());
                if (continuing_) {
                    if (continueTarget_ && continueTarget_ != s) break;
                    continuing_ = false; continueTarget_ = nullptr;
                }
                if (breaking_) {
                    if (!breakTarget_ || breakTarget_ == s) { breaking_ = false; breakTarget_ = nullptr; }
                    break;
                }
            }
            env_.pop_back();
            break;
        case StmtKind::ExprStmt:
            eval(s->expr.get());
            break;
        case StmtKind::Throw:
            thrownValue_ = eval(s->expr.get());
            if (!throwing_) throwing_ = true;   // (unless evaluating it threw)
            break;
        case StmtKind::Try: {
            exec(s->thenBranch.get());
            if (!throwing_) break;
            if (budgetExhausted_) break;   // budget exhaustion is not catchable
            for (const CatchClause& c : s->catches) {
                // catch is resolution by type — the SAME test as `is` / `match`.
                if (c.type && valueIsType(thrownValue_, c.type.get())) {
                    throwing_ = false;              // caught: resolution by type
                    env_.emplace_back();
                    if (!c.name.empty()) env_.back()[c.name] = thrownValue_;
                    exec(c.body.get());
                    env_.pop_back();
                    break;
                }
            }
            break;                                   // no match: keeps bubbling
        }
        default:
            break;   // Bind/Use/decls: not executed here
    }
}

void Evaluator::initGlobals(Program& prelude) {
    env_.emplace_back();
    std::function<void(std::vector<StmtPtr>&)> walk = [&](std::vector<StmtPtr>& items) {
        for (StmtPtr& item : items) {
            if (item->kind == StmtKind::Namespace) walk(item->body);
            else if (item->kind == StmtKind::Var && item->init)
                globals_[std::string(item->name)] = eval(item->init.get());
        }
    };
    walk(prelude.items);
    env_.pop_back();
}

// The top-level items loop — shared verbatim by both modes; task 0's body
// under LANG_TASKS (LA-30 techdesign-03 §2.2). Returns whether an uncaught
// throw was already reported inline.
bool Evaluator::execTopLevel(Program& program) {
    bool reportedUncaught = false;
    env_.emplace_back();
    // Phase A (known_bugs #75/#76/#79/#80): default-construct EVERY global —
    // top-level and namespace-scoped, bare or with an initializer — to its
    // §3 auto-constructed value BEFORE any namespace-scoped initializer or
    // top-level statement runs. A namespace global's initializer (Phase B) and
    // top-level code (Phase C) can then read and MUTATE a shared global at a
    // real auto-constructed slot instead of an absent one — which is the whole
    // mechanism the metaprogramming registration idiom (a rule-injected
    // `namespace Reg { bool $C = seed(...); }` that appends to a top-level
    // registry array) depends on. Only "pure default" types are seeded here
    // (primitives, string, Array, Map, unions, None) — a class-typed global
    // still auto-constructs through its normal path (its ctor may have side
    // effects that must stay in statement order). bug.md #2's rule — a
    // top-level var's EXPLICIT initializer runs in source order (Phase C),
    // never hoisted — is preserved; only the pure default is hoisted here, and
    // a name already global (prelude collision) is left untouched.
    std::unordered_set<std::string> preConstructedTop;
    {
        // Gather every global var (top-level + namespace-scoped) once, then
        // construct in two sub-passes: pure-default types first, class types
        // second, so a class ctor run here can read the already-constructed
        // pure defaults. A name already global (prelude collision) is skipped.
        std::vector<std::pair<Stmt*, bool>> gathered;   // (var, isTop)
        std::function<void(std::vector<StmtPtr>&, bool)> gather =
            [&](std::vector<StmtPtr>& items, bool inNs) {
                for (StmtPtr& item : items) {
                    if (item->kind == StmtKind::Namespace) { gather(item->body, true); continue; }
                    if (item->kind == StmtKind::Var && !globals_.count(std::string(item->name)))
                        gathered.push_back({item.get(), !inNs});
                }
            };
        gather(program.items, false);
        auto isClassType = [](const Stmt* v) {
            Symbol* c = v->type ? v->type->resolvedSymbol : nullptr;
            return c && c->kind == SymbolKind::Class && !c->isPrimitive &&
                   c->name != "Array" && c->name != "Map";
        };
        for (auto& [item, isTop] : gathered) {
            if (isClassType(item)) continue;
            globals_[std::string(item->name)] =
                defaultForCanonical(item->type ? item->type->canonical : "");
            if (isTop) preConstructedTop.insert(std::string(item->name));
        }
        for (auto& [item, isTop] : gathered) {
            if (!isClassType(item)) continue;
            if (item->init) continue;   // an explicit initializer constructs it once,
                                        // in its normal phase — never auto-construct too
                                        // (that would double-run a ctor's side effects)
            Symbol* cls = item->type->resolvedSymbol;
            std::vector<Value> none;
            globals_[std::string(item->name)] = construct(cls, std::string(cls->name), none);
            if (isTop) preConstructedTop.insert(std::string(item->name));
            if (throwing_) return reportedUncaught;   // a ctor threw during startup
        }
    }
    // Phase B: namespace-scoped global initializers (bug 1: the top-level loop
    // never recurses into namespace bodies, so a namespace global's initializer
    // is run here, up front, keyed by bare name exactly like the prelude's own
    // std::read/write). Runs after Phase A, so an initializer sees every other
    // global's auto-constructed value; a bare namespace global keeps its Phase-A
    // default (previously it was never constructed at all — known_bugs #75).
    {
        std::function<void(std::vector<StmtPtr>&, bool)> initNs =
            [&](std::vector<StmtPtr>& items, bool inNs) {
                for (StmtPtr& item : items) {
                    if (throwing_) return;
                    if (item->kind == StmtKind::Namespace) initNs(item->body, true);
                    else if (inNs && item->kind == StmtKind::Var && item->init)
                        globals_[std::string(item->name)] = eval(item->init.get());
                }
            };
        initNs(program.items, false);
    }
    // Phase C: top-level statements in source order.
    for (StmtPtr& item : program.items) {
        if (throwing_) break;   // a namespace global initializer already threw
        if (item->kind == StmtKind::Var && preConstructedTop.count(std::string(item->name))) {
            // Pre-constructed top-level global (Phase A). An EXPLICIT initializer
            // overwrites its default in source order (bug.md #2); a BARE
            // declaration keeps the default plus any Phase-B mutation — never
            // re-constructed, so a namespace initializer's append survives.
            if (item->init) {
                Value v = eval(item->init.get());
                if (!throwing_) globals_[std::string(item->name)] = copyValue(v);
            }
        } else {
            if (item->kind == StmtKind::ExprStmt || item->kind == StmtKind::Var ||
                item->kind == StmtKind::Try || item->kind == StmtKind::Throw)
                exec(item.get());
            // bug.md #2: promote the just-bound top-level Var into globals_ — it
            // is shared program state (functions read/write it), not a top-level
            // local, and the IR engine models it as a real global slot. Names
            // already global (prelude/namespace collisions) stay env-bound,
            // matching the lowerer's skip rule. (Pre-constructed top-level Vars
            // are handled above and never reach here.)
            if (!throwing_ && item->kind == StmtKind::Var && !env_.empty()) {
                auto& base = env_.back();
                auto b = base.find(item->name);
                if (b != base.end() && !globals_.count(std::string(item->name))) {
                    globals_[std::string(item->name)] = std::move(b->second);
                    base.erase(b);
                }
            }
        }
        if (throwing_) {
            // env.exit unwinds via throwing_ too (§4): break WITHOUT reporting.
            if (!exiting_) {
                std::string cls = (thrownValue_.kind == VKind::Object && thrownValue_.obj &&
                                   thrownValue_.obj->cls)
                                      ? std::string(thrownValue_.obj->cls->name) : "value";
                std::string msg;
                if (thrownValue_.kind == VKind::Object && thrownValue_.obj) {
                    auto it = thrownValue_.obj->fields.find("message");
                    if (it != thrownValue_.obj->fields.end()) msg = it->second.s;
                }
                out_ += "Uncaught " + cls + (msg.empty() ? "" : ": " + msg) + "\n";
                reportedUncaught = true;
            }
            break;
        }
    }
    return reportedUncaught;
}

std::string Evaluator::run(Program& program) {
    RuntimeLoop::instance().reset();
    interpResetExit();                 // exit-codes.md §4: fresh per run
    exiting_ = false; exitCode_ = 0;
    bool reportedUncaught = false;
    if (lv_tasks_enabled()) {
        // LA-30 (techdesign-03 §2.2/§2.3): the program body is task 0 and the
        // scheduler replaces the pump drain — a top-level await parks like any
        // other. Hooks installed once per run, before lv_sched_run.
        lv_sched_init();
        gTaskEval_ = this;
        taskTermStashed_ = false; taskTermExiting_ = false;
        taskTermThrown_ = Value{}; taskReportedUncaught_ = false;
        lv_sched_hooks(&Evaluator::taskPollPromise, &Evaluator::taskLoopStep);
        lv_sched_throw_probe(&Evaluator::taskThrowProbe);
        lv_sched_run(&Evaluator::taskRunProgram, this, &program);
        gTaskEval_ = nullptr;
        // Re-raise the first task-captured termination (throw or env.exit) so
        // the shared tail below reports exactly what the pump unwind would.
        if (taskTermStashed_) {
            throwing_ = true;
            thrownValue_ = taskTermThrown_;
            exiting_ = taskTermExiting_;
        }
        reportedUncaught = taskReportedUncaught_;
    } else {
        reportedUncaught = execTopLevel(program);
        // The implicit event loop: keep dispatching while work is pending.
        // (Pump drain, verbatim behind the flag — deleted at M6.)
        RuntimeLoop& loop = RuntimeLoop::instance();
        while (!throwing_ && loop.hasWork()) {
            for (LoopCallback& job : loop.nextBatch()) {
                if (job.callback.kind != VKind::Closure || !job.callback.closure) continue;
                std::vector<Value> args{job.argument};
                callClosure(job.callback.closure, args);
                if (throwing_) break;
            }
        }
    }
    if (throwing_ && !exiting_ && !reportedUncaught) {
        std::string cls = (thrownValue_.kind == VKind::Object && thrownValue_.obj &&
                           thrownValue_.obj->cls)
                              ? std::string(thrownValue_.obj->cls->name) : "value";
        std::string msg;
        if (thrownValue_.kind == VKind::Object && thrownValue_.obj) {
            auto it = thrownValue_.obj->fields.find("message");
            if (it != thrownValue_.obj->fields.end()) msg = it->second.s;
        }
        out_ += "Uncaught " + cls + (msg.empty() ? "" : ": " + msg) + "\n";
    }
    // Exit code (designs/exit-codes.md §4/§5): an uncaught throw that reached the
    // top exits 1 (gap b); env.exit / env.setExitCode's recorded code otherwise
    // (default 0). exiting_ is a clean termination, so it is NOT the uncaught 1.
    bool uncaught = throwing_ && !exiting_;
    exitCode_ = uncaught ? 1 : interpExitCode();
    interpSignalCleanup();             // terminal-floor.md §3 M4: unblock + close signal fds
    return out_;
}

// ---------------------------------------------------------------------------
//  LA-30 tasks: substrate hooks and park-site state discipline (techdesign-03)
// ---------------------------------------------------------------------------

Evaluator* Evaluator::gTaskEval_ = nullptr;

namespace {
// One loop-dispatched job (callback + argument), heap-carried into its task.
// The Values own their references until the thunk consumes them (or drops
// them after a program termination) — shared_ptr RAII either way.
struct EvalTaskJob {
    Value callback;
    Value argument;
};
}

// S3/G7: move the volatile set out and leave the engine PRISTINE. A fresh task
// starting while this one is parked begins from a clean context — deliberately
// NOT the pump's behavior (nested dispatch leaked the awaiter's rawField_/
// inCtor_/primThis_/curNamespace_ into callbacks: an undocumented stack
// accident of the same family as C2, not a contract).
Evaluator::TaskState Evaluator::saveTaskState() {
    TaskState s;
    s.env = std::move(env_);                 env_.clear();
    s.thisObj = std::move(thisObj_);         thisObj_ = nullptr;
    s.thisClass = thisClass_;                thisClass_ = nullptr;
    s.curNamespace = curNamespace_;          curNamespace_ = nullptr;
    s.primThis = std::move(primThis_);       primThis_ = Value{};
    s.hasPrimThis = hasPrimThis_;            hasPrimThis_ = false;
    s.inCtor = inCtor_;                      inCtor_ = false;
    s.returning = returning_;                returning_ = false;
    s.breaking = breaking_;                  breaking_ = false;
    s.continuing = continuing_;              continuing_ = false;
    s.breakTarget = breakTarget_;            breakTarget_ = nullptr;
    s.continueTarget = continueTarget_;      continueTarget_ = nullptr;
    s.returnValue = std::move(returnValue_); returnValue_ = Value{};
    s.thrownValue = std::move(thrownValue_); thrownValue_ = Value{};
    s.rawField = std::move(rawField_);       rawField_.clear();
    return s;
}

void Evaluator::restoreTaskState(TaskState&& s) {
    env_ = std::move(s.env);
    thisObj_ = std::move(s.thisObj);
    thisClass_ = s.thisClass;
    curNamespace_ = s.curNamespace;
    primThis_ = std::move(s.primThis);
    hasPrimThis_ = s.hasPrimThis;
    inCtor_ = s.inCtor;
    returning_ = s.returning;
    breaking_ = s.breaking;
    continuing_ = s.continuing;
    breakTarget_ = s.breakTarget;
    continueTarget_ = s.continueTarget;
    returnValue_ = std::move(s.returnValue);
    thrownValue_ = std::move(s.thrownValue);
    rawField_ = std::move(s.rawField);
}

// G11: the throw flag never crosses a task boundary. First termination wins —
// the pump stopped all dispatch at the first throw, so later ones were
// unreachable states; taskLoopStep and taskRunJob gate on the stash.
void Evaluator::taskCaptureTermination() {
    if (!throwing_) return;
    if (!taskTermStashed_) {
        taskTermStashed_ = true;
        taskTermThrown_ = thrownValue_;
        taskTermExiting_ = exiting_;
    }
    throwing_ = false;
    exiting_ = false;
}

// G15: bool immediates only — the exact reads the pump's ready-check made;
// no Value copies of object-typed fields, so the poll path is ARC-silent.
int Evaluator::taskPollPromise(const void* obj) {
    auto* o = static_cast<const Object*>(obj);
    int s = 0;
    auto r = o->fields.find("ready");
    if (r != o->fields.end() && r->second.b) s |= 1;
    auto f = o->fields.find("failed");
    if (f != o->fields.end() && f->second.b) s |= 2;
    return s;
}

// One loop batch; every user callback becomes a task (spawn order = FIFO runq
// order = the pump's inline dispatch order). Returns 0 when the loop has no
// work — or when a termination is stashed, mirroring the pump drain's
// `while (!throwing_ && loop.hasWork())` gate.
int Evaluator::taskLoopStep() {
    Evaluator* ev = gTaskEval_;
    if (!ev || ev->taskTermStashed_) return 0;
    RuntimeLoop& loop = RuntimeLoop::instance();
    if (!loop.hasWork()) return 0;
    for (LoopCallback& job : loop.nextBatch()) {
        if (job.callback.kind != VKind::Closure || !job.callback.closure) continue;
        lv_task_spawn(&Evaluator::taskRunJob,
                      new EvalTaskJob{job.callback, job.argument}, nullptr);
    }
    return 1;
}

int Evaluator::taskThrowProbe() {
    return gTaskEval_ && gTaskEval_->throwing_ ? 1 : 0;
}

void Evaluator::taskRunProgram(void* selfp, void* prog) {
    auto* ev = static_cast<Evaluator*>(selfp);
    ev->taskReportedUncaught_ = ev->execTopLevel(*static_cast<Program*>(prog));
    ev->taskCaptureTermination();
}

void Evaluator::taskRunJob(void* ctxp, void*) {
    std::unique_ptr<EvalTaskJob> job(static_cast<EvalTaskJob*>(ctxp));
    Evaluator* ev = gTaskEval_;
    if (!ev) return;
    // A prior task terminated the program (throw or env.exit): drop the job —
    // the pump skipped the remainder of its batch on a throw, same shape.
    if (ev->taskTermStashed_) return;
    std::vector<Value> args{job->argument};
    ev->callClosure(job->callback.closure, args);
    ev->taskCaptureTermination();   // G11: never return to the scheduler throwing
}

// B2 (doc 06 §4): a group child's body — taskRunJob's zero-arg twin. An
// uncaught throw here is program-uncaught (C2's rule; the CancelledException
// absorption for group children lives in the PRELUDE wrapper, TaskGroup.run).
void Evaluator::taskRunGroupBody(void* ctxp, void*) {
    std::unique_ptr<EvalTaskJob> job(static_cast<EvalTaskJob*>(ctxp));
    Evaluator* ev = gTaskEval_;
    if (!ev) return;
    if (ev->taskTermStashed_) return;   // program already terminated: drop
    std::vector<Value> none;
    ev->callClosure(job->callback.closure, none);
    ev->taskCaptureTermination();       // G11: never return to the scheduler throwing
}

// B2 (doc 06 §4): sysTaskRun / sysTaskJoinAll / sysAwaitAny2 — the natives that
// need the ENGINE (closure spawn, or a park with the S3/G7 state discipline).
// Returns true when `name` was one of them (out/throwing_ carry the result).
bool Evaluator::taskNative(const std::string& name, std::vector<Value>& args, Value& out) {
    if (name != "sysTaskRun" && name != "sysTaskJoinAll" && name != "sysAwaitAny2")
        return false;
    out = vvoid();
    if (!lv_tasks_enabled()) {
        out = throwRuntime("TaskGroup requires tasks (running under LANG_PUMP=1)");
        return true;
    }
    if (name == "sysTaskRun") {
        if (args.empty() || args[0].kind != VKind::Closure || !args[0].closure) {
            out = throwRuntime("sysTaskRun: body must be a closure");
            return true;
        }
        uint64_t id = lv_task_spawn_registered(&Evaluator::taskRunGroupBody,
                                               new EvalTaskJob{args[0], Value{}}, nullptr);
        out = vint((long long)id);
        return true;
    }
    if (name == "sysTaskJoinAll") {
        std::vector<uint64_t> ids;
        if (!args.empty() && args[0].kind == VKind::Array && args[0].arr)
            for (const Value& v : *args[0].arr) ids.push_back((uint64_t)v.i);
        // The ids vector lives on THIS task's C++ frame — the substrate borrows
        // it per G12 from a frame that owns it for the whole park.
        TaskState saved = saveTaskState();
        int r = lv_task_park_join(ids.data(), (int)ids.size());
        restoreTaskState(std::move(saved));
        if (taskTermStashed_ && taskTermExiting_) {          // env.exit while parked
            exiting_ = true; throwing_ = true;
            return true;
        }
        if (r == LV_PARK_DRAINED)
            out = throwRuntime("TaskGroup.close: event loop drained with tasks unjoined");
        else if (r == LV_PARK_CANCELLED)
            out = throwClass("CancelledException", "task cancelled");
        else
            out = vint((long long)ids.size());
        return true;
    }
    // sysAwaitAny2(a, b) -> 0/1. Reads NO value fields (G15/S2: the value read
    // and failed-rethrow stay in the ordinary await the prelude issues after
    // this returns) — the native only learns WHICH promise settled.
    if (args.size() < 2 || args[0].kind != VKind::Object || !args[0].obj ||
        args[1].kind != VKind::Object || !args[1].obj) {
        out = throwRuntime("sysAwaitAny2: both arguments must be promises");
        return true;
    }
    // ready -> proceed without yielding (doc 5 C4), first-declared-wins.
    if (taskPollPromise(args[0].obj.get())) { out = vint(0); return true; }
    if (taskPollPromise(args[1].obj.get())) { out = vint(1); return true; }
    int which = 0;
    TaskState saved = saveTaskState();
    int r = lv_task_park_any2(args[0].obj.get(), args[1].obj.get(), &which);
    restoreTaskState(std::move(saved));
    if (taskTermStashed_ && taskTermExiting_) {              // env.exit while parked
        exiting_ = true; throwing_ = true;
        return true;
    }
    if (r == LV_PARK_DRAINED)
        out = throwRuntime("await: event loop drained with promise unresolved");
    else if (r == LV_PARK_CANCELLED)
        out = throwClass("CancelledException", "task cancelled");
    else
        out = vint(which);
    return true;
}
