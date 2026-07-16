#pragma once
#include "Ast.hpp"
#include "Diagnostic.hpp"
#include "Ir.hpp"
#include "Symbols.hpp"
#include <set>

// Lowers a checked Program (plus the prelude) to the bytecode IR. Constructs
// outside coverage produce "IR: not yet lowerable: ..." and fail the lowering.
class Lowerer {
public:
    Lowerer(const Sema& sema, DiagnosticSink& sink) : sema_(sema), sink_(sink) {}

    // techdesign-columnar-arrays.md: compile in columnar mode (staged --columnar
    // flag). Enables the ColGet fusion peephole and the gathered-element
    // ownership discipline; recorded into IrModule::columnar for codegen.
    void setColumnar(bool c) { columnar_ = c; }

    // `prelude` is the resolver's prelude program (its in-language stdlib
    // bodies are lowered like any other code).
    bool lower(Program& program, Program& prelude, IrModule& out);

private:
    const Sema& sema_;
    DiagnosticSink& sink_;
    IrModule* mod_ = nullptr;
    bool columnar_ = false;
    bool ok_ = true;
    // The prelude Console class's member decls: calls the checker resolved to
    // one of these lower to Print even through an aliased receiver.
    std::set<const Stmt*> consoleMembers_;

    // one entry per function to lower
    struct Pending {
        const Stmt* decl;
        Symbol* cls;          // enclosing class, or null for free functions
        int index;
    };
    std::vector<Pending> pending_;

    // per-function lowering context
    int cur_ = -1;                                  // index into mod_->functions
    Symbol* curClass_ = nullptr;                    // enclosing class of the body
    // bug.md #32 M2: the enclosing namespace of the free function currently
    // being lowered (from Stmt::enclosingNs; null outside one/inside a
    // method). Lets the ctor fallback resolve a bare `Class(...)` inside
    // unchecked prelude code against its own namespace.
    Symbol* curNamespace_ = nullptr;
    std::string curAccessor_;                       // accessor's own field (raw access)
    bool curIsCtor_ = false;
    bool curIsLambda_ = false;
    std::vector<std::unordered_map<std::string_view, int>> scopes_;
    // techdesign-02 F1: break/continue jump-patch lists, one per enclosing
    // loop. Break/Continue lower to `emit(Op::Jump, 0)`, recorded here and
    // patched once the target pc is known (loop end / continue point).
    // techdesign-02 F3: `usingsFloor` is usings_.size() at this loop's entry
    // (before its body lowers) — every using at-or-above this floor was
    // declared INSIDE this loop, so an unlabeled break/continue here crosses
    // exactly that suffix and none of the enclosing scope's usings.
    struct LoopCtx { std::vector<int> breakJumps, continueJumps; size_t usingsFloor = 0; };
    std::vector<LoopCtx> loops_;
    // techdesign-02 F3: one entry per currently-active `using` local, flat
    // across the whole function (nesting order == vector order, regardless of
    // block boundaries — see the design doc's cleanup-group layout). A
    // Return/Break/Continue statement records its jump into usings_.back()'s
    // matching list when non-empty; a using's own cleanup-group stub, if it
    // needs to keep closing outward, records ITS jump into usings_[i-1]'s
    // list the same way — so each list accumulates from both statement sites
    // and inner stubs before the owning group is ever emitted.
    struct UsingCtx {
        int slotReg = 0;
        const Stmt* closeDecl = nullptr;
        int rangeStart = 0;              // pc right after init: handler range open
        std::vector<int> retJumps, brkJumps, cntJumps;
        bool needRet = false, needBrk = false, needCnt = false;
    };
    std::vector<UsingCtx> usings_;
    // Per-function: the register every cross-using return's value funnels
    // into before chaining outward, and whether this function's returns are
    // void-shaped (both lazily fixed by the FIRST cross-using return in the
    // function; every return in one function is consistently the same shape).
    int chainRetReg_ = -1;
    bool chainRetIsVoid_ = false;
    void emitCloseCall(int slotReg, const Stmt* closeDecl);
    // techdesign-02 F3: emit the cleanup groups for every UsingCtx pushed
    // since `watermark` (a block's direct-child usings), innermost first, per
    // the design doc's normative layout — called from the Block case only
    // when it actually pushed usings.
    void lowerUsingCleanupGroups(size_t watermark);
    // Block-scoped import overlays (bug.md #1): while lowering a block that
    // carries a `uses`/`use` importScope (bug.md #8's model), that scope is
    // pushed here so namespace re-derivation sees block imports exactly like
    // the checker does (nearest block first, then the file overlay, then
    // global).
    std::vector<Scope*> blockImportScopes_;
    // The namespace symbol `name` resolves to at `offset`, through block
    // overlays -> file overlay -> global; null if unbound or shadowed by a
    // non-namespace binding (the refuse-to-guess rule).
    Symbol* namespaceSym(std::string_view name, uint32_t offset);
    // bug #37/#46: resolve a namespace qualifier that may be a nested chain
    // (`A::B::…`), returning the innermost namespace Symbol (or null).
    Symbol* namespaceChainSym(const Expr* e, uint32_t offset);
    // bug.md #2: the user program's bare top-level Vars — registered as
    // globals up front (functions lower separately from @main and cannot see
    // @main locals), but INITIALIZED in @main, in statement order.
    std::set<const Stmt*> topLevelGlobals_;
    // known_bugs #75/#76/#79/#80: top-level globals of a pure-default type
    // (primitive/string/Array/Map/union/None) whose auto-constructed value is
    // emitted once in @ginit BEFORE any namespace-scoped initializer, so a
    // namespace initializer can mutate a real slot. A BARE such global must NOT
    // be re-defaulted in @main (that would clobber the mutation); @main skips it.
    std::set<const Stmt*> preDefaultedGlobals_;

