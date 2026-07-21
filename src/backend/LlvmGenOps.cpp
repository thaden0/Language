// refactor_1 session 05 (techdesign-05-llvmgen-split-fable.md): the per-IR-op
// lowering — Gen::genFunction, including the destination-ownership / ARC
// retain-release emission — plus its single-user file-static helpers. Moved
// verbatim from LlvmGen.cpp.
#include "backend/LlvmGenInternal.hpp"

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

// Track W hard-03 (techdesign-02-backend-column.md §5): the wasm column's
// declared "Lost" subset (techdesign-00-overview.md §3) — the ONE table the
// CallNativeFn dispatch gates against. filesystem/dirs, raw TCP/UDP, TLS
// (rides raw TCP), argv/env, tty/termios, signals, raw threads/channels,
// blocking sync reads. NOT here (Kept row): console writes (sysWrite),
// timers, fd watches/event loop (the floor retargets them, doc 03), time,
// randomness, sysExit/sysSetExitCode, sysCpuCount, the sysTask* engine floor
// (async is doc 04's, not gated). Process spawn and sysResolve never reach
// this dispatch (no LLVM lowering exists — the generic "native floor
// function" fail already rejects them).
struct WasmGatedNative { const char* native; const char* wrapper; const char* why; };
static const WasmGatedNative* wasmGatedNative(const std::string& n) {
    static const WasmGatedNative kGated[] = {
        {"sysOpen",         "File",      "no filesystem in a browser"},
        {"sysStat",         "File",      "no filesystem in a browser"},
        {"sysMkdir",        "File",      "no filesystem in a browser"},
        {"sysRemove",       "File",      "no filesystem in a browser"},
        {"sysRename",       "File",      "no filesystem in a browser"},
        {"sysListDir",      "File",      "no filesystem in a browser"},
        {"sysRead",         "read",      "no blocking reads in a browser"},
        {"sysReadLine",     "readLine",  "no blocking reads in a browser"},
        {"sysTcpConnect",   "TcpClient", "no raw sockets in a browser"},
        {"sysTcpConnectNb", "TcpClient", "no raw sockets in a browser"},
        {"sysConnectResult","TcpClient", "no raw sockets in a browser"},
        {"sysTcpListen",    "TcpListener", "no raw sockets in a browser"},
        {"sysAccept",       "TcpListener", "no raw sockets in a browser"},
        {"sysSend",         "TcpStream", "no raw sockets in a browser"},
        {"sysRecv",         "TcpStream", "no raw sockets in a browser"},
        {"sysSocketBuffer", "TcpStream", "no raw sockets in a browser"},
        {"sysTlsConnect",   "Tls",       "no raw sockets in a browser"},
        {"sysTlsAccept",    "Tls",       "no raw sockets in a browser"},
        {"sysTlsHandshake", "Tls",       "no raw sockets in a browser"},
        {"sysTlsError",     "Tls",       "no raw sockets in a browser"},
        {"sysTlsAlpn",      "Tls",       "no raw sockets in a browser"},
        {"sysTlsVersion",   "Tls",       "no raw sockets in a browser"},
        {"sysRsaEncrypt",   "Rsa",       "no native crypto in a browser"},
        {"sysArgs",         "args",      "no argv in a browser"},
        {"sysEnv",          "env",       "no environment variables in a browser"},
        {"sysTermRaw",      "Terminal",  "no tty in a browser"},
        {"sysTermRestore",  "Terminal",  "no tty in a browser"},
        {"sysTermIsRaw",    "Terminal",  "no tty in a browser"},
        {"sysWinSize",      "Terminal",  "no tty in a browser"},
        {"sysSignalOpen",   "signals",   "no OS signals in a browser"},
        {"sysSignalNext",   "signals",   "no OS signals in a browser"},
        {"sysSignalClose",  "signals",   "no OS signals in a browser"},
        {"sysThreadTransfer","spawn",    "no shared-memory threads on wasm (v1)"},
        {"sysThreadStart",  "spawn",     "no shared-memory threads on wasm (v1)"},
        {"sysThreadResult", "spawn",     "no shared-memory threads on wasm (v1)"},
        {"sysChannelNew",   "Channel",   "no shared-memory threads on wasm (v1)"},
        {"sysChannelSend",  "Channel",   "no shared-memory threads on wasm (v1)"},
        {"sysChannelReceive","Channel",  "no shared-memory threads on wasm (v1)"},
        {"sysChannelClose", "Channel",   "no shared-memory threads on wasm (v1)"},
    };
    for (const WasmGatedNative& g : kGated)
        if (n == g.native) return &g;
    return nullptr;
}

}  // namespace

