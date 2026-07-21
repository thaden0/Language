// refactor_1 session 05 (techdesign-05-llvmgen-split-fable.md): the internal
// header shared by LlvmGen.cpp / LlvmGenOps.cpp / LlvmGenGlue.cpp — the Gen
// lowering struct's declaration plus the shared file-static helpers.
#pragma once
#include "LlvmGen.hpp"
#include "Ownership.hpp"

#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

#include <cstring>
#include <functional>

using namespace llvm;

namespace llvmDetail {

// All callable members of a class, own decls first, then bases (dispatch
// checks in this order — first name match wins). Verbatim port of X64Gen's
// collectMembers (src/X64Gen.cpp:55-60).
inline void collectMembersLG(Symbol* cls, std::vector<const Stmt*>& out) {
    if (!cls || !cls->decl) return;
    for (const StmtPtr& m : cls->decl->body)
        if (m->kind == StmtKind::Member && m->callable) out.push_back(m.get());
    for (const TypeRefPtr& b : cls->decl->bases) collectMembersLG(b->resolvedSymbol, out);
}

struct Gen {
    const IrModule& mod;
    DiagnosticSink& sink;
    bool ok = true;
    bool targetWindows = false;   // Track 10 §7: reject sysThread*/sysChannel* here
    bool targetWasm = false;      // Track W hard-03: the two-tier capability gate

    LLVMContext ctx;
    std::unique_ptr<Module> module;
    Type* i64Ty = nullptr;
    Type* i32Ty = nullptr;
    Type* i8Ty = nullptr;
    Type* f64Ty = nullptr;
    Type* ptrTy = nullptr;
    Type* voidTy = nullptr;
    StructType* lvTy = nullptr;                  // {i64 tag, i64 payload} — doc-2 §2.3
    StructType* classInfoTy = nullptr;           // the FULL LvClassInfo (lv_abi.h)
    std::vector<Function*> fns;                  // per IR function (or null)
    std::vector<bool> reachable;
    std::vector<bool> userReach;                 // Track W hard-03 tier 1 (⊆ reachable)
    std::unordered_map<std::string, GlobalVariable*> strLiterals;
    std::unordered_map<Symbol*, int> clsIds;     // class symbol -> runtime classId (>=1)
    GlobalVariable* globalsArray = nullptr;      // [nglobals x LV] for Load/StoreGlobal
    Function* dispatchFn = nullptr;              // the LvDispatchFn trampoline
    Function* spawnCheckFn = nullptr;            // bug #35: lv_spawn_global_check
    // §9b/A-M7, doc-2 §2.10: the two inline hot-state globals the runtime
    // exports (extern, no initializer here — the linker resolves them
    // against lv_runtime.c's definitions). Load-only fast paths for
    // lvrt_throwing() and the lvrt_arena_save/lvrt_arena_restore pair.
    GlobalVariable* gThrowing = nullptr;
    GlobalVariable* gArenaCursor = nullptr;

    // Reachability byproducts (X64Gen-parity dispatch model, X64Gen.cpp:3690-3760):
    std::vector<Symbol*> instClasses;    // classes some NewObject instantiates
    std::vector<Symbol*> callmClasses;   // inst + Array/Map + primitive masks
    std::vector<int> closureFns_;        // MakeClosure targets
    bool usesColl = false;

    // runtime entry points — the normative contract is runtime/lv_abi.h; every
    // signature below mirrors it (values cross as LvValue*, out-param first).
    FunctionCallee rtRetain, rtRelease, rtVfree, rtCopyval,
        rtObjNew, rtClosureNew, rtCaptureSet, rtCaptureGet,
        rtGetfield, rtSetfield, rtWeakLoad, rtWeakStore, rtWeakGetfield, rtWeakSetfield,
        rtTruth, rtNot, rtNeg, rtArith, rtOpm,
        rtPrintVal, rtPrintNl, rtSysWrite, rtReadLine,
        rtGetm, rtSetm, rtRegister,
        rtValueHasPromise, rtRegisterSpawnCheck,   // bug #35 spawn-global guard
        rtArrNew, rtArrAppend, rtArrFill, rtArrConcatall, rtMapNew, rtMapWith, rtIdxGet, rtIdxSet, rtKeyEq,
        rtCanonEq,   // struct-equality design §8: the canonical float relation

