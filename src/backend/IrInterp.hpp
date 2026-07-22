#pragma once
#include "ir/Ir.hpp"
#include "ir/Ownership.hpp"
#include "ir/MemVerify.hpp"
#include <memory>
#include <string>
#include <vector>

// Executes an IrModule. Shares the runtime value model and native cores with
// the tree-walking oracle, so observable behavior is identical.
class IrInterp {
public:
    explicit IrInterp(const IrModule& mod, const OwnershipInfo* own = nullptr,
                      bool memVerify = false)
        : mod_(mod), own_(own), memOn_(memVerify) {}

    // Runs the entry function; returns captured console output.
    std::string run();

    // Process exit code after run() (designs/exit-codes.md §4): env.exit /
    // env.setExitCode's code, or 1 for an uncaught throw (gap b), else 0.
    int exitCode() const { return exitCode_; }

    // Memory-safety verifier report (when constructed with memVerify = true).
    bool memVerifyOn() const { return memOn_; }
    std::string memReport() const { return mem_.report(); }

    // Ownership verification results (when constructed with OwnershipInfo):
    // every scope-owned allocation must be dead when its frame exits.
    size_t trackedAllocs() const { return tracked_; }
    const std::vector<std::string>& violations() const { return violations_; }

private:
    const IrModule& mod_;
    const OwnershipInfo* own_ = nullptr;
    bool memOn_ = false;
    MemVerifier mem_;
    std::string out_;
    std::vector<Value> globals_;
    size_t tracked_ = 0;
    std::vector<std::string> violations_;

    // exception signal (bubbles through frames until a handler matches)
    bool throwing_ = false;
    Value thrown_;
    // exit signal (designs/exit-codes.md §4): env.exit rides the throwing_
    // unwind but marks a clean, non-error termination (no "Uncaught" report).
    bool exiting_ = false;
    int exitCode_ = 0;

    Value call(int fnIndex, std::vector<Value> args);
    void invokeClosure(const Value& cb, const Value& arg);

    // bug #35 (reject route A): true (err set) if the spawn `body` closure's IR
    // function references a program global whose current value reaches a Promise
    // A-1 forbids across a thread boundary. Consulted at the sysThreadStart
    // call (spawn only), byte-identical to the oracle and LLVM guards. The
    // per-function global-slot sets (transitive over nested lambdas) are
    // computed once and cached — see computeFnGlobalRefs (Ir.hpp).
    bool spawnBodyGlobalPromise(const Value& body, std::string& err);
    std::vector<std::vector<int>> fnGlobalRefs_;
    bool fnGlobalRefsReady_ = false;

    // --- LA-30 tasks (techdesign-03): fiber-hosted interpretation ----------
    // S3/G7 audit result for this engine: the register file and pc are locals
    // of call()/frameExec (fiber-carried), globals_/out_ are heap-shaped
    // program state shared by design, and thrown_ is dead whenever throwing_
    // is false (G11 asserts the flag is clear at every park) — it is still
    // saved/restored at the park site so every mutable member is either
    // fiber-carried, shared-by-design, or park-restored. Termination stash:
    // same first-wins discipline as the oracle (see Eval.hpp).
    bool taskTermStashed_ = false;
    bool taskTermExiting_ = false;
    Value taskTermThrown_;
    void taskCaptureTermination();
    static IrInterp* gTaskInterp_;
    static int  taskPollPromise(const void* obj);   // G15: bool immediates only
    static int  taskLoopStep();                     // one nextBatch, jobs task-spawned
    static int  taskThrowProbe();                   // G11 debug assert probe
    static void taskRunProgram(void* selfp, void*); // task 0: ginit + entry
    static void taskRunJob(void* ctxp, void*);      // dispatched-callback body
    static void taskRunGroupBody(void* ctxp, void*);// B2: group-child body (0-arg)
    // B2 (doc 06 §4): engine-level task natives (see Eval.hpp's twin).
    bool taskNative(const std::string& name, std::vector<Value>& args, Value& out);
    Value frameExec(int fnIndex, std::vector<Value>& regs,
                    std::vector<std::pair<int, std::weak_ptr<void>>>* weaks);
    Value callDecl(const Stmt* decl, Symbol* cls, std::vector<Value> args,
                   bool cowReceiver = false);
    void raise(const std::string& message);
    void raiseClass(const std::string& clsName, const std::string& message);   // B2 carrier

    // dynamic-dispatch helpers over the semantic model
    Symbol* classOfValue(const Value& v) const;
    const Stmt* findMethodByName(Symbol* cls, const std::string& name, int argc = -1) const;
    const Stmt* findAccessor(Symbol* cls, const std::string& name, bool wantGet) const;
    bool isSubclassOrSelf(Symbol* a, Symbol* b) const;
    std::string keyFor(Symbol* cls, const std::string& nameOrQualified) const;

    Value getMember(const Value& obj, const std::string& key);
    void setMember(const Value& obj, const std::string& key, const Value& v);
    Value getIndex(const Value& base, const Value& idx);
    Value indexStore(const Value& base, const Value& idx, const Value& v);
    Value objectArith(TokenKind op, const Inst& in, const Value& l, const Value& r);
    Value iterLen(const Value& iter);
    Value iterAt(const Value& iter, long long idx);
};
