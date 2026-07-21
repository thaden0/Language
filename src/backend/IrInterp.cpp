#include "backend/IrInterp.hpp"
#include "runtime/RuntimeCore.hpp"
#include "runtime/RuntimeLoop.hpp"
#include "lv_task.h"   // LA-30: stackful-task substrate (techdesign-03)
#include <cassert>
#include <memory>

// ---------------------------------------------------------------------------
//  semantic-model helpers
// ---------------------------------------------------------------------------

Symbol* IrInterp::classOfValue(const Value& v) const {
    Scope* g = mod_.sema->global;
    switch (v.kind) {
        case VKind::Object: return v.obj ? v.obj->cls : nullptr;
        case VKind::Array:  return g->lookup("Array");
        case VKind::Map:    return g->lookup("Map");
        case VKind::Block:  return g->lookup("Block");
        case VKind::Int:    return g->lookup("int");
        case VKind::Char:   return g->lookup("char");
        case VKind::String: return g->lookup("string");
        case VKind::Bool:   return g->lookup("bool");
        case VKind::Float:  return g->lookup("float");
        default:            return nullptr;
    }
}

const Stmt* IrInterp::findMethodByName(Symbol* cls, const std::string& name, int argc) const {
    // bug.md #13 arity-aware disambiguation, now the shared rtFindMethod (the
    // oracle's findMethod is the same call) — see RuntimeCore.hpp.
    return rtFindMethod(cls, name, argc);
}

const Stmt* IrInterp::findAccessor(Symbol* cls, const std::string& name, bool wantGet) const {
    if (!cls || !cls->decl) return nullptr;
    for (const StmtPtr& m : cls->decl->body)
        if (((wantGet && m->isGet) || (!wantGet && m->isSet)) && m->name == name)
            return m.get();
    for (const TypeRefPtr& base : cls->decl->bases)
        if (const Stmt* a = findAccessor(base->resolvedSymbol, name, wantGet)) return a;
    return nullptr;
}

bool IrInterp::isSubclassOrSelf(Symbol* a, Symbol* b) const {
    if (!a || !b) return false;
    if (a == b) return true;
    if (!a->decl) return false;
    for (const TypeRefPtr& base : a->decl->bases)
        if (isSubclassOrSelf(base->resolvedSymbol, b)) return true;
    return false;
}

// "name" or "Source::name" -> the storage key, honoring distinct slots. The
// raw key computation is shared (rtKeyFor); this engine's key arrives already
// spelled "Source::name", so it splits the qualifier out first.
std::string IrInterp::keyFor(Symbol* cls, const std::string& nameOrQualified) const {
    std::string source, name = nameOrQualified;
    size_t p = nameOrQualified.find("::");
    if (p != std::string::npos) {
        source = nameOrQualified.substr(0, p);
        name = nameOrQualified.substr(p + 2);
    }
    return rtKeyFor(cls, name, source);
}

// LA-30 B2 (doc 06 §2): raise a NAMED prelude exception class — the same
// construct-and-flag machinery as raise(), parameterized for the cancellation
// carrier. Catch matches by type (Op::IsType), so CancelledException /
// ICancelledException / IException all catch it.
void IrInterp::raiseClass(const std::string& clsName, const std::string& message) {
    Symbol* cls = mod_.sema->global->lookup(clsName);
    if (!cls) cls = mod_.sema->global->lookup("RuntimeException");   // belt-and-braces
    if (cls) {
        auto obj = std::make_shared<Object>();
        obj->cls = cls;
        obj->fields["message"] = vstr(message);
        Value v; v.kind = VKind::Object; v.obj = obj;
        thrown_ = v;
    } else {
        thrown_ = vstr(message);
    }
    throwing_ = true;
}

void IrInterp::raise(const std::string& message) {
    raiseClass("RuntimeException", message);
}

// ---------------------------------------------------------------------------
//  calls
// ---------------------------------------------------------------------------

Value IrInterp::callDecl(const Stmt* decl, Symbol* /*cls*/, std::vector<Value> args,
                         bool cowReceiver) {
    auto it = mod_.byDecl.find(decl);
    if (it != mod_.byDecl.end()) return call(it->second, std::move(args));
    // empty body: native intrinsic on the receiver's class
    if (!args.empty()) {
        Symbol* rc = classOfValue(args[0]);
        if (rc) {
            const Value& self = args[0];
            std::vector<Value> rest(args.begin() + 1, args.end());
            Value out; std::string err;
            if (nativeCall(rc->name, std::string(decl->name), self, rest, out, err,
                           cowReceiver)) {
                if (!err.empty()) { raise(err); return vvoid(); }
                return out;
            }
        }
    }
    return vvoid();
}

Value IrInterp::call(int fnIndex, std::vector<Value> args) {
    const IrFunction& fn = mod_.functions[fnIndex];
    std::vector<Value> regs(fn.nregs);
    for (size_t i = 0; i < args.size() && (int)i < fn.nregs; ++i) regs[i] = std::move(args[i]);

    if (!own_) {
        return frameExec(fnIndex, regs, nullptr);
    }
    std::vector<std::pair<int, std::weak_ptr<void>>> weaks;
    Value result = frameExec(fnIndex, regs, &weaks);
    regs.clear();                       // the frame dies here (§15 scope exit)
    for (auto& [pc, w] : weaks) {
        if (!w.expired())
            violations_.push_back(mod_.functions[fnIndex].name + " @" +
                                  std::to_string(pc) +
                                  ": scope-owned allocation survived its frame");
    }
    return result;
}

