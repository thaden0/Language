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

namespace {

// Operator index table — must match X64Gen::opCode (src/X64Gen.cpp:20-38) and
// the runtime's LV_OP_* enum (runtime/lv_abi.h), which was ratified FROM it.
int opCode(TokenKind k) {
    switch (k) {
        case TokenKind::Plus:    return 0;
        case TokenKind::EqEq:    return 1;
        case TokenKind::BangEq:  return 2;
        case TokenKind::Minus:   return 3;
        case TokenKind::Star:    return 4;
        case TokenKind::Slash:   return 5;
        case TokenKind::Percent: return 6;
        case TokenKind::Lt:      return 7;
        case TokenKind::Gt:      return 8;
        case TokenKind::Le:      return 9;
        case TokenKind::Ge:      return 10;
        case TokenKind::Amp:     return 11;
        case TokenKind::Pipe:    return 12;
        case TokenKind::LtLt:    return 13;
        case TokenKind::GtGt:    return 14;
        case TokenKind::Caret:   return 15;
        default:                 return -1;
    }
}

// Ported from X64Gen.cpp:3042-3058 (the frozen reference backend's ARC-slot-
// write discipline — see docs/techdesign-portable-backend.md §4.1). X64Gen.cpp
// itself is never edited (frozen); this is an independent copy, not a shared
// extraction, per that doc's §4.2.
//
// destKind classifies an op by what it does to its OWN register `a`:
//   0 = `a` is not a destination for this op at all (a read, a jump target, or
//       an op that writes some OTHER memory location and handles its own
//       release-old/retain-new internally — SetMember, RawSet, StoreGlobal,
//       CaptureVar; VFree frees `a`'s value and clears the slot itself).
//   1 = writes a value the slot now OWNS -> the generic wrap releases the old
//       value and retains the new one (a fresh alloc/read/alias — the default
//       bucket, deliberately "everything else").
//   2 = writes a value ALREADY at +1 (a return-transfer) -> release-old only,
//       no retain (calls, string-concat Arith, MoveClear).
//
// DIVERGENCES from X64Gen's table, each forced by a ratified runtime-v2
// ownership ruling (lv_abi.h, 2026-07-05 maintenance pass):
//   GetMember: X64Gen dk=1 (its getm returns a borrowed ref; the wrap adds the
//     reader's +1). lvrt_getm returns +1 (TRANSFER) uniformly — a wrap retain
//     on top would be +2 the moment getter dispatch exists. Same net count,
//     the retain just moved inside the runtime. -> dk=2.
//   NewArray/NewArraySized: X64Gen dk=1 over an arena/creation-count scheme
//     (its mkarr births rc=1; scope-owned arrays go to the arena tier).
//     Runtime v2 births containers rc=0/unowned and Track A has no arena tier
//     yet (logged as an A-M6 perf follow-up), so the op body retains the fresh
//     array itself BEFORE running its element appends — the base must read
//     rc==1 during construction or every append takes the COW copy path.
//     -> dk=2 (the body's own retain IS the register's count).
int destKind(Op op) {
    switch (op) {
        case Op::Jump: case Op::JumpIfFalse: case Op::JumpIfTrue:
        case Op::Print: case Op::PrintNl: case Op::Ret: case Op::RetVoid:
        case Op::SetMember: case Op::RawSet: case Op::RawSetWeak:
        case Op::StoreGlobal: case Op::CaptureVar: case Op::Throw:
        case Op::VFree:
            return 0;
        case Op::Call: case Op::CallDyn: case Op::CallValue:
        case Op::CallNativeFn: case Op::Arith: case Op::MoveClear:
        case Op::NewObject:
        case Op::GetMember:                        // divergence: getm returns +1
        case Op::RawGetWeak:                       // weak_load returns +1 if live
        case Op::NewArray: case Op::NewArraySized: // divergence: body retains at birth
            return 2;
        default:
            return 1;
    }
}

// All callable members of a class, own decls first, then bases (dispatch
// checks in this order — first name match wins). Verbatim port of X64Gen's
// collectMembers (src/X64Gen.cpp:55-60).
void collectMembersLG(Symbol* cls, std::vector<const Stmt*>& out) {
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
        rtSysRead, rtSysOpen, rtSysClose, rtSysStat, rtSysMkdir, rtSysArgs, rtSysNow, rtSysMonotonic,
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
        // LA-2 (techdesign-tls-crypto.md §5.2): TLS/crypto natives + sysRandom leg
        rtSysTlsConnect, rtSysTlsAccept, rtSysTlsHandshake, rtSysTlsError,
        rtSysTlsAlpn, rtSysTlsVersion, rtSysRsaEncrypt, rtSysRandom, rtSysEnv,
        rtSysTimerStart, rtSysTimerCancel, rtSysWatch, rtSysWatchWrite, rtSysUnwatch,
        // LA-30 B2 (doc 06 §4): the sysTask* floor
        rtSysTaskRun, rtSysTaskCancel, rtSysTaskShield, rtSysTaskJoinAll, rtSysAwaitAny2,
        rtLoopHasWork, rtLoopStep, rtAwait, rtPeakBytes, rtLiveBytes, rtThreadStats, rtToString,
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

    Gen(const IrModule& m, DiagnosticSink& s) : mod(m), sink(s) {
        module = std::make_unique<Module>("lang", ctx);
        i64Ty = Type::getInt64Ty(ctx);
        i32Ty = Type::getInt32Ty(ctx);
        i8Ty = Type::getInt8Ty(ctx);
        f64Ty = Type::getDoubleTy(ctx);
        ptrTy = PointerType::get(ctx, 0);
        voidTy = Type::getVoidTy(ctx);
        lvTy = StructType::create(ctx, {i64Ty, i64Ty}, "LV");
        // The FULL lv_abi.h LvClassInfo — {name, classId, nslots, isValue,
        // slotNames, subtypeIds, nSubtypeIds, methodNames, methodFnIndex,
        // methodKinds, nMethods}. Emitting a shorter prefix changes the array
        // STRIDE and the runtime reads garbage past entry 0 (the A-M2 stub-era
        // emission did exactly that; re-emitted at the runtime-v2 relink per
        // lv_abi.h's emission-shape note). Unused tail columns are null/0.
        classInfoTy = StructType::create(
            ctx, {ptrTy, i64Ty, i64Ty, i32Ty, ptrTy, ptrTy, i64Ty, ptrTy, ptrTy, ptrTy, i64Ty},
            "LvClassInfo");

        auto fn = [&](const char* name, Type* ret, std::vector<Type*> args) {
            return module->getOrInsertFunction(name,
                                               FunctionType::get(ret, args, false));
        };
        rtRetain     = fn("lvrt_retain", voidTy, {ptrTy});
        rtRelease    = fn("lvrt_release", voidTy, {ptrTy});
        rtVfree      = fn("lvrt_vfree", voidTy, {ptrTy});
        rtCopyval    = fn("lvrt_copyval", voidTy, {ptrTy, ptrTy});
        rtObjNew     = fn("lvrt_obj_new", voidTy, {ptrTy, i64Ty});
        rtClosureNew = fn("lvrt_closure_new", voidTy, {ptrTy, i64Ty});
        rtCaptureSet = fn("lvrt_capture_set", voidTy, {ptrTy, ptrTy, ptrTy});
        rtCaptureGet = fn("lvrt_capture_get", voidTy, {ptrTy, ptrTy, ptrTy});
        rtGetfield   = fn("lvrt_getfield", voidTy, {ptrTy, ptrTy, ptrTy});
        rtSetfield   = fn("lvrt_setfield", voidTy, {ptrTy, ptrTy, ptrTy});
        rtWeakLoad   = fn("lvrt_weak_load", voidTy, {ptrTy, ptrTy});
        rtWeakStore  = fn("lvrt_weak_store", voidTy, {ptrTy, ptrTy});
        rtWeakGetfield = fn("lvrt_weak_getfield", voidTy, {ptrTy, ptrTy, ptrTy});
        rtWeakSetfield = fn("lvrt_weak_setfield", voidTy, {ptrTy, ptrTy, ptrTy});
        rtTruth      = fn("lvrt_truth", i32Ty, {ptrTy});
        rtNot        = fn("lvrt_not", voidTy, {ptrTy, ptrTy});
        rtNeg        = fn("lvrt_neg", voidTy, {ptrTy, ptrTy});
        rtArith      = fn("lvrt_arith", voidTy, {i32Ty, ptrTy, ptrTy, ptrTy});
        rtOpm        = fn("lvrt_opm", voidTy, {ptrTy, i64Ty, ptrTy, ptrTy});
        rtPrintVal   = fn("lvrt_print_val", voidTy, {ptrTy});
        rtPrintNl    = fn("lvrt_print_nl", voidTy, {});
        rtSysWrite   = fn("lvrt_syswrite", voidTy, {ptrTy, ptrTy, ptrTy});
        rtReadLine   = fn("lvrt_readline", voidTy, {ptrTy, ptrTy});
        rtGetm       = fn("lvrt_getm", voidTy, {ptrTy, ptrTy, ptrTy});
        rtSetm       = fn("lvrt_setm", voidTy, {ptrTy, ptrTy, ptrTy});
        rtRegister   = fn("lvrt_register", voidTy, {ptrTy, i64Ty, ptrTy});
        // bug #35: the spawn-body global-Promise guard seam (reject route A).
        rtValueHasPromise    = fn("lvrt_value_has_promise", ptrTy, {ptrTy});
        rtRegisterSpawnCheck = fn("lvrt_register_spawn_check", voidTy, {ptrTy});
        rtArrNew     = fn("lvrt_arr_new", voidTy, {ptrTy, i64Ty});
        rtArrAppend  = fn("lvrt_arr_append", voidTy, {ptrTy, ptrTy, ptrTy});
        rtArrFill    = fn("lvrt_arr_fill", voidTy, {ptrTy, i64Ty, ptrTy});
        rtArrConcatall = fn("lvrt_arr_concatall", voidTy, {ptrTy, ptrTy});
        rtMapNew     = fn("lvrt_map_new", voidTy, {ptrTy, i64Ty});
        // bug.md #30: map.with under the CallDyn (+1 transfer) convention — see
        // the `with` native row and designs/complete/techdesign-bug30-map-with-ownership.md.
        rtMapWith    = fn("lvrt_map_with", voidTy, {ptrTy, ptrTy, ptrTy, ptrTy});
        rtIdxGet     = fn("lvrt_idxget", voidTy, {ptrTy, ptrTy, ptrTy});
        rtIdxSet     = fn("lvrt_idxset", voidTy, {ptrTy, ptrTy, ptrTy, ptrTy});
        // Track 05 C3: struct-recursive Map key equality (primitives by value,
        // structs field-wise, classes by identity) — shared with lvrt_idxget's
        // map branch, so "has" agrees with "at"/"[]" on the same key.
        rtKeyEq      = fn("lvrt_keyeq", i32Ty, {ptrTy, ptrTy});
        rtStrEq      = fn("lvrt_str_eq", i32Ty, {ptrTy, ptrTy});
        rtStrSubstr  = fn("lvrt_str_substr", voidTy, {ptrTy, ptrTy, i64Ty, i64Ty});
        rtStrIndexof = fn("lvrt_str_indexof", i64Ty, {ptrTy, ptrTy});
        // Track 04 M2: toInt/toFloat became optional-returning (int?/float?),
        // so the runtime signature moved to the out-param LvValue* shape
        // (tag INT/FLOAT or NONE) matching rtStrSubstr/rtStrTrim — was
        // `int64_t lvrt_str_toint(const LvValue*)` pre-Track-04.
        rtStrToint   = fn("lvrt_str_toint", voidTy, {ptrTy, ptrTy});
        rtStrTofloat = fn("lvrt_str_tofloat", voidTy, {ptrTy, ptrTy});
        rtStrTrim    = fn("lvrt_str_trim", voidTy, {ptrTy, ptrTy});
        rtStrCase    = fn("lvrt_str_case", voidTy, {ptrTy, ptrTy, i32Ty});
        // Track 04 M3: byte-level access; OOB/out-of-range raises (never None).
        rtStrByteat   = fn("lvrt_str_byteat", voidTy, {ptrTy, ptrTy, i64Ty});
        rtStrFrombyte = fn("lvrt_str_frombyte", voidTy, {ptrTy, i64Ty});
        // Track 03 §1 char: UTF-8 encode a scalar; decode string.at / chars.
        rtCharToStr   = fn("lvrt_char_to_string", voidTy, {ptrTy, i64Ty});
        rtStrAt       = fn("lvrt_str_at", voidTy, {ptrTy, ptrTy, i64Ty});
        rtStrChars    = fn("lvrt_str_chars", voidTy, {ptrTy, ptrTy});
        // Track 03 §3 Block: constructors + bounds-checked byte-buffer natives.
        rtBlockNew     = fn("lvrt_block_new", voidTy, {ptrTy, i64Ty});
        rtBlockFromstr = fn("lvrt_block_fromstr", voidTy, {ptrTy, ptrTy});
        rtBlockByteat  = fn("lvrt_block_byteat", voidTy, {ptrTy, ptrTy, i64Ty});
        rtBlockSetbyte = fn("lvrt_block_setbyte", voidTy, {ptrTy, i64Ty, i64Ty});
        rtBlockSlice   = fn("lvrt_block_slice", voidTy, {ptrTy, ptrTy, i64Ty, i64Ty});
        rtBlockTostring= fn("lvrt_block_tostring", voidTy, {ptrTy, ptrTy, i64Ty, i64Ty});
        rtBlockInt32at = fn("lvrt_block_int32at", voidTy, {ptrTy, ptrTy, i64Ty});
        rtBlockSetint32= fn("lvrt_block_setint32", voidTy, {ptrTy, i64Ty, i64Ty});
        rtBlockInt64at = fn("lvrt_block_int64at", voidTy, {ptrTy, ptrTy, i64Ty});
        rtBlockSetint64= fn("lvrt_block_setint64", voidTy, {ptrTy, i64Ty, i64Ty});
        rtBlockFill    = fn("lvrt_block_fill", voidTy, {ptrTy, i64Ty, i64Ty, i64Ty});
        rtBlockBlit    = fn("lvrt_block_blit", voidTy, {ptrTy, i64Ty, ptrTy, i64Ty, i64Ty});
        rtBlockEquals  = fn("lvrt_block_equals", voidTy, {ptrTy, ptrTy, ptrTy});
        rtBlockMismatch= fn("lvrt_block_mismatch", voidTy, {ptrTy, ptrTy, ptrTy, i64Ty});
        rtIntToStr   = fn("lvrt_int_to_str", voidTy, {ptrTy, i64Ty});
        rtRaiseOob   = fn("lvrt_raise_oob", voidTy, {i64Ty, i64Ty});
        rtRaise      = fn("lvrt_raise", voidTy, {ptrTy});
        rtRaiseStr   = fn("lvrt_raise_str", voidTy, {ptrTy});   // Track 10 §3.3 await rethrow
        // §2.8 exception state — frozen ABI (lv_abi.h): throw_set RETAINS the
        // in-flight value; catch_bind clears the flag, releases the previous
        // binding, and transfers the thrown value (both do their ARC internally,
        // so generated code adds NO retain on the thrown value — §15 in-flight
        // ownership). uncaught reports byte-matching the oracle and does not exit.
        rtThrowing   = fn("lvrt_throwing", i32Ty, {});
        rtThrown     = fn("lvrt_thrown", voidTy, {ptrTy});
        rtThrowSet   = fn("lvrt_throw_set", voidTy, {ptrTy});
        rtCatchBind  = fn("lvrt_catch_bind", voidTy, {ptrTy});
        rtIssub      = fn("lvrt_issub", i32Ty, {i64Ty, i64Ty});
        rtUncaught   = fn("lvrt_uncaught", voidTy, {});
        // §2.7 natives — all args cross as LvValue* (boundary rule), out-param
        // first for value-returning ones; the void ones (close/timercancel/
        // unwatch) return nothing (dest written int 0, X64Gen parity).
        rtSysRead      = fn("lvrt_sysread", voidTy, {ptrTy, ptrTy, ptrTy});
        // Track 03 M4 — zero-copy Block I/O overloads (arity-selected below).
        rtSysReadBlock  = fn("lvrt_sysread_block",  voidTy, {ptrTy, ptrTy, ptrTy, ptrTy});
        rtSysWriteBlock = fn("lvrt_syswrite_block", voidTy, {ptrTy, ptrTy, ptrTy, ptrTy, ptrTy});
        rtSysSendBlock  = fn("lvrt_syssend_block",  voidTy, {ptrTy, ptrTy, ptrTy, ptrTy, ptrTy});
        rtSysRecvBlock  = fn("lvrt_sysrecv_block",  voidTy, {ptrTy, ptrTy, ptrTy, ptrTy});
        rtSysOpen      = fn("lvrt_sysopen", voidTy, {ptrTy, ptrTy, ptrTy});
        rtSysClose     = fn("lvrt_sysclose", voidTy, {ptrTy});
        rtSysStat      = fn("lvrt_sysstat", voidTy, {ptrTy, ptrTy, ptrTy});
        rtSysMkdir     = fn("lvrt_sysmkdir", voidTy, {ptrTy, ptrTy});   // Track 08 F3 dirs
        rtSysArgs      = fn("lvrt_sysargs", voidTy, {ptrTy});   // argv (designs/argv.md §5.1)
        rtSysNow       = fn("lvrt_sysnow", voidTy, {ptrTy});    // wall clock (Track 08 C6)
        rtSysMonotonic = fn("lvrt_sysmonotonic", voidTy, {ptrTy}); // monotonic ms (Track 08 F2)
        rtSysTermRaw     = fn("lvrt_systermraw", voidTy, {ptrTy, ptrTy});     // raw mode (terminal-raw-mode.md)
        rtSysTermRestore = fn("lvrt_systermrestore", voidTy, {ptrTy, ptrTy});
        // terminal floor completion (designs/techdesign-terminal-floor.md).
        rtSysWinSize     = fn("lvrt_syswinsize", voidTy, {ptrTy, ptrTy, ptrTy});   // §2 (fd, field) -> int
        rtSysTermIsRaw   = fn("lvrt_systermisraw", voidTy, {ptrTy, ptrTy});        // §2 raw-state query
        rtSysSignalOpen  = fn("lvrt_syssignalopen", voidTy, {ptrTy, ptrTy});       // §3 (Array<int>) -> fd
        rtSysSignalNext  = fn("lvrt_syssignalnext", voidTy, {ptrTy, ptrTy});       // §3 (fd) -> signo/-1
        rtSysSignalClose = fn("lvrt_syssignalclose", voidTy, {ptrTy, ptrTy});      // §3 (fd) -> 0
        // Process exit (designs/exit-codes.md §3.1). Code crosses as LvValue*
        // (boundary rule). syssetexitcode writes g_exit_code (lv_entry.c returns
        // it at process end); sysexit routes through the terminal-restore
        // epilogue then lv_plat_exit — it never returns.
        rtSysSetExitCode = fn("lvrt_syssetexitcode", voidTy, {ptrTy});
        rtSysExit        = fn("lvrt_sysexit", voidTy, {ptrTy});
        rtSysTcpConnect= fn("lvrt_systcpconnect", voidTy, {ptrTy, ptrTy, ptrTy});
        rtSysTcpListen = fn("lvrt_systcplisten", voidTy, {ptrTy, ptrTy});
        rtSysTcpListenReuse = fn("lvrt_systcplisten_reuse", voidTy, {ptrTy, ptrTy, ptrTy});
        rtSysCpuCount = fn("lvrt_syscpucount", voidTy, {ptrTy});
        rtSysSocketBuffer = fn("lvrt_syssocketbuffer", voidTy, {ptrTy, ptrTy, ptrTy, ptrTy});
        // Process floor (techdesign-spawn-llvm.md §5): POSIX-only, Windows-rejected below.
        rtSysSpawn     = fn("lvrt_sysspawn",     voidTy, {ptrTy, ptrTy, ptrTy});
        rtSysPidfdOpen = fn("lvrt_syspidfdopen", voidTy, {ptrTy, ptrTy});
        rtSysReap      = fn("lvrt_sysreap",      voidTy, {ptrTy, ptrTy});
        rtSysKill      = fn("lvrt_syskill",      voidTy, {ptrTy, ptrTy, ptrTy});
        rtSysThreadTransfer = fn("lvrt_systhreadtransfer", voidTy, {ptrTy, ptrTy});
        rtSysThreadStart = fn("lvrt_systhreadstart", voidTy, {ptrTy, ptrTy});
        rtSysThreadResult = fn("lvrt_systhreadresult", voidTy, {ptrTy, ptrTy});
        rtSysChannelNew = fn("lvrt_syschannelnew", voidTy, {ptrTy, ptrTy, ptrTy});
        rtSysChannelSend = fn("lvrt_syschannelsend", voidTy, {ptrTy, ptrTy, ptrTy});
        rtSysChannelReceive = fn("lvrt_syschannelreceive", voidTy, {ptrTy, ptrTy});
        rtSysChannelClose = fn("lvrt_syschannelclose", voidTy, {ptrTy, ptrTy});
        rtSysAccept    = fn("lvrt_sysaccept", voidTy, {ptrTy, ptrTy});
        rtSysSend      = fn("lvrt_syssend", voidTy, {ptrTy, ptrTy, ptrTy});
        rtSysRecv      = fn("lvrt_sysrecv", voidTy, {ptrTy, ptrTy, ptrTy});
        // Track 08 F5: connect-timeout floor + write-watch (lv_abi.h §2.7).
        rtSysTcpConnectNb = fn("lvrt_systcpconnectnb", voidTy, {ptrTy, ptrTy, ptrTy});
        rtSysConnectResult= fn("lvrt_sysconnectresult", voidTy, {ptrTy, ptrTy});
        rtSysTimerStart= fn("lvrt_systimerstart", voidTy, {ptrTy, ptrTy, ptrTy, ptrTy});
        rtSysTimerCancel= fn("lvrt_systimercancel", voidTy, {ptrTy});
        // LA-2 §5.2 — TLS/crypto natives (thin shims over the lv_tls_* seam).
        rtSysTlsConnect  = fn("lvrt_systlsconnect", voidTy, {ptrTy, ptrTy, ptrTy, ptrTy, ptrTy, ptrTy});
        rtSysTlsAccept   = fn("lvrt_systlsaccept", voidTy, {ptrTy, ptrTy, ptrTy, ptrTy, ptrTy});
        rtSysTlsHandshake= fn("lvrt_systlshandshake", voidTy, {ptrTy, ptrTy});
        rtSysTlsError    = fn("lvrt_systlserror", voidTy, {ptrTy, ptrTy});
        rtSysTlsAlpn     = fn("lvrt_systlsalpn", voidTy, {ptrTy, ptrTy});
        rtSysTlsVersion  = fn("lvrt_systlsversion", voidTy, {ptrTy, ptrTy});
        rtSysRsaEncrypt  = fn("lvrt_sysrsaencrypt", voidTy, {ptrTy, ptrTy, ptrTy, ptrTy});
        rtSysRandom      = fn("lvrt_sysrandom", voidTy, {ptrTy, ptrTy});
        rtSysEnv         = fn("lvrt_sysenv", voidTy, {ptrTy, ptrTy});   // bug #68
        rtSysWatch     = fn("lvrt_syswatch", voidTy, {ptrTy, ptrTy, ptrTy});
        rtSysWatchWrite= fn("lvrt_syswatchwrite", voidTy, {ptrTy, ptrTy, ptrTy});
        rtSysUnwatch   = fn("lvrt_sysunwatch", voidTy, {ptrTy});
        // §2.9 event loop — lv_main's tail drain pumps these directly.
        rtLoopHasWork  = fn("lvrt_loop_has_work", i32Ty, {});
        rtLoopStep     = fn("lvrt_loop_step", voidTy, {});
        // LA-30 (doc 2 §2/§4): Op::Await is ONE runtime call — park (LANG_TASKS=1)
        // or pump (=0) lives inside lvrt_await, where it can switch stacks.
        rtAwait        = fn("lvrt_await", voidTy, {ptrTy, ptrTy});
        // LA-30 B2 (doc 06 §4): the sysTask* floor — ids across the boundary,
        // scalar results; joinAll/awaitAny2 park and may raise (throw check
        // rides CallNativeFn's blanket emitThrowCheck).
        rtSysTaskRun    = fn("lvrt_systaskrun", voidTy, {ptrTy, ptrTy});
        rtSysTaskCancel = fn("lvrt_systaskcancel", voidTy, {ptrTy, ptrTy});
        rtSysTaskShield = fn("lvrt_systaskshield", voidTy, {ptrTy, ptrTy});
        rtSysTaskJoinAll= fn("lvrt_systaskjoinall", voidTy, {ptrTy, ptrTy});
        rtSysAwaitAny2  = fn("lvrt_sysawaitany2", voidTy, {ptrTy, ptrTy, ptrTy});
        // §2.5 escaping-tier accounting meter — lv_main prints it at exit for the
        // churn leak harness (X64Gen's main does the same, X64Gen.cpp:3798-3812).
        rtPeakBytes    = fn("lvrt_peak_bytes", i64Ty, {});
        rtLiveBytes    = fn("lvrt_live_bytes", i64Ty, {});
        rtThreadStats  = fn("lvrt_thread_stats_report", voidTy, {});
        rtToString     = fn("lvrt_to_string", voidTy, {ptrTy, ptrTy});
        // §9 A-M6 arena tier — all four already exist in lv_abi.h §2.5/§2.6;
        // Track A wires them, per doc-1 §9, without any ABI addition.
        rtHalloc       = fn("lvrt_halloc", ptrTy, {i64Ty, i32Ty});
        rtArenaSave    = fn("lvrt_arena_save", i64Ty, {});
        rtArenaRestore = fn("lvrt_arena_restore", voidTy, {i64Ty});
        rtIsvalueclass = fn("lvrt_isvalueclass", i32Ty, {i64Ty});
        rtFieldcount   = fn("lvrt_fieldcount", i64Ty, {i64Ty});
        // §9b A-M7 / doc-2 §2.10: declare (not define) the runtime's exported
        // hot-state globals — no initializer, ExternalLinkage, so the linker
        // binds these to lv_runtime.c's `lv_g_throwing`/`lv_g_arena_cursor`.
        gThrowing = new GlobalVariable(*module, i32Ty, /*isConstant=*/false,
                                        GlobalValue::ExternalLinkage, nullptr,
                                        "lv_g_throwing");
        gThrowing->setThreadLocal(true);
        gArenaCursor = new GlobalVariable(*module, ptrTy, /*isConstant=*/false,
                                          GlobalValue::ExternalLinkage, nullptr,
                                          "lv_g_arena_cursor");
        gArenaCursor->setThreadLocal(true);
        // The TLS *model* is set later in emitObject once the target triple is
        // known: initial-exec (the design's fast %fs-relative load) on ELF, the
        // portable general-dynamic default elsewhere. COFF (Windows) has no
        // initial-exec relocation — forcing it there emits IMAGE_REL_AMD64_SECREL
        // against an undefined symbol and the mingw link fails. Threads are
        // POSIX/LLVM-only in v1, so Windows never spawns; general-dynamic keeps
        // the single-thread runtime correct and links cleanly.
    }

    Constant* i64C(int64_t v) { return ConstantInt::get(i64Ty, (uint64_t)v); }
    Constant* i32C(int32_t v) { return ConstantInt::get(i32Ty, (uint32_t)v); }
    Constant* nullPtr() { return ConstantPointerNull::get(cast<PointerType>(ptrTy)); }

    int classIdOf(Symbol* cls) {
        auto it = clsIds.find(cls);
        if (it != clsIds.end()) return it->second;
        int id = (int)clsIds.size() + 1;   // 1-based, mirrors X64Gen::clsId
        clsIds[cls] = id;
        return id;
    }
    // A prelude/global class by name (X64Gen::lookupClsId); 0 if absent.
    int classIdByName(const char* name) {
        Symbol* s = mod.sema ? mod.sema->global->lookup(name) : nullptr;
        return s ? classIdOf(s) : 0;
    }
    Symbol* classByName(const char* name) {
        return mod.sema ? mod.sema->global->lookup(name) : nullptr;
    }

    // Non-method slots in shape order — the packed-slot key list, mirroring
    // X64Gen::fieldKeys (distinct fields reach by "Source::name").
    std::vector<std::string> fieldKeysOf(Symbol* cls) const {
        std::vector<std::string> keys;
        if (!cls) return keys;
        for (const Slot& s : cls->shape.slots) {
            if (s.isMethod) continue;
            keys.push_back((s.distinct && s.source)
                ? std::string(s.source->name) + "::" + std::string(s.name)
                : std::string(s.name));
        }
        return keys;
    }

    // A private, constant, NUL-terminated C string; returns its address (an
    // opaque ptr constant usable directly in an aggregate initializer or as a
    // call argument). Deduped by content — the runtime's key comparison is
    // pointer-first with a content fallback, so this dedup is the fast path,
    // not a correctness requirement (lv_abi.h key-comparison ruling).
    std::unordered_map<std::string, GlobalVariable*> cstrCache;
    Constant* cstr(const std::string& s) {
        auto it = cstrCache.find(s);
        if (it != cstrCache.end()) return it->second;
        Constant* init = ConstantDataArray::getString(ctx, s, /*AddNull=*/true);
        auto* gv = new GlobalVariable(*module, init->getType(), true,
                                      GlobalValue::PrivateLinkage, init, "cstr");
        cstrCache[s] = gv;
        return gv;
    }

    void fail(const std::string& what) {
        if (ok) sink.error({}, "LLVM backend: " + what);
        ok = false;
    }

    // A string literal becomes a private constant global {i64 len, i8 bytes[len+1]}
    // (the descriptor layout of doc-2 §2.4; the trailing NUL is for safe C
    // interop, not semantically part of the string). Literals carry NO LvHeader
    // and live outside the runtime's registered regions, so retain/release/
    // temp-free all skip them by the region check. Dedup by content.
    llvm::Value* stringGlobal(IRBuilder<>& b, const std::string& s) {
        auto it = strLiterals.find(s);
        GlobalVariable* gv;
        if (it != strLiterals.end()) {
            gv = it->second;
        } else {
            std::vector<Constant*> bytes;
            for (char c : s) bytes.push_back(ConstantInt::get(i8Ty, (uint8_t)c));
            bytes.push_back(ConstantInt::get(i8Ty, 0));
            ArrayType* bytesTy = ArrayType::get(i8Ty, bytes.size());
            Constant* bytesConst = ConstantArray::get(bytesTy, bytes);
            StructType* strTy = StructType::get(ctx, {i64Ty, bytesTy});
            Constant* structConst = ConstantStruct::get(
                strTy, {i64C((int64_t)s.size()), bytesConst});
            gv = new GlobalVariable(*module, strTy, /*isConstant=*/true,
                                    GlobalValue::PrivateLinkage, structConst, "str.lit");
            strLiterals[s] = gv;
        }
        return b.CreatePtrToInt(gv, i64Ty);
    }

    // ---- reachability + dispatch-class discovery ----------------------------
    // X64Gen-parity (src/X64Gen.cpp:3690-3760): instantiating a class marks all
    // its members (name-based dispatch may reach any of them); any collection
    // op seeds the in-language Array/Map method surface; MakeClosure marks its
    // lambda; primitive masks join the dispatch candidates WITHOUT being
    // force-marked (their methods arrive reachable only via resolved calls —
    // unresolved primitive-receiver calls fall to the native rows, as on ELF).
    void computeReachable() {
        reachable.assign(mod.functions.size(), false);
        std::vector<int> work;
        auto mark = [&](int fi) {
            if (fi >= 0 && fi < (int)mod.functions.size() && !reachable[fi]) {
                reachable[fi] = true;
                work.push_back(fi);
            }
        };
        // bug.md #27/#28: an unresolved by-name CallDyn (a bare self-call inside
        // an unchecked prelude body) carries only a method NAME. Index every
        // in-language method by name so such a call can mark all same-named
        // candidates and pull their class into the callm dispatch set — without
        // this, e.g. Array.first/skip (from StreamBuffer.pull) or
        // string.indexOfFrom (from split) are pruned, then the LLVM
        // nativeMethodCovered guard hard-errors ("native method not covered").
        std::unordered_map<std::string, std::vector<std::pair<int, Symbol*>>> methodFns;
        std::vector<Symbol*> byNameClasses;
        if (mod.sema && mod.sema->global)
            for (const auto& [nm, syms] : mod.sema->global->names)
                for (Symbol* cls : syms)
                    if (cls->kind == SymbolKind::Class)
                        for (const Slot& s : cls->shape.slots)
                            if (s.isMethod && s.decl && mod.byDecl.count(s.decl))
                                methodFns[std::string(s.name)].push_back(
                                    {mod.byDecl.at(s.decl), cls});
        auto markByName = [&](const std::string& sname) {
            auto it = methodFns.find(sname);
            if (it == methodFns.end()) return;
            for (auto& [fnIdx, cls] : it->second) {
                // Track 03 §1: `char` is now LLVM-supported (the LV_CHAR ABI
                // addendum landed — deferal-char-block-abi.md), so its
                // in-language helpers (toUpper/toLower/isDigit/…) lower here like
                // any other primitive mask; their native leaves (code/charFromCode)
                // are covered by the emitNativeRows char rows below.
                mark(fnIdx);
                bool seen = false;
                for (Symbol* c : byNameClasses) if (c == cls) seen = true;
                if (!seen) byNameClasses.push_back(cls);
            }
        };
        auto scan = [&](int fi) {
            for (const Inst& in : mod.functions[fi].code) {
                if (in.op == Op::Call) mark(in.b);
                else if (in.op == Op::NewObject) {
                    if (in.b >= 0) mark(in.b);   // $init runs directly, not via Op::Call
                    if (in.sym) {
                        bool seen = false;
                        for (Symbol* c : instClasses) if (c == in.sym) seen = true;
                        if (!seen) {
                            instClasses.push_back(in.sym);
                            std::vector<const Stmt*> mem;
                            collectMembersLG(in.sym, mem);
                            for (const Stmt* m : mem)
                                if (mod.byDecl.count(m)) mark(mod.byDecl.at(m));
                        }
                    }
                } else if (in.op == Op::CallDyn) {
                    if (in.decl && mod.byDecl.count(in.decl)) mark(mod.byDecl.at(in.decl));
                    else markByName(in.sname);   // unresolved: follow by name
                } else if (in.op == Op::MakeClosure) {
                    mark(in.b);
                    bool seen = false;
                    for (int f : closureFns_) if (f == in.b) seen = true;
                    if (!seen) closureFns_.push_back(in.b);
                }
            }
        };
        mark(mod.entry);
        // ginit runs before entry (H-9: Ir.hpp:113's "interp-only" comment is
        // stale — X64Gen calls it too, X64Gen.cpp:3793). It initializes prelude
        // and top-level globals, so it must be emitted and reachable.
        mark(mod.ginit);
        while (!work.empty()) { int fi = work.back(); work.pop_back(); scan(fi); }

        usesColl = false;
        for (size_t i = 0; i < mod.functions.size(); ++i) {
            if (!reachable[i]) continue;
            for (const Inst& in : mod.functions[i].code) {
                switch (in.op) {
                    case Op::NewArray: case Op::NewArraySized: case Op::NewMap:
                    case Op::GetIndex: case Op::IndexStore: case Op::IterAt:
                    case Op::MakeRange:
                        usesColl = true; break;
                    case Op::Default:
                        if (in.sname.rfind("Array", 0) == 0 || in.sname.rfind("Map", 0) == 0)
                            usesColl = true;
                        break;
                    default: break;
                }
            }
        }
        std::vector<Symbol*> collClasses;
        if (usesColl) {
            // Range joins Array/Map: `for (x in it)` over an IIterable-typed
            // variable holding a Range dispatches Range.iterator() by name
            // (techdesign-07 §2.1 uniformity). scan() below expands each seeded
            // method to fixpoint, so the iterator classes it constructs get their
            // hasNext()/next() emitted too.
            for (const char* cn : {"Array", "Map", "Range"}) {
                Symbol* c = classByName(cn);
                if (!c || !c->decl) continue;
                collClasses.push_back(c);
                for (const StmtPtr& m : c->decl->body)
                    if (m->kind == StmtKind::Member && m->callable && !m->isCtor &&
                        mod.byDecl.count(m.get()))
                        mark(mod.byDecl.at(m.get()));
            }
            while (!work.empty()) { int fi = work.back(); work.pop_back(); scan(fi); }
        }

        callmClasses = instClasses;
        for (Symbol* c : collClasses) callmClasses.push_back(c);
        for (Symbol* c : byNameClasses) callmClasses.push_back(c);   // bug.md #27/#28
        for (const char* p : {"string", "int", "bool", "float"}) {
            Symbol* c = classByName(p);
            if (c) callmClasses.push_back(c);
        }
        // dedupe, preserving order (a mask/coll class may also be instantiated)
        std::vector<Symbol*> uniq;
        for (Symbol* c : callmClasses) {
            bool seen = false;
            for (Symbol* u : uniq) if (u == c) seen = true;
            if (!seen) uniq.push_back(c);
        }
        callmClasses = uniq;
    }

    bool run() {
        computeReachable();

        // Pre-seed classIds for the prelude classes generated code references
        // by identity (Range for MakeRange/spread/to_string's "start..end"
        // rendering; Pair for map iteration) and the dispatch masks. Extra
        // table rows are harmless; a MISSING Range row breaks to_string.
        for (const char* n : {"Range", "Pair", "Array", "Map", "string", "int", "bool", "float"})
            classIdByName(n);

        // A-M4: the runtime raises RuntimeException for OOB / bad-key / unresolved
        // dispatch even when the program never instantiates one explicitly, so its
        // whole hierarchy must carry class-table rows — a NAMED row so lvrt_raise's
        // lv_find_class_by_name resolves it, with complete subtypeIds so a
        // `catch (RuntimeException)`/`catch (IException)` issub succeeds and
        // lvrt_uncaught can name it. Mirrors X64Gen's idChain(RuntimeException)
        // (src/X64Gen.cpp:3859). Extra rows are harmless (ids are symbolic).
        std::function<void(Symbol*)> idChain = [&](Symbol* c) {
            if (!c || !c->decl) return;
            classIdOf(c);
            for (const TypeRefPtr& t : c->decl->bases) idChain(t->resolvedSymbol);
        };
        idChain(classByName("RuntimeException"));
        // LA-30 B2 (doc 06 §2): the cancellation carrier rides the same
        // machinery — lvrt_raise_cls resolves it BY NAME at a park's cancel
        // delivery even when the program never instantiates one, so its
        // hierarchy (CancelledException -> Exception + ICancelledException ->
        // IException) needs the same named rows for catch-by-type/interface.
        idChain(classByName("CancelledException"));

        fns.assign(mod.functions.size(), nullptr);
        for (size_t i = 0; i < mod.functions.size(); ++i) {
            if (!reachable[i]) continue;
            std::vector<Type*> params(1 + mod.functions[i].nparams, ptrTy);
            FunctionType* ft = FunctionType::get(voidTy, params, false);
            fns[i] = Function::Create(ft, Function::InternalLinkage,
                                      "f" + std::to_string(i), module.get());
        }

        // The trampoline only calls the (already-created) function declarations,
        // so it can be emitted up front — CallValue sites reference it.
        emitDispatchTrampoline();
        emitArenaHelpers();

        // §9 A-M6 arena tier: which allocation sites provably die with their
        // frame — the same shared analysis X64Gen calls at X64Gen.cpp:3943
        // (src/Ownership.cpp, not X64Gen-owned/frozen). genFunction consults
        // this to route CopyVal(c==1)/NewObject(isValue) sites to the arena.
        ownership = analyzeOwnership(mod);

        // The globals array backs Load/StoreGlobal. Zero-initialized: every
        // slot reads as void until first written, so the ARC store discipline
        // (StoreGlobal retains) starts from a clean state.
        if (mod.nglobals > 0) {
            ArrayType* arrTy = ArrayType::get(lvTy, mod.nglobals);
            globalsArray = new GlobalVariable(
                *module, arrTy, /*isConstant=*/false, GlobalValue::InternalLinkage,
                ConstantAggregateZero::get(arrTy), "lv_globals");
        }

        // bug #35: the spawn-body global-Promise guard. Emitted here (after
        // lv_globals exists) so it can GEP the module-private globals array.
        emitSpawnGlobalCheck();

        for (size_t i = 0; i < mod.functions.size() && ok; ++i)
            if (reachable[i]) genFunction((int)i);
        if (!ok) return false;

        // Transitive base classes must have ids before the table is emitted
        // (subtypeIds columns reference them). Fixpoint: registering a base
        // can expose ITS bases.
        for (bool grew = true; grew; ) {
            grew = false;
            std::vector<Symbol*> snapshot;
            for (auto& [sym, id] : clsIds) snapshot.push_back(sym);
            for (Symbol* sym : snapshot) {
                std::vector<Symbol*> bases;
                collectBases(sym, bases);
                for (Symbol* b : bases)
                    if (!clsIds.count(b)) { classIdOf(b); grew = true; }
            }
        }

        // A-M5 entry flip (§3 / doc-2 §2.9): emit `lv_main(LvValue* ret)` with
        // EXTERNAL linkage instead of `main`. The runtime owns `main()`
        // (runtime/lv_entry.c) — it calls lv_rt_init (heap/arena/registries,
        // argv capture) THEN lv_main THEN drains the loop THEN returns
        // lv_rt_exit_code. Emitting lv_main leaves the generated object's main
        // undefined, so the linker pulls lv_entry.o's main from liblvrt.a (or
        // the loose lv_entry.o in the test lanes). lv_main's body is X64Gen's
        // main verbatim minus the process exit: register → ginit → entry →
        // run_loop drain → uncaught.
        FunctionType* lvMainTy = FunctionType::get(voidTy, {ptrTy}, false);
        Function* lvMainFn = Function::Create(lvMainTy, Function::ExternalLinkage,
                                              "lv_main", module.get());
        IRBuilder<> b(BasicBlock::Create(ctx, "entry", lvMainFn));
        emitClassRegistration(b);
        emitColumnarDescriptors();   // techdesign-columnar §4.3/§4.4 (after clsIds is final)
        if (mod.ginit >= 0) {
            llvm::Value* ginitRet = b.CreateAlloca(lvTy);
            b.CreateCall(fns[mod.ginit], {ginitRet});
        }
        llvm::Value* slot = b.CreateAlloca(lvTy);
        b.CreateCall(fns[mod.entry], {slot});
        // Drain the event loop like X64Gen's run_loop (src/X64Gen.cpp:2504):
        // pump loop_step while there is live work AND no pending throw — timers,
        // fd watches, and async continuations scheduled by the program body but
        // not already consumed by an inline Await pump run here. The runtime's
        // own main() drains again afterward (a no-op once this loop has emptied
        // the queue), so uncaught reporting still lands after all work.
        BasicBlock* loopCond = BasicBlock::Create(ctx, "loop.cond", lvMainFn);
        BasicBlock* loopStep = BasicBlock::Create(ctx, "loop.step", lvMainFn);
        BasicBlock* loopDone = BasicBlock::Create(ctx, "loop.done", lvMainFn);
        b.CreateBr(loopCond);
        b.SetInsertPoint(loopCond);
        llvm::Value* hasWork = b.CreateICmpNE(b.CreateCall(rtLoopHasWork, {}), i32C(0));
        llvm::Value* notThrowing = b.CreateICmpEQ(b.CreateCall(rtThrowing, {}), i32C(0));
        b.CreateCondBr(b.CreateAnd(hasWork, notThrowing), loopStep, loopDone);
        b.SetInsertPoint(loopStep);
        b.CreateCall(rtLoopStep, {});
        b.CreateBr(loopCond);
        b.SetInsertPoint(loopDone);
        // Report any throw that propagated to the top (byte-matching the oracle).
        // lvrt_uncaught no-ops when nothing is pending and does NOT itself exit;
        // when a throw IS pending it sets the exit code to 1 (designs/exit-codes.md
        // §5 gap b), which lv_entry.c returns after the drain — a crashed program
        // reports failure. Set-and-complete's code is likewise returned there.
        b.CreateCall(rtUncaught, {});
        // §2.5 escaping-tier meter to fd 2 (X64Gen main parity, X64Gen.cpp:3798-
        // 3812) — the churn leak harness parses `live-at-exit` from it. stdout is
        // untouched, so the corpus differentials are unaffected. Read peak AND
        // live FIRST (before any int_to_str allocation inflates g_live_bytes),
        // then format/print; the temps allocated here don't perturb the reported
        // value. The runtime-owned main() drains the loop before calling lv_main,
        // so by here all async work has settled.
        {
            llvm::Value* peak = b.CreateCall(rtPeakBytes, {});
            llvm::Value* live = b.CreateCall(rtLiveBytes, {});
            llvm::Value* fdSlot = b.CreateAlloca(lvTy);
            llvm::Value* sSlot = b.CreateAlloca(lvTy);
            llvm::Value* wSlot = b.CreateAlloca(lvTy);
            storeTP(b, fdSlot, i64C(1), i64C(2));   // fd 2 (stderr)
            auto writeStr = [&](llvm::Value* strLv) {
                b.CreateCall(rtSysWrite, {wSlot, fdSlot, strLv});
            };
            auto writeLit = [&](const char* s) {
                storeTP(b, sSlot, i64C(4), stringGlobal(b, s));
                writeStr(sSlot);
            };
            writeLit("[heap] escaping-tier peak=");
            b.CreateCall(rtIntToStr, {sSlot, peak}); writeStr(sSlot);
            writeLit(" live-at-exit=");
            b.CreateCall(rtIntToStr, {sSlot, live}); writeStr(sSlot);
            writeLit(" bytes\n");
            // Track 10 §4.2: the env-gated [threads] line right after (the
            // runtime reads LANG_THREAD_STATS and the transfer counters itself).
            b.CreateCall(rtThreadStats, {});
        }
        storeVoid(b, lvMainFn->getArg(0));   // ret is unused by the runtime
        b.CreateRetVoid();

        std::string err;
        raw_string_ostream os(err);
        if (verifyModule(*module, &os)) { fail("verifier: " + os.str()); return false; }
        return true;
    }

    void collectBases(Symbol* cls, std::vector<Symbol*>& out) {
        if (!cls || !cls->decl) return;
        for (const TypeRefPtr& t : cls->decl->bases) {
            Symbol* bs = t->resolvedSymbol;
            if (!bs) continue;
            bool seen = false;
            for (Symbol* o : out) if (o == bs) seen = true;
            if (seen) continue;
            out.push_back(bs);
            collectBases(bs, out);
        }
    }

    // The LvDispatchFn trampoline: switch on the IR function index, unpack the
    // contiguous LvValue args array into per-param pointers, tail into the
    // generated function. Serves both lvrt_register (getm/setm/opm/callm
    // dispatch) and CallValue (closure invocation) — one mechanism, mirroring
    // X64Gen's genCallClosure/genCallM per-program switches.
    void emitDispatchTrampoline() {
        FunctionType* dt = FunctionType::get(voidTy, {i64Ty, ptrTy, ptrTy, i64Ty}, false);
        dispatchFn = Function::Create(dt, Function::InternalLinkage, "lv_dispatch",
                                      module.get());
        IRBuilder<> b(BasicBlock::Create(ctx, "entry", dispatchFn));
        llvm::Value* fnIndex = dispatchFn->getArg(0);
        llvm::Value* ret = dispatchFn->getArg(1);
        llvm::Value* args = dispatchFn->getArg(2);
        BasicBlock* defBB = BasicBlock::Create(ctx, "dflt", dispatchFn);
        SwitchInst* sw = b.CreateSwitch(fnIndex, defBB);
        for (size_t i = 0; i < fns.size(); ++i) {
            if (!fns[i]) continue;
            BasicBlock* c = BasicBlock::Create(ctx, "f" + std::to_string(i), dispatchFn);
            sw->addCase(cast<ConstantInt>(i64C((int64_t)i)), c);
            b.SetInsertPoint(c);
            std::vector<llvm::Value*> call{ret};
            for (int p = 0; p < mod.functions[i].nparams; ++p)
                call.push_back(b.CreateGEP(lvTy, args, i64C(p)));
            b.CreateCall(fns[i], call);
            b.CreateRetVoid();
        }
        b.SetInsertPoint(defBB);
        b.CreateStore(i64C(0), b.CreateStructGEP(lvTy, ret, 0));
        b.CreateStore(i64C(0), b.CreateStructGEP(lvTy, ret, 1));
        b.CreateRetVoid();
    }

    // bug #35 (reject route A): `const char* lv_spawn_global_check(i64 fnIndex)`
    // — the codegen half of the spawn-body global-Promise guard. The runtime
    // flatten sees only a spawn body's CAPTURE list, so a Promise the body
    // reaches through a bare GLOBAL (read straight from the module-private
    // lv_globals) never trips A-1's cross-thread-Promise reject. This function
    // — the only code that can GEP lv_globals — switches on the body closure's
    // IR fn index to the set of global slots that function (transitively over
    // the nested lambdas it CREATES, computeFnGlobalRefs) references, and calls
    // lvrt_value_has_promise on each slot's CURRENT value. It returns the first
    // non-null reject message (byte-identical to the capture reject), else null.
    // Registered with the runtime (lvrt_register_spawn_check) so lv_thread.c's
    // spawn path consults it before flattening, before any worker starts.
    void emitSpawnGlobalCheck() {
        FunctionType* ft = FunctionType::get(ptrTy, {i64Ty}, false);
        spawnCheckFn = Function::Create(ft, Function::InternalLinkage,
                                        "lv_spawn_global_check", module.get());
        BasicBlock* entry = BasicBlock::Create(ctx, "entry", spawnCheckFn);
        BasicBlock* nullBB = BasicBlock::Create(ctx, "none", spawnCheckFn);
        IRBuilder<> nb(nullBB);
        nb.CreateRet(nullPtr());
        // No globals => nothing a spawn body could reference; always null.
        if (!globalsArray) {
            IRBuilder<>(entry).CreateBr(nullBB);
            return;
        }
        std::vector<std::vector<int>> refs = computeFnGlobalRefs(mod);
        // Only functions with at least one referenced global need a case.
        unsigned ncase = 0;
        for (auto& r : refs) if (!r.empty()) ncase++;
        IRBuilder<> b(entry);
        SwitchInst* sw = b.CreateSwitch(spawnCheckFn->getArg(0), nullBB, ncase);
        for (size_t fi = 0; fi < refs.size(); ++fi) {
            if (refs[fi].empty()) continue;
            BasicBlock* c = BasicBlock::Create(ctx, "f" + std::to_string(fi), spawnCheckFn);
            sw->addCase(cast<ConstantInt>(i64C((int64_t)fi)), c);
            b.SetInsertPoint(c);
            // Per referenced slot: scan lv_globals[slot]'s current value; a
            // non-null message short-circuits the whole check.
            for (int slot : refs[fi]) {
                llvm::Value* g = b.CreateGEP(ArrayType::get(lvTy, mod.nglobals),
                                             globalsArray, {i64C(0), i64C(slot)});
                llvm::Value* msg = b.CreateCall(rtValueHasPromise, {g});
                BasicBlock* hit  = BasicBlock::Create(ctx, "hit", spawnCheckFn);
                BasicBlock* next = BasicBlock::Create(ctx, "next", spawnCheckFn);
                b.CreateCondBr(b.CreateICmpNE(msg, nullPtr()), hit, next);
                IRBuilder<>(hit).CreateRet(msg);
                b.SetInsertPoint(next);
            }
            b.CreateBr(nullBB);
        }
    }

    // Zero `nslots` LV-sized (16-byte) slots starting at `slotsBase`, an i8*
    // one past the {classId,dyn} header (X64Gen-parity default state: every
    // declared field reads VOID until $init/an initializer writes it). Hand-
    // rolled loop (no memset intrinsic) to match this file's existing style
    // (see genFunction's emitLoop).
    void emitZeroSlots(IRBuilder<>& b, Function* fn, llvm::Value* slotsBase,
                       llvm::Value* nslots) {
        llvm::Value* ctr = b.CreateAlloca(i64Ty);
        b.CreateStore(i64C(0), ctr);
        BasicBlock* cond = BasicBlock::Create(ctx, "zero.cond", fn);
        BasicBlock* body = BasicBlock::Create(ctx, "zero.body", fn);
        BasicBlock* done = BasicBlock::Create(ctx, "zero.done", fn);
        b.CreateBr(cond);
        b.SetInsertPoint(cond);
        llvm::Value* i = b.CreateLoad(i64Ty, ctr);
        b.CreateCondBr(b.CreateICmpSLT(i, nslots), body, done);
        b.SetInsertPoint(body);
        llvm::Value* off = b.CreateMul(i, i64C(16));
        llvm::Value* slot = b.CreateGEP(i8Ty, slotsBase, off);
        b.CreateStore(i64C(0), slot);
        b.CreateStore(i64C(0), b.CreateGEP(i8Ty, slot, i64C(8)));
        b.CreateStore(b.CreateAdd(i, i64C(1)), ctr);
        b.CreateBr(cond);
        b.SetInsertPoint(done);
    }

    // Allocate a fresh ARENA-tier object body of `nslots` fields, writing the
    // LvHeader (rc=-1 sentinel) + {classId, dyn=0} + zeroed slots (lv_abi.h
    // §2.4; mirrors lv_alloc_packed/lv_halloc_prefixed, lv_runtime.c, with
    // LV_TIER_ARENA in place of the heap tier those always use). Returns the
    // i8* payload address (post-header).
    llvm::Value* emitAllocPackedArena(IRBuilder<>& b, Function* fn, llvm::Value* word0,
                                      llvm::Value* nslots) {
        llvm::Value* bodySize = b.CreateAdd(i64C(16), b.CreateMul(nslots, i64C(16)));
        llvm::Value* total = b.CreateAdd(bodySize, i64C(16));
        llvm::Value* raw = b.CreateCall(rtHalloc, {total, i32C(1) /* LV_TIER_ARENA */});
        b.CreateStore(i64C(-1), raw);                                  // rc: arena sentinel
        llvm::Value* metaAddr = b.CreateGEP(i8Ty, raw, i64C(8));
        b.CreateStore(total, metaAddr);
        llvm::Value* payload = b.CreateGEP(i8Ty, raw, i64C(16));
        b.CreateStore(word0, payload);
        b.CreateStore(i64C(0), b.CreateGEP(i8Ty, payload, i64C(8)));    // dyn = 0
        llvm::Value* slotsBase = b.CreateGEP(i8Ty, payload, i64C(16));
        emitZeroSlots(b, fn, slotsBase, nslots);
        return payload;
    }

    // §9 A-M6 arena tier. Two module-level helpers, generated once, that
    // mirror lvrt_obj_new/lvrt_copyval (lv_runtime.c) exactly except the top
    // (and, for the copy helper, every recursively-nested value-struct field)
    // allocation lands on LV_TIER_ARENA instead of LV_TIER_HEAP. Built ONLY
    // from already-frozen ABI primitives (lvrt_halloc's explicit tier param,
    // lvrt_isvalueclass, lvrt_fieldcount) — no lv_abi.h change. Called only at
    // sites Ownership.hpp's analyzeOwnership() proves scope-owned (never
    // reached by a Ret/Throw/store/capture/global — see genFunction's
    // Op::NewObject/Op::CopyVal cases), so the resulting rc=-1 object is
    // reclaimed by the enclosing frame's arena_restore, never individually.
    void emitArenaHelpers() {
        {
            // nslots is the CALLER's third arg (a compile-time constant at every
            // NewObject site, in.sym's own field count) rather than a
            // lvrt_fieldcount(classId) call here — one fewer cross-.o call per
            // arena-tier construction (measured: object-heavy churn is call-
            // overhead bound; §12 perf note).
            FunctionType* ft = FunctionType::get(voidTy, {ptrTy, i64Ty, i64Ty}, false);
            objNewArenaFn = Function::Create(ft, Function::InternalLinkage,
                                             "lv_objnew_arena", module.get());
            IRBuilder<> b(BasicBlock::Create(ctx, "entry", objNewArenaFn));
            llvm::Value* out = objNewArenaFn->getArg(0);
            llvm::Value* classId = objNewArenaFn->getArg(1);
            llvm::Value* nslots = objNewArenaFn->getArg(2);
            llvm::Value* payload = emitAllocPackedArena(b, objNewArenaFn, classId, nslots);
            storeTP(b, out, i64C(5) /* LV_OBJ */, b.CreatePtrToInt(payload, i64Ty));
            b.CreateRetVoid();
        }
        {
            FunctionType* ft = FunctionType::get(voidTy, {ptrTy, ptrTy}, false);
            copyValArenaFn = Function::Create(ft, Function::InternalLinkage,
                                              "lv_copyval_arena", module.get());
            IRBuilder<> b(BasicBlock::Create(ctx, "entry", copyValArenaFn));
            llvm::Value* out = copyValArenaFn->getArg(0);
            llvm::Value* in = copyValArenaFn->getArg(1);
            BasicBlock* checkValBB = BasicBlock::Create(ctx, "cv.checkval", copyValArenaFn);
            BasicBlock* aliasBB = BasicBlock::Create(ctx, "cv.alias", copyValArenaFn);
            BasicBlock* deepBB = BasicBlock::Create(ctx, "cv.deep", copyValArenaFn);
            llvm::Value* inTag = loadTag(b, in);
            b.CreateCondBr(b.CreateICmpEQ(inTag, i64C(5)), checkValBB, aliasBB);
            b.SetInsertPoint(checkValBB);
            llvm::Value* classId = b.CreateLoad(i64Ty, b.CreateIntToPtr(loadPay(b, in), ptrTy));
            llvm::Value* isVal = b.CreateCall(rtIsvalueclass, {classId});
            b.CreateCondBr(b.CreateICmpNE(isVal, i32C(0)), deepBB, aliasBB);
            b.SetInsertPoint(aliasBB);
            // non-value-struct source (or a non-object): alias, exactly like
            // lvrt_copyval's `*out = *in` fallback — no header, no recursion.
            copyLV(b, out, in);
            b.CreateRetVoid();
            b.SetInsertPoint(deepBB);
            llvm::Value* nslots = b.CreateCall(rtFieldcount, {classId});
            llvm::Value* payload = emitAllocPackedArena(b, copyValArenaFn, classId, nslots);
            // recurse field-by-field, source and dest at the SAME slot index —
            // a nested value-struct field lands on the arena too (dies with
            // the outer copy); anything else takes the alias branch above.
            llvm::Value* srcPayload = b.CreateIntToPtr(loadPay(b, in), ptrTy);
            llvm::Value* ctr = b.CreateAlloca(i64Ty);
            b.CreateStore(i64C(0), ctr);
            BasicBlock* loopCond = BasicBlock::Create(ctx, "cv.loop.cond", copyValArenaFn);
            BasicBlock* loopBody = BasicBlock::Create(ctx, "cv.loop.body", copyValArenaFn);
            BasicBlock* loopDone = BasicBlock::Create(ctx, "cv.loop.done", copyValArenaFn);
            b.CreateBr(loopCond);
            b.SetInsertPoint(loopCond);
            llvm::Value* i = b.CreateLoad(i64Ty, ctr);
            b.CreateCondBr(b.CreateICmpSLT(i, nslots), loopBody, loopDone);
            b.SetInsertPoint(loopBody);
            llvm::Value* off = b.CreateAdd(i64C(16), b.CreateMul(i, i64C(16)));
            llvm::Value* srcSlot = b.CreateGEP(i8Ty, srcPayload, off);
            llvm::Value* dstSlot = b.CreateGEP(i8Ty, payload, off);
            b.CreateCall(copyValArenaFn, {dstSlot, srcSlot});
            b.CreateStore(b.CreateAdd(i, i64C(1)), ctr);
            b.CreateBr(loopCond);
            b.SetInsertPoint(loopDone);
            storeTP(b, out, i64C(5) /* LV_OBJ */, b.CreatePtrToInt(payload, i64Ty));
            b.CreateRetVoid();
        }
    }

    // Emit the constant class table and lvrt_register(table, count, dispatch)
    // (doc-2 §2.6, full-struct shape per lv_abi.h). Method tables carry the
    // LV_M_* kind column ruled at the 2026-07-05 maintenance pass: getters,
    // setters, operators and plain methods dispatch ONLY within their kind.
    // techdesign-columnar §4.3/§4.4: emit the three descriptor symbols generated
    // code owes the runtime — lv_cfg_columnar (compile-time mode) and the per-class
    // lv_col_eligible / lv_col_typecode tables (as switch functions). Emitted in
    // BOTH modes so the runtime archive's extern refs resolve and the link graph is
    // mode-independent; flag-off => every class ineligible, lv_cfg_columnar == 0.
    void emitColumnarDescriptors() {
        bool cm = mod.columnar;
        new GlobalVariable(*module, i32Ty, /*isConstant=*/true, GlobalValue::ExternalLinkage,
                           i32C(cm ? 1 : 0), "lv_cfg_columnar");
        std::vector<std::pair<int, Symbol*>> elig;
        if (cm)
            for (auto& [sym, id] : clsIds)
                if (columnarEligibleStruct(sym)) elig.push_back({id, sym});

        {   // int32_t lv_col_eligible(int64_t classId)
            FunctionType* ft = FunctionType::get(i32Ty, {i64Ty}, false);
            Function* fn = Function::Create(ft, Function::ExternalLinkage,
                                            "lv_col_eligible", module.get());
            BasicBlock* entry = BasicBlock::Create(ctx, "entry", fn);
            BasicBlock* yes   = BasicBlock::Create(ctx, "yes", fn);
            BasicBlock* def   = BasicBlock::Create(ctx, "def", fn);
            IRBuilder<> fb(entry);
            SwitchInst* sw = fb.CreateSwitch(fn->getArg(0), def, (unsigned)elig.size());
            for (auto& [id, sym] : elig) { (void)sym; sw->addCase(cast<ConstantInt>(i64C(id)), yes); }
            IRBuilder<>(yes).CreateRet(i32C(1));
            IRBuilder<>(def).CreateRet(i32C(0));
        }
        {   // int32_t lv_col_typecode(int64_t classId, int64_t k)
            FunctionType* ft = FunctionType::get(i32Ty, {i64Ty, i64Ty}, false);
            Function* fn = Function::Create(ft, Function::ExternalLinkage,
                                            "lv_col_typecode", module.get());
            BasicBlock* entry = BasicBlock::Create(ctx, "entry", fn);
            BasicBlock* def   = BasicBlock::Create(ctx, "def", fn);
            IRBuilder<>(def).CreateRet(i32C(0));
            std::unordered_map<int, BasicBlock*> retBlocks;
            auto retBlockFor = [&](int tc) -> BasicBlock* {
                auto it = retBlocks.find(tc);
                if (it != retBlocks.end()) return it->second;
                BasicBlock* bb = BasicBlock::Create(ctx, "tc", fn);
                IRBuilder<>(bb).CreateRet(i32C(tc));
                retBlocks[tc] = bb; return bb;
            };
            IRBuilder<> fb(entry);
            SwitchInst* clsSw = fb.CreateSwitch(fn->getArg(0), def, (unsigned)elig.size());
            for (auto& [id, sym] : elig) {
                BasicBlock* clsBB = BasicBlock::Create(ctx, "cls", fn);
                std::vector<int> tcs;
                for (const Slot& s : sym->shape.slots)
                    if (!s.isMethod) tcs.push_back(columnarTypecodeOf(s.canonical));
                IRBuilder<> cb(clsBB);
                SwitchInst* kSw = cb.CreateSwitch(fn->getArg(1), def, (unsigned)tcs.size());
                for (size_t k = 0; k < tcs.size(); ++k)
                    kSw->addCase(cast<ConstantInt>(i64C((int64_t)k)), retBlockFor(tcs[k]));
                clsSw->addCase(cast<ConstantInt>(i64C(id)), clsBB);
            }
        }
    }

    void emitClassRegistration(IRBuilder<>& b) {
        // bug #35: install the spawn-body global-Promise checker (emitted with
        // lv_globals in genModule) alongside the class table, once at startup.
        if (spawnCheckFn) b.CreateCall(rtRegisterSpawnCheck, {spawnCheckFn});
        if (clsIds.empty()) {
            b.CreateCall(rtRegister, {nullPtr(), i64C(0), nullPtr()});
            return;
        }
        std::vector<std::pair<Symbol*, int>> classes(clsIds.begin(), clsIds.end());
        std::vector<Constant*> infos(classes.size());
        for (auto& [sym, id] : classes) {
            std::vector<std::string> keys = fieldKeysOf(sym);
            Constant* namesGv = nullPtr();
            if (!keys.empty()) {
                std::vector<Constant*> namePtrs;
                for (const std::string& k : keys) namePtrs.push_back(cstr(k));
                ArrayType* at = ArrayType::get(ptrTy, namePtrs.size());
                namesGv = new GlobalVariable(*module, at, true, GlobalValue::PrivateLinkage,
                                             ConstantArray::get(at, namePtrs), "slotnames");
            }
            // subtypeIds: transitive bases incl. self (lvrt_issub scans this)
            std::vector<Symbol*> bases;
            collectBases(sym, bases);
            std::vector<Constant*> subIds{i64C(id)};
            for (Symbol* bs : bases) subIds.push_back(i64C(classIdOf(bs)));
            ArrayType* st = ArrayType::get(i64Ty, subIds.size());
            Constant* subGv = new GlobalVariable(*module, st, true, GlobalValue::PrivateLinkage,
                                                 ConstantArray::get(st, subIds), "subtypes");
            // method table: name + IR fn index + kind, own-class-first order
            std::vector<Constant*> mNames, mIdx, mKinds;
            std::vector<const Stmt*> mem;
            collectMembersLG(sym, mem);
            for (const Stmt* m : mem) {
                if (m->isCtor || !mod.byDecl.count(m)) continue;
                int fi = mod.byDecl.at(m);
                if (fi >= (int)fns.size() || !fns[fi]) continue;
                int kind = m->isGet ? 1 : m->isSet ? 2 : m->selector.symbolic ? 3 : 0;
                // symbolic members carry their symbol in selector.text ("+",
                // "==", "[]"), not in name (which is empty) — the ratified
                // LV_M_OP contract keys operator dispatch on the symbol text
                // (lv_abi.h; X64Gen::genOpm reads selector.text the same way)
                mNames.push_back(cstr(m->selector.symbolic ? std::string(m->selector.text)
                                                           : std::string(m->name)));
                mIdx.push_back(i64C(fi));
                mKinds.push_back(ConstantInt::get(i8Ty, (uint8_t)kind));
            }
            Constant *mNamesGv = nullPtr(), *mIdxGv = nullPtr(), *mKindsGv = nullPtr();
            if (!mNames.empty()) {
                ArrayType* nt = ArrayType::get(ptrTy, mNames.size());
                mNamesGv = new GlobalVariable(*module, nt, true, GlobalValue::PrivateLinkage,
                                              ConstantArray::get(nt, mNames), "mnames");
                ArrayType* xt = ArrayType::get(i64Ty, mIdx.size());
                mIdxGv = new GlobalVariable(*module, xt, true, GlobalValue::PrivateLinkage,
                                            ConstantArray::get(xt, mIdx), "midx");
                ArrayType* kt = ArrayType::get(i8Ty, mKinds.size());
                mKindsGv = new GlobalVariable(*module, kt, true, GlobalValue::PrivateLinkage,
                                              ConstantArray::get(kt, mKinds), "mkinds");
            }
            Constant* info = ConstantStruct::get(classInfoTy, {
                cstr(std::string(sym->name)),
                i64C(id),
                i64C((int64_t)keys.size()),
                i32C(sym->isValueType() ? 1 : 0),
                namesGv,
                subGv,
                i64C((int64_t)subIds.size()),
                mNamesGv,
                mIdxGv,
                mKindsGv,
                i64C((int64_t)mNames.size()),
            });
            infos[id - 1] = info;   // id-1 index: dense, ordered table
        }
        ArrayType* tableTy = ArrayType::get(classInfoTy, infos.size());
        auto* tableGv = new GlobalVariable(*module, tableTy, true, GlobalValue::PrivateLinkage,
                                           ConstantArray::get(tableTy, infos), "lv_classtable");
        b.CreateCall(rtRegister, {tableGv, i64C((int64_t)infos.size()), dispatchFn});
    }

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

    void genFunction(int index) {
        const IrFunction& irfn = mod.functions[index];
        Function* fn = fns[index];
        IRBuilder<> b(ctx);

        BasicBlock* entry = BasicBlock::Create(ctx, "entry", fn);
        std::vector<BasicBlock*> blocks(irfn.code.size() + 1);
        for (size_t pc = 0; pc <= irfn.code.size(); ++pc)
            blocks[pc] = BasicBlock::Create(ctx, "pc" + std::to_string(pc), fn);

        // Jump-target map for the IndexStore rebind-chain peephole: the chain's
        // stale-temp release is only valid on straight-line code (X64Gen.cpp:3068).
        std::vector<bool> jumpTarget(irfn.code.size() + 1, false);
        for (const Inst& ji : irfn.code) {
            if (ji.op == Op::Jump && ji.a >= 0 && ji.a <= (int)irfn.code.size())
                jumpTarget[ji.a] = true;
            if ((ji.op == Op::JumpIfFalse || ji.op == Op::JumpIfTrue) &&
                ji.b >= 0 && ji.b <= (int)irfn.code.size())
                jumpTarget[ji.b] = true;
        }
        // A-M4: a catch handler entry is a jump target too (throwCheck branches
        // into it) — the IndexStore rebind-chain peephole must not fire across
        // one (X64Gen.cpp:3078-3080).
        for (const Handler& h : irfn.handlers)
            if (h.handlerPc >= 0 && h.handlerPc <= (int)irfn.code.size())
                jumpTarget[h.handlerPc] = true;

        // Widest CallValue window: one shared args buffer, hoisted to entry so
        // loops don't grow the stack.
        int maxArgs = 0;
        for (const Inst& in : irfn.code)
            // bug.md #2: the CallDyn field-closure fallback below also uses
            // this shared buffer (its dispatchFn call needs the same window
            // shape as CallValue), so its arity must be covered too — else a
            // fallback call site wider than every CallValue in the function
            // would GEP past the buffer.
            if ((in.op == Op::CallValue || in.op == Op::CallDyn) && in.d > maxArgs)
                maxArgs = in.d;

        // §9 A-M6 perf: most functions (fib, scalar-only code, ...) never hit
        // an arena-routable site at all — skip the save/restore call pair
        // entirely for them rather than paying two cross-.o calls per
        // invocation for nothing. Purely a fast path; usesArena==false means
        // restoreArena() below is a no-op and arenaMarkSlot is never read.
        bool usesArena = false;
        for (size_t pc = 0; pc < irfn.code.size() && !usesArena; ++pc) {
            const Inst& in = irfn.code[pc];
            bool candidate = (in.op == Op::CopyVal && in.c == 1) ||
                            (in.op == Op::NewObject && in.c != 1 && in.sym && in.sym->isValue);
            if (candidate && ownership.scopeOwned.count({index, (int)pc})) usesArena = true;
        }

        b.SetInsertPoint(entry);
        std::vector<llvm::Value*> regs(irfn.nregs);
        for (int r = 0; r < irfn.nregs; ++r) regs[r] = b.CreateAlloca(lvTy);
        // §4.3 rule 1: every register must be observably void before its first
        // write, or the ARC wrap's first "release old" reads garbage.
        for (int r = 0; r < irfn.nregs; ++r) storeVoid(b, regs[r]);
        llvm::Value* retSlot = fn->getArg(0);
        for (int p = 0; p < irfn.nparams; ++p) copyLV(b, regs[p], fn->getArg(1 + p));
        // §15 ARC (X64Gen.cpp:2963-2969): the callee OWNS its parameter slots, so
        // retain each on entry; the frame-exit releaseAllRegs balances it. Args
        // arrive at +0 (the caller never retains for the call — it keeps its own
        // register's count), so each value stays counted once per live slot.
        for (int p = 0; p < irfn.nparams; ++p) b.CreateCall(rtRetain, {regs[p]});
        // scratch cells: arcScratch holds a wrapped op's stashed "old" value
        // across the release/retain sequence; callRetScratch is a throwaway
        // out-param (kept void/valid at all times — it doubles as the padding
        // arg for arity-short dynamic dispatch); lvA/lvB are op-local LvValue
        // temps; ctrSlot is the loop counter for inline native loops.
        llvm::Value* arcScratch = b.CreateAlloca(lvTy);
        llvm::Value* callRetScratch = b.CreateAlloca(lvTy);
        llvm::Value* lvA = b.CreateAlloca(lvTy);
        llvm::Value* lvB = b.CreateAlloca(lvTy);
        // A-M4: a peek slot for the in-flight thrown value during a throwCheck
        // (lvrt_thrown copies g_thrown here so codegen can read its tag/classId).
        llvm::Value* thrownSlot = b.CreateAlloca(lvTy);
        llvm::Value* ctrSlot = b.CreateAlloca(i64Ty);
        // bug.md #19: scratch accumulator for Map.without()'s two-pass scan
        // (found-index, then compacted dest-index) — same "shared, sequential
        // (never nested)" discipline as ctrSlot above.
        llvm::Value* delIdxSlot = b.CreateAlloca(i64Ty);
        llvm::Value* argsBuf = maxArgs > 0
            ? b.CreateAlloca(ArrayType::get(lvTy, maxArgs)) : nullptr;
        storeVoid(b, callRetScratch);
        storeVoid(b, arcScratch);
        // §9 A-M6 arena tier: watermark this frame's arena-tier allocations
        // (X64Gen.cpp:3148's saved arena cursor). Every return/unwind path
        // resets to this mark, bulk-freeing whatever scope-owned CopyVal/
        // NewObject(isValue) sites (below) allocated in between — the fix
        // for the two value-struct churn XFAILs (doc-1 §9).
        // §9b A-M7: the mark is now a direct load/store of the exported
        // lv_g_arena_cursor pointer word (doc-2 §2.10) instead of a call pair
        // to lvrt_arena_save/lvrt_arena_restore — both cross-.o calls per
        // value-struct-allocating invocation. arenaMarkSlot holds the raw
        // pointer (ptrTy), matching the global's type directly.
        llvm::Value* arenaMarkSlot = usesArena ? b.CreateAlloca(ptrTy) : nullptr;
        if (usesArena) b.CreateStore(b.CreateLoad(ptrTy, gArenaCursor), arenaMarkSlot);
        b.CreateBr(blocks[0]);

        // §9 A-M6 perf: inline lvrt_truth's tag==BOOL && payload!=0 test — hit
        // on every JumpIfFalse/JumpIfTrue (every `if`/loop condition).
        auto truth = [&](llvm::Value* v) {
            llvm::Value* isBool = b.CreateICmpEQ(loadTag(b, v), i64C(3));
            llvm::Value* nz = b.CreateICmpNE(loadPay(b, v), i64C(0));
            return b.CreateAnd(isBool, nz);
        };
        auto newBB = [&](const char* name) { return BasicBlock::Create(ctx, name, fn); };

        // for (i = lo; i < hi; ++i) body(i) — counter in ctrSlot (shared; inline
        // loops never nest).
        auto emitLoop = [&](llvm::Value* lo, llvm::Value* hi, bool inclusive,
                            const std::function<void(llvm::Value*)>& body) {
            b.CreateStore(lo, ctrSlot);
            BasicBlock* condBB = newBB("loop.cond");
            BasicBlock* bodyBB = newBB("loop.body");
            BasicBlock* doneBB = newBB("loop.done");
            b.CreateBr(condBB);
            b.SetInsertPoint(condBB);
            llvm::Value* i = b.CreateLoad(i64Ty, ctrSlot);
            llvm::Value* cond = inclusive ? b.CreateICmpSLE(i, hi) : b.CreateICmpSLT(i, hi);
            b.CreateCondBr(cond, bodyBB, doneBB);
            b.SetInsertPoint(bodyBB);
            body(i);
            llvm::Value* i2 = b.CreateLoad(i64Ty, ctrSlot);   // body may not clobber it
            b.CreateStore(b.CreateAdd(i2, i64C(1)), ctrSlot);
            b.CreateBr(condBB);
            b.SetInsertPoint(doneBB);
        };

        // The generic ARC wrap (§4.1/§4.2): stash the dest register's old
        // value, run `body` (which overwrites it), then release the old value
        // and — for dk==1 only — retain the new one, UNLESS old and new are
        // bit-identical (a self-assign / in-place mutation that legitimately
        // returns the same pointer: release-then-retain on that one pointer
        // would free it out from under itself before the retain "revives" a
        // now-dangling address).
        auto wrapDest = [&](Op op, int destReg, const std::function<void()>& body) {
            int dk = destKind(op);
            if (dk == 0) { body(); return; }
            llvm::Value* oldTag = loadTag(b, regs[destReg]);
            llvm::Value* oldPay = loadPay(b, regs[destReg]);
            body();
            llvm::Value* newTag = loadTag(b, regs[destReg]);
            llvm::Value* newPay = loadPay(b, regs[destReg]);
            llvm::Value* same = b.CreateAnd(b.CreateICmpEQ(oldTag, newTag),
                                      b.CreateICmpEQ(oldPay, newPay));
            BasicBlock* diffBB = newBB("arcdiff");
            BasicBlock* contBB = newBB("arccont");
            b.CreateCondBr(same, contBB, diffBB);
            b.SetInsertPoint(diffBB);
            storeTP(b, arcScratch, oldTag, oldPay);
            b.CreateCall(rtRelease, {arcScratch});
            if (dk == 1) b.CreateCall(rtRetain, {regs[destReg]});
            b.CreateBr(contBB);
            b.SetInsertPoint(contBB);
        };
        auto releaseAllRegs = [&]() {
            for (int r = 0; r < irfn.nregs; ++r) b.CreateCall(rtRelease, {regs[r]});
        };
        // §9 A-M6: bulk-free this frame's arena-tier allocations (X64Gen.cpp:
        // 3151's restoreArena). Called at every return AND unwind path, after
        // releaseAllRegs — matching X64Gen's Ret/RetVoid ordering exactly.
        // §9b A-M7: lvrt_arena_restore is exactly `cursor = mark` (no
        // poisoning, no bookkeeping — doc-2 §2.10), so this is a direct
        // store back to the exported global instead of a call.
        auto restoreArena = [&]() {
            if (usesArena) b.CreateStore(b.CreateLoad(ptrTy, arenaMarkSlot), gArenaCursor);
        };

        // A-M4 checked-call dispatch (the pending-throw model, X64Gen::throwCheck
        // at src/X64Gen.cpp:3008-3034). Emitted INSIDE a call-like op's wrapDest
        // body, right after the call: on a pending throw, dispatch to the first
        // covering handler whose clause type matches (lvrt_issub in clause order),
        // binding the value via lvrt_catch_bind; else propagate by jumping to the
        // frame's void-return block (releaseAllRegs runs there — the thrown value
        // survives because lvrt_throw_set retained it into g_thrown). On the
        // NOT-throwing path the builder is left at `contBB`, so wrapDest's tail
        // (release-old / retain-new) runs normally; on a throw we branch away
        // BEFORE that tail — the stale dest ref is dropped by releaseAllRegs at
        // the unwind point instead (X64Gen leaks it identically; output-neutral).
        auto emitThrowCheck = [&](int pc) {
            // §9b A-M7: load the exported hot word directly instead of calling
            // lvrt_throwing() (an un-inlinable cross-.o call after every
            // call-like op) — one edit site, every checked op inherits it.
            llvm::Value* flag = b.CreateLoad(i32Ty, gThrowing);
            BasicBlock* dispBB = newBB("thr.disp");
            BasicBlock* contBB = newBB("thr.cont");
            b.CreateCondBr(b.CreateICmpNE(flag, i32C(0)), dispBB, contBB);
            b.SetInsertPoint(dispBB);
            b.CreateCall(rtThrown, {thrownSlot});          // peek (no transfer)
            llvm::Value* tag = loadTag(b, thrownSlot);
            for (const Handler& h : irfn.handlers) {
                if (pc < h.start || pc >= h.end) continue;
                // A thrown value must be an object (tag 5) to satisfy a class
                // clause — X64Gen tests this even for an untyped catch, so a
                // non-object throw is never caught (matches the frozen backend).
                BasicBlock* tagOkBB = newBB("thr.tagok");
                BasicBlock* nextBB = newBB("thr.next");
                b.CreateCondBr(b.CreateICmpEQ(tag, i64C(5)), tagOkBB, nextBB);
                b.SetInsertPoint(tagOkBB);
                if (h.type) {
                    llvm::Value* hdr = b.CreateIntToPtr(loadPay(b, thrownSlot), ptrTy);
                    llvm::Value* cid = b.CreateLoad(i64Ty, hdr);   // header[0] = classId
                    llvm::Value* sub = b.CreateCall(rtIssub, {cid, i64C(classIdOf(h.type))});
                    BasicBlock* matchBB = newBB("thr.match");
                    b.CreateCondBr(b.CreateICmpNE(sub, i32C(0)), matchBB, nextBB);
                    b.SetInsertPoint(matchBB);
                }
                // Matched: clear the flag, release the previous binding, transfer
                // the thrown value into the bind register (all inside catch_bind).
                b.CreateCall(rtCatchBind, {regs[h.bindReg]});
                b.CreateBr(blocks[h.handlerPc]);
                b.SetInsertPoint(nextBB);
            }
            // No covering handler matched -> propagate to the frame's void return.
            b.CreateBr(blocks[irfn.code.size()]);
            b.SetInsertPoint(contBB);
        };

        // Effective classId of a receiver for name-based dispatch: tag-5 reads
        // the object header; primitive/container tags map to their mask class
        // (X64Gen::genCallM's tagToId chain).
        auto effClassId = [&](llvm::Value* recv) -> llvm::Value* {
            llvm::Value* tag = loadTag(b, recv);
            BasicBlock* objBB = newBB("eff.obj");
            BasicBlock* elseBB = newBB("eff.mask");
            BasicBlock* joinBB = newBB("eff.join");
            b.CreateCondBr(b.CreateICmpEQ(tag, i64C(5)), objBB, elseBB);
            b.SetInsertPoint(objBB);
            llvm::Value* hdr = b.CreateIntToPtr(loadPay(b, recv), ptrTy);
            llvm::Value* fromObj = b.CreateLoad(i64Ty, hdr);
            b.CreateBr(joinBB);
            b.SetInsertPoint(elseBB);
            auto maskId = [&](const char* n) -> llvm::Value* { return i64C(classIdByName(n)); };
            llvm::Value* v = i64C(0);
            v = b.CreateSelect(b.CreateICmpEQ(tag, i64C(2)), maskId("float"), v);
            v = b.CreateSelect(b.CreateICmpEQ(tag, i64C(3)), maskId("bool"), v);
            v = b.CreateSelect(b.CreateICmpEQ(tag, i64C(1)), maskId("int"), v);
            v = b.CreateSelect(b.CreateICmpEQ(tag, i64C(4)), maskId("string"), v);
            v = b.CreateSelect(b.CreateICmpEQ(tag, i64C(7)), maskId("Map"), v);
            v = b.CreateSelect(b.CreateICmpEQ(tag, i64C(6)), maskId("Array"), v);
            v = b.CreateSelect(b.CreateICmpEQ(tag, i64C(10)), maskId("char"), v);   // Track 03 §1
            b.CreateBr(joinBB);
            b.SetInsertPoint(joinBB);
            PHINode* phi = b.CreatePHI(i64Ty, 2);
            phi->addIncoming(fromObj, objBB);
            phi->addIncoming(v, elseBB);
            return phi;
        };

        // In-language dispatch candidates for a method name: first matching
        // member per candidate class, X64Gen::genCallM's class order and
        // filters (plain methods only — never ctors/accessors/operators).
        auto callmCandidates = [&](const std::string& name, int argc) {
            std::vector<std::pair<int, int>> cands;   // (classId, fnIndex)
            for (Symbol* cls : callmClasses) {
                // Iterate the class's EFFECTIVE method slots (`shape.slots`),
                // which already hold the collapse/override winner per resolved
                // member (info.md §4). The old raw inheritance walk
                // (`collectMembersLG`) with first-arity-match-wins relied on "own
                // decls before inherited", which is correct only for single
                // inheritance: for `Panel : Container, Bordered` where a SIBLING
                // mixin `Bordered` overrides `kind`, the walk listed the base
                // `Widget::kind` first and picked it over the override (bug.md
                // #56). shape.slots is override-correct and still dedups
                // overloads by signature, so the arity filter below stays valid.
                //
                // bug.md #13/#27: still require an EXACT arity match when the
                // call site's arg count is known — an in-language method whose
                // name also has a native overload of another arity (e.g.
                // `int.toString(radix)` beside native `int.toString()`) must not
                // be chosen for a mismatched-arity call. argc < 0 keeps the
                // legacy first-name-match.
                int firstFi = -1, arityFi = -1;
                for (const Slot& sl : cls->shape.slots) {
                    if (!sl.isMethod || !sl.decl) continue;
                    const Stmt* m = sl.decl;
                    if (m->isCtor || m->isGet || m->isSet || m->selector.symbolic) continue;
                    if (std::string(m->name) != name) continue;
                    if (!mod.byDecl.count(m)) continue;
                    int fi = mod.byDecl.at(m);
                    if (fi >= (int)fns.size() || !fns[fi]) continue;
                    if (firstFi < 0) firstFi = fi;
                    if (argc >= 0 && arityFi < 0 && mod.functions[fi].nparams - 1 == argc)
                        arityFi = fi;
                }
                int chosen = (argc < 0) ? firstFi : arityFi;
                if (chosen >= 0) cands.push_back({classIdOf(cls), chosen});
            }
            return cands;
        };

        // ---- the native method rows (X64Gen::genCallNative, verbatim port
        // incl. ownership: every heap result honors the CallDyn +1 transfer
        // contract; unboxed results carry no ARC). Emits guard-per-(tag,name)
        // blocks; every taken row jumps to doneBB. Falls through with the
        // builder on the final "no row matched" path (caller writes void).
        // `consumed` is the IR op's in.b flag (COW self-append). Native rows
        // that return a fresh copy must release the transferred receiver ref;
        // rows that reuse or internally free the base have already taken its
        // fate. Guarding this INSIDE the container-tagged row (not at the
        // generic call site) is required: only there is the receiver's runtime
        // representation and copy/reuse contract known.
        auto emitNativeRows = [&](const std::string& n, llvm::Value* dst,
                                  llvm::Value* recv, int argBase, int argc,
                                  BasicBlock* doneBB, bool consumed) {
            auto arg = [&](int k) -> llvm::Value* {
                return k < argc ? regs[argBase + k] : callRetScratch;
            };
            llvm::Value* rtag = loadTag(b, recv);
            auto row = [&](int tagK, const std::function<void()>& genRow) {
                BasicBlock* hit = newBB("nat.hit");
                BasicBlock* next = newBB("nat.next");
                b.CreateCondBr(b.CreateICmpEQ(rtag, i64C(tagK)), hit, next);
                b.SetInsertPoint(hit);
                genRow();
                b.CreateBr(doneBB);
                b.SetInsertPoint(next);
            };
            auto lenOf = [&](llvm::Value* lv) {   // [payload+0]
                return b.CreateLoad(i64Ty, b.CreateIntToPtr(loadPay(b, lv), ptrTy));
            };
            auto retainDst = [&]() { b.CreateCall(rtRetain, {dst}); };
            auto storeFloatCall = [&](llvm::Value* result) {
                storeTP(b, dst, i64C(2), b.CreateBitCast(result, i64Ty));
            };
            auto floatIntrinsic = [&](Intrinsic::ID iid, std::initializer_list<llvm::Value*> args) {
                llvm::Function* f = Intrinsic::getDeclaration(module.get(), iid, {f64Ty});
                storeFloatCall(b.CreateCall(f, args));
            };

            if (n == "length") {
                row(4, [&] { storeTP(b, dst, i64C(1), lenOf(recv)); });
                row(6, [&] {   // mask the dense-array marker bit
                    llvm::Value* raw = lenOf(recv);
                    // clear BOTH array top bits (dense marker 63 + columnar marker 62)
                    storeTP(b, dst, i64C(1), b.CreateAnd(raw, i64C(0x3fffffffffffffffLL)));
                });
                row(7, [&] { storeTP(b, dst, i64C(1), lenOf(recv)); });
                row(11, [&] {   // Track 03 §3 Block: len at body offset 16 (§2.4)
                    storeTP(b, dst, i64C(1), b.CreateLoad(i64Ty, payAddr(b, recv, i64C(16))));
                });
            } else if (n == "isEmpty") {
                row(4, [&] {
                    llvm::Value* z = b.CreateICmpEQ(lenOf(recv), i64C(0));
                    storeTP(b, dst, i64C(3), b.CreateZExt(z, i64Ty));
                });
            } else if (n == "toString") {
                row(4, [&] { copyLV(b, dst, recv); retainDst(); });   // receiver, retained (+1)
                row(1, [&] {
                    b.CreateCall(rtIntToStr, {dst, loadPay(b, recv)});
                    retainDst();
                });
                row(3, [&] {
                    llvm::Value* t = stringGlobal(b, "true");
                    llvm::Value* f = stringGlobal(b, "false");
                    llvm::Value* c = b.CreateICmpNE(loadPay(b, recv), i64C(0));
                    // static literals: ARC-exempt, no retain (region check no-ops it)
                    storeTP(b, dst, i64C(4), b.CreateSelect(c, t, f));
                });
                row(2, [&] {   // float -> "%f" fresh string (lvrt_to_string parity
                    b.CreateCall(rtToString, {dst, recv});   // with valueToString, +1)
                    retainDst();
                });
                row(10, [&] {   // Track 03 §1 char: UTF-8 encode -> fresh string (+1)
                    b.CreateCall(rtCharToStr, {dst, loadPay(b, recv)});
                    retainDst();
                });
                row(11, [&] {   // Track 03 §3 Block.toString(off, len) -> fresh string (+1)
                    b.CreateCall(rtBlockTostring,
                                 {dst, recv, loadPay(b, arg(0)), loadPay(b, arg(1))});
                    retainDst();
                });
            } else if (n == "indexOf") {
                row(4, [&] {
                    storeTP(b, dst, i64C(1), b.CreateCall(rtStrIndexof, {recv, arg(0)}));
                });
            } else if (n == "contains") {
                row(4, [&] {
                    llvm::Value* i = b.CreateCall(rtStrIndexof, {recv, arg(0)});
                    storeTP(b, dst, i64C(3),
                            b.CreateZExt(b.CreateICmpSGE(i, i64C(0)), i64Ty));
                });
            } else if (n == "subStr") {
                row(4, [&] {
                    b.CreateCall(rtStrSubstr, {dst, recv, loadPay(b, arg(0)), loadPay(b, arg(1))});
                    retainDst();
                });
            } else if (n == "charAt") {
                row(4, [&] {   // charAt(i) == subStr(i, 1)
                    b.CreateCall(rtStrSubstr, {dst, recv, loadPay(b, arg(0)), i64C(1)});
                    retainDst();
                });
            } else if (n == "toInt") {
                row(4, [&] { b.CreateCall(rtStrToint, {dst, recv}); });
                // Track 06: float.toInt() — truncation; NaN/±inf/out-of-
                // int64-range raises (loud, §1.2 of the design). Range-check
                // BEFORE fptosi: an out-of-range/NaN input is POISON through
                // that instruction, unlike X64Gen's hardware cvttsd2si (which
                // at least yields a defined, if ambiguous, sentinel) — so the
                // guard is load-bearing here, not just belt-and-suspenders.
                // Ordered-and-in-range via OGE/OLT: NaN is unordered, so both
                // compares are false and the raise path fires for free (same
                // honesty argument as kPrelude's isNaN/isInfinite).
                row(2, [&] {
                    llvm::Value* x = b.CreateBitCast(loadPay(b, recv), f64Ty);
                    llvm::Value* lo = ConstantFP::get(f64Ty, -9223372036854775808.0);
                    llvm::Value* hi = ConstantFP::get(f64Ty, 9223372036854775808.0);
                    llvm::Value* ok = b.CreateAnd(b.CreateFCmpOGE(x, lo), b.CreateFCmpOLT(x, hi));
                    BasicBlock* okBB = newBB("toint.ok");
                    BasicBlock* badBB = newBB("toint.bad");
                    BasicBlock* doneToInt = newBB("toint.done");
                    b.CreateCondBr(ok, okBB, badBB);
                    b.SetInsertPoint(badBB);
                    b.CreateCall(rtRaise, {cstr("float is not finite or out of int64 range for toInt()")});
                    storeVoid(b, dst);
                    b.CreateBr(doneToInt);
                    b.SetInsertPoint(okBB);
                    storeTP(b, dst, i64C(1), b.CreateFPToSI(x, i64Ty));
                    b.CreateBr(doneToInt);
                    b.SetInsertPoint(doneToInt);
                });
            } else if (n == "toFloat") {
                row(4, [&] { b.CreateCall(rtStrTofloat, {dst, recv}); });
                row(1, [&] {   // Track 06: int.toFloat() — exact, sitofp direct
                    llvm::Value* d = b.CreateSIToFP(loadPay(b, recv), f64Ty);
                    storeTP(b, dst, i64C(2), b.CreateBitCast(d, i64Ty));
                });
            } else if (n == "floor" || n == "ceil" || n == "round" || n == "trunc") {
                // Track 06: unlike X64Gen's SSE (no half-away-from-zero mode
                // in hardware, problem #1), LLVM's `llvm.round` intrinsic IS
                // specified as half-away-from-zero (matches C `round`
                // exactly) — no manual copysign composition needed here.
                row(2, [&] {
                    llvm::Value* x = b.CreateBitCast(loadPay(b, recv), f64Ty);
                    Intrinsic::ID iid = n == "floor" ? Intrinsic::floor
                                       : n == "ceil"  ? Intrinsic::ceil
                                       : n == "round" ? Intrinsic::round
                                                      : Intrinsic::trunc;
                    llvm::Function* f = Intrinsic::getDeclaration(module.get(), iid, {f64Ty});
                    storeTP(b, dst, i64C(2), b.CreateBitCast(b.CreateCall(f, {x}), i64Ty));
                });
            } else if (n == "sqrt") {
                row(2, [&] {   // negative -> NaN (IEEE, not a throw)
                    llvm::Value* x = b.CreateBitCast(loadPay(b, recv), f64Ty);
                    llvm::Function* f = Intrinsic::getDeclaration(module.get(), Intrinsic::sqrt, {f64Ty});
                    storeTP(b, dst, i64C(2), b.CreateBitCast(b.CreateCall(f, {x}), i64Ty));
                });
            } else if (n == "pow") {
                // Track 06 deferred tail: `float.pow(float)` is now live on
                // LLVM. `llvm.pow.f64` lowers through the target/libm path the
                // backend already links for the runtime's float `%`.
                row(2, [&] {
                    llvm::Value* x = b.CreateBitCast(loadPay(b, recv), f64Ty);
                    llvm::Value* y = b.CreateBitCast(loadPay(b, arg(0)), f64Ty);
                    floatIntrinsic(Intrinsic::pow, {x, y});
                });
            } else if (n == "byteAt") {
                row(4, [&] { b.CreateCall(rtStrByteat, {dst, recv, loadPay(b, arg(0))}); });
                row(11, [&] {   // Track 03 §3 Block.byteAt(i) -> int (bounds-checked)
                    b.CreateCall(rtBlockByteat, {dst, recv, loadPay(b, arg(0))});
                });
            } else if (n == "trim") {
                row(4, [&] { b.CreateCall(rtStrTrim, {dst, recv}); retainDst(); });
            } else if (n == "toUpper" || n == "toLower") {
                row(4, [&] {
                    b.CreateCall(rtStrCase, {dst, recv, i32C(n == "toLower" ? 1 : 0)});
                    retainDst();
                });
            } else if (n == "startsWith" || n == "endsWith") {
                row(4, [&] {   // probe = subStr(s, a, plen), bytewise equality
                    llvm::Value* plen = lenOf(arg(0));
                    llvm::Value* start = n == "startsWith"
                        ? (llvm::Value*)i64C(0)
                        : b.CreateSub(lenOf(recv), plen);   // neg -> substr yields ""
                    b.CreateCall(rtStrSubstr, {lvA, recv, start, plen});
                    llvm::Value* eq = b.CreateCall(rtStrEq, {lvA, arg(0)});
                    emitStrTempFree(b, lvA);   // drop the transient probe
                    storeTP(b, dst, i64C(3), b.CreateZExt(b.CreateICmpNE(eq, i32C(0)), i64Ty));
                });
            } else if (n == "at") {
                row(4, [&] {   // Track 03 §1: string.at(i) decodes a scalar -> char
                    b.CreateCall(rtStrAt, {dst, recv, loadPay(b, arg(0))});  // char immediate: no ARC
                });
                row(6, [&] {
                    llvm::Value* idx = loadPay(b, arg(0));
                    llvm::Value* raw = lenOf(recv);
                    llvm::Value* len = b.CreateAnd(raw, i64C(0x3fffffffffffffffLL));  // mask dense+columnar bits
                    llvm::Value* oob = b.CreateOr(b.CreateICmpSGE(idx, len),
                                                  b.CreateICmpSLT(idx, i64C(0)));
                    BasicBlock* oobBB = newBB("at.oob");
                    BasicBlock* okBB = newBB("at.ok");
                    BasicBlock* atDone = newBB("at.done");
                    b.CreateCondBr(oob, oobBB, okBB);
                    b.SetInsertPoint(oobBB);
                    b.CreateCall(rtRaiseOob, {idx, len});
                    storeVoid(b, dst);
                    b.CreateBr(atDone);
                    b.SetInsertPoint(okBB);
                    BasicBlock* denseBB = newBB("at.dense");
                    BasicBlock* boxedBB = newBB("at.boxed");
                    BasicBlock* colBB = newBB("at.col");
                    BasicBlock* nbBB = newBB("at.notboxed");
                    BasicBlock* loadedBB = newBB("at.loaded");
                    b.CreateCondBr(b.CreateICmpSLT(raw, i64C(0)), nbBB, boxedBB);
                    // not boxed: columnar (bit 62) gathers via the runtime; dense
                    // (bit 62 clear) uses the inline record-pointer trick (§5.2/§5.5).
                    b.SetInsertPoint(nbBB);
                    b.CreateCondBr(
                        b.CreateICmpNE(b.CreateAnd(raw, i64C(0x4000000000000000LL)), i64C(0)),
                        colBB, denseBB);
                    b.SetInsertPoint(colBB);
                    b.CreateCall(rtIdxGet, {dst, recv, arg(0)});   // gather: fresh rc=0 record
                    b.CreateBr(loadedBB);
                    b.SetInsertPoint(denseBB);
                    llvm::Value* recBytes = b.CreateLoad(i64Ty, payAddr(b, recv, i64C(8)));
                    llvm::Value* recOff = b.CreateAdd(i64C(16), b.CreateMul(idx, recBytes));
                    llvm::Value* recAddr = b.CreatePtrToInt(payAddr(b, recv, recOff), i64Ty);
                    storeTP(b, dst, i64C(5), recAddr);
                    b.CreateBr(loadedBB);
                    b.SetInsertPoint(boxedBB);
                    llvm::Value* off = b.CreateAdd(i64C(8), b.CreateMul(idx, i64C(16)));
                    // bug #74: a boxed element that is a VALUE STRUCT (the #66 case —
                    // a struct with heap fields lives boxed and is individually
                    // freed) must be returned as a FRESH OWNED copy, not a bare
                    // alias. `.at()`'s result is treated as an owned temp by the
                    // lowerer (§15: `MediaRange r = arr.at(i)` CopyVals it into `r`
                    // then VFrees the temp) — VFree'ing a bare alias reclaims the
                    // ARRAY's own rc=0 element, leaving a dangling pointer that is
                    // freed again later (freelist corruption, SIGSEGV away from
                    // site). lvrt_copyval deep-copies a value struct (fresh rc=0)
                    // and is `*out=*in` for a reference — so the retain below still
                    // does the +1 transfer for reference elements, unchanged. This
                    // matches the columnar path (rtIdxGet gathers a fresh record)
                    // and the for-in fix (Lower.cpp §bug #66).
                    b.CreateCall(rtCopyval, {dst, payAddr(b, recv, off)});
                    b.CreateBr(loadedBB);
                    b.SetInsertPoint(loadedBB);
                    b.CreateCall(rtRetain, {dst});   // ref element -> +1 transfer; value struct: no-op
                    b.CreateBr(atDone);
                    b.SetInsertPoint(atDone);
                });
                row(7, [&] {
                    b.CreateCall(rtIdxGet, {dst, recv, arg(0)});
                    retainDst();   // borrowed entry ref -> +1 transfer
                });
            } else if (n == "add") {
                row(6, [&] {
                    // bug.md #31: lvrt_arr_append takes a consumed base's fate
                    // on rc==1 (reuse in place, or grow and free internally),
                    // but rc>=2 copies and leaves the base untouched. Snapshot
                    // the count BEFORE the call so the unique grow path cannot
                    // leave us reading its freed header; on the shared path,
                    // release exactly the one receiver reference transferred
                    // by CallDyn before its window slot is cleared.
                    llvm::Value* shared = nullptr;
                    if (consumed) {
                        llvm::Value* body = b.CreateIntToPtr(loadPay(b, recv), ptrTy);
                        llvm::Value* hdr = b.CreateGEP(i8Ty, body, i64C(-16));
                        llvm::Value* rc = b.CreateLoad(i64Ty, hdr);
                        shared = b.CreateICmpSGT(rc, i64C(1));
                    }
                    b.CreateCall(rtArrAppend, {dst, recv, arg(0)});
                    if (consumed) {
                        BasicBlock* relBB = newBB("add.consrel");
                        BasicBlock* afterBB = newBB("add.consafter");
                        b.CreateCondBr(shared, relBB, afterBB);
                        b.SetInsertPoint(relBB);
                        b.CreateCall(rtRelease, {recv});
                        b.CreateBr(afterBB);
                        b.SetInsertPoint(afterBB);
                    }
                });
            } else if (n == "concatAll") {
                row(6, [&] { b.CreateCall(rtArrConcatall, {dst, recv}); retainDst(); });
            } else if (n == "has") {
                row(7, [&] {   // scan for key; Track 05 C3 equality (lvrt_keyeq)
                    storeTP(b, dst, i64C(3), i64C(0));
                    llvm::Value* len = lenOf(recv);
                    BasicBlock* hitBB = newBB("has.hit");
                    BasicBlock* scanDone = newBB("has.done");
                    emitLoop(i64C(0), len, false, [&](llvm::Value* i) {
                        llvm::Value* off = b.CreateAdd(i64C(8), b.CreateMul(i, i64C(32)));
                        llvm::Value* kAddr = payAddr(b, recv, off);
                        BasicBlock* cont = newBB("has.cont");
                        llvm::Value* eq = b.CreateCall(rtKeyEq, {kAddr, arg(0)});
                        b.CreateCondBr(b.CreateICmpNE(eq, i32C(0)), hitBB, cont);
                        b.SetInsertPoint(cont);
                    });
                    b.CreateBr(scanDone);
                    b.SetInsertPoint(hitBB);
                    storeTP(b, dst, i64C(3), i64C(1));
                    b.CreateBr(scanDone);
                    b.SetInsertPoint(scanDone);
                });
            } else if (n == "keys" || n == "values") {
                row(7, [&] {   // fresh array of entry keys/values, +1, elements retained
                    int fieldOff = n == "keys" ? 0 : 16;
                    llvm::Value* len = lenOf(recv);
                    b.CreateCall(rtArrNew, {lvA, len});
                    emitLoop(i64C(0), len, false, [&](llvm::Value* i) {
                        llvm::Value* srcOff = b.CreateAdd(i64C(8 + fieldOff),
                                                          b.CreateMul(i, i64C(32)));
                        llvm::Value* src = payAddr(b, recv, srcOff);
                        llvm::Value* dstOff = b.CreateAdd(i64C(8), b.CreateMul(i, i64C(16)));
                        llvm::Value* dstSlot = payAddr(b, lvA, dstOff);
                        copyLV(b, dstSlot, src);
                        b.CreateCall(rtRetain, {dstSlot});   // the array owns each copied ref
                    });
                    b.CreateCall(rtRetain, {lvA});           // the array itself: +1 transfer
                    copyLV(b, dst, lvA);
                });
            } else if (n == "with") {
                // bug.md #30: pure add/update — CGen.cpp:314's `if (m == "with")`
                // ported. Under the CallDyn (+1 transfer) convention the result
                // must cross the boundary OWNED; lv_map_upsert alone leaves a
                // fresh copy at rc 0 (correct for IndexStore/dk==1, which retains
                // on top, but wrong here — the map is freed at the first return).
                // rtMapWith wraps lv_map_upsert and retains a fresh copy. See
                // designs/complete/techdesign-bug30-map-with-ownership.md §4.
                row(7, [&] {
                    b.CreateCall(rtMapWith, {dst, recv, arg(0), arg(1)});
                    if (consumed) {
                        // `m = m.with(...)`: the callee owns the receiver's fate,
                        // but lv_map_upsert never takes it. In-place COW returned
                        // the SAME payload — the consumed +1 IS the result's
                        // count, nothing to do. A fresh copy retained its own
                        // entries, so release exactly the one consumed receiver
                        // ref here (the CallDyn tail then clears the window slot
                        // without a second release).
                        llvm::Value* same = b.CreateICmpEQ(loadPay(b, dst), loadPay(b, recv));
                        BasicBlock* relBB = newBB("with.consrel");
                        BasicBlock* afterBB = newBB("with.consafter");
                        b.CreateCondBr(same, afterBB, relBB);
                        b.SetInsertPoint(relBB);
                        b.CreateCall(rtRelease, {recv});
                        b.CreateBr(afterBB);
                        b.SetInsertPoint(afterBB);
                    }
                });
            } else if (n == "without") {
                // bug.md #19: CGen.cpp:315's `if (m == "without")` ported. No
                // shared runtime helper exists for this one (unlike `with`,
                // which reuses lvrt_idxset) — two sequential scans (never
                // nested, per emitLoop's own discipline): first find the
                // matching key's slot (maps have unique keys, so at most one
                // entry is ever removed), then copy every OTHER entry into a
                // fresh, densely packed map.
                row(7, [&] {
                    llvm::Value* len = lenOf(recv);
                    b.CreateStore(i64C(-1), delIdxSlot);
                    emitLoop(i64C(0), len, false, [&](llvm::Value* i) {
                        llvm::Value* off = b.CreateAdd(i64C(8), b.CreateMul(i, i64C(32)));
                        llvm::Value* kAddr = payAddr(b, recv, off);
                        llvm::Value* eq = b.CreateCall(rtKeyEq, {kAddr, arg(0)});
                        BasicBlock* hitBB = newBB("wo.hit");
                        BasicBlock* contBB = newBB("wo.cont");
                        b.CreateCondBr(b.CreateICmpNE(eq, i32C(0)), hitBB, contBB);
                        b.SetInsertPoint(hitBB);
                        b.CreateStore(i, delIdxSlot);
                        b.CreateBr(contBB);
                        b.SetInsertPoint(contBB);
                    });
                    llvm::Value* delIdx = b.CreateLoad(i64Ty, delIdxSlot);
                    llvm::Value* found = b.CreateICmpSGE(delIdx, i64C(0));
                    llvm::Value* newLen = b.CreateSub(len, b.CreateZExt(found, i64Ty));
                    b.CreateCall(rtMapNew, {lvA, newLen});
                    b.CreateStore(i64C(0), delIdxSlot);   // reused as the dest-index counter
                    emitLoop(i64C(0), len, false, [&](llvm::Value* i) {
                        BasicBlock* keepBB = newBB("wo.keep");
                        BasicBlock* stepDone = newBB("wo.stepdone");
                        b.CreateCondBr(b.CreateICmpEQ(i, delIdx), stepDone, keepBB);
                        b.SetInsertPoint(keepBB);
                        llvm::Value* dstIdx = b.CreateLoad(i64Ty, delIdxSlot);
                        llvm::Value* srcOff = b.CreateAdd(i64C(8), b.CreateMul(i, i64C(32)));
                        llvm::Value* dstOff = b.CreateAdd(i64C(8), b.CreateMul(dstIdx, i64C(32)));
                        copyLV(b, payAddr(b, lvA, dstOff), payAddr(b, recv, srcOff));
                        copyLV(b, payAddr(b, lvA, b.CreateAdd(dstOff, i64C(16))),
                                  payAddr(b, recv, b.CreateAdd(srcOff, i64C(16))));
                        b.CreateCall(rtRetain, {payAddr(b, lvA, dstOff)});
                        b.CreateCall(rtRetain, {payAddr(b, lvA, b.CreateAdd(dstOff, i64C(16)))});
                        b.CreateStore(b.CreateAdd(dstIdx, i64C(1)), delIdxSlot);
                        b.CreateBr(stepDone);
                        b.SetInsertPoint(stepDone);
                    });
                    b.CreateCall(rtRetain, {lvA});           // the map itself: +1 transfer
                    copyLV(b, dst, lvA);
                    if (consumed) {
                        // bug.md #30 leak (4): `m = m.without(...)` — without's
                        // result is ALWAYS a fresh map (never the receiver), so
                        // the consumed receiver's +1 is always dropped here (the
                        // CallDyn tail then clears the window slot without a
                        // second release). Kept entries survive: the loop above
                        // retained each into the fresh map.
                        b.CreateCall(rtRelease, {recv});
                    }
                });
            } else if (n == "code") {
                row(10, [&] {   // Track 03 §1 char.code(): the scalar, retagged int
                    storeTP(b, dst, i64C(1), loadPay(b, recv));
                });
            } else if (n == "chars") {
                row(4, [&] {   // Track 03 §1 string.chars() -> fresh Array<char> (+1)
                    b.CreateCall(rtStrChars, {dst, recv});
                    retainDst();
                });
            } else if (n == "setByte") {
                row(11, [&] {   // Track 03 §3 Block.setByte(i, v) — mutates, void result
                    b.CreateCall(rtBlockSetbyte, {recv, loadPay(b, arg(0)), loadPay(b, arg(1))});
                    storeVoid(b, dst);
                });
            } else if (n == "slice") {
                row(11, [&] {   // Track 03 §3 Block.slice(off, len) -> aliasing view (+1)
                    b.CreateCall(rtBlockSlice,
                                 {dst, recv, loadPay(b, arg(0)), loadPay(b, arg(1))});
                    retainDst();   // fresh block; the slice already retained its root
                });
            } else if (n == "int32At") {
                row(11, [&] {   // little-endian, sign-extended
                    b.CreateCall(rtBlockInt32at, {dst, recv, loadPay(b, arg(0))});
                });
            } else if (n == "setInt32") {
                row(11, [&] {
                    b.CreateCall(rtBlockSetint32, {recv, loadPay(b, arg(0)), loadPay(b, arg(1))});
                    storeVoid(b, dst);
                });
            } else if (n == "int64At") {
                row(11, [&] {   // little-endian
                    b.CreateCall(rtBlockInt64at, {dst, recv, loadPay(b, arg(0))});
                });
            } else if (n == "setInt64") {
                row(11, [&] {
                    b.CreateCall(rtBlockSetint64, {recv, loadPay(b, arg(0)), loadPay(b, arg(1))});
                    storeVoid(b, dst);
                });
            } else if (n == "fill") {
                row(11, [&] {
                    b.CreateCall(rtBlockFill, {recv, loadPay(b, arg(0)), loadPay(b, arg(1)), loadPay(b, arg(2))});
                    storeVoid(b, dst);
                });
            } else if (n == "blit") {
                row(11, [&] {
                    b.CreateCall(rtBlockBlit, {recv, loadPay(b, arg(0)), arg(1), loadPay(b, arg(2)), loadPay(b, arg(3))});
                    storeVoid(b, dst);
                });
            } else if (n == "equals") {
                row(11, [&] { b.CreateCall(rtBlockEquals, {dst, recv, arg(0)}); });
            } else if (n == "mismatch") {
                row(11, [&] { b.CreateCall(rtBlockMismatch, {dst, recv, arg(0), loadPay(b, arg(1))}); });
            }
            // fallthrough: no native row for this (tag, name)
        };

        // bug.md #18: emitNativeRows above is called once PER unresolved
        // CallDyn call site with that site's own (compile-time-known) method
        // name — unlike X64Gen::genCallNative's single generated-once dispatch
        // subroutine, this is plain C++ string comparison at codegen time, so
        // (unlike the X64Gen/genCallNative case bug.md #18 describes) whether
        // a name is covered IS knowable here without waiting for a runtime
        // dispatch failure. This list must stay in sync with emitNativeRows'
        // `else if (n == ...)` chain above; every entry there needs an entry
        // here (the reverse doesn't hold — a covered name can still miss at
        // runtime if the receiver's actual tag matches no `row()` in its
        // branch, e.g. a name valid on String called on an Object — a rarer,
        // separate gap not addressed here).
        auto nativeMethodCovered = [](const std::string& n) {
            static const char* const kCovered[] = {
                "length", "isEmpty", "toString", "indexOf", "contains", "subStr",
                "charAt", "toInt", "toFloat", "floor", "ceil", "round", "trunc",
                "sqrt", "pow", "byteAt", "trim", "toUpper", "toLower", "startsWith",
                "endsWith", "at", "add", "concatAll", "has", "keys", "values",
                "with", "without",
                // Track 03 §1 char + §3 Block (deferal-char-block-abi.md)
                "code", "chars", "setByte", "slice", "int32At", "setInt32",
                "int64At", "setInt64", "fill", "blit", "equals", "mismatch",
            };
            for (const char* c : kCovered) if (n == c) return true;
            return false;
        };

        for (size_t pc = 0; pc < irfn.code.size() && ok; ++pc) {
            const Inst& in = irfn.code[pc];
            b.SetInsertPoint(blocks[pc]);
            bool terminated = false;
            Op op = in.op;

            // §11/§15 COW (X64Gen.cpp:3086-3117): the dest temp still holds the
            // PREVIOUS IndexStore result — in an `arr[i] = v` loop that stale +1
            // makes a uniquely-owned base look shared (rc 2), defeating idxset's
            // in-place path and turning the loop O(n^2). Release it BEFORE the
            // op instead of stashing. Safe: if an op input aliases it, that
            // input's own slot holds a counted ref too (every reg write
            // retains), so rc >= 2 and this release cannot free a live input.
            if (op == Op::IndexStore) {
                b.CreateCall(rtRelease, {regs[in.a]});
                storeVoid(b, regs[in.a]);
                // lowerAssign's rebind chain `CopyVal t<-nb; Move L<-t` parks ONE
                // more stale +1 in t (registers are never reused, so t is written
                // only by that CopyVal). Release it here too — otherwise the base
                // still reads rc 2 at the check, one temp behind, forever. Only on
                // straight-line code (no jump may land between the chain's ops).
                if (pc + 2 < irfn.code.size()) {
                    const Inst& n1 = irfn.code[pc + 1];
                    const Inst& n2 = irfn.code[pc + 2];
                    if (n1.op == Op::CopyVal && n1.b == in.a &&
                        n2.op == Op::Move && n2.b == n1.a &&
                        n1.a != in.b && n1.a != in.c && n1.a != in.d &&
                        !jumpTarget[pc + 1] && !jumpTarget[pc + 2]) {
                        b.CreateCall(rtRelease, {regs[n1.a]});
                        storeVoid(b, regs[n1.a]);
                    }
                }
            }

            wrapDest(op, in.a, [&] {
                switch (op) {
                    case Op::LoadConst: {
                        const ::Value& c = irfn.consts[in.b];
                        switch (c.kind) {
                            case VKind::Int:
                                storeTP(b, regs[in.a], i64C(1), i64C(c.i));
                                break;
                            case VKind::Float: {
                                uint64_t bits; double d = c.f;
                                std::memcpy(&bits, &d, 8);
                                storeTP(b, regs[in.a], i64C(2), i64C((int64_t)bits));
                                break;
                            }
                            case VKind::Bool:
                                storeTP(b, regs[in.a], i64C(3), i64C(c.b ? 1 : 0));
                                break;
                            case VKind::String:
                                storeTP(b, regs[in.a], i64C(4), stringGlobal(b, c.s));
                                break;
                            case VKind::None:
                                storeTP(b, regs[in.a], i64C(8), i64C(0));
                                break;
                            case VKind::Char:   // Track 03 §1: pure immediate, tag 10
                                storeTP(b, regs[in.a], i64C(10), i64C(c.i));
                                break;
                            default:
                                storeVoid(b, regs[in.a]);
                                break;
                        }
                        break;
                    }
                    case Op::Default: {
                        const std::string& t = in.sname;
                        if (t == "int")
                            storeTP(b, regs[in.a], i64C(1), i64C(0));
                        else if (t == "bool")
                            storeTP(b, regs[in.a], i64C(3), i64C(0));
                        else if (t == "float")
                            storeTP(b, regs[in.a], i64C(2), i64C(0));
                        else if (t == "string")
                            storeTP(b, regs[in.a], i64C(4), stringGlobal(b, ""));
                        else if (t == "None" ||
                                (t.find(" | ") != std::string::npos && t.find("None") != std::string::npos))
                            storeTP(b, regs[in.a], i64C(8), i64C(0));
                        else if (t.rfind("Array", 0) == 0)
                            b.CreateCall(rtArrNew, {regs[in.a], i64C(0)});   // dk==1 wrap owns it
                        else if (t.rfind("Map", 0) == 0)
                            b.CreateCall(rtMapNew, {regs[in.a], i64C(0)});
                        else   // unmodeled -> void (object defaults arrive as NewObject)
                            storeVoid(b, regs[in.a]);
                        break;
                    }
                    case Op::Move:
                        copyLV(b, regs[in.a], regs[in.b]);
                        break;
                    case Op::MoveClear:
                        copyLV(b, regs[in.a], regs[in.b]);
                        storeVoid(b, regs[in.b]);
                        break;
                    case Op::CopyVal:
                        // deep-copies iff src is a value struct (runtime checks
                        // isvalueclass); plain alias copy otherwise. dk==1: the
                        // wrap retains an alias copy; a struct copy is never
                        // counted, so the retain no-ops on it.
                        // §9 A-M6: a definite copy (c==1) that Ownership.hpp
                        // proves never escapes this frame is arena-safe — route
                        // it to lv_copyval_arena instead of the heap-tier
                        // lvrt_copyval (fixes the returned_value_struct.ext
                        // churn XFAIL: the rebind/loop case nothing else frees).
                        if (in.c == 1 && ownership.scopeOwned.count({index, (int)pc}))
                            b.CreateCall(copyValArenaFn, {regs[in.a], regs[in.b]});
                        else
                            b.CreateCall(rtCopyval, {regs[in.a], regs[in.b]});
                        break;
                    case Op::Arith: {
                        int oc = opCode(in.tk);
                        if (oc < 0) { fail("operator"); break; }
                        llvm::Value* lt = loadTag(b, regs[in.b]);
                        llvm::Value* rt = loadTag(b, regs[in.c]);
                        // §9 A-M6 perf: inline int-int and float-float arith/
                        // compare instead of a cross-.o lvrt_arith call for
                        // every scalar `+`/`-`/`<`/etc (measured: this is
                        // fib(30)'s dominant cost once the arena-skip fast
                        // path removed the other per-call overhead). Mixed
                        // int/float, strings, objects, and unresolved
                        // operators fall through to the ORIGINAL opm/rtArith
                        // dispatch below, byte-for-byte unchanged, so
                        // correctness never depends on this fast path's
                        // coverage — only which cases skip the call.
                        llvm::Value* bothInt = b.CreateAnd(b.CreateICmpEQ(lt, i64C(1)),
                                                           b.CreateICmpEQ(rt, i64C(1)));
                        llvm::Value* bothFloat = b.CreateAnd(b.CreateICmpEQ(lt, i64C(2)),
                                                             b.CreateICmpEQ(rt, i64C(2)));
                        BasicBlock* intBB = newBB("ar.int");
                        BasicBlock* notIntBB = newBB("ar.notint");
                        BasicBlock* floatBB = newBB("ar.float");
                        BasicBlock* checkOpmBB = newBB("ar.checkopm");
                        BasicBlock* opmBB = newBB("ar.opm");
                        BasicBlock* scalBB = newBB("ar.scal");
                        BasicBlock* arDone = newBB("ar.done");
                        b.CreateCondBr(bothInt, intBB, notIntBB);

                        b.SetInsertPoint(intBB);
                        {
                            llvm::Value* av = loadPay(b, regs[in.b]);
                            llvm::Value* rv = loadPay(b, regs[in.c]);
                            llvm::Value* resTag = i64C(1);
                            llvm::Value* resPay;
                            switch (oc) {
                                case 0: resPay = b.CreateAdd(av, rv); break;
                                case 3: resPay = b.CreateSub(av, rv); break;
                                case 4: resPay = b.CreateMul(av, rv); break;
                                case 5: case 6: {   // DIV/MOD by zero raises a
                                    // catchable RuntimeException (§3.7, bug.md #10),
                                    // same shape as the SHL/SHR range check below —
                                    // never a silent 0. Floats keep IEEE inf/nan
                                    // (bug.md #12, the float case further down).
                                    llvm::Value* nz = b.CreateICmpNE(rv, i64C(0));
                                    BasicBlock* nzBB = newBB("ar.int.divnz");
                                    BasicBlock* zeroBB = newBB("ar.int.divbad");
                                    BasicBlock* mergeBB = newBB("ar.int.divdone");
                                    b.CreateCondBr(nz, nzBB, zeroBB);
                                    b.SetInsertPoint(zeroBB);
                                    b.CreateCall(rtRaise, {cstr(oc == 5 ? "division by zero"
                                                                        : "modulo by zero")});
                                    emitThrowCheck((int)pc);   // dispatch at THIS pc (oracle parity)
                                    b.CreateBr(mergeBB);        // not-throwing residue: unreachable
                                    BasicBlock* zeroPred = b.GetInsertBlock();
                                    b.SetInsertPoint(nzBB);
                                    llvm::Value* q = (oc == 5) ? b.CreateSDiv(av, rv)
                                                               : b.CreateSRem(av, rv);
                                    b.CreateBr(mergeBB);
                                    BasicBlock* nzPred = b.GetInsertBlock();
                                    b.SetInsertPoint(mergeBB);
                                    PHINode* phi = b.CreatePHI(i64Ty, 2);
                                    phi->addIncoming(i64C(0), zeroPred);
                                    phi->addIncoming(q, nzPred);
                                    resPay = phi;
                                    break;
                                }
                                case 1: resTag = i64C(3);
                                        resPay = b.CreateZExt(b.CreateICmpEQ(av, rv), i64Ty); break;
                                case 2: resTag = i64C(3);
                                        resPay = b.CreateZExt(b.CreateICmpNE(av, rv), i64Ty); break;
                                case 7: resTag = i64C(3);
                                        resPay = b.CreateZExt(b.CreateICmpSLT(av, rv), i64Ty); break;
                                case 8: resTag = i64C(3);
                                        resPay = b.CreateZExt(b.CreateICmpSGT(av, rv), i64Ty); break;
                                case 9: resTag = i64C(3);
                                        resPay = b.CreateZExt(b.CreateICmpSLE(av, rv), i64Ty); break;
                                case 10: resTag = i64C(3);
                                        resPay = b.CreateZExt(b.CreateICmpSGE(av, rv), i64Ty); break;
                                case 11: resPay = b.CreateAnd(av, rv); break;
                                case 12: resPay = b.CreateOr(av, rv); break;
                                case 15: resPay = b.CreateXor(av, rv); break;
                                default: {  // 13/14: SHL/SHR. A count outside 0..63
                                    // raises a catchable RuntimeException (Track 01
                                    // F1 semantics; X64Gen genAr's range-check-then-
                                    // raise shape) — never x86's silent mask-to-6-
                                    // bits, which this path did while it was dead
                                    // code (pre-F1 the checker rejected int shifts).
                                    llvm::Value* ok = b.CreateICmpULE(rv, i64C(63));
                                    BasicBlock* shOk = newBB("ar.int.shok");
                                    BasicBlock* shBad = newBB("ar.int.shbad");
                                    BasicBlock* shDone = newBB("ar.int.shdone");
                                    b.CreateCondBr(ok, shOk, shBad);
                                    b.SetInsertPoint(shBad);
                                    b.CreateCall(rtRaise, {cstr("shift count out of range")});
                                    emitThrowCheck((int)pc);   // dispatch at THIS pc (oracle parity)
                                    b.CreateBr(shDone);        // not-throwing residue: unreachable
                                    BasicBlock* badPred = b.GetInsertBlock();
                                    b.SetInsertPoint(shOk);
                                    llvm::Value* sh = (oc == 13) ? b.CreateShl(av, rv)
                                                                 : b.CreateAShr(av, rv);
                                    b.CreateBr(shDone);
                                    BasicBlock* okPred = b.GetInsertBlock();
                                    b.SetInsertPoint(shDone);
                                    PHINode* phi = b.CreatePHI(i64Ty, 2);
                                    phi->addIncoming(i64C(0), badPred);
                                    phi->addIncoming(sh, okPred);
                                    resPay = phi;
                                    break;
                                }
                            }
                            storeTP(b, regs[in.a], resTag, resPay);
                        }
                        b.CreateBr(arDone);

                        b.SetInsertPoint(notIntBB);
                        b.CreateCondBr(bothFloat, floatBB, checkOpmBB);

                        b.SetInsertPoint(floatBB);
                        {
                            llvm::Value* af = b.CreateBitCast(loadPay(b, regs[in.b]), f64Ty);
                            llvm::Value* rf = b.CreateBitCast(loadPay(b, regs[in.c]), f64Ty);
                            llvm::Value* resTag = i64C(2);
                            llvm::Value* resPay;
                            auto boolRes = [&](llvm::Value* cond) {
                                resTag = i64C(3);
                                return b.CreateZExt(cond, i64Ty);
                            };
                            switch (oc) {
                                case 0: resPay = b.CreateBitCast(b.CreateFAdd(af, rf), i64Ty); break;
                                case 3: resPay = b.CreateBitCast(b.CreateFSub(af, rf), i64Ty); break;
                                case 4: resPay = b.CreateBitCast(b.CreateFMul(af, rf), i64Ty); break;
                                case 5: case 6: {   // bug.md #12: IEEE semantics
                                    // (inf/nan) — no divide-by-zero override,
                                    // matching lvrt_arith (lv_runtime.c).
                                    llvm::Value* raw = (oc == 5) ? b.CreateFDiv(af, rf)
                                                                 : b.CreateFRem(af, rf);
                                    resPay = b.CreateBitCast(raw, i64Ty);
                                    break;
                                }
                                case 1: resPay = boolRes(b.CreateFCmpOEQ(af, rf)); break;
                                case 2: resPay = boolRes(b.CreateFCmpUNE(af, rf)); break;
                                case 7: resPay = boolRes(b.CreateFCmpOLT(af, rf)); break;
                                case 8: resPay = boolRes(b.CreateFCmpOGT(af, rf)); break;
                                case 9: resPay = boolRes(b.CreateFCmpOLE(af, rf)); break;
                                case 10: resPay = boolRes(b.CreateFCmpOGE(af, rf)); break;
                                default:   // 11-15: AND/OR/SHL/SHR/XOR undefined on float -> VOID
                                        resTag = i64C(0); resPay = i64C(0); break;
                            }
                            storeTP(b, regs[in.a], resTag, resPay);
                        }
                        b.CreateBr(arDone);

                        // Original dispatch, unchanged: an object left operand
                        // goes to the operator-method dispatcher (opm);
                        // everything else (mixed int/float, strings, obj-vs-
                        // None) to the scalar core. obj-vs-None compares are
                        // a tag compare in the scalar core, not a dispatch
                        // (X64Gen.cpp:3178-3196).
                        b.SetInsertPoint(checkOpmBB);
                        llvm::Value* isOpm = b.CreateAnd(b.CreateICmpEQ(lt, i64C(5)),
                                                         b.CreateICmpNE(rt, i64C(8)));
                        b.CreateCondBr(isOpm, opmBB, scalBB);
                        b.SetInsertPoint(opmBB);
                        b.CreateCall(rtOpm, {regs[in.a], i64C(oc), regs[in.b], regs[in.c]});
                        b.CreateBr(arDone);
                        b.SetInsertPoint(scalBB);
                        b.CreateCall(rtArith, {i32C(oc), regs[in.a], regs[in.b], regs[in.c]});
                        // SOUNDNESS DIVERGENCE from X64Gen (§12 log, 2026-07-05):
                        // X64Gen leaves concat results "floating" at rc 0 and its
                        // Ret/exit releases genuinely FREE them — returned strings
                        // cross frames as freed-but-intact bytes, observably
                        // correct only because its allocator never poisons freed
                        // blocks. Runtime v2 0xFE-poisons on free (a deliberate
                        // hardening), so this backend retains the result instead:
                        // every register OWNS its string, the runtime's consume-
                        // unowned gates (concat/print temp-free) skip rc>=1 values,
                        // and frees happen through ordinary release paths. Scalar
                        // results no-op through the region check — this retain
                        // only bites on the string row. The opm path is excluded:
                        // operator dispatch already returns +1 (call transfer).
                        b.CreateCall(rtRetain, {regs[in.a]});
                        b.CreateBr(arDone);
                        b.SetInsertPoint(arDone);
                        break;
                    }
                    case Op::Not: {
                        // §9 A-M6 perf: inline lvrt_not (out = !truth(in)).
                        llvm::Value* notV = b.CreateNot(truth(regs[in.b]));
                        storeTP(b, regs[in.a], i64C(3), b.CreateZExt(notV, i64Ty));
                        break;
                    }
                    case Op::Neg:
                        b.CreateCall(rtNeg, {regs[in.a], regs[in.b]});
                        break;
                    case Op::Jump:
                        b.CreateBr(blocks[in.a]);
                        terminated = true;
                        break;
                    case Op::JumpIfFalse:
                        b.CreateCondBr(truth(regs[in.a]), blocks[pc + 1], blocks[in.b]);
                        terminated = true;
                        break;
                    case Op::JumpIfTrue:
                        b.CreateCondBr(truth(regs[in.a]), blocks[in.b], blocks[pc + 1]);
                        terminated = true;
                        break;
                    case Op::Call: {
                        std::vector<llvm::Value*> args{regs[in.a]};
                        for (int k = 0; k < in.d; ++k) args.push_back(regs[in.c + k]);
                        b.CreateCall(fns[in.b], args);
                        emitThrowCheck((int)pc);
                        break;
                    }
                    case Op::CallDyn: {
                        if (in.decl && mod.byDecl.count(in.decl)) {   // resolved: direct call
                            std::vector<llvm::Value*> args{regs[in.a]};
                            for (int k = 0; k < in.d; ++k) args.push_back(regs[in.c + k]);
                            b.CreateCall(fns[mod.byDecl.at(in.decl)], args);
                        } else {   // unresolved: name-based callm (X64Gen::genCallM)
                            // in-language methods of the effective class first...
                            auto cands = callmCandidates(in.sname, in.d - 1);
                            BasicBlock* doneBB = newBB("cm.done");
                            if (!cands.empty()) {
                                llvm::Value* eff = effClassId(regs[in.c]);
                                for (auto& [cid, fi] : cands) {
                                    BasicBlock* hit = newBB("cm.hit");
                                    BasicBlock* next = newBB("cm.next");
                                    b.CreateCondBr(b.CreateICmpEQ(eff, i64C(cid)), hit, next);
                                    b.SetInsertPoint(hit);
                                    int np = mod.functions[fi].nparams;
                                    std::vector<llvm::Value*> args{regs[in.a], regs[in.c]};
                                    for (int k = 0; k < np - 1; ++k)
                                        args.push_back(k < in.d - 1 ? regs[in.c + 1 + k]
                                                                    : callRetScratch);
                                    b.CreateCall(fns[fi], args);
                                    b.CreateBr(doneBB);
                                    b.SetInsertPoint(next);
                                }
                            }
                            // bug.md #18: emitNativeRows is called per call
                            // site with THIS site's own compile-time-known
                            // method name, unlike X64Gen::genCallNative's
                            // single generated-once runtime dispatch — so
                            // whether it's covered is knowable right here,
                            // same as CallNativeFn's free-function dispatch
                            // below already does. `cands.empty()` confirms no
                            // in-language method of this name exists anywhere
                            // either, so an uncovered name genuinely has
                            // nothing to dispatch to at this call site (was
                            // special-cased to just `float.pow` by Track 06;
                            // generalized here to close the same hole for
                            // every other uncovered native method, e.g.
                            // `string.byteAt`/`toFloat`/`Array.concatAll` on
                            // this backend before Bug 19's `with`/`without`
                            // rows above).
                            if (cands.empty() && !nativeMethodCovered(in.sname)) {
                                // bug.md #2: no in-language method or native
                                // covers this name at this call site — the
                                // checker doesn't distinguish "method call"
                                // from "field read + closure call"
                                // (Checker.cpp's typeOfCallInner leaves
                                // `resolved` unset either way), so mirror
                                // Eval.cpp's evalCall fallback: read `sname`
                                // as a FIELD on the receiver at runtime and,
                                // if it holds a closure, dispatch through it
                                // via the same trampoline CallValue uses.
                                // Only a genuinely unresolvable name (not a
                                // closure either) still hard-fails the
                                // compile, same as before.
                                // lvA: the op-local scratch LvValue cell (not
                                // a register — this name has no IR slot of
                                // its own); argsBuf: the shared, entry-hoisted
                                // window buffer CallValue uses (widened above
                                // to cover this arity too), so a fallback
                                // inside a loop doesn't grow the stack.
                                llvm::Value* fieldVal = lvA;
                                b.CreateCall(rtGetm, {fieldVal, regs[in.c], cstr(in.sname)});
                                emitThrowCheck((int)pc);
                                llvm::Value* isClosure =
                                    b.CreateICmpEQ(loadTag(b, fieldVal), i64C(9));
                                BasicBlock* fcCallBB = newBB("fieldcall.call");
                                BasicBlock* fcFailBB = newBB("fieldcall.fail");
                                b.CreateCondBr(isClosure, fcCallBB, fcFailBB);
                                b.SetInsertPoint(fcFailBB);
                                b.CreateCall(rtRaise,
                                             {cstr("cannot resolve call target '" +
                                                   in.sname + "'")});
                                storeVoid(b, regs[in.a]);
                                b.CreateBr(doneBB);
                                b.SetInsertPoint(fcCallBB);
                                {
                                    llvm::Type* winTy = ArrayType::get(lvTy, maxArgs);
                                    llvm::Value* slot0 =
                                        b.CreateGEP(winTy, argsBuf, {i64C(0), i64C(0)});
                                    copyLV(b, slot0, fieldVal);
                                    for (int k = 1; k < in.d; ++k) {
                                        llvm::Value* slot =
                                            b.CreateGEP(winTy, argsBuf, {i64C(0), i64C(k)});
                                        copyLV(b, slot, regs[in.c + k]);
                                    }
                                    llvm::Value* cloPtr =
                                        b.CreateIntToPtr(loadPay(b, fieldVal), ptrTy);
                                    llvm::Value* fnIdx = b.CreateLoad(i64Ty, cloPtr);
                                    llvm::Value* args0 =
                                        b.CreateGEP(winTy, argsBuf, {i64C(0), i64C(0)});
                                    b.CreateCall(dispatchFn,
                                                 {fnIdx, regs[in.a], args0, i64C(in.d)});
                                }
                                b.CreateBr(doneBB);
                                // shared tail (matches the direct-call/cands/
                                // native-rows branches' single checked-call
                                // point below): ONE throw check for whichever
                                // path (raise or dispatchFn) doneBB merges.
                                b.SetInsertPoint(doneBB);
                                emitThrowCheck((int)pc);
                                if (in.b) storeVoid(b, regs[in.c]);
                                break;
                            }
                            // ...then the native cores; no row -> void result
                            emitNativeRows(in.sname, regs[in.a], regs[in.c],
                                           in.c + 1, in.d - 1, doneBB, in.b != 0);
                            storeVoid(b, regs[in.a]);
                            b.CreateBr(doneBB);
                            b.SetInsertPoint(doneBB);
                        }
                        // A checked call: a method (in-language or native) may
                        // throw — dispatch before the consumed-receiver cleanup,
                        // matching X64Gen (throwCheck precedes the window clear).
                        emitThrowCheck((int)pc);
                        // §15: a consumed receiver (COW self-append, in.b=1) had its
                        // buffer's fate taken by the callee — clear the window slot
                        // WITHOUT releasing (matches X64Gen's CallDyn tail).
                        if (in.b) storeVoid(b, regs[in.c]);
                        break;
                    }
                    case Op::CallValue: {
                        // window[0] = the callable (a closure: [payload+0] = fnIndex,
                        // and the closure itself is the callee's r0 so LoadCapture
                        // can read it). Dispatch through the trampoline.
                        for (int k = 0; k < in.d; ++k) {
                            llvm::Value* slot = b.CreateGEP(ArrayType::get(lvTy, maxArgs),
                                                            argsBuf, {i64C(0), i64C(k)});
                            copyLV(b, slot, regs[in.c + k]);
                        }
                        llvm::Value* cloPtr = b.CreateIntToPtr(loadPay(b, regs[in.c]), ptrTy);
                        llvm::Value* fnIdx = b.CreateLoad(i64Ty, cloPtr);
                        llvm::Value* args0 = b.CreateGEP(ArrayType::get(lvTy, maxArgs),
                                                         argsBuf, {i64C(0), i64C(0)});
                        b.CreateCall(dispatchFn, {fnIdx, regs[in.a], args0, i64C(in.d)});
                        emitThrowCheck((int)pc);
                        break;
                    }
                    case Op::Ret:
                        b.CreateCall(rtRetain, {regs[in.a]});
                        releaseAllRegs();
                        restoreArena();
                        copyLV(b, retSlot, regs[in.a]);
                        b.CreateRetVoid();
                        terminated = true;
                        break;
                    case Op::RetVoid:
                        releaseAllRegs();
                        restoreArena();
                        storeVoid(b, retSlot);
                        b.CreateRetVoid();
                        terminated = true;
                        break;
                    case Op::CallNativeFn: {
                        // The std::sys floor (X64Gen::genCallNative, X64Gen.cpp:
                        // 3511-3567). Every arg crosses as an LvValue* (the
                        // runtime unpacks — the boundary rule, unlike X64Gen which
                        // passes raw payloads to its own helpers); results land in
                        // the dest via the out-param. Fresh-string returns
                        // (readLine/sysRead/sysRecv) are retained (+1) into the
                        // register per the A-M3 string discipline; the void-return
                        // natives (close/timerCancel/unwatch) leave int 0 in the
                        // dest (X64Gen parity). CallNativeFn is dk==2 (transfer),
                        // so the outer wrap only releases the dest's old value.
                        const std::string& n = in.sname;
                        auto arg = [&](int k) { return regs[in.c + k]; };
                        auto retainDst = [&] { b.CreateCall(rtRetain, {regs[in.a]}); };
                        if (n == "sysWrite") {
                            if (in.d == 4)   // Track 03 M4: (fd, Block, off, len) -> int
                                b.CreateCall(rtSysWriteBlock, {regs[in.a], arg(0), arg(1), arg(2), arg(3)});
                            else
                                b.CreateCall(rtSysWrite, {regs[in.a], arg(0), arg(1)});
                        } else if (n == "sysReadLine") {
                            b.CreateCall(rtReadLine, {regs[in.a], arg(0)});
                            retainDst();               // fresh heap string -> +1
                        } else if (n == "sysRead") {
                            if (in.d == 3) {           // Track 03 M4: (fd, Block, max) -> int
                                b.CreateCall(rtSysReadBlock, {regs[in.a], arg(0), arg(1), arg(2)});
                            } else {
                                b.CreateCall(rtSysRead, {regs[in.a], arg(0), arg(1)});
                                retainDst();           // fresh heap string -> +1
                            }
                        } else if (n == "sysOpen") {
                            b.CreateCall(rtSysOpen, {regs[in.a], arg(0), arg(1)});
                        } else if (n == "sysClose") {
                            b.CreateCall(rtSysClose, {arg(0)});
                            storeTP(b, regs[in.a], i64C(1), i64C(0));   // void native -> int 0
                        } else if (n == "sysStat") {
                            b.CreateCall(rtSysStat, {regs[in.a], arg(0), arg(1)});
                        } else if (n == "sysMkdir") {
                            b.CreateCall(rtSysMkdir, {regs[in.a], arg(0)});
                        } else if (n == "sysArgs") {
                            b.CreateCall(rtSysArgs, {regs[in.a]});
                            retainDst();               // fresh heap Array<string> -> +1
                        } else if (n == "sysTermRaw") {
                            b.CreateCall(rtSysTermRaw, {regs[in.a], arg(0)});      // int result, no ARC
                        } else if (n == "sysTermRestore") {
                            b.CreateCall(rtSysTermRestore, {regs[in.a], arg(0)});
                        } else if (n == "sysWinSize") {
                            b.CreateCall(rtSysWinSize, {regs[in.a], arg(0), arg(1)});   // §2 int, no ARC
                        } else if (n == "sysTermIsRaw") {
                            b.CreateCall(rtSysTermIsRaw, {regs[in.a], arg(0)});         // §2 bool, no ARC
                        } else if (n == "sysSignalOpen") {
                            b.CreateCall(rtSysSignalOpen, {regs[in.a], arg(0)});        // §3 Array<int> -> fd
                        } else if (n == "sysSignalNext") {
                            b.CreateCall(rtSysSignalNext, {regs[in.a], arg(0)});        // §3 fd -> signo/-1
                        } else if (n == "sysSignalClose") {
                            b.CreateCall(rtSysSignalClose, {regs[in.a], arg(0)});       // §3 fd -> 0
                        } else if (n == "sysSetExitCode") {
                            b.CreateCall(rtSysSetExitCode, {arg(0)});              // exit-codes.md §3.1
                            storeTP(b, regs[in.a], i64C(1), i64C(0));   // void native -> int 0
                        } else if (n == "sysExit") {
                            b.CreateCall(rtSysExit, {arg(0)});                     // never returns
                            storeTP(b, regs[in.a], i64C(1), i64C(0));   // unreachable, keeps IR well-formed
                        } else if (n == "sysTimerStart") {
                            b.CreateCall(rtSysTimerStart, {regs[in.a], arg(0), arg(1), arg(2)});
                        } else if (n == "sysTimerCancel") {
                            b.CreateCall(rtSysTimerCancel, {arg(0)});
                            storeTP(b, regs[in.a], i64C(1), i64C(0));
                        } else if (n == "sysTcpConnect") {
                            b.CreateCall(rtSysTcpConnect, {regs[in.a], arg(0), arg(1)});
                        } else if (n == "sysTcpListen") {
                            if (in.d == 2)
                                b.CreateCall(rtSysTcpListenReuse, {regs[in.a], arg(0), arg(1)});
                            else
                                b.CreateCall(rtSysTcpListen, {regs[in.a], arg(0)});
                        } else if (n == "sysCpuCount") {
                            b.CreateCall(rtSysCpuCount, {regs[in.a]});
                        } else if (n == "sysTaskRun" || n == "sysTaskCancel" ||
                                   n == "sysTaskShield" || n == "sysTaskJoinAll" ||
                                   n == "sysAwaitAny2") {
                            // LA-30 B2 (doc 06 §4): the task floor. POSIX-only
                            // for the same reason as threads below (tasks are
                            // pump-pinned on Windows — lv_task.c's G5 stub).
                            // All results are scalar ints (no ARC); joinAll and
                            // awaitAny2 PARK and may raise — the blanket
                            // emitThrowCheck below this else-chain dispatches.
                            if (targetWindows) {
                                fail("tasks: unsupported on Windows (v1) — "
                                     "'" + n + "' has no Windows lowering");
                            } else if (n == "sysTaskRun") {
                                b.CreateCall(rtSysTaskRun, {regs[in.a], arg(0)});
                            } else if (n == "sysTaskCancel") {
                                b.CreateCall(rtSysTaskCancel, {regs[in.a], arg(0)});
                            } else if (n == "sysTaskShield") {
                                b.CreateCall(rtSysTaskShield, {regs[in.a], arg(0)});
                            } else if (n == "sysTaskJoinAll") {
                                b.CreateCall(rtSysTaskJoinAll, {regs[in.a], arg(0)});
                            } else {   // sysAwaitAny2
                                b.CreateCall(rtSysAwaitAny2, {regs[in.a], arg(0), arg(1)});
                            }
                        } else if (n == "sysThreadTransfer" || n == "sysThreadStart" ||
                                   n == "sysThreadResult" || n == "sysChannelNew" ||
                                   n == "sysChannelSend" || n == "sysChannelReceive" ||
                                   n == "sysChannelClose") {
                            // Track 10 §7: threads/channels are POSIX-only in v1.
                            // On a Windows target LV_TLS is deliberately non-TLS
                            // (mingw emulated-TLS != LLVM COFF lowering), so real
                            // threads would share what must be isolated — reject
                            // at compile time rather than emit a broken worker.
                            if (targetWindows) {
                                fail("threads: unsupported on Windows (v1) — "
                                     "'" + n + "' has no Windows lowering");
                            } else if (n == "sysThreadTransfer") {
                                // The flatten/rebuild boundary (§4): one deep copy
                                // through a self-contained buffer, returned at +1.
                                b.CreateCall(rtSysThreadTransfer, {regs[in.a], arg(0)});
                            } else if (n == "sysThreadStart") {
                                // Flatten captures, create the join eventfd,
                                // pthread_create, return the eventfd (int, no ARC).
                                b.CreateCall(rtSysThreadStart, {regs[in.a], arg(0)});
                            } else if (n == "sysThreadResult") {
                                // Drain the join fd, rebuild the result into this
                                // heap at +1 (C engine retains), reap the thread.
                                b.CreateCall(rtSysThreadResult, {regs[in.a], arg(0)});
                            } else if (n == "sysChannelNew") {
                                b.CreateCall(rtSysChannelNew, {regs[in.a], arg(0), arg(1)});
                            } else if (n == "sysChannelSend") {
                                // flatten+enqueue; may throw (closed / overflow).
                                b.CreateCall(rtSysChannelSend, {regs[in.a], arg(0), arg(1)});
                            } else if (n == "sysChannelReceive") {
                                // dequeue+rebuild into this heap at +1, or None.
                                b.CreateCall(rtSysChannelReceive, {regs[in.a], arg(0)});
                            } else {   // sysChannelClose
                                b.CreateCall(rtSysChannelClose, {regs[in.a], arg(0)});
                            }
                        } else if (n == "sysAccept") {
                            b.CreateCall(rtSysAccept, {regs[in.a], arg(0)});
                        } else if (n == "sysSocketBuffer") {   // LA-29
                            b.CreateCall(rtSysSocketBuffer, {regs[in.a], arg(0), arg(1), arg(2)});
                        } else if (n == "sysSend") {
                            if (in.d == 4)   // Track 03 M4: (fd, Block, off, len) -> int
                                b.CreateCall(rtSysSendBlock, {regs[in.a], arg(0), arg(1), arg(2), arg(3)});
                            else
                                b.CreateCall(rtSysSend, {regs[in.a], arg(0), arg(1)});
                        } else if (n == "sysRecv") {
                            if (in.d == 3) {           // Track 03 M4: (fd, Block, max) -> int?
                                b.CreateCall(rtSysRecvBlock, {regs[in.a], arg(0), arg(1), arg(2)});
                            } else {
                                b.CreateCall(rtSysRecv, {regs[in.a], arg(0), arg(1)});
                                retainDst();           // fresh recv string -> +1 (None/"" gate-skips)
                            }
                        } else if (n == "sysTlsConnect") {   // LA-2 §5.2
                            b.CreateCall(rtSysTlsConnect,
                                {regs[in.a], arg(0), arg(1), arg(2), arg(3), arg(4)});
                        } else if (n == "sysTlsAccept") {
                            b.CreateCall(rtSysTlsAccept,
                                {regs[in.a], arg(0), arg(1), arg(2), arg(3)});
                        } else if (n == "sysTlsHandshake") {
                            b.CreateCall(rtSysTlsHandshake, {regs[in.a], arg(0)});
                        } else if (n == "sysTlsError") {
                            b.CreateCall(rtSysTlsError, {regs[in.a], arg(0)});
                            retainDst();               // fresh error string -> +1
                        } else if (n == "sysTlsAlpn") {
                            b.CreateCall(rtSysTlsAlpn, {regs[in.a], arg(0)});
                            retainDst();               // fresh alpn string -> +1
                        } else if (n == "sysTlsVersion") {
                            b.CreateCall(rtSysTlsVersion, {regs[in.a], arg(0)});
                        } else if (n == "sysRsaEncrypt") {
                            b.CreateCall(rtSysRsaEncrypt, {regs[in.a], arg(0), arg(1), arg(2)});
                            retainDst();               // fresh string -> +1 (None gate-skips)
                        } else if (n == "sysRandom") {
                            b.CreateCall(rtSysRandom, {regs[in.a], arg(0)});
                            retainDst();               // fresh random-bytes string -> +1
                        } else if (n == "sysEnv") {
                            // bug #68: env var read on the compiled backends —
                            // fresh string (retain +1) or None (retain gate-skips).
                            b.CreateCall(rtSysEnv, {regs[in.a], arg(0)});
                            retainDst();
                        } else if (n == "sysWatch") {
                            b.CreateCall(rtSysWatch, {regs[in.a], arg(0), arg(1)});
                        } else if (n == "sysWatchWrite") {
                            b.CreateCall(rtSysWatchWrite, {regs[in.a], arg(0), arg(1)});
                        } else if (n == "sysTcpConnectNb") {
                            b.CreateCall(rtSysTcpConnectNb, {regs[in.a], arg(0), arg(1)});
                        } else if (n == "sysConnectResult") {
                            b.CreateCall(rtSysConnectResult, {regs[in.a], arg(0)});
                        } else if (n == "sysUnwatch") {
                            b.CreateCall(rtSysUnwatch, {arg(0)});
                            storeTP(b, regs[in.a], i64C(1), i64C(0));
                        } else if (n == "sysNow") {
                            b.CreateCall(rtSysNow, {regs[in.a]});
                        } else if (n == "sysMonotonic") {
                            b.CreateCall(rtSysMonotonic, {regs[in.a]});
                        } else if (n == "charFromCode") {
                            // Track 03 §1: scalar int -> char immediate (tag 10),
                            // no heap, no ARC — a pure retag.
                            storeTP(b, regs[in.a], i64C(10), loadPay(b, arg(0)));
                        } else if (n == "byteToString") {
                            b.CreateCall(rtStrFrombyte, {regs[in.a], loadPay(b, arg(0))});
                            retainDst();               // fresh 1-byte string -> +1
                        } else if (n == "log" || n == "log2" || n == "exp" ||
                                   n == "sin" || n == "cos" || n == "tan") {
                            llvm::Value* x = b.CreateBitCast(loadPay(b, arg(0)), f64Ty);
                            if (n == "tan") {
                                FunctionCallee f = module->getOrInsertFunction(
                                    "tan", FunctionType::get(f64Ty, {f64Ty}, false));
                                storeTP(b, regs[in.a], i64C(2),
                                        b.CreateBitCast(b.CreateCall(f, {x}), i64Ty));
                            } else {
                                Intrinsic::ID iid = n == "log"  ? Intrinsic::log
                                                 : n == "log2" ? Intrinsic::log2
                                                 : n == "exp"  ? Intrinsic::exp
                                                 : n == "sin"  ? Intrinsic::sin
                                                               : Intrinsic::cos;
                                llvm::Function* f = Intrinsic::getDeclaration(module.get(), iid, {f64Ty});
                                storeTP(b, regs[in.a], i64C(2),
                                        b.CreateBitCast(b.CreateCall(f, {x}), i64Ty));
                            }
                        } else if (n == "atan2") {
                            llvm::Value* y = b.CreateBitCast(loadPay(b, arg(0)), f64Ty);
                            llvm::Value* x = b.CreateBitCast(loadPay(b, arg(1)), f64Ty);
                            FunctionCallee f = module->getOrInsertFunction(
                                "atan2", FunctionType::get(f64Ty, {f64Ty, f64Ty}, false));
                            storeTP(b, regs[in.a], i64C(2),
                                    b.CreateBitCast(b.CreateCall(f, {y, x}), i64Ty));
                        } else if (n == "sysSpawn" || n == "sysPidfdOpen" ||
                                   n == "sysReap" || n == "sysKill") {
                            // G-LANG-2 process floor (techdesign-spawn-llvm.md §5):
                            // POSIX-only, the threads Windows-reject precedent (D4).
                            if (targetWindows) {
                                fail("process spawn: unsupported on Windows (v1) — '" + n +
                                     "' has no Windows lowering (techdesign-spawn-llvm.md)");
                            } else if (n == "sysSpawn") {
                                b.CreateCall(rtSysSpawn, {regs[in.a], arg(0), arg(1)});
                                retainDst();   // fresh heap Array<int> -> +1 (D1, sysArgs parity)
                            } else if (n == "sysPidfdOpen") {
                                b.CreateCall(rtSysPidfdOpen, {regs[in.a], arg(0)});
                            } else if (n == "sysReap") {
                                b.CreateCall(rtSysReap, {regs[in.a], arg(0)});
                            } else {  // sysKill
                                b.CreateCall(rtSysKill, {regs[in.a], arg(0), arg(1)});
                            }
                        } else {
                            fail("native floor function '" + n + "'");
                        }
                        emitThrowCheck((int)pc);
                        break;
                    }
                    case Op::Print:
                        b.CreateCall(rtPrintVal, {regs[in.a]});
                        break;
                    case Op::PrintNl:
                        b.CreateCall(rtPrintNl, {});
                        break;
                    case Op::VFree:
                        // §15: free the dead standalone value-struct copy in in.a,
                        // then clear the slot so no later hook/exit path sees the
                        // freed value (X64Gen.cpp:3640-3647).
                        b.CreateCall(rtVfree, {regs[in.a]});
                        storeVoid(b, regs[in.a]);
                        break;
                    case Op::NewObject: {
                        int64_t classId = in.sym ? classIdOf(in.sym) : 0;
                        // §9 A-M6: a fresh value-struct instance (in.c==1 is the
                        // separate $init field-auto-construct form, handled
                        // below unconditionally like X64Gen — it always escapes
                        // via the RawSet/SetMember that follows, per Ownership.
                        // hpp, so it never hits this branch) that Ownership.hpp
                        // proves scope-owned is arena-safe (struct_array_field.
                        // ext's per-iteration loop-local Point: X64Gen frees it
                        // via the arena too; nothing else reclaims a rebind).
                        bool arenaSite = in.c != 1 && in.sym && in.sym->isValue &&
                                        ownership.scopeOwned.count({index, (int)pc});
                        if (arenaSite) {
                            int64_t nslots = (int64_t)fieldKeysOf(in.sym).size();
                            b.CreateCall(objNewArenaFn, {regs[in.a], i64C(classId), i64C(nslots)});
                        } else
                            b.CreateCall(rtObjNew, {regs[in.a], i64C(classId)});
                        // NewObject is dk==2 (transfer): the outer wrap will only
                        // release the old dest value, never retain the new one, so
                        // this manual retain IS the dest's one reference — matching
                        // X64Gen's "own the object at +1 before $init runs" (§15).
                        b.CreateCall(rtRetain, {regs[in.a]});
                        if (in.b >= 0)
                            b.CreateCall(fns[in.b], {callRetScratch, regs[in.a]});
                        break;
                    }
                    case Op::RawGet: {
                        if (in.d > 0) {   // compile-time packed slot
                            llvm::Value* slotPtr = payAddr(b, regs[in.b], i64C(16 + (in.d - 1) * 16));
                            copyLV(b, regs[in.a], slotPtr);
                        } else {          // dynamic key (dyn-list fallback)
                            b.CreateCall(rtGetfield, {regs[in.a], regs[in.b], cstr(in.sname)});
                        }
                        break;   // dk==1: the outer wrap retains the read-out value
                    }
                    case Op::RawGetWeak: {
                        if (in.d > 0) {
                            llvm::Value* slotPtr = payAddr(b, regs[in.b], i64C(16 + (in.d - 1) * 16));
                            b.CreateCall(rtWeakLoad, {regs[in.a], slotPtr});
                        } else {
                            b.CreateCall(rtWeakGetfield, {regs[in.a], regs[in.b], cstr(in.sname)});
                        }
                        break;
                    }
                    case Op::GetMember: {
                        // Accessor-aware name→slot read. lvrt_getm returns +1
                        // (transfer, ruled 2026-07-05) — dk==2, NO wrap retain.
                        // A user getter may throw — check the accessor path (§7).
                        b.CreateCall(rtGetm, {regs[in.a], regs[in.b], cstr(in.sname)});
                        emitThrowCheck((int)pc);
                        break;
                    }
                    case Op::LoadGlobal: {
                        llvm::Value* g = b.CreateGEP(
                            ArrayType::get(lvTy, mod.nglobals), globalsArray,
                            {i64C(0), i64C(in.b)});
                        copyLV(b, regs[in.a], g);   // dk==1: outer wrap retains
                        break;
                    }
                    case Op::NewArray: {
                        // Fresh array owned by the dest register BEFORE elements
                        // append (rc==1 -> in-place append fast path; see the
                        // destKind divergence note). Range elements spread.
                        b.CreateCall(rtArrNew, {regs[in.a], i64C(0)});
                        b.CreateCall(rtRetain, {regs[in.a]});
                        int rangeId = classIdByName("Range");
                        for (int k = 0; k < in.d; ++k) {
                            llvm::Value* elem = regs[in.c + k];
                            BasicBlock* spreadBB = newBB("arr.spread");
                            BasicBlock* plainBB = newBB("arr.plain");
                            BasicBlock* nextBB = newBB("arr.next");
                            llvm::Value* isObj = b.CreateICmpEQ(loadTag(b, elem), i64C(5));
                            BasicBlock* clsChk = newBB("arr.cls");
                            b.CreateCondBr(isObj, clsChk, plainBB);
                            b.SetInsertPoint(clsChk);
                            llvm::Value* hdr = b.CreateIntToPtr(loadPay(b, elem), ptrTy);
                            llvm::Value* cid = b.CreateLoad(i64Ty, hdr);
                            b.CreateCondBr(b.CreateICmpEQ(cid, i64C(rangeId)), spreadBB, plainBB);
                            b.SetInsertPoint(spreadBB);
                            b.CreateCall(rtGetfield, {lvA, elem, cstr("start")});
                            b.CreateCall(rtGetfield, {lvB, elem, cstr("end")});
                            llvm::Value* lo = loadPay(b, lvA);
                            llvm::Value* hi = loadPay(b, lvB);
                            emitLoop(lo, hi, /*inclusive=*/true, [&](llvm::Value* v) {
                                storeTP(b, lvA, i64C(1), v);
                                b.CreateCall(rtArrAppend, {regs[in.a], regs[in.a], lvA});
                            });
                            b.CreateBr(nextBB);
                            b.SetInsertPoint(plainBB);
                            b.CreateCall(rtArrAppend, {regs[in.a], regs[in.a], elem});
                            b.CreateBr(nextBB);
                            b.SetInsertPoint(nextBB);
                        }
                        break;
                    }
                    case Op::NewArraySized: {
                        // Array(n, fill): one-shot O(n) sized construction (§5.1) —
                        // supersedes the old O(n^2) copy-and-grow append loop for
                        // ALL three layouts (columnar / dense / boxed), producing the
                        // identical array leak-free. Returns owned (+1), so no wrap
                        // retain (dk==2). d==0 stays the empty-array birth path.
                        if (in.d == 2)
                            b.CreateCall(rtArrFill, {regs[in.a], loadPay(b, regs[in.c]),
                                                     regs[in.c + 1]});
                        else {
                            b.CreateCall(rtArrNew, {regs[in.a], i64C(0)});
                            b.CreateCall(rtRetain, {regs[in.a]});
                        }
                        break;
                    }
                    case Op::NewMap:
                        b.CreateCall(rtMapNew, {regs[in.a], i64C(0)});
                        break;   // dk==1: the wrap's retain is the register's count
                    // Track 03 §3: Block(n) / Block::fromString(s). The runtime
                    // constructor births a fresh root block at rc=0; dk==1 (both
                    // are the default bucket), so the wrap's retain is the
                    // register's one reference — same shape as NewMap/NewArray's
                    // birth-then-wrap-retain (X64Gen births its Block at +1
                    // inline; runtime v2 defers the +1 to the wrap).
                    case Op::NewBlock:
                        b.CreateCall(rtBlockNew, {regs[in.a], loadPay(b, regs[in.c])});
                        break;
                    case Op::NewBlockStr:   // borrows the string operand (no consume)
                        b.CreateCall(rtBlockFromstr, {regs[in.a], regs[in.c]});
                        break;
                    case Op::MakeRange: {
                        int rangeId = classIdByName("Range");
                        if (!rangeId) { fail("MakeRange without a Range class"); break; }
                        b.CreateCall(rtObjNew, {regs[in.a], i64C(rangeId)});
                        b.CreateCall(rtSetfield, {regs[in.a], cstr("start"), regs[in.b]});
                        b.CreateCall(rtSetfield, {regs[in.a], cstr("end"), regs[in.c]});
                        break;   // int fields, no slot ARC (X64Gen parity); dk==1 owns it
                    }
                    case Op::ColGet: {
                        // techdesign-columnar §5.4: the fused element-field read —
                        // dst = arr[idx].field, straight from column `in.d` of a
                        // columnar buffer with no gather (THE win). Cold boxed
                        // fallback (array still empty/boxed) reads the boxed way.
                        llvm::Value* arrPay = loadPay(b, regs[in.b]);
                        llvm::Value* word0 = b.CreateLoad(i64Ty, b.CreateIntToPtr(arrPay, ptrTy));
                        llvm::Value* cleanLen = b.CreateAnd(word0, i64C(0x3fffffffffffffffLL));
                        llvm::Value* idx = loadPay(b, regs[in.c]);
                        llvm::Value* oob = b.CreateOr(b.CreateICmpSGE(idx, cleanLen),
                                                      b.CreateICmpSLT(idx, i64C(0)));
                        BasicBlock* oobBB  = newBB("cg.oob");
                        BasicBlock* okBB   = newBB("cg.ok");
                        BasicBlock* colBB  = newBB("cg.col");
                        BasicBlock* boxBB  = newBB("cg.boxed");
                        BasicBlock* doneBB = newBB("cg.done");
                        b.CreateCondBr(oob, oobBB, okBB);
                        b.SetInsertPoint(oobBB);
                        b.CreateCall(rtRaiseOob, {idx, cleanLen});
                        storeVoid(b, regs[in.a]);
                        b.CreateBr(doneBB);
                        b.SetInsertPoint(okBB);
                        // columnar (bit 62) fast path; else the cold boxed fallback
                        b.CreateCondBr(
                            b.CreateICmpNE(b.CreateAnd(word0, i64C(0x4000000000000000LL)), i64C(0)),
                            colBB, boxBB);
                        b.SetInsertPoint(colBB);
                        // field payload at arrPay + 16 + slotK*8*cleanLen + 8*idx;
                        // tag is the compile-time field typecode (all fields scalar).
                        int typecode = 0;
                        { int k = 0; for (const Slot& s : in.sym->shape.slots) {
                              if (s.isMethod) continue;
                              if (k == in.d) { typecode = columnarTypecodeOf(s.canonical); break; }
                              k++; } }
                        llvm::Value* colBase = b.CreateAdd(
                            i64C(16), b.CreateMul(i64C((int64_t)in.d * 8), cleanLen));
                        llvm::Value* off = b.CreateAdd(colBase, b.CreateMul(idx, i64C(8)));
                        llvm::Value* payload = b.CreateLoad(
                            i64Ty, b.CreateIntToPtr(b.CreateAdd(arrPay, off), ptrTy));
                        storeTP(b, regs[in.a], i64C(typecode), payload);
                        b.CreateBr(doneBB);
                        b.SetInsertPoint(boxBB);   // cold: boxed/empty element field read
                        b.CreateCall(rtIdxGet, {lvA, regs[in.b], regs[in.c]});
                        b.CreateCall(rtGetfield, {regs[in.a], lvA, cstr(in.sname)});
                        b.CreateBr(doneBB);
                        b.SetInsertPoint(doneBB);
                        emitThrowCheck((int)pc);   // OOB / boxed idxget may raise
                        break;   // result is a scalar immediate — dk==1 retain no-ops
                    }
                    case Op::GetIndex: {
                        // Object receivers dispatch a declared ([]) getter, if any
                        // instantiated class has one (X64Gen::genIdxGet's classes
                        // pass); arrays/maps go to the runtime core.
                        std::vector<std::pair<int, int>> gets;   // (classId, fnIndex)
                        for (Symbol* cls : instClasses) {
                            std::vector<const Stmt*> mem;
                            collectMembersLG(cls, mem);
                            for (const Stmt* m : mem) {
                                if (!m->isGet || !m->selector.symbolic) continue;
                                if (!mod.byDecl.count(m)) continue;
                                int fi = mod.byDecl.at(m);
                                if (fi >= (int)fns.size() || !fns[fi]) continue;
                                gets.push_back({classIdOf(cls), fi});
                                break;
                            }
                        }
                        if (gets.empty()) {
                            b.CreateCall(rtIdxGet, {regs[in.a], regs[in.b], regs[in.c]});
                        } else {
                            BasicBlock* doneBB = newBB("ix.done");
                            BasicBlock* coreBB = newBB("ix.core");
                            llvm::Value* isObj = b.CreateICmpEQ(loadTag(b, regs[in.b]), i64C(5));
                            BasicBlock* objBB = newBB("ix.obj");
                            b.CreateCondBr(isObj, objBB, coreBB);
                            b.SetInsertPoint(objBB);
                            llvm::Value* hdr = b.CreateIntToPtr(loadPay(b, regs[in.b]), ptrTy);
                            llvm::Value* cid = b.CreateLoad(i64Ty, hdr);
                            for (auto& [gcid, fi] : gets) {
                                BasicBlock* hit = newBB("ix.hit");
                                BasicBlock* next = newBB("ix.next");
                                b.CreateCondBr(b.CreateICmpEQ(cid, i64C(gcid)), hit, next);
                                b.SetInsertPoint(hit);
                                // getter result is a call transfer (+1); the dk==1 wrap
                                // adds the reader's retain on top, exactly as X64Gen's
                                // genIdxGet accessor path does — parity, warts included.
                                b.CreateCall(fns[fi], {regs[in.a], regs[in.b], regs[in.c]});
                                b.CreateBr(doneBB);
                                b.SetInsertPoint(next);
                            }
                            b.CreateCall(rtIdxGet, {regs[in.a], regs[in.b], regs[in.c]});
                            b.CreateBr(doneBB);
                            b.SetInsertPoint(coreBB);
                            b.CreateCall(rtIdxGet, {regs[in.a], regs[in.b], regs[in.c]});
                            b.CreateBr(doneBB);
                            b.SetInsertPoint(doneBB);
                        }
                        emitThrowCheck((int)pc);   // idxget raises RuntimeException on OOB
                        break;   // dk==1: wrap retains the read-out value
                    }
                    case Op::IndexStore: {
                        // Object receivers dispatch a declared ([]) SETTER (the
                        // mirror of GetIndex's getter path — X64Gen::genIdxSet's
                        // classes pass): call `set ([])(i, v)` and leave the base
                        // unchanged (the object itself). Arrays/maps go to the
                        // runtime core, which COWs internally (in-place iff base
                        // rc==1). dest was pre-released above; the dk==1 wrap sees
                        // a void old-ref and retains the result.
                        std::vector<std::pair<int, int>> sets;   // (classId, fnIndex)
                        for (Symbol* cls : instClasses) {
                            std::vector<const Stmt*> mem;
                            collectMembersLG(cls, mem);
                            for (const Stmt* m : mem) {
                                if (!m->isSet || !m->selector.symbolic) continue;
                                if (!mod.byDecl.count(m)) continue;
                                int fi = mod.byDecl.at(m);
                                if (fi >= (int)fns.size() || !fns[fi]) continue;
                                sets.push_back({classIdOf(cls), fi});
                                break;
                            }
                        }
                        if (sets.empty()) {
                            b.CreateCall(rtIdxSet, {regs[in.a], regs[in.b], regs[in.c], regs[in.d]});
                        } else {
                            BasicBlock* doneBB = newBB("is.done");
                            BasicBlock* coreBB = newBB("is.core");
                            BasicBlock* objBB = newBB("is.obj");
                            b.CreateCondBr(b.CreateICmpEQ(loadTag(b, regs[in.b]), i64C(5)),
                                           objBB, coreBB);
                            b.SetInsertPoint(objBB);
                            llvm::Value* cid = b.CreateLoad(
                                i64Ty, b.CreateIntToPtr(loadPay(b, regs[in.b]), ptrTy));
                            for (auto& [scid, fi] : sets) {
                                BasicBlock* hit = newBB("is.hit");
                                BasicBlock* next = newBB("is.next");
                                b.CreateCondBr(b.CreateICmpEQ(cid, i64C(scid)), hit, next);
                                b.SetInsertPoint(hit);
                                // set ([])(this, i, v) -> void; base unchanged
                                b.CreateCall(fns[fi], {callRetScratch, regs[in.b],
                                                       regs[in.c], regs[in.d]});
                                copyLV(b, regs[in.a], regs[in.b]);
                                b.CreateBr(doneBB);
                                b.SetInsertPoint(next);
                            }
                            b.CreateCall(rtIdxSet, {regs[in.a], regs[in.b], regs[in.c], regs[in.d]});
                            b.CreateBr(doneBB);
                            b.SetInsertPoint(coreBB);
                            b.CreateCall(rtIdxSet, {regs[in.a], regs[in.b], regs[in.c], regs[in.d]});
                            b.CreateBr(doneBB);
                            b.SetInsertPoint(doneBB);
                        }
                        emitThrowCheck((int)pc);   // idxset raises RuntimeException on OOB
                        break;
                    }
                    case Op::IterLen: {
                        llvm::Value* tag = loadTag(b, regs[in.b]);
                        BasicBlock* contBB = newBB("il.cont");
                        BasicBlock* rangeBB = newBB("il.range");
                        BasicBlock* collBB = newBB("il.coll");
                        BasicBlock* zeroBB = newBB("il.zero");
                        llvm::Value* isColl = b.CreateOr(b.CreateICmpEQ(tag, i64C(6)),
                                                         b.CreateICmpEQ(tag, i64C(7)));
                        BasicBlock* chk5 = newBB("il.chk5");
                        b.CreateCondBr(isColl, collBB, chk5);
                        b.SetInsertPoint(chk5);
                        b.CreateCondBr(b.CreateICmpEQ(tag, i64C(5)), rangeBB, zeroBB);
                        b.SetInsertPoint(collBB);
                        llvm::Value* raw = b.CreateLoad(
                            i64Ty, b.CreateIntToPtr(loadPay(b, regs[in.b]), ptrTy));
                        storeTP(b, regs[in.a], i64C(1),
                                b.CreateAnd(raw, i64C(0x3fffffffffffffffLL)));  // dense+columnar bits
                        b.CreateBr(contBB);
                        b.SetInsertPoint(rangeBB);   // end - start + 1, clamped >= 0
                        b.CreateCall(rtGetfield, {lvA, regs[in.b], cstr("start")});
                        b.CreateCall(rtGetfield, {lvB, regs[in.b], cstr("end")});
                        llvm::Value* n = b.CreateAdd(
                            b.CreateSub(loadPay(b, lvB), loadPay(b, lvA)), i64C(1));
                        n = b.CreateSelect(b.CreateICmpSGE(n, i64C(0)), n, i64C(0));
                        storeTP(b, regs[in.a], i64C(1), n);
                        b.CreateBr(contBB);
                        b.SetInsertPoint(zeroBB);
                        storeTP(b, regs[in.a], i64C(1), i64C(0));
                        b.CreateBr(contBB);
                        b.SetInsertPoint(contBB);
                        break;
                    }
                    case Op::IterAt: {
                        llvm::Value* tag = loadTag(b, regs[in.b]);
                        llvm::Value* idx = loadPay(b, regs[in.c]);
                        BasicBlock* contBB = newBB("ia.cont");
                        BasicBlock* arrBB = newBB("ia.arr");
                        BasicBlock* rngBB = newBB("ia.rng");
                        BasicBlock* mapBB = newBB("ia.map");
                        BasicBlock* voidBB = newBB("ia.void");
                        BasicBlock* c5 = newBB("ia.c5");
                        BasicBlock* c7 = newBB("ia.c7");
                        b.CreateCondBr(b.CreateICmpEQ(tag, i64C(6)), arrBB, c5);
                        b.SetInsertPoint(c5);
                        b.CreateCondBr(b.CreateICmpEQ(tag, i64C(5)), rngBB, c7);
                        b.SetInsertPoint(c7);
                        b.CreateCondBr(b.CreateICmpEQ(tag, i64C(7)), mapBB, voidBB);

                        b.SetInsertPoint(arrBB);
                        llvm::Value* raw = b.CreateLoad(
                            i64Ty, b.CreateIntToPtr(loadPay(b, regs[in.b]), ptrTy));
                        BasicBlock* denseBB = newBB("ia.dense");
                        BasicBlock* boxedBB = newBB("ia.boxed");
                        BasicBlock* colBB = newBB("ia.col");
                        BasicBlock* nbBB = newBB("ia.notboxed");
                        b.CreateCondBr(b.CreateICmpSLT(raw, i64C(0)), nbBB, boxedBB);
                        b.SetInsertPoint(nbBB);      // columnar (bit 62) gathers; dense inlines
                        b.CreateCondBr(
                            b.CreateICmpNE(b.CreateAnd(raw, i64C(0x4000000000000000LL)), i64C(0)),
                            colBB, denseBB);
                        b.SetInsertPoint(colBB);     // gather a fresh rc=0 record (§5.5)
                        b.CreateCall(rtIdxGet, {regs[in.a], regs[in.b], regs[in.c]});
                        b.CreateBr(contBB);
                        b.SetInsertPoint(denseBB);   // tag-5 pointer INTO the buffer
                        llvm::Value* recBytes = b.CreateLoad(
                            i64Ty, payAddr(b, regs[in.b], i64C(8)));
                        llvm::Value* recOff = b.CreateAdd(i64C(16), b.CreateMul(idx, recBytes));
                        llvm::Value* recAddr = b.CreatePtrToInt(
                            payAddr(b, regs[in.b], recOff), i64Ty);
                        storeTP(b, regs[in.a], i64C(5), recAddr);
                        b.CreateBr(contBB);
                        b.SetInsertPoint(boxedBB);
                        llvm::Value* eOff = b.CreateAdd(i64C(8), b.CreateMul(idx, i64C(16)));
                        copyLV(b, regs[in.a], payAddr(b, regs[in.b], eOff));
                        b.CreateBr(contBB);

                        b.SetInsertPoint(rngBB);     // start + index
                        b.CreateCall(rtGetfield, {lvA, regs[in.b], cstr("start")});
                        storeTP(b, regs[in.a], i64C(1), b.CreateAdd(loadPay(b, lvA), idx));
                        b.CreateBr(contBB);

                        b.SetInsertPoint(mapBB);     // Pair(first=key, second=value)
                        {
                            int pairId = classIdByName("Pair");
                            if (pairId) {
                                b.CreateCall(rtObjNew, {regs[in.a], i64C(pairId)});
                                llvm::Value* entryOff = b.CreateAdd(i64C(8),
                                                                    b.CreateMul(idx, i64C(32)));
                                llvm::Value* kSrc = payAddr(b, regs[in.b], entryOff);
                                llvm::Value* vSrc = payAddr(b, regs[in.b],
                                                            b.CreateAdd(entryOff, i64C(16)));
                                llvm::Value* fstSlot = payAddr(b, regs[in.a], i64C(16));
                                llvm::Value* sndSlot = payAddr(b, regs[in.a], i64C(32));
                                copyLV(b, fstSlot, kSrc);
                                copyLV(b, sndSlot, vSrc);
                                // §15: the Pair's fields OWN their refs (raw stores
                                // bypass the SetMember hook) — retain each, or the
                                // Pair's death releases the map's own entry refs
                                // unbalanced (X64Gen::genIterAt).
                                b.CreateCall(rtRetain, {fstSlot});
                                b.CreateCall(rtRetain, {sndSlot});
                            } else {
                                storeVoid(b, regs[in.a]);
                            }
                        }
                        b.CreateBr(contBB);

                        b.SetInsertPoint(voidBB);
                        storeVoid(b, regs[in.a]);
                        b.CreateBr(contBB);
                        b.SetInsertPoint(contBB);
                        break;   // dk==1: wrap retains (fresh Pair 0->1; borrowed elem +1;
                                 // dense record / int are uncounted no-ops)
                    }
                    case Op::MakeClosure:
                        b.CreateCall(rtClosureNew, {regs[in.a], i64C(in.b)});
                        break;   // dk==1: wrap's retain is the register's count
                    case Op::CaptureVar:
                        // the capture node owns the value — lvrt_capture_set
                        // retains internally (unlike X64Gen, whose capset helper
                        // needed a separate retain call); dk==0.
                        b.CreateCall(rtCaptureSet, {regs[in.a], cstr(in.sname), regs[in.b]});
                        break;
                    case Op::LoadCapture:
                        // reads from this-closure = r0; borrowed -> dk==1 wrap retains
                        b.CreateCall(rtCaptureGet, {regs[in.a], regs[0], cstr(in.sname)});
                        break;
                    case Op::Throw:
                        // Pending-throw: throw_set stores the value into g_thrown,
                        // sets the flag, and RETAINS it for in-flight ownership
                        // (§15 — do NOT add an emission-side retain; the catch
                        // bind's transfer consumes exactly this +1). Then run the
                        // same dispatch as any checked op. dk==0 (no reg dest).
                        b.CreateCall(rtThrowSet, {regs[in.a]});
                        emitThrowCheck((int)pc);
                        break;
                    case Op::IsType: {
                        // `is` / `match` type test (X64Gen.cpp:3595, CGen IsType):
                        // true iff the subject matches ANY union member — a tag
                        // compare for primitives/collections, an lvrt_issub for
                        // classes. Result is an unboxed bool (dk==1; the wrap's
                        // retain no-ops on it).
                        std::vector<std::string> members;
                        const std::string& sn = in.sname;
                        if (sn.find(" | ") != std::string::npos) {
                            size_t p = 0, q;
                            while ((q = sn.find(" | ", p)) != std::string::npos) {
                                members.push_back(sn.substr(p, q - p)); p = q + 3;
                            }
                            members.push_back(sn.substr(p));
                        } else members.push_back(sn);
                        auto primTag = [](const std::string& base) -> int {
                            if (base == "None") return 8;
                            if (base == "int") return 1;
                            if (base == "bool") return 3;
                            if (base == "float") return 2;
                            if (base == "string") return 4;
                            if (base.rfind("Array", 0) == 0) return 6;
                            if (base.rfind("Map", 0) == 0) return 7;
                            if (base == "char") return 10;    // Track 03 §1
                            if (base == "Block") return 11;   // Track 03 §3
                            return -1;   // a class
                        };
                        llvm::Value* src = regs[in.b];
                        llvm::Value* srcTag = loadTag(b, src);
                        BasicBlock* trueBB = newBB("is.true");
                        BasicBlock* falseBB = newBB("is.false");
                        BasicBlock* doneBB = newBB("is.done");
                        bool single = members.size() == 1;
                        for (const std::string& m : members) {
                            std::string base = m.substr(0, m.find('<'));   // Array<T> -> Array
                            int t = primTag(base);
                            BasicBlock* nextBB = newBB("is.next");
                            if (t >= 0) {
                                b.CreateCondBr(b.CreateICmpEQ(srcTag, i64C(t)), trueBB, nextBB);
                            } else {
                                // a class: subject must be an object whose class
                                // issub the member. Resolve by name (per-member,
                                // handles multi-class unions); fall back to in.sym
                                // for a single class member (X64Gen parity).
                                Symbol* cs = classByName(base.c_str());
                                int cid = cs ? classIdOf(cs)
                                             : (single && in.sym ? classIdOf(in.sym) : 0);
                                if (!cid) { b.CreateBr(nextBB); }
                                else {
                                    BasicBlock* objBB = newBB("is.obj");
                                    b.CreateCondBr(b.CreateICmpEQ(srcTag, i64C(5)), objBB, nextBB);
                                    b.SetInsertPoint(objBB);
                                    llvm::Value* hdr = b.CreateIntToPtr(loadPay(b, src), ptrTy);
                                    llvm::Value* clsid = b.CreateLoad(i64Ty, hdr);
                                    llvm::Value* sub = b.CreateCall(rtIssub, {clsid, i64C(cid)});
                                    b.CreateCondBr(b.CreateICmpNE(sub, i32C(0)), trueBB, nextBB);
                                }
                            }
                            b.SetInsertPoint(nextBB);
                        }
                        b.CreateBr(falseBB);
                        b.SetInsertPoint(trueBB);
                        storeTP(b, regs[in.a], i64C(3), i64C(1));
                        b.CreateBr(doneBB);
                        b.SetInsertPoint(falseBB);
                        storeTP(b, regs[in.a], i64C(3), i64C(0));
                        b.CreateBr(doneBB);
                        b.SetInsertPoint(doneBB);
                        break;
                    }
                    case Op::Await: {
                        // LA-30 (doc 2 §1/§4): ONE runtime call. Park/pump lives
                        // in lvrt_await (runtime/lv_runtime.c) — under LANG_TASKS=1
                        // it parks the current task and switches stacks; under =0
                        // it contains the pump fallback, a C transcription of the
                        // inline BB graph this replaced (same two ABI calls, same
                        // exit order). dk==1: lvrt_await writes a BORROWED value
                        // and the wrap retains it — the old readBB contract,
                        // now lvrt_await's final getfield's contract.
                        //
                        // HISTORY: an lvrt_await call existed once and was removed
                        // by the maintenance-pass-2 §8 correction in favor of the
                        // inline pump (lineage: X64Gen.cpp:3573-3593). That ruling
                        // was correct FOR PUMPING — a pump is just a loop over two
                        // ABI calls with no semantic payload. Parking is different
                        // in kind: it must run on the scheduler's stack discipline
                        // and cannot be expressed as inline BBs. Do NOT "restore"
                        // the inline form at a future maintenance pass.
                        b.CreateCall(rtAwait, {regs[in.a], regs[in.b]});
                        emitThrowCheck((int)pc);   // dispatch at THIS pc (oracle parity)
                        break;
                    }
                    case Op::RawSet:
                    case Op::RawSetWeak:
                    case Op::SetMember:
                    case Op::StoreGlobal:
                        break;   // real work is in the post-switch block below (dk==0, §4.1)
                    default:
                        fail("this construct is not in the LLVM backend's coverage yet (A-M3 in progress)");
                        break;
                }
            });
            // The dk==0 stores below write some OTHER location than register `a`
            // (a field slot or a global slot — Ir.hpp's own field comments say
            // `a` is the VALUE being stored), so they do their target slot's ARC
            // themselves rather than through wrapDest, mirroring X64Gen's
            // genSetField / SetMember / StoreGlobal.
            if (op == Op::RawSet && ok) {
                // §15: the field owns its value — release the field's OLD ref and
                // retain the new one (so a read that borrows+drops it can't free it).
                if (in.d > 0) {   // fixed-offset slot
                    llvm::Value* slotPtr = payAddr(b, regs[in.b], i64C(16 + (in.d - 1) * 16));
                    copyLV(b, arcScratch, slotPtr);
                    copyLV(b, slotPtr, regs[in.a]);
                } else {          // dynamic key
                    b.CreateCall(rtGetfield, {arcScratch, regs[in.b], cstr(in.sname)});
                    b.CreateCall(rtSetfield, {regs[in.b], cstr(in.sname), regs[in.a]});
                }
                b.CreateCall(rtRelease, {arcScratch});
                b.CreateCall(rtRetain, {regs[in.a]});
            } else if (op == Op::RawSetWeak && ok) {
                if (in.d > 0) {
                    llvm::Value* slotPtr = payAddr(b, regs[in.b], i64C(16 + (in.d - 1) * 16));
                    b.CreateCall(rtWeakStore, {slotPtr, regs[in.a]});
                } else {
                    b.CreateCall(rtWeakSetfield, {regs[in.b], cstr(in.sname), regs[in.a]});
                }
            } else if (op == Op::SetMember && ok) {
                // Name→slot store; lvrt_setm does the field slot's ARC internally
                // (release old, retain new — ruled 2026-07-05), so nothing more
                // is needed here. A user setter may throw — check the accessor
                // path (§7). SetMember is dk==0, so there is no wrapDest tail to
                // race with the dispatch's branch-away.
                b.CreateCall(rtSetm, {regs[in.b], cstr(in.sname), regs[in.a]});
                emitThrowCheck((int)pc);
            } else if (op == Op::StoreGlobal && ok) {
                // global[in.b] = reg[in.a]: release the OLD global value, store the
                // new one, retain it. Fixes bug #73 — the earlier parity-with-X64Gen
                // path (X64Gen.cpp:3504-3510) skipped the release-old on the false
                // premise that globals are write-once, so reassigning a global
                // Array<T> grown by `xs = xs.add(...)` leaked every superseded COW
                // buffer (O(N²) live bytes, `lvrt: heap exhausted` near N≈10k). The
                // globals array is zero-initialized (see §691), so the FIRST store
                // releases a zeroed slot — lvrt_release no-ops on that (lv_runtime.c
                // :274). Retain-new before release-old keeps a self-assign (`xs = xs`)
                // safe. Mirrors the RawSet field release-old/retain-new above.
                llvm::Value* g = b.CreateGEP(
                    ArrayType::get(lvTy, mod.nglobals), globalsArray,
                    {i64C(0), i64C(in.b)});
                copyLV(b, arcScratch, g);          // stash old
                copyLV(b, g, regs[in.a]);          // store new
                b.CreateCall(rtRetain, {regs[in.a]});
                b.CreateCall(rtRelease, {arcScratch});   // drop old (no-op if zeroed)
            }
            if (!terminated) b.CreateBr(blocks[pc + 1]);
        }
        b.SetInsertPoint(blocks[irfn.code.size()]);
        releaseAllRegs();
        restoreArena();
        storeVoid(b, retSlot);
        b.CreateRetVoid();
    }
};

}  // namespace