        rtStrEq, rtStrSubstr, rtStrIndexof, rtStrToint, rtStrTofloat, rtStrTrim, rtStrCase,
        rtStrByteat, rtStrFrombyte,
        // Track 03 §1/§3 (deferal-char-block-abi.md): char UTF-8 en/decode +
        // Block byte-buffer natives (LV_CHAR/LV_BLOCK ABI addendum).
        rtCharToStr, rtStrAt, rtStrChars,
        rtBlockNew, rtBlockFromstr, rtBlockByteat, rtBlockSetbyte, rtBlockSlice,
        rtBlockTostring, rtBlockInt32at, rtBlockSetint32, rtBlockInt64at, rtBlockSetint64,
        rtBlockFill, rtBlockBlit, rtBlockEquals, rtBlockMismatch,
        rtIntToStr, rtRaiseOob, rtRaise, rtRaiseStr,
        // §2.8 exception state (A-M4): pending-throw model, no landingpads.
        rtThrowing, rtThrown, rtThrowSet, rtCatchBind, rtIssub, rtUncaught,
        // §2.7 system natives + §2.9 event loop (A-M5).
        rtSysRead, rtSysOpen, rtSysClose, rtSysStat, rtSysMkdir,
        rtSysRemove, rtSysRename, rtSysListDir,
        rtSysArgs, rtSysNow, rtSysMonotonic,
        rtSysReadBlock, rtSysWriteBlock, rtSysSendBlock, rtSysRecvBlock,   // Track 03 M4
        rtSysTermRaw, rtSysTermRestore,
        rtSysWinSize, rtSysTermIsRaw,     // terminal-floor.md §2
        rtSysSignalOpen, rtSysSignalNext, rtSysSignalClose,   // terminal-floor.md §3
        rtSysSetExitCode, rtSysExit,      // exit-codes.md §3.1
        rtSysTcpConnect, rtSysTcpListen, rtSysTcpListenReuse, rtSysCpuCount,
        rtSysThreadTransfer, rtSysThreadStart, rtSysThreadResult,
        rtSysChannelNew, rtSysChannelSend, rtSysChannelReceive, rtSysChannelClose,
        rtSysAccept, rtSysSend, rtSysRecv,
        rtSysTcpConnectNb, rtSysConnectResult,   // Track 08 F5 connect floor
        rtSysSocketBuffer,   // LA-29: advisory SO_SNDBUF/SO_RCVBUF sizing
        // G-LANG-2 process floor (techdesign-spawn-llvm.md §5)
        rtSysSpawn, rtSysPidfdOpen, rtSysReap, rtSysKill,
        // G-LANG-2 terminal half: pty floor (designs/pty/ 02 §3)
        rtSysPtySpawn, rtSysPtyResize,
        // LA-2 (techdesign-tls-crypto.md §5.2): TLS/crypto natives + sysRandom leg
        rtSysTlsConnect, rtSysTlsAccept, rtSysTlsHandshake, rtSysTlsError,
        rtSysTlsAlpn, rtSysTlsVersion, rtSysRsaEncrypt, rtSysRandom, rtSysEnv,
        rtSysTimerStart, rtSysTimerCancel, rtSysWatch, rtSysWatchWrite, rtSysUnwatch,
        // LA-30 B2 (doc 06 §4): the sysTask* floor
        rtSysTaskRun, rtSysTaskCancel, rtSysTaskShield, rtSysTaskJoinAll, rtSysAwaitAny2,
        rtLoopHasWork, rtLoopStep, rtAwait, rtPeakBytes, rtLiveBytes, rtThreadStats, rtToString,
        // Track W hard-03 (doc 02 §5 tier 2): the wasm capability-gate trap stub
        rtUnsupported,
        // Track W hard-06 (doc 05 §2-§4): the JS/DOM host-bridge seam
        rtHostCall, rtHostCloReg, rtHostEcho,
        // §9 A-M6 arena tier: the raw allocator + tier-blind helpers already in
        // lv_abi.h (§2.5/§2.6), reused (not extended) to build the two arena-
        // tier constructors below without touching the frozen ABI.
        rtHalloc, rtArenaSave, rtArenaRestore, rtIsvalueclass, rtFieldcount;