Value IrInterp::frameExec(int fnIndex, std::vector<Value>& regs,
                          std::vector<std::pair<int, std::weak_ptr<void>>>* weaks) {
    const IrFunction& fn = mod_.functions[fnIndex];
    auto track = [&](size_t pc, const Value& v) {
        if (!weaks || !own_->scopeOwned.count({fnIndex, (int)pc})) return;
        ++tracked_;
        std::shared_ptr<void> p;
        if (v.obj) p = v.obj;
        else if (v.arr) p = v.arr;
        else if (v.map) p = v.map;
        else if (v.closure) p = v.closure;
        if (p) weaks->push_back({(int)pc, std::weak_ptr<void>(p)});
    };

    size_t pc = 0;
    while (pc < fn.code.size()) {
        const Inst& in = fn.code[pc];
        switch (in.op) {
            case Op::LoadConst: regs[in.a] = fn.consts[in.b]; break;
            case Op::Default:   regs[in.a] = defaultForCanonical(in.sname); break;
            case Op::Move:      regs[in.a] = regs[in.b]; break;
            case Op::MoveClear: regs[in.a] = std::move(regs[in.b]); regs[in.b] = Value{}; break;
            case Op::CopyVal:   regs[in.a] = copyValue(regs[in.b]); break;
            case Op::Arith: {
                const Value& l = regs[in.b];
                if ((in.tk == TokenKind::EqEq || in.tk == TokenKind::BangEq) &&
                    (l.kind == VKind::None || regs[in.c].kind == VKind::None)) {
                    regs[in.a] = arithPrim(in.tk, l, regs[in.c]);
                } else if (l.kind == VKind::Object && l.obj && l.obj->cls &&
                    l.obj->cls->name != "Range") {
                    regs[in.a] = objectArith(in.tk, in, l, regs[in.c]);
                } else {
                    std::string err;
                    regs[in.a] = arithPrim(in.tk, l, regs[in.c], &err);
                    if (!err.empty()) { raise(err); break; }
                }
                break;
            }
            case Op::Not: regs[in.a] = vbool(!regs[in.b].b); break;
            case Op::Neg:
                regs[in.a] = regs[in.b].kind == VKind::Float ? vfloat(-regs[in.b].f)
                                                             : vint(-regs[in.b].i);
                break;
            case Op::Jump: pc = (size_t)in.a; goto dispatched;
            case Op::JumpIfFalse:
                pc = regs[in.a].b ? pc + 1 : (size_t)in.b;
                goto dispatched;
            case Op::JumpIfTrue:
                pc = regs[in.a].b ? (size_t)in.b : pc + 1;
                goto dispatched;
            case Op::Ret:     return regs[in.a];
            case Op::RetVoid: return vvoid();
            case Op::Throw:
                thrown_ = regs[in.a];
                throwing_ = true;
                break;
            case Op::Call: {
                std::vector<Value> callArgs(regs.begin() + in.c,
                                            regs.begin() + in.c + in.d);
                regs[in.a] = call(in.b, std::move(callArgs));
                break;
            }
            case Op::CallDyn: {
                // bug.md #15: capture semantic uniqueness BEFORE the call
                // window copies its shared_ptr. MoveClear has already removed
                // the source local/field on a consumed self-append (in.b=1),
                // so use_count()==1 here means there is no program-visible
                // alias. Thread that fact through callDecl; the incidental
                // vector/self copies made below must not defeat IR COW.
                bool cowReceiver = in.b && regs[in.c].kind == VKind::Array &&
                                   regs[in.c].arr && regs[in.c].arr.use_count() == 1;
                std::vector<Value> window(regs.begin() + in.c,
                                          regs.begin() + in.c + in.d);
                const Stmt* decl = in.decl;
                Symbol* rc = classOfValue(window[0]);
                if (!decl) decl = findMethodByName(rc, in.sname, (int)window.size() - 1);
                if (decl) {
                    regs[in.a] = callDecl(decl, rc, std::move(window), cowReceiver);
                } else {
                    // bug.md #2: no method named `sname` — the checker doesn't
                    // distinguish "method call" from "field read + closure
                    // call" (Checker.cpp's typeOfCallInner leaves `resolved`
                    // unset either way), so mirror Eval.cpp's evalCall
                    // fallback: read the name as a FIELD on the receiver and,
                    // if it holds a closure, call that instead of raising.
                    Value fieldVal = getMember(window[0], in.sname);
                    long long irFn = -1;
                    if (fieldVal.kind == VKind::Closure && fieldVal.closure &&
                        !fieldVal.closure->env.empty()) {
                        auto f = fieldVal.closure->env[0].find("@fn");
                        if (f != fieldVal.closure->env[0].end()) irFn = f->second.i;
                    }
                    if (irFn >= 0) {
                        window[0] = fieldVal;
                        regs[in.a] = call((int)irFn, std::move(window));
                    } else {
                        raise("cannot resolve call target '" + in.sname + "'");
                    }
                }
                break;
            }
            case Op::CallNativeFn: {
                std::vector<Value> nargs(regs.begin() + in.c, regs.begin() + in.c + in.d);
                // bug #35 (reject route A): a spawn body reaching a Promise
                // through a bare GLOBAL bypasses the capture flatten. Re-apply
                // A-1's reject at the spawn call — sysThreadStart is spawn-only;
                // TaskGroup's same-thread sysTaskRun never reaches here.
                if (in.sname == "sysThreadStart" && !nargs.empty()) {
                    std::string gerr;
                    if (spawnBodyGlobalPromise(nargs[0], gerr)) { raise(gerr); break; }
                }
                // LA-30 B2 (doc 06 §4): engine-level task natives intercept
                // first — they spawn closures / park with this engine's state
                // discipline (see taskNative below); the shared dispatcher
                // handles the id-only pair.
                {
                    Value tout;
                    if (taskNative(in.sname, nargs, tout)) {
                        regs[in.a] = tout;
                        break;   // a raise inside rides the dispatch throw check
                    }
                }
                Value out; std::string err;
                if (nativeFreeCall(in.sname, nargs, out, err, &out_)) {
                    if (!err.empty()) { raise(err); break; }
                    regs[in.a] = out;
                    // env.exit (designs/exit-codes.md §4): ride the throwing_
                    // unwind, flagged as a clean exit (no report, code threaded).
                    if (interpExitRequested() && !exiting_) { exiting_ = true; throwing_ = true; }
                } else {
                    raise("unknown native '" + in.sname + "'");
                }
                break;
            }
            case Op::CallValue: {
                std::vector<Value> window(regs.begin() + in.c,
                                          regs.begin() + in.c + in.d);
                const Value& fnv = window[0];
                if (fnv.kind == VKind::Closure && fnv.closure &&
                    fnv.closure->lambda == nullptr) {
                    // IR closure: thisClass field repurposed? irFn stored via env slot
                }
                if (fnv.kind == VKind::Closure && fnv.closure) {
                    // irFn index is stored in closure->thisClass as an offset? No —
                    // stored in closure->lambda ptr-free path: we keep it in
                    // closure->env[0]["@fn"] as an int Value.
                    long long irFn = -1;
                    if (!fnv.closure->env.empty()) {
                        auto f = fnv.closure->env[0].find("@fn");
                        if (f != fnv.closure->env[0].end()) irFn = f->second.i;
                    }
                    if (irFn >= 0) {
                        regs[in.a] = call((int)irFn, std::move(window));
                        break;
                    }
                }
                raise("cannot resolve call target");
                break;
            }
            case Op::NewObject: {
                auto obj = std::make_shared<Object>();
                obj->cls = in.sym;
                Value v; v.kind = VKind::Object; v.obj = obj;
                regs[in.a] = v;
                track(pc, v);
                if (in.b >= 0) call(in.b, {v});      // $init(this)
                break;
            }
            case Op::GetMember: regs[in.a] = getMember(regs[in.b], in.sname); break;
            case Op::SetMember: setMember(regs[in.b], in.sname, regs[in.a]); break;
            case Op::RawGet: {
                const Value& o = regs[in.b];
                if (o.kind == VKind::Object && o.obj) {
                    auto f = o.obj->fields.find(keyFor(o.obj->cls, in.sname));
                    regs[in.a] = f != o.obj->fields.end() ? f->second : vvoid();
                } else {
                    regs[in.a] = vvoid();
                }
                break;
            }
            case Op::RawGetWeak: {
                const Value& o = regs[in.b];
                regs[in.a] = (o.kind == VKind::Object && o.obj)
                    ? weakObjectRead(o.obj, keyFor(o.obj->cls, in.sname)) : vnone();
                break;
            }
            case Op::RawSet: {
                const Value& o = regs[in.b];
                if (o.kind == VKind::Object && o.obj)
                    o.obj->fields[keyFor(o.obj->cls, in.sname)] = regs[in.a];
                break;
            }
            case Op::RawSetWeak: {
                const Value& o = regs[in.b];
                if (o.kind == VKind::Object && o.obj)
                    weakObjectWrite(o.obj, keyFor(o.obj->cls, in.sname), regs[in.a]);
                break;
            }
            case Op::NewArray: {
                auto vec = std::make_shared<std::vector<Value>>();
                for (int k = 0; k < in.d; ++k) {
                    const Value& ev = regs[in.c + k];
                    if (ev.kind == VKind::Object && ev.obj && ev.obj->cls &&
                        ev.obj->cls->name == "Range") {
                        long long lo = ev.obj->fields["start"].i;
                        long long hi = ev.obj->fields["end"].i;
                        for (long long x = lo; x <= hi; ++x) vec->push_back(vint(x));
                    } else {
                        vec->push_back(ev);
                    }
                }
                Value v; v.kind = VKind::Array; v.arr = vec;
                regs[in.a] = v;
                track(pc, v);
                break;
            }
            case Op::NewArraySized: {
                auto vec = std::make_shared<std::vector<Value>>();
                if (in.d == 2) {
                    long long n = regs[in.c].i;
                    for (long long k = 0; k < n; ++k) vec->push_back(regs[in.c + 1]);
                }
                Value v; v.kind = VKind::Array; v.arr = vec;
                regs[in.a] = v;
                track(pc, v);
                break;
            }
            case Op::NewBlock: {                          // Track 03 §3: Block(n)
                Value v = vblockNew(regs[in.c].i);
                regs[in.a] = v;
                track(pc, v);
                break;
            }
            case Op::NewBlockStr: {                       // Block::fromString(s)
                const std::string& s = regs[in.c].s;
                auto bd = std::make_shared<BlockData>();
                bd->bytes = std::make_shared<std::vector<uint8_t>>(s.begin(), s.end());
                bd->off = 0; bd->len = bd->bytes->size();
                Value v = vblock(bd);
                regs[in.a] = v;
                track(pc, v);
                break;
            }
            case Op::NewMap: {
                Value v; v.kind = VKind::Map;
                v.map = std::make_shared<std::vector<std::pair<Value, Value>>>();
                regs[in.a] = v;
                track(pc, v);
                break;
            }
            case Op::MakeRange: {
                auto obj = std::make_shared<Object>();
                obj->cls = in.sym;
                obj->fields["start"] = regs[in.b];
                obj->fields["end"] = regs[in.c];
                Value v; v.kind = VKind::Object; v.obj = obj;
                regs[in.a] = v;
                track(pc, v);
                break;
            }
            case Op::GetIndex:  regs[in.a] = getIndex(regs[in.b], regs[in.c]); break;
            // techdesign-columnar §5.4: the fused element-field read is, in the
            // boxed interpreter, just the element read followed by the field read
            // (no layout — this is the semantic reference the native leg matches).
            case Op::ColGet:
                regs[in.a] = getMember(getIndex(regs[in.b], regs[in.c]), in.sname);
                break;
            case Op::IndexStore: {
                // §11/§15 COW: drop the dest temp's stale ref from the previous
                // execution, and the rebind chain's (`CopyVal t <- nb; Move L <- t`)
                // stale t, so a uniquely-owned base reads use_count 1 and mutates
                // in place. Registers are never reused, so both are written-once
                // temps rewritten before any read on every path.
                regs[in.a] = Value();
                if (pc + 2 < fn.code.size()) {
                    const Inst& n1 = fn.code[pc + 1];
                    const Inst& n2 = fn.code[pc + 2];
                    if (n1.op == Op::CopyVal && n1.b == in.a &&
                        n2.op == Op::Move && n2.b == n1.a &&
                        n1.a != in.b && n1.a != in.c && n1.a != in.d)
                        regs[n1.a] = Value();
                }
                regs[in.a] = indexStore(regs[in.b], regs[in.c], regs[in.d]);
                break;
            }
            case Op::IterLen: regs[in.a] = iterLen(regs[in.b]); break;
            case Op::IterAt:  regs[in.a] = iterAt(regs[in.b], regs[in.c].i); break;
            case Op::Await: {          // LA-30: park the task (pump until M6)
                Value p = regs[in.b];
                if (p.kind == VKind::Object && p.obj) {
                    if (lv_tasks_enabled()) {
                        // LA-30 S2 (techdesign-03 §2): ready → proceed WITHOUT
                        // yielding (doc 5 C4, the pump's first ready-check).
                        // Not ready → park; the frame's `p` copy holds the
                        // reference, so the borrowed key cannot dangle (G12).
                        if (!(taskPollPromise(p.obj.get()) & 1)) {
                            // S3/G7: regs/pc are fiber-carried locals; thrown_
                            // is dead while !throwing_ (G11 asserts the flag is
                            // clear at the park) — saved/restored anyway so
                            // every mutable member is park-restored.
                            Value savedThrown = std::move(thrown_);
                            thrown_ = Value{};
                            int r = lv_task_park_on(p.obj.get());
                            thrown_ = std::move(savedThrown);
                            // env.exit fired while we were parked: resume by
                            // riding the SAME throwing_ unwind the pump rode
                            // (exit-codes §4) — an exit stops every task,
                            // parked ones included. A plain callback throw
                            // deliberately does NOT re-raise here (doc 5 C2).
                            if (taskTermStashed_ && taskTermExiting_) {
                                exiting_ = true;
                                throwing_ = true;
                                break;   // unwind via the dispatch throw check
                            }
                            // LV_PARK_DRAINED → C3, live since the M5 flip
                            // (doc 5 §5, S5): the loop drained with the
                            // promise still pending — fabricating the default
                            // value is the disease None exists to cure
                            // (info.md §9). Loud, local, catchable; inside a
                            // spawn body it lands in spawn's catch → reject →
                            // rethrows at the spawner's await (doc 2 §7).
                            // LANG_PUMP=1 keeps the old silent fall-through
                            // via the pump branch below; deleted at M6.
                            if (r == LV_PARK_DRAINED) {
                                raise("await: event loop drained with promise unresolved");
                                break;
                            }
                            // B2 (doc 06 §2): cancellation delivered at the
                            // park — same rethrow-point machinery as
                            // Worker-failed and C3.
                            if (r == LV_PARK_CANCELLED) {
                                raiseClass("CancelledException", "task cancelled");
                                break;
                            }
                        }
                    } else {
                        // today's pump, verbatim behind the flag — deleted at M6
                        RuntimeLoop& loop = RuntimeLoop::instance();
                        while (!throwing_) {
                            auto r = p.obj->fields.find("ready");
                            if (r != p.obj->fields.end() && r->second.b) break;
                            if (!loop.hasWork()) break;
                            for (LoopCallback& job : loop.nextBatch()) {
                                invokeClosure(job.callback, job.argument);
                                if (throwing_) break;
                            }
                        }
                    }
                    // techdesign-threads-3 §3.3: a Worker whose body threw sets
                    // `failed`; await rethrows it (a catchable RuntimeException
                    // carrying failMessage). Plain Promises lack the field
                    // (absent = not failed) — unchanged for every existing await.
                    if (!throwing_) {
                        auto f = p.obj->fields.find("failed");
                        if (f != p.obj->fields.end() && f->second.b) {
                            auto m = p.obj->fields.find("failMessage");
                            raise(m != p.obj->fields.end() && m->second.kind == VKind::String
                                      ? m->second.s : "worker failed");
                            break;
                        }
                    }
                    auto v = p.obj->fields.find("value");
                    regs[in.a] = v != p.obj->fields.end() ? v->second : vvoid();
                } else {
                    regs[in.a] = p;
                }
                break;
            }
            case Op::IsType: {
                const Value& v = regs[in.b];
                auto match1 = [&](const std::string& n) {
                    if (n == "None")   return v.kind == VKind::None;
                    if (n == "int")    return v.kind == VKind::Int;
                    if (n == "char")   return v.kind == VKind::Char;
                    if (n == "string") return v.kind == VKind::String;
                    if (n == "bool")   return v.kind == VKind::Bool;
                    if (n == "float")  return v.kind == VKind::Float;
                    if (n.rfind("Array", 0) == 0) return v.kind == VKind::Array;
                    if (n.rfind("Map", 0) == 0)   return v.kind == VKind::Map;
                    if (n == "Block")  return v.kind == VKind::Block;
                    if (in.sym && v.kind == VKind::Object && v.obj)
                        return isSubclassOrSelf(v.obj->cls, in.sym);
                    return false;
                };
                bool ok = false;
                if (in.sname.find(" | ") != std::string::npos) {
                    for (const std::string& m : unionMembersOf(in.sname))
                        if (match1(m)) { ok = true; break; }
                } else {
                    ok = match1(in.sname);
                }
                regs[in.a] = vbool(ok);
                break;
            }
            case Op::LoadGlobal:  regs[in.a] = globals_[in.b]; break;
            case Op::StoreGlobal: globals_[in.b] = regs[in.a]; break;
            case Op::MakeClosure: {
                auto cl = std::make_shared<Closure>();
                cl->env.emplace_back();
                cl->env[0]["@fn"] = vint(in.b);
                Value v; v.kind = VKind::Closure; v.closure = cl;
                regs[in.a] = v;
                track(pc, v);
                break;
            }
            case Op::CaptureVar:
                if (regs[in.a].kind == VKind::Closure && regs[in.a].closure) {
                    // interned capture names: sname's storage lives in the Inst
                    regs[in.a].closure->env[0][std::string_view(in.sname)] = regs[in.b];
                }
                break;
            case Op::LoadCapture: {
                const Value& self = regs[0];
                regs[in.a] = vvoid();
                if (self.kind == VKind::Closure && self.closure &&
                    !self.closure->env.empty()) {
                    auto f = self.closure->env[0].find(std::string_view(in.sname));
                    if (f != self.closure->env[0].end()) regs[in.a] = f->second;
                }
                break;
            }
            case Op::Print:   interpEmitStdout(out_, valueToString(regs[in.a])); break;
            case Op::PrintNl: interpEmitStdout(out_, "\n", 1); break;
            case Op::VFree:   break;   // §15: ELF-backend reclamation; shared_ptr frees here
        }
        if (memOn_) {
            switch (in.op) {
                case Op::NewObject: case Op::NewArray: case Op::NewArraySized:
                case Op::NewMap: case Op::MakeRange: case Op::MakeClosure:
                    mem_.onAlloc(regs[in.a], fnIndex, (int)pc); break;
                default: break;
            }
            ++mem_.opClock;
            mem_.sweep();                 // record newly-unreachable cells (free-schedule oracle)
        }
        ++pc;
    dispatched:
        if (throwing_) {
            size_t at = pc > 0 ? pc - 1 : 0;
            bool handled = false;
            for (const Handler& h : fn.handlers) {
                if ((int)at < h.start || (int)at >= h.end) continue;
                Symbol* thrownCls =
                    (thrown_.kind == VKind::Object && thrown_.obj) ? thrown_.obj->cls
                                                                    : nullptr;
                if (thrownCls && h.type && isSubclassOrSelf(thrownCls, h.type)) {
                    throwing_ = false;
                    regs[h.bindReg] = thrown_;
                    pc = (size_t)h.handlerPc;
                    handled = true;
                    break;
                }
            }
            if (!handled) return vvoid();          // unwind to caller
        }
    }
    return vvoid();
}

