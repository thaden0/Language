// RuntimeCore — shared runtime semantics for both interpreters.
// Filled by refactor_1 session 02 (techdesign-02-runtime-core-opus.md).
#include "RuntimeCore.hpp"
#include "RuntimeLoop.hpp"
#include "lv_task.h"   // LA-30: stackful-task substrate (techdesign-03)

// --- (1) arity-aware method lookup -----------------------------------------

const Stmt* rtFindMethod(Symbol* cls, const std::string& name, int argc) {
    if (!cls) return nullptr;
    // bug.md #13 walk order: first name-match wins by default; a UNIQUE arity
    // match overrides it. Preserved exactly — see the header rationale.
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

// --- (2) object arithmetic --------------------------------------------------

const char* rtOpSymbol(TokenKind op) {
    switch (op) {
        case TokenKind::Plus: return "+";     case TokenKind::Minus: return "-";
        case TokenKind::Star: return "*";     case TokenKind::Slash: return "/";
        case TokenKind::Percent: return "%";  case TokenKind::EqEq: return "==";
        case TokenKind::BangEq: return "!=";  case TokenKind::Lt: return "<";
        case TokenKind::Gt: return ">";       case TokenKind::Le: return "<=";
        case TokenKind::Ge: return ">=";      case TokenKind::LtLt: return "<<";
        case TokenKind::GtGt: return ">>";
        // refactor_1/02 (b) fix: the oracle's old opSymbol omitted | and &, so an
        // unresolved object bitwise operator (a bare self-call in an unchecked
        // prelude class body) mapped to "?" and raised "no operator '?'" where the
        // IR executor dispatched it — the bug.md #13 by-name-fallback drift family.
        // Unified on the IR side (the one that matches the checker's opMethodName).
        case TokenKind::Pipe: return "|";     case TokenKind::Amp: return "&";
        default: return "?";
    }
}

Value rtObjectArith(TokenKind op, Symbol* cls, const Stmt* resolved,
                    const Value& l, const Value& r,
                    const std::function<Value(const Stmt*, const Value&, const Value&)>& invoke,
                    const std::function<Value(const std::string&)>& raiseNoOp) {
    const char* sym = rtOpSymbol(op);
    const Stmt* m = resolved ? resolved : rtFindMethod(cls, sym);
    if (m) return invoke(m, l, r);
    if (op == TokenKind::BangEq)
        if (const Stmt* eq = rtFindMethod(cls, "==")) {
            Value res = invoke(eq, l, r);
            return vbool(!res.b);
        }
    if ((op == TokenKind::EqEq || op == TokenKind::BangEq) && cls && !cls->isValue) {
        // a class with no (==) is reference identity (design §5.2); a value struct
        // gets a synthesized field-wise (==) at resolve time (§5.5) so it never
        // reaches here from checked code — be loud if a hole ever does.
        bool same = r.kind == VKind::Object && l.obj == r.obj;
        return vbool(op == TokenKind::EqEq ? same : !same);
    }
    return raiseNoOp(sym);
}

// --- (3) raw member (slot/field/key) access --------------------------------

std::string rtKeyFor(Symbol* cls, const std::string& name, const std::string& source) {
    if (cls)
        for (const Slot& s : cls->shape.slots) {
            if (s.isMethod || s.name != name) continue;
            if (!source.empty() && (!s.source || std::string(s.source->name) != source))
                continue;
            if (s.distinct && s.source) return std::string(s.source->name) + "::" + name;
            return name;
        }
    if (!source.empty()) return source + "::" + name;
    return name;
}

Value rtRawFieldGet(const std::shared_ptr<Object>& obj, const std::string& storageKey) {
    for (const Slot& s : obj->cls->shape.slots)
        if (!s.isMethod && s.isWeak &&
            (storageKey == s.name ||
             (s.source && storageKey == std::string(s.source->name) + "::" + std::string(s.name))))
            return weakObjectRead(obj, storageKey);
    auto it = obj->fields.find(storageKey);
    return it != obj->fields.end() ? it->second : vvoid();
}

void rtRawFieldSet(const std::shared_ptr<Object>& obj, const std::string& storageKey,
                   const Value& v) {
    for (const Slot& s : obj->cls->shape.slots)
        if (!s.isMethod && s.isWeak &&
            (storageKey == s.name ||
             (s.source && storageKey == std::string(s.source->name) + "::" + std::string(s.name)))) {
            weakObjectWrite(obj, storageKey, v);
            return;
        }
    obj->fields[storageKey] = v;
}

// --- (4) task / promise state transitions ----------------------------------

int rtPollPromise(const void* obj) {
    auto* o = static_cast<const Object*>(obj);
    int s = 0;
    auto r = o->fields.find("ready");
    if (r != o->fields.end() && r->second.b) s |= 1;
    auto f = o->fields.find("failed");
    if (f != o->fields.end() && f->second.b) s |= 2;
    return s;
}

int rtTaskLoopStep(bool termStashed, void (*spawn)(void*, void*)) {
    if (termStashed) return 0;                    // pump drain's !throwing_ gate
    RuntimeLoop& loop = RuntimeLoop::instance();
    if (!loop.hasWork()) return 0;
    for (LoopCallback& job : loop.nextBatch()) {
        if (job.callback.kind != VKind::Closure || !job.callback.closure) continue;
        lv_task_spawn(spawn, new RtTaskJob{job.callback, job.argument}, nullptr);
    }
    return 1;
}

void rtCaptureTermination(bool& throwing, bool& exiting, const Value& thrown,
                          bool& termStashed, Value& termThrown, bool& termExiting) {
    if (!throwing) return;
    if (!termStashed) {
        termStashed = true;
        termThrown = thrown;
        termExiting = exiting;
    }
    throwing = false;
    exiting = false;
}