namespace llvmDetail {

    void Gen::genFunction(int index) {
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
            } else if (n == "bits") {
                // struct-equality design §8: the float's payload IS its raw
                // IEEE-754 bit pattern (§2.3) — bits() is a pure retag to
                // LV_INT, no reinterpret needed.
                row(2, [&] { storeTP(b, dst, i64C(1), loadPay(b, recv)); });
            } else if (n == "canonEq") {
                // struct-equality design §8: route both payloads (raw bits)
                // through the runtime's ONE canon (lvrt_canoneq); result bool.
                row(2, [&] {
                    llvm::Value* eq = b.CreateCall(rtCanonEq, {loadPay(b, recv), loadPay(b, arg(0))});
                    storeTP(b, dst, i64C(3), b.CreateZExt(eq, i64Ty));
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
                // struct-equality design §8: float bit access + canonical relation
                "bits", "canonEq",
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
                        // A direct/dynamic call's callee is an ORDINARY in-language
                        // function: it retains its own copy of each parameter on
                        // entry and releases it at its own exit (§15 ARC, entry
                        // setup above) — a self-contained +1/-1 that never touches
                        // the CALLER's reference. That's a different contract from
                        // a hand-written native row (e.g. "add" above), which
                        // explicitly frees/reuses a `consumed` receiver itself.
                        // `consumed` (in.b, COW self-append) only elides the
                        // CALLER's own release on the assumption the callee took
                        // the receiver's fate — true for a native row, false for
                        // an in-language callee, which leaves that one reference
                        // owned by nobody (leaked) once the shared tail below
                        // voids the window slot without releasing it. Release it
                        // here for both in-language call shapes (direct + by-name
                        // dynamic dispatch); the native-rows path further below
                        // keeps handling its own consumed contract unchanged.
                        if (in.decl && mod.byDecl.count(in.decl)) {   // resolved: direct call
                            std::vector<llvm::Value*> args{regs[in.a]};
                            for (int k = 0; k < in.d; ++k) args.push_back(regs[in.c + k]);
                            b.CreateCall(fns[mod.byDecl.at(in.decl)], args);
                            if (in.b) b.CreateCall(rtRelease, {regs[in.c]});
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
                                    if (in.b) b.CreateCall(rtRelease, {regs[in.c]});
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
                        // Track W hard-03 (doc 02 §5): the two-tier wasm
                        // capability gate, against the wasmGatedNative table
                        // above. Tier 1: a gated native in a user-reachable
                        // function (userReach, computeReachable) is a compile-
                        // time diagnostic — the Windows precedents' shape
                        // below. Tier 2: in a prelude body emitted only via
                        // @ginit/instantiation over-approximation, it compiles
                        // to the lvrt_unsupported trap (never returns; the
                        // int-0 store keeps the IR well-formed, the sysExit
                        // precedent). Non-wasm targets: byte-identical.
                        if (targetWasm) {
                            if (const WasmGatedNative* gw = wasmGatedNative(n)) {
                                if (userReach[index]) {
                                    fail(std::string("wasm-browser: '") + gw->wrapper +
                                         "' is not available on this target (" +
                                         gw->why + ")");
                                } else {
                                    b.CreateCall(rtUnsupported, {cstr(gw->wrapper)});
                                    storeTP(b, regs[in.a], i64C(1), i64C(0));
                                }
                                emitThrowCheck((int)pc);
                                break;
                            }
                        }
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
                        } else if (n == "sysRemove") {
                            b.CreateCall(rtSysRemove, {regs[in.a], arg(0)});
                        } else if (n == "sysRename") {
                            b.CreateCall(rtSysRename, {regs[in.a], arg(0), arg(1)});
                        } else if (n == "sysListDir") {
                            b.CreateCall(rtSysListDir, {regs[in.a], arg(0)});
                            retainDst();               // fresh Array<string> -> +1; None skips
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
                        } else if (n == "floatFromBits") {
                            // struct-equality design §8: int64 bits -> float. The
                            // int payload IS the bit pattern (§2.3) — a pure
                            // retag to LV_FLOAT (tag 2), no reinterpret.
                            storeTP(b, regs[in.a], i64C(2), loadPay(b, arg(0)));
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
                        } else if (n == "sysSpawn" || n == "sysPidfdOpen") {
                            // G-LANG-2 process floor (techdesign-spawn-llvm.md §5):
                            // pipes-spawn stays POSIX-only, the threads
                            // Windows-reject precedent (D4). The pty path is the
                            // sanctioned Windows child story (designs/pty/ 03).
                            if (targetWindows) {
                                fail("process spawn: unsupported on Windows (v1) — '" + n +
                                     "' has no Windows lowering (techdesign-spawn-llvm.md)");
                            } else if (n == "sysSpawn") {
                                b.CreateCall(rtSysSpawn, {regs[in.a], arg(0), arg(1)});
                                retainDst();   // fresh heap Array<int> -> +1 (D1, sysArgs parity)
                            } else {  // sysPidfdOpen
                                b.CreateCall(rtSysPidfdOpen, {regs[in.a], arg(0)});
                            }
                        } else if (n == "sysReap" || n == "sysKill") {
                            // Windows-clean since designs/pty/ 03 (D-P10/D-W3):
                            // both have registry-backed win32 bodies keyed by the
                            // pids lv_plat_pty_spawn hands out, so the reject
                            // narrowed to the two rows above. Scalar rows: no
                            // retain, no ABI change — gating surgery only.
                            if (n == "sysReap") {
                                b.CreateCall(rtSysReap, {regs[in.a], arg(0)});
                            } else {  // sysKill
                                b.CreateCall(rtSysKill, {regs[in.a], arg(0), arg(1)});
                            }
                        } else if (n == "sysPtySpawn" || n == "sysPtyResize") {
                            // Pty floor (designs/pty/ 02 §3.2): a SEPARATE arm from
                            // the sysSpawn family — no Windows reject, that is
                            // D-P8's runtime degrade. Since designs/pty/ 03 the
                            // win32 floor IS ConPTY; on a pre-1809 host the same
                            // binary still degrades at runtime to [].
                            if (n == "sysPtySpawn") {
                                b.CreateCall(rtSysPtySpawn,
                                             {regs[in.a], arg(0), arg(1), arg(2), arg(3), arg(4)});
                                retainDst();   // fresh heap Array<int> -> +1 (sysSpawn parity)
                            } else {  // sysPtyResize
                                b.CreateCall(rtSysPtyResize, {regs[in.a], arg(0), arg(1), arg(2)});
                            }
                        } else if (n == "sysHostI" || n == "sysHostS" ||
                                   n == "sysHostV") {
                            // Track W hard-06 (doc 05 §2-§4): the generic sync
                            // JS/DOM host bridge. One C entry for all three
                            // return-type-distinguished decls (op, h0, h1, a, b);
                            // the tag of the result is decided host-side by `op`.
                            // NOT gated (a wasm-GAINED capability, not a hard-03
                            // LOST one); on native lvrt_hostcall raises. Result
                            // retained +1 (fresh string/handle transfer; the
                            // int/void results no-op the retain via the
                            // immediate gate, exactly as sysRecv's None/"" path).
                            b.CreateCall(rtHostCall, {regs[in.a], arg(0), arg(1),
                                                      arg(2), arg(3), arg(4)});
                            retainDst();
                        } else if (n == "sysHostCloReg") {
                            // Register a handler closure as a doc-05 §5 root
                            // (retained host-side); returns its table index. The
                            // result is always an int index — no retainDst (a
                            // retain on an immediate would be a no-op anyway).
                            b.CreateCall(rtHostCloReg, {regs[in.a], arg(0)});
                        } else if (n == "sysHostEcho") {
                            // Reflective marshaler round-trip probe (doc 05 §8):
                            // fresh host-rendered string -> +1.
                            b.CreateCall(rtHostEcho, {regs[in.a], arg(0)});
                            retainDst();
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

}  // namespace llvmDetail