// ---------------------------------------------------------------------------
//  member / index / iteration / operators
// ---------------------------------------------------------------------------

Value IrInterp::getMember(const Value& base, const std::string& key) {
    if (base.kind != VKind::Object || !base.obj) return vvoid();
    std::string plainName = key;
    size_t p = key.find("::");
    if (p != std::string::npos) plainName = key.substr(p + 2);
    // engine-specific: accessor resolution stays here (the IR carries no
    // rawField_ re-entrancy guard — raw access is a distinct lowered op).
    if (const Stmt* getter = findAccessor(base.obj->cls, plainName, true))
        return callDecl(getter, base.obj->cls, {base});
    return rtRawFieldGet(base.obj, keyFor(base.obj->cls, key));
}

void IrInterp::setMember(const Value& base, const std::string& key, const Value& v) {
    if (base.kind != VKind::Object || !base.obj) return;
    std::string plainName = key;
    size_t p = key.find("::");
    if (p != std::string::npos) plainName = key.substr(p + 2);
    // engine-specific: accessor resolution stays here (see getMember).
    if (const Stmt* setter = findAccessor(base.obj->cls, plainName, false)) {
        callDecl(setter, base.obj->cls, {base, v});
        return;
    }
    rtRawFieldSet(base.obj, keyFor(base.obj->cls, key), v);
}

