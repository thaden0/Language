#pragma once
#include "Diagnostic.hpp"
#include "Ir.hpp"
#include "Symbols.hpp"
#include <functional>
#include <map>
#include <string>
#include <vector>

// The native (AOT) backend, first rung: translate the IR to freestanding C++
// and let the system compiler produce the binary. LLVM-API emission replaces
// this when LLVM headers are available; the IR consumed is identical.
//
// Coverage: the scalar core (ints/bools/floats/strings, control flow, static
// calls, console I/O). Ops outside coverage fail with a diagnostic.
class CGen {
public:
    CGen(const IrModule& mod, DiagnosticSink& sink) : mod_(mod), sink_(sink) {}

    // Returns the generated translation unit, or "" on failure.
    std::string generate();

private:
    const IrModule& mod_;
    DiagnosticSink& sink_;
    bool ok_ = true;

    std::string genFunction(int index);
    std::string genDispatchers(const std::vector<Symbol*>& instClasses);
    std::string genClosureDispatch();
    std::string genIsSub();
    std::string genFieldSlot();
    std::string genFieldCount();
    std::string genIsValueClass();
    std::vector<std::string> fieldKeys(Symbol* cls);
    std::string genClsName();
    std::string genCallM(const std::vector<Symbol*>& instClasses);
    std::vector<Symbol*> collClasses_;
    // bug.md #27/#28: classes reached ONLY through an unresolved by-name CallDyn
    // (a bare self-call inside an unchecked prelude body). They must join the
    // callm dispatch table even though nothing NewObject-constructs them here.
    std::vector<Symbol*> byNameClasses_;
    std::vector<bool> reachable_;
    int clsId(Symbol* cls);
    int rangeId();
    int pairId();
    std::vector<int> closureFns_;
    std::map<Symbol*, int> clsId_;
    static std::string escapeString(const std::string& s);
};
