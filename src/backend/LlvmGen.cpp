#include "backend/LlvmGenInternal.hpp"

namespace llvmDetail {

    Gen::Gen(const IrModule& m, DiagnosticSink& s) : mod(m), sink(s) {
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
        rtIdxSetMove = fn("lvrt_idxset_move", voidTy, {ptrTy, ptrTy, ptrTy, ptrTy});
        // Track 05 C3: struct-recursive Map key equality (primitives by value,
        // structs field-wise, classes by identity) — shared with lvrt_idxget's
        // map branch, so "has" agrees with "at"/"[]" on the same key.
        rtKeyEq      = fn("lvrt_keyeq", i32Ty, {ptrTy, ptrTy});
        // struct-equality design §8: canonEq(abits, bbits) -> i32; a runtime
        // call (not hand-built IR) — the map-key path never forces inline here.
        rtCanonEq    = fn("lvrt_canoneq", i32Ty, {i64Ty, i64Ty});
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
        rtSysRemove    = fn("lvrt_sysremove", voidTy, {ptrTy, ptrTy});
        rtSysRename    = fn("lvrt_sysrename", voidTy, {ptrTy, ptrTy, ptrTy});
        rtSysListDir   = fn("lvrt_syslistdir", voidTy, {ptrTy, ptrTy});
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
        // Pty floor (designs/pty/ 02 §3): lowers on ALL targets incl. Windows —
        // pre-S3 win32 stubs return the failure sentinels (D-P8 runtime degrade).
        rtSysPtySpawn  = fn("lvrt_sysptyspawn",  voidTy, {ptrTy, ptrTy, ptrTy, ptrTy, ptrTy, ptrTy});
        rtSysPtyResize = fn("lvrt_sysptyresize", voidTy, {ptrTy, ptrTy, ptrTy, ptrTy});
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
        // Track W hard-03 (doc 02 §5 tier 2): trap stub for gated natives in
        // emitted-but-not-user-reachable prelude bodies. Never returns.
        rtUnsupported  = fn("lvrt_unsupported", voidTy, {ptrTy});
        // Track W hard-06 (doc 05 §2-§4): the JS/DOM host-bridge seam.
        rtHostCall     = fn("lvrt_hostcall", voidTy,
                            {ptrTy, ptrTy, ptrTy, ptrTy, ptrTy, ptrTy});
        rtHostCloReg   = fn("lvrt_host_clo_reg", voidTy, {ptrTy, ptrTy});
        rtHostEcho     = fn("lvrt_hostecho", voidTy, {ptrTy, ptrTy});
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

    bool Gen::run() {
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

}  // namespace llvmDetail

using llvmDetail::Gen;

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
        g.targetWasm = Triple(t).isWasm();   // Track W hard-03: two-tier gate
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
        } else if (tt.isWasm()) {
            // Track W hard-01: single-threaded v1 — wasm TLS without shared
            // memory lowers to plain globals, and LocalExec is the honest
            // single-instance model. Revisited only when the Workers leg
            // opens (techdesign-04-async-jspi.md §7).
            g.gThrowing->setThreadLocalMode(GlobalVariable::LocalExecTLSModel);
            g.gArenaCursor->setThreadLocalMode(GlobalVariable::LocalExecTLSModel);
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