Value IrInterp::getIndex(const Value& base, const Value& idx) {
    Symbol* cls = classOfValue(base);
    if (cls)
        if (const Stmt* g = findAccessor(cls, "[]", true))
            return callDecl(g, cls, {base, idx});
    if (base.kind == VKind::Array && base.arr) {
        long long i = idx.i;
        if (i < 0 || i >= (long long)base.arr->size()) {
            raise("index " + std::to_string(i) + " out of bounds (length " +
                  std::to_string(base.arr->size()) + ")");
            return vvoid();
        }
        return (*base.arr)[(size_t)i];
    }
    return vvoid();
}

Value IrInterp::indexStore(const Value& base, const Value& idx, const Value& v) {
    if (base.kind == VKind::Object && base.obj) {
        if (const Stmt* s = findAccessor(base.obj->cls, "[]", false))
            callDecl(s, base.obj->cls, {base, idx, v});
        return base;                                    // same reference
    }
    if (base.kind == VKind::Array) {
        // §11/§15 COW: uniquely owned -> mutate in place (pure surface, in-place
        // implementation); aliased arrays take the pure-value copy below.
        if (base.arr && base.arr.use_count() == 1) {
            long long i = idx.i;
            if (i >= 0 && i < (long long)base.arr->size()) (*base.arr)[(size_t)i] = v;
            return base;
        }
        auto nv = std::make_shared<std::vector<Value>>(base.arr ? *base.arr
                                                                 : std::vector<Value>{});
        long long i = idx.i;
        if (i >= 0 && i < (long long)nv->size()) (*nv)[(size_t)i] = v;
        Value r; r.kind = VKind::Array; r.arr = nv;
        return r;
    }
    if (base.kind == VKind::Map) {
        if (base.map && base.map.use_count() == 1) {   // §15 COW: unique -> upsert in place
            for (auto& [k, val] : *base.map)
                if (keyEquals(k, idx)) { val = v; return base; }
            base.map->push_back({idx, v});
            return base;
        }
        auto nv = std::make_shared<std::vector<std::pair<Value, Value>>>(
            base.map ? *base.map : std::vector<std::pair<Value, Value>>{});
        bool replaced = false;
        for (auto& [k, val] : *nv)
            if (keyEquals(k, idx)) { val = v; replaced = true; break; }
        if (!replaced) nv->push_back({idx, v});
        Value r; r.kind = VKind::Map; r.map = nv;
        return r;
    }
    return base;
}