    IrFunction& F() { return mod_->functions[cur_]; }
    void fail(SourceSpan span, const std::string& what);
    int newReg() { return F().nregs++; }
    int emit(Op op, int a = 0, int b = 0, int c = 0, int d = 0);
    Inst& last() { return F().code.back(); }
    int addConst(Value v);
    int* findLocal(std::string_view name);
    // The register holding the receiver in the CURRENT function: inside a
    // lambda it is the captured-`this` local (closure conversion snapshots the
    // receiver under the keyword name, which no user local can shadow); in a
    // member body it is r0. -1 when there is no receiver at all.
    int thisReg();
    bool classHasMember(Symbol* cls, std::string_view name);
    bool isBaseOrSelf(Symbol* derived, Symbol* base);
    // §7 compile-time field offsets: the packed slot of a plain field, and the
    // slot usable as a fixed offset for a receiver statically typed `cls`.
    int fieldSlotOf(Symbol* cls, std::string_view key);
    int packedSlot(Symbol* cls, std::string_view key, bool requireNoAccessor);

    // collection & synthesis
    void collect(std::vector<StmtPtr>& items);
    void collectClass(Stmt* cls);
    void collectBinds(Stmt* s);           // §12.5: register factory `bind`s (anywhere) as fns
    int synthesizeInit(Symbol* cls);
    Symbol* classToAutoConstruct(const TypeRef* t);   // §3 bare-decl class, or null
    const Stmt* nullaryCtor(Symbol* cls);
    void lowerPending(const Pending& p);

    // statements / expressions
    void lowerStmt(Stmt* s);
    int lowerExpr(Expr* e);
    void lowerAssign(Expr* lhs, int valueReg, const Expr* rhs = nullptr);
    bool moveRecvClear_ = false;   // set for `x = x.method(...)`: move the receiver in (COW)
    // Track 04 M4 (bug.md problem #2, StringBuilder): the field analogue of
    // moveRecvClear_ for `f = f.method(...)` where `f` is a plain (no
    // accessor) field of `this`. Reading a field always copies (retains) into
    // a fresh register — unlike a local, whose register IS its storage — so
    // clearing the read copy wouldn't drop the field's own reference.
    // Armed right before lowering the RHS call; captured-and-reset at the
    // TOP of the method-call lowering (same instant as moveRecvClear_'s own
    // capture, so a nested call reached while lowering an ARGUMENT can't see
    // it) and the actual void-write is emitted AFTER the call's arguments are
    // lowered — not as soon as the field is read. An argument that itself
    // reads the same field (`f.method(f.otherMethod())`) must see the real
    // value, not a premature clear; this was a real bug (wrong slice length
    // on LLVM, masked by lucky size_t-clamping on the other three engines —
    // see this track's Implementation Log), not a hypothetical one.
    struct PendingFieldClear { bool pending = false; std::string name; int slot = 0; };
    PendingFieldClear pendingFieldClear_;
    // §15: call-result regs holding a FRESH standalone value-struct copy (the
    // callee's return-site CopyVal escaped to the heap). Once consumed — copied
    // out at a binding / arg / return / statement site — the copy is dead and
    // Op::VFree reclaims it. Reg indices are per-function: cleared at each
    // function entry, saved/restored around nested lowering (lambdas, $init).
    std::set<int> freshStructRegs_;
    void noteFreshStructResult(const Expr* e, int reg);
    void maybeVFree(int reg);
    void emitArgCopies(const std::vector<int>& argRegs, const std::vector<ExprPtr>& args);
    int lowerCall(Expr* e);
    int emitIsType(int valueReg, const TypeRef* type);  // shared by `is` and `match`
    int lowerLambda(Expr* e);
    // For a member expression, compute the "name" or "Source::name" key and
    // the base register. Returns false if not a member-shaped target.
    bool memberKey(Expr* e, int& baseReg, std::string& key);
};
