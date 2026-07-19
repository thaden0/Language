#pragma once
#include "Ast.hpp"
#include "Diagnostic.hpp"
#include "RuntimeValue.hpp"
#include "Symbols.hpp"
#include <map>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// A throwaway tree-walking interpreter: the *semantics oracle*. It runs
// type-checked programs directly over the AST to confirm the runtime model
// behaves as designed. It is not the shipped interpreter (§17) and is not
// meant to be fast — just correct. Covers the core language; the standard
// library (arrays, streams, fork/await) is out of scope here.

// Comptime evaluation gates (§16.5 Layer C): hermetic (the sys* floor is
// denied, stdout/stderr writes excepted) and step-bounded (a runaway comptime
// loop is a compile error, not a hang).
struct ComptimeOptions {
    bool hermetic = true;
    long long stepBudget = 100000000;   // ~100M steps; --comptime-budget overrides
    // Layer D (metaprog Phase 4 §4): the reentrant-rule re-trigger cap. A real
    // chain converges in 1-2 rounds; 8 catches a runaway without masking a slow-
    // but-legitimate fixpoint. `--reentrant-budget` overrides.
    int reentrantRounds = 8;
    // Item Q (techdesign-target-predicate.md): the `--target` cross triple.
    // Empty = host build; sources `target::os`/`target::arch`/`target::triple`.
    std::string targetTriple;
};

// LA-20: `std::import(path)` — a declared build input read at compile time.
// One record per distinct absolute path actually read, in first-read order
// (deterministic — the fold walk itself is deterministic), for `--assets`.
struct ImportedAsset {
    std::string rel;        // the path as written / declared ("views/index.html")
    std::string abs;        // what was actually read
    std::string moduleId;   // "" = root project / single file
    size_t bytes = 0;
};

// Wires the comptime `import()` intrinsic to its two build shapes (LA-20 §5):
// plan builds resolve `rel` against the importing file's own module's
// declared asset table; single-file builds resolve it against `rootDir`
// (the source file's own directory). Set once by the RuleEngine before
// running the rule stage, with `currentModule` updated per comptime root.
struct ImportContext {
    // Plan builds: moduleId -> (rel -> abs). Single-file: empty map + rootDir set.
    const std::map<std::string, std::map<std::string, std::string>>* assets = nullptr;
    std::string rootDir;                       // single-file mode only
    const Stmt* importFn = nullptr;            // the prelude std::import decl (identity)
    // Which module the current comptime root's FILE belongs to — set by the
    // RuleEngine before each comptime evaluation (fileOf on the root's span).
    std::string currentModule;
};

// F4: layer-clean seam from the comptime oracle back to the front-end parser.
// The evaluator knows only that meta::parse* maps a heap string + call span to
// an opaque Value; RuleEngine owns parsing, diagnostics, and AST storage.
using ComptimeParseHook = std::function<Value(bool, const std::string&, SourceSpan,
                                               std::string&)>;

class Evaluator {
public:
    Evaluator(const Sema& sema, DiagnosticSink& sink) : sema_(sema), sink_(sink) {}

    // Runs the program's top-level statements; returns captured console output.
    std::string run(Program& program);

    // Process exit code after run() (designs/exit-codes.md §4): env.setExitCode's
    // value, or the env.exit code, or 1 for an uncaught throw (gap b), else 0.
    // main.cpp returns this as the process status.
    int exitCode() const { return exitCode_; }

    // Execute the prelude's top-level variable initializers (namespace globals
    // like std::read). Call once before run().
    void initGlobals(Program& prelude);