Value IrInterp::objectArith(TokenKind op, const Inst& in, const Value& l, const Value& r) {
    Symbol* cls = l.obj->cls;
    return rtObjectArith(
        op, cls, in.decl, l, r,
        // engine-specific: invoke the operator method (receiver + operand).
        [&](const Stmt* m, const Value& lhs, const Value& rhs) {
            return callDecl(m, cls, {lhs, rhs});
        },
        // engine-specific: raise this engine's runtime exception.
        [&](const std::string& sym) {
            raise("no operator '" + sym + "' on '" + std::string(cls->name) + "'");
            return vvoid();
        });
}

Value IrInterp::iterLen(const Value& iter) {
    if (iter.kind == VKind::Array) return vint(iter.arr ? (long long)iter.arr->size() : 0);
    if (iter.kind == VKind::Map) return vint(iter.map ? (long long)iter.map->size() : 0);
    if (iter.kind == VKind::Object && iter.obj && iter.obj->cls &&
        iter.obj->cls->name == "Range") {
        long long lo = iter.obj->fields["start"].i, hi = iter.obj->fields["end"].i;
        return vint(hi >= lo ? hi - lo + 1 : 0);
    }
    return vint(0);
}

Value IrInterp::iterAt(const Value& iter, long long idx) {
    if (iter.kind == VKind::Array && iter.arr) {
        if (idx >= 0 && idx < (long long)iter.arr->size()) return (*iter.arr)[(size_t)idx];
        return vvoid();
    }
    if (iter.kind == VKind::Map && iter.map) {
        if (idx < 0 || idx >= (long long)iter.map->size()) return vvoid();
        const auto& [k, v] = (*iter.map)[(size_t)idx];
        Symbol* pairSym = mod_.sema->global->lookup("Pair");
        auto obj = std::make_shared<Object>();
        obj->cls = pairSym;
        obj->fields["first"] = k;
        obj->fields["second"] = v;
        Value pv; pv.kind = VKind::Object; pv.obj = obj;
        return pv;
    }
    if (iter.kind == VKind::Object && iter.obj && iter.obj->cls &&
        iter.obj->cls->name == "Range")
        return vint(iter.obj->fields["start"].i + idx);
    return vvoid();
}

