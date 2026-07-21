#include "ir/Ownership.hpp"
#include <unordered_map>

namespace {

bool isAlloc(Op op) {
    switch (op) {
        case Op::NewObject: case Op::NewArray: case Op::NewArraySized:
        case Op::NewMap: case Op::MakeRange: case Op::MakeClosure:
            return true;
        default:
            return false;
    }
}

// Per-function summary used interprocedurally.
struct Summary {
    std::vector<bool> paramEscapes;   // param value stored/thrown/etc.
    std::vector<bool> paramToRet;     // param value may alias the return value
};

// Native-core annotations, by method name. Sound because anything stored
// INTO a container was already escape-marked at insertion time, so returning
// container *contents* introduces no untracked aliases; the pure builders
// (add/with/without) alias their inputs into the RESULT, which we model.
struct NativeInfo {
    bool selfToResult = false;   // result aliases the receiver's allocations
    bool argsToResult = false;   // result aliases argument allocations
};
const NativeInfo* nativeInfo(const std::string& name) {
    static const std::unordered_map<std::string, NativeInfo> table = {
        {"length", {}}, {"at", {}}, {"has", {}}, {"keys", {}}, {"values", {}},
        {"isEmpty", {}}, {"contains", {}}, {"indexOf", {}}, {"toString", {}},
        {"charAt", {}}, {"subStr", {}}, {"toUpper", {}}, {"toLower", {}},
        {"trim", {}}, {"startsWith", {}}, {"endsWith", {}},
        {"abs", {}}, {"max", {}}, {"min", {}},
        {"add", {true, true}}, {"with", {true, true}}, {"without", {true, false}},
    };
    auto it = table.find(name);
    return it == table.end() ? nullptr : &it->second;
}

struct Analyzer {
    const IrModule& mod;
    std::vector<Summary> summaries;
    // per function: site pc -> escape reason ("" = not escaped)
    std::vector<std::unordered_map<int, std::string>> siteEscape;

    explicit Analyzer(const IrModule& m) : mod(m) {
        summaries.resize(mod.functions.size());
        siteEscape.resize(mod.functions.size());
        for (size_t i = 0; i < mod.functions.size(); ++i) {
            summaries[i].paramEscapes.assign(mod.functions[i].nparams, false);
            summaries[i].paramToRet.assign(mod.functions[i].nparams, false);
        }
    }

    // Track: positive ids = allocation pcs; negative ids = -(param+1).
    using Sites = std::set<int>;