    // --- comptime driver (the rule stage runs the oracle pre-lowering) ---
    void setComptime(const ComptimeOptions& o);
    // Evaluate one expression under the comptime gates; failures (throw,
    // budget, hermeticity) return through err/failed, never crash.
    Value evalComptime(Expr* e, std::string& err, bool& failed);
    // Same, with a `where`-clause-shaped env of plain-name locals seeded into
    // the pushed frame (§16.5 Phase 3 §4) — ordinary comptime code with extra
    // names in scope, not a template/hole position.
    Value evalComptime(Expr* e, const std::unordered_map<std::string, Value>& locals,
                       std::string& err, bool& failed);
    // Execute a procedural macro's body as an isolated comptime call frame.
    Value evalComptimeBody(std::string_view macroName, std::string_view paramName, Stmt* body,
                           const std::unordered_map<std::string, Value>& locals,
                           std::string& err, bool& failed);
    // Make a folded comptime value visible to later comptime sites by name.
    void defineGlobal(const std::string& name, const Value& v);

    // LA-20: wire the comptime `import()` intrinsic (engine-owned; set once
    // before running the rule stage). `setImportModule` updates just the
    // per-comptime-root module, cheap enough to call before every root.
    void setImportContext(ImportContext ctx) { importCtx_ = std::move(ctx); }
    void setImportModule(const std::string& moduleId) { importCtx_.currentModule = moduleId; }
    void setComptimeParseHook(ComptimeParseHook hook) { parseHook_ = std::move(hook); }
    const std::vector<ImportedAsset>& importedAssets() const { return importedAssets_; }
    SourceSpan comptimeReturnSpan() const { return comptimeReturnSpan_; }

private:
    const Sema& sema_;
    DiagnosticSink& sink_;
    std::string out_;

    // execution state
    std::unordered_map<std::string, Value> globals_;   // prelude namespace globals
    std::vector<std::unordered_map<std::string_view, Value>> env_;
    std::shared_ptr<Object> thisObj_;
    Symbol* thisClass_ = nullptr;
    // bug.md #32 M2: the enclosing namespace of the currently-executing FREE
    // function (null outside one), set from Stmt::enclosingNs in callFunction.
    // Lets ctorTarget resolve a bare `Class(...)` inside unchecked prelude
    // code against its own namespace, mirroring the qualified `ns::Class(...)`
    // descent it already does.
    Symbol* curNamespace_ = nullptr;
    Value primThis_;                 // `this` when the receiver is a primitive value
    bool hasPrimThis_ = false;
    bool inCtor_ = false;
    bool returning_ = false;
    Value returnValue_;
    // techdesign-02 F1: break/continue signals, confined to call frames the
    // same way returning_ is (the checker statically prevents one crossing a
    // call; these are belt-and-braces).
    bool breaking_ = false;
    bool continuing_ = false;
    // techdesign-labeled-break-continue.md F4: null = unlabeled = innermost
    // loop (no behavior change). Non-null = the checker-resolved target loop
    // Stmt (Stmt::labelTarget) a labeled break/continue must unwind to; each
    // loop's consume-pair tests "is this mine?" before clearing the flag,
    // letting a foreign target propagate outward unchanged. Frame-confined
    // exactly like breaking_/continuing_ (checker statically prevents a
    // labeled break/continue from crossing a call; belt-and-braces).
    const Stmt* breakTarget_ = nullptr;
    const Stmt* continueTarget_ = nullptr;
    // Exception signal: set by `throw`, cleared by a matching catch. Unlike
    // returning_, it is deliberately NOT restored by call frames — it bubbles.
    bool throwing_ = false;
    Value thrownValue_;
    // Exit signal (designs/exit-codes.md §4): env.exit sets exiting_ (and rides
    // the throwing_ unwind so all `if (throwing_)` guards stop cleanly), but
    // exiting_ marks it a NON-error termination — no "Uncaught" report, and the
    // recorded code (not 1) is threaded out. exitCode_ is computed at run() end.
    bool exiting_ = false;
    int exitCode_ = 0;
    std::string rawField_;   // field being read/written raw inside its own accessor

    // statements / expressions
    void exec(Stmt* s);
    Value eval(Expr* e);
    Value evalCall(Expr* e);
    Value evalBinary(Expr* e);
    Value evalAssign(Expr* lhs, const Value& v);