void IrInterp::invokeClosure(const Value& cb, const Value& arg) {
    if (cb.kind != VKind::Closure || !cb.closure || cb.closure->env.empty()) return;
    auto f = cb.closure->env[0].find("@fn");
    if (f == cb.closure->env[0].end()) return;
    call((int)f->second.i, {cb, arg});
}

std::string IrInterp::run() {
    RuntimeLoop::instance().reset();
    interpResetExit();                 // exit-codes.md §4: fresh per run
    exiting_ = false; exitCode_ = 0;
    globals_.assign(mod_.nglobals, Value{});
    if (lv_tasks_enabled()) {
        // LA-30 (techdesign-03 §2.2/§2.3): ginit + entry run as task 0; the
        // scheduler replaces the pump drain. Hooks installed once per run.
        lv_sched_init();
        gTaskInterp_ = this;
        taskTermStashed_ = false; taskTermExiting_ = false;
        taskTermThrown_ = Value{};
        lv_sched_hooks(&IrInterp::taskPollPromise, &IrInterp::taskLoopStep);
        lv_sched_throw_probe(&IrInterp::taskThrowProbe);
        lv_sched_run(&IrInterp::taskRunProgram, this, nullptr);
        gTaskInterp_ = nullptr;
        // Re-raise the first task-captured termination (throw or env.exit) so
        // the shared tail below reports exactly what the pump unwind would.
        if (taskTermStashed_) {
            throwing_ = true;
            thrown_ = taskTermThrown_;
            exiting_ = taskTermExiting_;
        }
    } else {
        if (mod_.ginit >= 0) call(mod_.ginit, {});
        if (mod_.entry >= 0) call(mod_.entry, {});
        // Pump drain, verbatim behind the flag — deleted at M6.
        RuntimeLoop& loop = RuntimeLoop::instance();
        while (!throwing_ && loop.hasWork()) {
            for (LoopCallback& job : loop.nextBatch()) {
                invokeClosure(job.callback, job.argument);
                if (throwing_) break;
            }
        }
    }
    if (throwing_ && !exiting_) {
        std::string cls = (thrown_.kind == VKind::Object && thrown_.obj && thrown_.obj->cls)
                              ? std::string(thrown_.obj->cls->name) : "value";
        std::string msg;
        if (thrown_.kind == VKind::Object && thrown_.obj) {
            auto it = thrown_.obj->fields.find("message");
            if (it != thrown_.obj->fields.end()) msg = it->second.s;
        }
        out_ += "Uncaught " + cls + (msg.empty() ? "" : ": " + msg) + "\n";
    }
    // Exit code (§4/§5): uncaught => 1 (gap b); else the recorded code / 0.
    bool uncaught = throwing_ && !exiting_;
    exitCode_ = uncaught ? 1 : interpExitCode();
    interpSignalCleanup();             // terminal-floor.md §3 M4: unblock + close signal fds
    return out_;
}