    // §9 A-M6: module-level helpers, built once (emitArenaHelpers), that mirror
    // lvrt_obj_new/lvrt_copyval (lv_runtime.c) but allocate on LV_TIER_ARENA —
    // used only at CopyVal(c==1)/NewObject(isValue) sites the ownership
    // analysis (Ownership.hpp) proves scope-owned. Never touches lv_abi.h.
    Function* objNewArenaFn = nullptr;
    Function* copyValArenaFn = nullptr;
    OwnershipInfo ownership;

    Gen(const IrModule& m, DiagnosticSink& s);

    Constant* i64C(int64_t v) { return ConstantInt::get(i64Ty, (uint64_t)v); }
    Constant* i32C(int32_t v) { return ConstantInt::get(i32Ty, (uint32_t)v); }
    Constant* nullPtr() { return ConstantPointerNull::get(cast<PointerType>(ptrTy)); }

    int classIdOf(Symbol* cls);
    int classIdByName(const char* name);
    Symbol* classByName(const char* name);

    std::vector<std::string> fieldKeysOf(Symbol* cls) const;

    // A private, constant, NUL-terminated C string; returns its address (an
    // opaque ptr constant usable directly in an aggregate initializer or as a
    // call argument). Deduped by content — the runtime's key comparison is
    // pointer-first with a content fallback, so this dedup is the fast path,
    // not a correctness requirement (lv_abi.h key-comparison ruling).
    std::unordered_map<std::string, GlobalVariable*> cstrCache;
    Constant* cstr(const std::string& s);

    void fail(const std::string& what);

    llvm::Value* stringGlobal(IRBuilder<>& b, const std::string& s);

    void computeReachable();

    bool run();

    void collectBases(Symbol* cls, std::vector<Symbol*>& out);

    void emitDispatchTrampoline();
    void emitSpawnGlobalCheck();
    void emitZeroSlots(IRBuilder<>& b, Function* fn, llvm::Value* slotsBase,
                       llvm::Value* nslots);
    llvm::Value* emitAllocPackedArena(IRBuilder<>& b, Function* fn, llvm::Value* word0,
                                      llvm::Value* nslots);
    void emitArenaHelpers();
    void emitColumnarDescriptors();
    void emitClassRegistration(IRBuilder<>& b);

    // ---- per-value-slot helpers (an LV* is any pointer to a {tag,payload}
    // pair — a register alloca, an object's field slot, or the ARC scratch
    // cell; GEP semantics don't care how the pointer was computed). ----
    llvm::Value* tagGEP(IRBuilder<>& b, llvm::Value* lv) { return b.CreateStructGEP(lvTy, lv, 0); }
    llvm::Value* payGEP(IRBuilder<>& b, llvm::Value* lv) { return b.CreateStructGEP(lvTy, lv, 1); }
    llvm::Value* loadTag(IRBuilder<>& b, llvm::Value* lv) { return b.CreateLoad(i64Ty, tagGEP(b, lv)); }
    llvm::Value* loadPay(IRBuilder<>& b, llvm::Value* lv) { return b.CreateLoad(i64Ty, payGEP(b, lv)); }
    void storeTP(IRBuilder<>& b, llvm::Value* lv, llvm::Value* tag, llvm::Value* pay) {
        b.CreateStore(tag, tagGEP(b, lv));
        b.CreateStore(pay, payGEP(b, lv));
    }
    void storeVoid(IRBuilder<>& b, llvm::Value* lv) { storeTP(b, lv, i64C(0), i64C(0)); }
    void copyLV(IRBuilder<>& b, llvm::Value* dst, llvm::Value* src) {
        storeTP(b, dst, loadTag(b, src), loadPay(b, src));
    }
    // address of byte offset `off` past a payload pointer held in `lv`
    llvm::Value* payAddr(IRBuilder<>& b, llvm::Value* lv, llvm::Value* off) {
        llvm::Value* p = b.CreateIntToPtr(loadPay(b, lv), ptrTy);
        return b.CreateGEP(i8Ty, p, off);
    }

    // Free an rc-0 heap string temp: retain (0->1) + release (1->0 frees).
    // Literal / owned / out-of-region values pass through both untouched —
    // the runtime-side equivalent of X64Gen::emitStrTempFree's guards.
    void emitStrTempFree(IRBuilder<>& b, llvm::Value* lv) {
        b.CreateCall(rtRetain, {lv});
        b.CreateCall(rtRelease, {lv});
    }

    void genFunction(int index);
};

}  // namespace llvmDetail