    // objects
    Value combine(TokenKind op, const Value& l, const Value& r, const Stmt* resolved);
    Value construct(Symbol* cls, const std::string& label, std::vector<Value>& args,
                    const Stmt* ctorOverride = nullptr);
    void runCtor(Symbol* cls, const std::string& label, std::vector<Value>& args,
                 std::shared_ptr<Object> self, const Stmt* ctorOverride = nullptr);
    void initFields(Object* obj, Symbol* cls);
    Value callFunction(const Stmt* fn, std::vector<Value>& args,
                       std::shared_ptr<Object> self, Symbol* selfClass);
    Value callClosure(std::shared_ptr<Closure> cl, std::vector<Value>& args);
    // Call a method whose receiver is a primitive value (object mask).
    Value callPrimMethod(const Stmt* fn, const Value& self, std::vector<Value>& args,
                         std::string_view className);
    bool nativeCall(const std::string& cls, const std::string& method,
                    const Value& self, std::vector<Value>& args, Value& out);

    // member access
    bool memberTarget(Expr* e, std::shared_ptr<Object>& obj,
                      std::string& name, std::string& source);
    Value memberRead(std::shared_ptr<Object> obj, const std::string& name,
                     const std::string& source);
    void memberWrite(std::shared_ptr<Object> obj, const std::string& name,
                     const std::string& source, const Value& v);

    // Raise a language-level RuntimeException (runtime failures are loud and
    // catchable, not silent voids). Returns void for use in value positions.
    Value throwRuntime(const std::string& message);
    // B2 (doc 06 §2): same machinery, named class — the cancellation carrier.
    Value throwClass(const std::string& clsName, const std::string& message);

    // --- LA-30 tasks (techdesign-03): fiber-hosted interpretation ----------
    // Under LANG_TASKS the program body is task 0 and every loop-dispatched
    // callback runs in its own task; `await` parks instead of pumping. The
    // Evaluator object is SHARED by every task's recursion, so:
    //
    // S3/G7 — the volatile engine-state set. While a task is parked, other
    // tasks overwrite these members (call sites save/restore them LIFO on the
    // C++ stack, which the pump's strictly-nested dispatch preserved but fiber
    // interleaving does not). The park site is the ONLY point a fiber loses
    // the engine, so saving this set into the fiber's own frame around
    // lv_task_park_on — and starting fresh tasks pristine — restores the
    // invariant: park-time view == resume-time view. Members NOT in the set
    // are heap-shaped program state shared by design (globals_, out_, the
    // RuntimeLoop registry), comptime-only (G14: the scheduler is unreachable
    // from resolver-phase evaluation), or run()-lifetime scalars; the audit
    // table lives in designs/*/techdesign-03-interpreter-legs.md §3.
    struct TaskState {
        std::vector<std::unordered_map<std::string_view, Value>> env;
        std::shared_ptr<Object> thisObj;
        Symbol* thisClass = nullptr;
        Symbol* curNamespace = nullptr;
        Value primThis;
        bool hasPrimThis = false, inCtor = false;
        bool returning = false, breaking = false, continuing = false;
        const Stmt* breakTarget = nullptr;
        const Stmt* continueTarget = nullptr;
        Value returnValue, thrownValue;
        std::string rawField;
    };
    TaskState saveTaskState();               // move out + reset to pristine
    void restoreTaskState(TaskState&& s);
    // G11: a task must never return to the scheduler (or park) with the throw
    // flag set. The FIRST terminating task (uncaught throw, or env.exit riding
    // throwing_) stashes here and clears the flag; taskLoopStep gates on the
    // stash (the pump drain's `while (!throwing_ ...)` gate), and run()
    // re-raises after lv_sched_run returns so the shared uncaught/exit-code
    // tail reports exactly what the pump-mode unwind would have.
    bool taskTermStashed_ = false;
    bool taskTermExiting_ = false;
    Value taskTermThrown_;
    bool taskReportedUncaught_ = false;
    void taskCaptureTermination();
    // The top-level items loop (namespace-global init + item execution +
    // inline uncaught reporting), shared verbatim by both modes; task 0's
    // body under LANG_TASKS. Returns whether it reported an uncaught throw.
    bool execTopLevel(Program& program);
    // Substrate hooks (C function pointers — captureless; these engines are
    // single-threaded, so one run()-scoped instance pointer suffices).
    static Evaluator* gTaskEval_;
    static int  taskPollPromise(const void* obj);   // G15: bool immediates only
    static int  taskLoopStep();                     // one nextBatch, jobs task-spawned
    static int  taskThrowProbe();                   // G11 debug assert probe
    static void taskRunProgram(void* selfp, void* prog);   // task 0 body
    static void taskRunJob(void* ctxp, void*);              // dispatched-callback body
    static void taskRunGroupBody(void* ctxp, void*);        // B2: group-child body (0-arg)
    // B2 (doc 06 §4): the engine-level task natives (sysTaskRun/sysTaskJoinAll/
    // sysAwaitAny2 — they spawn closures or park with engine-state discipline;
    // the id-only pair sysTaskCancel/sysTaskShield lives in RuntimeNatives).
    bool taskNative(const std::string& name, std::vector<Value>& args, Value& out);