// ---------------------------------------------------------------------------
//  LA-30 tasks: substrate hooks (techdesign-03) — mirrors the oracle's
// ---------------------------------------------------------------------------

IrInterp* IrInterp::gTaskInterp_ = nullptr;

// G11: the throw flag never crosses a task boundary; first termination wins
// (see RuntimeCore rtCaptureTermination for the rationale).
void IrInterp::taskCaptureTermination() {
    rtCaptureTermination(throwing_, exiting_, thrown_,
                         taskTermStashed_, taskTermThrown_, taskTermExiting_);
}

// G15: bool immediates only — the pump's ready-check reads, ARC-silent.
int IrInterp::taskPollPromise(const void* obj) {
    return rtPollPromise(obj);
}

int IrInterp::taskLoopStep() {
    IrInterp* ir = gTaskInterp_;
    if (!ir) return 0;
    return rtTaskLoopStep(ir->taskTermStashed_, &IrInterp::taskRunJob);
}

int IrInterp::taskThrowProbe() {
    return gTaskInterp_ && gTaskInterp_->throwing_ ? 1 : 0;
}

void IrInterp::taskRunProgram(void* selfp, void*) {
    auto* ir = static_cast<IrInterp*>(selfp);
    // Same shape (and same no-check-between) as the pump path: ginit's throw
    // unwinds through the dispatch loop's own throwing_ check.
    if (ir->mod_.ginit >= 0) ir->call(ir->mod_.ginit, {});
    if (ir->mod_.entry >= 0) ir->call(ir->mod_.entry, {});
    ir->taskCaptureTermination();
}

void IrInterp::taskRunJob(void* ctxp, void*) {
    std::unique_ptr<RtTaskJob> job(static_cast<RtTaskJob*>(ctxp));
    IrInterp* ir = gTaskInterp_;
    if (!ir) return;
    if (ir->taskTermStashed_) return;   // program already terminated: drop
    ir->invokeClosure(job->callback, job->argument);
    ir->taskCaptureTermination();       // G11: never return to the scheduler throwing
}