    bool analyzeFn(int fi) {
        const IrFunction& fn = mod.functions[fi];
        std::vector<Sites> regs(fn.nregs);
        for (int p = 0; p < fn.nparams && p < fn.nregs; ++p) regs[p].insert(-(p + 1));

        auto& esc = siteEscape[fi];
        Summary& sum = summaries[fi];
        bool summaryChanged = false;

        auto escapeSet = [&](const Sites& s, const std::string& why) {
            for (int id : s) {
                if (id >= 0) {
                    if (!esc.count(id)) esc[id] = why;
                } else {
                    int p = -id - 1;
                    if (p < (int)sum.paramEscapes.size() && !sum.paramEscapes[p]) {
                        sum.paramEscapes[p] = true;
                        summaryChanged = true;
                    }
                }
            }
        };
        auto callSinks = [&](int fnIdx, int base, int argc, std::vector<Sites>& r,
                             Sites& dstAliases) {
            const Summary& cs = summaries[fnIdx];
            for (int k = 0; k < argc; ++k) {
                bool pe = k < (int)cs.paramEscapes.size() && cs.paramEscapes[k];
                bool pr = k < (int)cs.paramToRet.size() && cs.paramToRet[k];
                if (pe) escapeSet(r[base + k], "passed to a retaining callee");
                if (pr) dstAliases.insert(r[base + k].begin(), r[base + k].end());
            }
        };
        // §15: a value-struct argument is copied BY VALUE into a container (a dense
        // array inlines the record's bytes), so the OBJECT does not alias the
        // result — it is dead after the call. Skip the argsToResult alias only when
        // EVERY source is provably a value-struct alloc (definite CopyVal / value
        // NewObject), so the copy routes to the arena instead of leaking on the heap.
        auto argIsValueStruct = [&](const Sites& s) -> bool {
            if (s.empty()) return false;
            for (int id : s) {
                if (id < 0) return false;
                const Inst& si = fn.code[id];
                bool vs = (si.op == Op::CopyVal && si.c == 1) ||
                          (si.op == Op::NewObject && si.sym && si.sym->isValue);
                if (!vs) return false;
            }
            return true;
        };
        // iterate to a local fixpoint (alias sets only grow)
        bool changed = true;
        while (changed) {
            changed = false;
            auto merge = [&](Sites& into, const Sites& from) {
                size_t before = into.size();
                into.insert(from.begin(), from.end());
                if (into.size() != before) changed = true;
            };
            for (int pc = 0; pc < (int)fn.code.size(); ++pc) {
                const Inst& in = fn.code[pc];
                // §15: a DEFINITE value-struct copy (in.c=1) is a fresh independent
                // object — treat it as an alloc and do NOT alias it to the source
                // (unlike a Move). A maybe-copy stays a passthrough alias below.
                bool cvAlloc = in.op == Op::CopyVal && in.c == 1;
                if (isAlloc(in.op) || cvAlloc) {
                    if (!regs[in.a].count(pc)) { regs[in.a].insert(pc); changed = true; }
                    if (in.op == Op::NewArray)                   // elements alias the array
                        for (int k = 0; k < in.d; ++k) merge(regs[in.a], regs[in.c + k]);
                    if (in.op == Op::NewArraySized && in.d == 2) // fill aliases the array
                        merge(regs[in.a], regs[in.c + 1]);
                    continue;
                }
                switch (in.op) {
                    case Op::Move: merge(regs[in.a], regs[in.b]); break;
                    case Op::MoveClear: merge(regs[in.a], regs[in.b]); break;   // ownership transfer
                    // A maybe-value-struct copy may be a passthrough (non-struct) —
                    // alias it to the source so escape propagates like a Move.
                    case Op::CopyVal: merge(regs[in.a], regs[in.b]); break;
                    case Op::IndexStore:
                        merge(regs[in.a], regs[in.b]);           // new base aliases old
                        break;
                    case Op::Call: {
                        Sites alias;
                        callSinks(in.b, in.c, in.d, regs, alias);
                        merge(regs[in.a], alias);
                        break;
                    }
                    case Op::CallDyn: {
                        int callee = -1;
                        if (in.decl) {
                            auto it = mod.byDecl.find(in.decl);
                            if (it != mod.byDecl.end()) callee = it->second;
                        }
                        if (callee >= 0) {
                            Sites alias;
                            callSinks(callee, in.c, in.d, regs, alias);
                            merge(regs[in.a], alias);
                        } else if (const NativeInfo* ni = nativeInfo(in.sname)) {
                            Sites alias;
                            if (ni->selfToResult) alias.insert(regs[in.c].begin(),
                                                               regs[in.c].end());
                            if (ni->argsToResult)
                                for (int k = 1; k < in.d; ++k)
                                    if (!argIsValueStruct(regs[in.c + k]))
                                        alias.insert(regs[in.c + k].begin(),
                                                     regs[in.c + k].end());
                            merge(regs[in.a], alias);
                        }
                        break;
                    }
                    default: break;
                }
            }
        }

        // sinks (one pass over the stable sets)
        for (int pc = 0; pc < (int)fn.code.size(); ++pc) {
            const Inst& in = fn.code[pc];
            switch (in.op) {
                case Op::Ret:
                    for (int id : regs[in.a]) {
                        if (id >= 0) {
                            if (!esc.count(id)) esc[id] = "returned to caller";
                        } else {
                            int p = -id - 1;
                            if (p < (int)sum.paramToRet.size() && !sum.paramToRet[p]) {
                                sum.paramToRet[p] = true;
                                summaryChanged = true;
                            }
                        }
                    }
                    break;
                case Op::Throw:      escapeSet(regs[in.a], "thrown"); break;
                case Op::SetMember:
                case Op::RawSet:     escapeSet(regs[in.a], "stored in an object"); break;
                case Op::RawSetWeak: break; // non-owning slot is not an escape
                case Op::IndexStore: escapeSet(regs[in.d], "stored by index"); break;
                case Op::CaptureVar: escapeSet(regs[in.b], "captured by a closure"); break;
                case Op::StoreGlobal: escapeSet(regs[in.a], "stored in a global"); break;
                case Op::CallNativeFn:
                    for (int k = 0; k < in.d; ++k)
                        escapeSet(regs[in.c + k], "passed to a native");
                    break;
                case Op::Call: {
                    const Summary& cs = summaries[in.b];
                    for (int k = 0; k < in.d; ++k)
                        if (k < (int)cs.paramEscapes.size() && cs.paramEscapes[k])
                            escapeSet(regs[in.c + k], "passed to a retaining callee");
                    break;
                }
                case Op::CallDyn: {
                    int callee = -1;
                    if (in.decl) {
                        auto it = mod.byDecl.find(in.decl);
                        if (it != mod.byDecl.end()) callee = it->second;
                    }
                    if (callee >= 0) {
                        const Summary& cs = summaries[callee];
                        for (int k = 0; k < in.d; ++k)
                            if (k < (int)cs.paramEscapes.size() && cs.paramEscapes[k])
                                escapeSet(regs[in.c + k], "passed to a retaining callee");
                    } else if (!nativeInfo(in.sname)) {
                        for (int k = 0; k < in.d; ++k)
                            escapeSet(regs[in.c + k], "passed to an unknown callee");
                    }
                    break;
                }
                case Op::CallValue:
                    for (int k = 0; k < in.d; ++k)
                        escapeSet(regs[in.c + k], "passed through a function value");
                    break;
                case Op::Arith:
                    // object operator with unknown target: conservative
                    if (!in.decl) {
                        bool tracked = !regs[in.b].empty() || !regs[in.c].empty();
                        if (tracked && in.tk != TokenKind::End) {
                            // primitive arithmetic never retains; only object
                            // receivers matter. We cannot see types here, so
                            // only escape when a set is non-empty AND the op
                            // could dispatch to a method (comparison/arith on
                            // a tracked allocation).
                            escapeSet(regs[in.b], "operand of an unresolved operator");
                            escapeSet(regs[in.c], "operand of an unresolved operator");
                        }
                    } else {
                        auto it = mod.byDecl.find(in.decl);
                        if (it != mod.byDecl.end()) {
                            const Summary& cs = summaries[it->second];
                            if (!cs.paramEscapes.empty() && cs.paramEscapes[0])
                                escapeSet(regs[in.b], "passed to a retaining operator");
                            if (cs.paramEscapes.size() > 1 && cs.paramEscapes[1])
                                escapeSet(regs[in.c], "passed to a retaining operator");
                        }
                    }
                    break;
                default: break;
            }
        }
        return summaryChanged;
    }
};

}  // namespace