    // Console emission: captured (out_) normally; real stdout under comptime.
    void emitConsole(const std::string& text);

    // comptime state (§16.5): budget countdown + hermeticity flag
    bool comptime_ = false;
    bool hermetic_ = true;
    long long steps_ = 0;
    bool budgetExhausted_ = false;
    // Item Q: the reserved comptime namespace `target` — derived once by
    // setComptime from ComptimeOptions.targetTriple (host fallback).
    std::string targetOs_;
    std::string targetArch_;
    std::string targetTriple_;
    bool spendStep();
    // Shared post-eval tail for both evalComptime overloads (budget/throw
    // reporting) — identical behavior, factored to avoid duplicating it.
    Value finishComptime(Value v, std::string& err, bool& failed);

    // LA-20: the comptime `import()` intercept (see setImportContext above).
    // Intercepted in callFunction by Stmt* identity, before the body ever
    // runs, so a user's own `import` (a different decl) is never hijacked.
    ImportContext importCtx_;
    ComptimeParseHook parseHook_;
    std::string_view comptimeMacroName_;
    std::string_view comptimeMacroParam_;
    Stmt* comptimeMacroBody_ = nullptr;
    Value callComptimeMacro(const Value& arg);
    SourceSpan lastReturnSpan_{};
    SourceSpan comptimeReturnSpan_{};
    std::map<std::string, std::string> importCache_;     // abs path -> content
    std::vector<ImportedAsset> importedAssets_;          // in first-read order
    Value comptimeImport(std::vector<Value>& args);

    // lookup helpers
    Value* localLookup(std::string_view name);
    bool ctorTarget(Expr* callee, Symbol*& cls, std::string& label);
    const Stmt* resolveFunction(Expr* callee);
    bool classHasMember(Symbol* cls, std::string_view name);
    const Stmt* findMethod(Symbol* cls, const std::string& name, int argc = -1);
    bool isMethodOf(Symbol* cls, const Stmt* decl);
    const Stmt* findAccessor(Symbol* cls, const std::string& name, bool wantGet);
    std::string keyFor(Symbol* cls, const std::string& name, const std::string& source);
    bool isSubclassOrSelf(Symbol* a, Symbol* b);
    Symbol* classOfValue(const Value& v);   // the class backing a value (for [] dispatch)
    bool valueIsType(const Value& v, const TypeRef* t);   // `is` classification
    bool matchesValue(const Value& subj, Expr* pattern);  // match value/range arm
};