// B2 (doc 06 §4): a group child's body — taskRunJob's zero-arg twin (the
// closure is the callee's own arg0; no event argument). An uncaught throw is
// program-uncaught (C2); the cancellation absorption lives in TaskGroup.run.
void IrInterp::taskRunGroupBody(void* ctxp, void*) {
    std::unique_ptr<RtTaskJob> job(static_cast<RtTaskJob*>(ctxp));
    IrInterp* ir = gTaskInterp_;
    if (!ir) return;
    if (ir->taskTermStashed_) return;   // program already terminated: drop
    const Value& cb = job->callback;
    if (cb.kind == VKind::Closure && cb.closure && !cb.closure->env.empty()) {
        auto f = cb.closure->env[0].find("@fn");
        if (f != cb.closure->env[0].end())
            ir->call((int)f->second.i, {cb});
    }
    ir->taskCaptureTermination();       // G11: never return to the scheduler throwing
}

// bug #35 (reject route A): the IR twin of Eval's spawnBodyGlobalPromise. An
// IR-lowered closure carries no source AST (lambda == null), so instead of a
// free-name walk this uses the body function's precomputed global-slot set
// (computeFnGlobalRefs, transitive over the nested lambdas it CREATES) and
// scans each referenced global's current value — byte-identical to the oracle
// and LLVM guards.
bool IrInterp::spawnBodyGlobalPromise(const Value& body, std::string& err) {
    if (body.kind != VKind::Closure || !body.closure || body.closure->env.empty())
        return false;
    auto f = body.closure->env[0].find("@fn");
    if (f == body.closure->env[0].end()) return false;
    long long fnIndex = f->second.i;
    if (fnIndex < 0) return false;
    if (!fnGlobalRefsReady_) {
        fnGlobalRefs_ = computeFnGlobalRefs(mod_);
        fnGlobalRefsReady_ = true;
    }
    if ((size_t)fnIndex >= fnGlobalRefs_.size()) return false;
    for (int slot : fnGlobalRefs_[(size_t)fnIndex]) {
        if (slot < 0 || (size_t)slot >= globals_.size()) continue;
        if (lvThreadValueHasPromise(globals_[(size_t)slot], err)) return true;
    }
    return false;
}

// B2 (doc 06 §4): sysTaskRun / sysTaskJoinAll / sysAwaitAny2 for this engine —
// the Eval.cpp twin (S3/G7 here: regs/pc are fiber-carried; thrown_ is dead
// while !throwing_, saved/restored at the parks anyway, mirroring Op::Await).
bool IrInterp::taskNative(const std::string& name, std::vector<Value>& args, Value& out) {
    if (name != "sysTaskRun" && name != "sysTaskJoinAll" && name != "sysAwaitAny2")
        return false;
    out = vvoid();
    if (!lv_tasks_enabled()) {
        raise("TaskGroup requires tasks (running under LANG_PUMP=1)");
        return true;
    }
    if (name == "sysTaskRun") {
        if (args.empty() || args[0].kind != VKind::Closure || !args[0].closure) {
            raise("sysTaskRun: body must be a closure");
            return true;
        }
        uint64_t id = lv_task_spawn_registered(&IrInterp::taskRunGroupBody,
                                               new RtTaskJob{args[0], Value{}}, nullptr);
        out = vint((long long)id);
        return true;
    }
    if (name == "sysTaskJoinAll") {
        std::vector<uint64_t> ids;
        if (!args.empty() && args[0].kind == VKind::Array && args[0].arr)
            for (const Value& v : *args[0].arr) ids.push_back((uint64_t)v.i);
        Value savedThrown = std::move(thrown_);
        thrown_ = Value{};
        int r = lv_task_park_join(ids.data(), (int)ids.size());
        thrown_ = std::move(savedThrown);
        if (taskTermStashed_ && taskTermExiting_) {          // env.exit while parked
            exiting_ = true; throwing_ = true;
            return true;
        }
        if (r == LV_PARK_DRAINED)
            raise("TaskGroup.close: event loop drained with tasks unjoined");
        else if (r == LV_PARK_CANCELLED)
            raiseClass("CancelledException", "task cancelled");
        else
            out = vint((long long)ids.size());
        return true;
    }
    // sysAwaitAny2 — reads NO value fields (G15/S2); only WHICH promise settled.
    if (args.size() < 2 || args[0].kind != VKind::Object || !args[0].obj ||
        args[1].kind != VKind::Object || !args[1].obj) {
        raise("sysAwaitAny2: both arguments must be promises");
        return true;
    }
    if (taskPollPromise(args[0].obj.get())) { out = vint(0); return true; }
    if (taskPollPromise(args[1].obj.get())) { out = vint(1); return true; }
    int which = 0;
    Value savedThrown = std::move(thrown_);
    thrown_ = Value{};
    int r = lv_task_park_any2(args[0].obj.get(), args[1].obj.get(), &which);
    thrown_ = std::move(savedThrown);
    if (taskTermStashed_ && taskTermExiting_) {              // env.exit while parked
        exiting_ = true; throwing_ = true;
        return true;
    }
    if (r == LV_PARK_DRAINED)
        raise("await: event loop drained with promise unresolved");
    else if (r == LV_PARK_CANCELLED)
        raiseClass("CancelledException", "task cancelled");
    else
        out = vint(which);
    return true;
}