OwnershipInfo analyzeOwnership(const IrModule& mod) {
    Analyzer a(mod);

    // interprocedural fixpoint: iterate until no summary changes
    bool changed = true;
    int rounds = 0;
    while (changed && rounds < 32) {
        changed = false;
        ++rounds;
        for (size_t fi = 0; fi < mod.functions.size(); ++fi) {
            for (auto& e : a.siteEscape[fi]) (void)e;   // reasons re-derived below
            a.siteEscape[fi].clear();
            if (a.analyzeFn((int)fi)) changed = true;
        }
    }

    OwnershipInfo info;
    for (size_t fi = 0; fi < mod.functions.size(); ++fi) {
        const IrFunction& fn = mod.functions[fi];
        for (int pc = 0; pc < (int)fn.code.size(); ++pc) {
            const Inst& in = fn.code[pc];
            bool cvAlloc = in.op == Op::CopyVal && in.c == 1;
            if (!isAlloc(in.op) && !cvAlloc) continue;
            AllocSite s;
            s.fn = (int)fi;
            s.pc = pc;
            s.op = in.op;
            auto it = a.siteEscape[fi].find(pc);
            bool escapes = it != a.siteEscape[fi].end();
            // §15: value structs are copied BY VALUE, so being PASSED to a callee /
            // native does not make the OBJECT escape (the callee copies it). Only a
            // REAL escape — returned / thrown / stored / captured — outlives the
            // frame; everything else is scope-owned (arena, bulk-freed at exit).
            bool valueStruct = (in.op == Op::NewObject && in.sym && in.sym->isValue) || cvAlloc;
            bool realEscape = escapes && it->second.rfind("passed t", 0) != 0;
            bool own = valueStruct ? !realEscape : !escapes;
            if (own) info.scopeOwned.insert({(int)fi, pc});
            else { s.escapes = true; s.reason = it->second; }
            info.sites.push_back(std::move(s));
        }
    }
    return info;
}

std::string ownershipReport(const IrModule& mod, const OwnershipInfo& info) {
    auto opStr = [](Op op) {
        switch (op) {
            case Op::NewObject:    return "NewObject";
            case Op::NewArray:     return "NewArray";
            case Op::NewArraySized:return "NewArraySized";
            case Op::NewMap:       return "NewMap";
            case Op::MakeRange:    return "MakeRange";
            case Op::MakeClosure:  return "MakeClosure";
            default:               return "?";
        }
    };
    std::string out;
    int lastFn = -1;
    for (const AllocSite& s : info.sites) {
        if (s.fn != lastFn) {
            out += mod.functions[s.fn].name + ":\n";
            lastFn = s.fn;
        }
        out += "  @" + std::to_string(s.pc) + " " + opStr(s.op) + " -> ";
        out += s.escapes ? ("ESCAPES (" + s.reason + ")") : "scope-owned";
        out += "\n";
    }
    int owned = info.owned(), total = info.total();
    int transferred = 0;
    std::unordered_map<std::string, int> byReason;
    for (const AllocSite& s : info.sites)
        if (s.escapes) {
            ++byReason[s.reason];
            if (s.reason == "returned to caller") ++transferred;
        }
    int refcount = total - owned - transferred;
    out += "----\n";
    out += std::to_string(total) + " allocation site(s): " + std::to_string(owned) +
           " scope-owned (free at scope exit), " + std::to_string(transferred) +
           " ownership-transferred by return (freed by the receiving scope, §15), " +
           std::to_string(refcount) + " refcount tier\n";
    if (total > 0)
        out += "deterministic (no-refcount) share: " +
               std::to_string((owned + transferred) * 100 / total) + "%\n";
    for (const auto& [reason, n] : byReason)
        out += "  " + std::to_string(n) + " x " + reason + "\n";
    return out;
}