std::string LlvmGen::emitIr() {
    Gen g(mod_, sink_);
    if (!g.run()) return "";
    std::string out;
    raw_string_ostream os(out);
    g.module->print(os, nullptr);
    return out;
}

bool LlvmGen::emitObject(const std::string& path, const std::string& tripleArg, int optLevel) {
    Gen g(mod_, sink_);
    // Track 10 §7: decide the Windows reject BEFORE lowering — the seam natives
    // are emitted during run(), and threads have no Windows lowering (LV_TLS is
    // non-TLS there). A cross --target names the format; the host default never
    // is Windows. Emitted only when spawn/Channel are actually reached, so a
    // thread-free Windows program (corpus_win_wine) never trips it.
    if (!tripleArg.empty()) {
        std::string t = Triple::normalize(tripleArg);
        g.targetWindows = Triple(t).isOSWindows();
    }
    if (!g.run()) return false;

    // Host emission registers only the native target; a cross triple
    // (--target, Track B doc §6 B-M4) needs the full registry. Both calls
    // are idempotent, so the split costs nothing on repeat invocations.
    if (tripleArg.empty()) {
        InitializeNativeTarget();
        InitializeNativeTargetAsmPrinter();
    } else {
        InitializeAllTargetInfos();
        InitializeAllTargets();
        InitializeAllTargetMCs();
        InitializeAllAsmPrinters();
    }

    std::string triple = tripleArg.empty() ? sys::getDefaultTargetTriple()
                                           : Triple::normalize(tripleArg);
    std::string err;
    const Target* target = TargetRegistry::lookupTarget(triple, err);
    if (!target) { sink_.error({}, "LLVM backend: " + err); return false; }

    bool o0 = optLevel <= 0;
    TargetMachine* tm = target->createTargetMachine(
        triple, "generic", "", TargetOptions(), Reloc::PIC_, std::nullopt,
        o0 ? CodeGenOptLevel::None : CodeGenOptLevel::Default);
    g.module->setDataLayout(tm->createDataLayout());
    g.module->setTargetTriple(triple);

    // Pin the TLS access model to the target's binary format (see the global
    // declarations in Gen's ctor). ELF gets initial-exec (one %fs-relative
    // load — the design's arena/throw fast path). Windows/COFF keeps these as
    // plain (non-TLS) globals: threads are POSIX/LLVM-only in v1, so a Windows
    // build never spawns, and mingw's emulated-TLS ABI does not match LLVM's
    // native COFF TLS lowering (undefined-symbol SECREL link failure). The
    // runtime C side matches via the same `!defined(_WIN32)` gate. Other
    // formats (Mach-O) take the portable general-dynamic model.
    {
        Triple tt(triple);
        if (tt.isOSWindows()) {
            g.gThrowing->setThreadLocal(false);
            g.gArenaCursor->setThreadLocal(false);
        } else {
            auto model = tt.isOSBinFormatELF()
                             ? GlobalVariable::InitialExecTLSModel
                             : GlobalVariable::GeneralDynamicTLSModel;
            g.gThrowing->setThreadLocalMode(model);
            g.gArenaCursor->setThreadLocalMode(model);
        }
    }

    // §9 A-M6 hurdle H-12: replace the legacy::PassManager-only flow (codegen
    // only, no IR-level optimization at all) with a real PassBuilder module
    // pipeline before object emission — inlining, InstCombine, GVN, etc. The
    // codegen (isel/emission) pass below still goes through the legacy PM;
    // LLVM 18's TargetMachine::addPassesToEmitFile has no new-PM overload yet.
    PassBuilder pb(tm);
    LoopAnalysisManager lam;
    FunctionAnalysisManager fam;
    CGSCCAnalysisManager cgam;
    ModuleAnalysisManager mam;
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);
    ModulePassManager mpm = o0 ? pb.buildO0DefaultPipeline(OptimizationLevel::O0)
                              : pb.buildPerModuleDefaultPipeline(OptimizationLevel::O2);
    mpm.run(*g.module, mam);

    std::error_code ec;
    raw_fd_ostream out(path, ec, sys::fs::OF_None);
    if (ec) { sink_.error({}, "LLVM backend: cannot write '" + path + "'"); return false; }

    legacy::PassManager pm;
    if (tm->addPassesToEmitFile(pm, out, nullptr, CodeGenFileType::ObjectFile)) {
        sink_.error({}, "LLVM backend: target cannot emit object files");
        return false;
    }
    pm.run(*g.module);
    out.flush();
    return true;
}
