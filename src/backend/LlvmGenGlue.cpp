// refactor_1 session 05 (techdesign-05-llvmgen-split-fable.md): runtime-glue
// emission — string/constant pools, class registration, dispatch trampoline,
// arena helpers, columnar descriptors, reachability. Moved verbatim from
// LlvmGen.cpp.
#include "backend/LlvmGenInternal.hpp"

namespace llvmDetail {

    int Gen::classIdOf(Symbol* cls) {
        auto it = clsIds.find(cls);
        if (it != clsIds.end()) return it->second;
        int id = (int)clsIds.size() + 1;   // 1-based, mirrors X64Gen::clsId
        clsIds[cls] = id;
        return id;
    }

    // A prelude/global class by name (X64Gen::lookupClsId); 0 if absent.
    int Gen::classIdByName(const char* name) {
        Symbol* s = mod.sema ? mod.sema->global->lookup(name) : nullptr;
        return s ? classIdOf(s) : 0;
    }
    Symbol* Gen::classByName(const char* name) {
        return mod.sema ? mod.sema->global->lookup(name) : nullptr;
    }

    // Non-method slots in shape order — the packed-slot key list, mirroring
    // X64Gen::fieldKeys (distinct fields reach by "Source::name").
    std::vector<std::string> Gen::fieldKeysOf(Symbol* cls) const {
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

    Constant* Gen::cstr(const std::string& s) {
        auto it = cstrCache.find(s);
        if (it != cstrCache.end()) return it->second;
        Constant* init = ConstantDataArray::getString(ctx, s, /*AddNull=*/true);
        auto* gv = new GlobalVariable(*module, init->getType(), true,
                                      GlobalValue::PrivateLinkage, init, "cstr");
        cstrCache[s] = gv;
        return gv;
    }

    void Gen::fail(const std::string& what) {
        if (ok) sink.error({}, "LLVM backend: " + what);
        ok = false;
    }

    // A string literal becomes a private constant global {i64 len, i8 bytes[len+1]}
    // (the descriptor layout of doc-2 §2.4; the trailing NUL is for safe C
    // interop, not semantically part of the string). Literals carry NO LvHeader
    // and live outside the runtime's registered regions, so retain/release/
    // temp-free all skip them by the region check. Dedup by content.
    llvm::Value* Gen::stringGlobal(IRBuilder<>& b, const std::string& s) {
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
    void Gen::computeReachable() {
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

        // Track W hard-03 tier 1: user-reachability — the SAME edges as the
        // walk above, but rooted at @main only. @ginit (prelude + namespace
        // global initializers) is deliberately NOT a root: a gated native
        // reached only through prelude initialization is tier 2's runtime
        // trap, never a compile-time brick (the target-selected prelude lowers
        // into every module — Resolver.cpp's parsePrelude, per-target segment
        // load — so a blanket compile-time fail would brick every wasm build).
        // The coll
        // seeding is skipped: Array/Map/Range bodies hold no gated natives.
        userReach.assign(mod.functions.size(), false);
        {
            std::vector<int> uwork;
            std::vector<Symbol*> uinst;
            auto umark = [&](int fi) {
                if (fi >= 0 && fi < (int)mod.functions.size() && !userReach[fi]) {
                    userReach[fi] = true;
                    uwork.push_back(fi);
                }
            };
            umark(mod.entry);
            while (!uwork.empty()) {
                int fi = uwork.back(); uwork.pop_back();
                for (const Inst& in : mod.functions[fi].code) {
                    if (in.op == Op::Call) umark(in.b);
                    else if (in.op == Op::NewObject) {
                        if (in.b >= 0) umark(in.b);
                        if (in.sym) {
                            bool seen = false;
                            for (Symbol* c : uinst) if (c == in.sym) seen = true;
                            if (!seen) {
                                uinst.push_back(in.sym);
                                std::vector<const Stmt*> mem;
                                collectMembersLG(in.sym, mem);
                                for (const Stmt* m : mem)
                                    if (mod.byDecl.count(m)) umark(mod.byDecl.at(m));
                            }
                        }
                    } else if (in.op == Op::CallDyn) {
                        if (in.decl && mod.byDecl.count(in.decl))
                            umark(mod.byDecl.at(in.decl));
                        // Unresolved by-name CallDyn: NO name-fallback here,
                        // deliberately diverging from scan() above (doc 04
                        // as-built, W-M2). The full walk's markByName is an
                        // EMISSION over-approximation (nothing may be pruned);
                        // reused verbatim for gating it marked every same-named
                        // method on every class — e.g. any `close()` on a
                        // generic stream reached Channel.close ->
                        // sysChannelClose and bricked every Timer user at
                        // compile time (found by the W-M2 async corpus). For
                        // this walk the receiver's class is covered anyway:
                        // every user-instantiated class marks ALL its members
                        // at its NewObject (collectMembersLG above), so a
                        // by-name edge adds only never-constructed classes.
                        // Under-marking is gate-safe by construction: a gated
                        // native missed here still compiles to the tier-2
                        // lvrt_unsupported trap — loud at runtime, never a
                        // silent pass and never wrong emission. (Receivers
                        // constructed only in @ginit take that trap tier too,
                        // consistent with "@ginit is not a root".)
                    } else if (in.op == Op::MakeClosure) umark(in.b);
                }
            }
        }
    }

    void Gen::collectBases(Symbol* cls, std::vector<Symbol*>& out) {
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
    void Gen::emitDispatchTrampoline() {
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
    void Gen::emitSpawnGlobalCheck() {
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
    void Gen::emitZeroSlots(IRBuilder<>& b, Function* fn, llvm::Value* slotsBase,
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
    llvm::Value* Gen::emitAllocPackedArena(IRBuilder<>& b, Function* fn, llvm::Value* word0,
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
    void Gen::emitArenaHelpers() {
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
    void Gen::emitColumnarDescriptors() {
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

    void Gen::emitClassRegistration(IRBuilder<>& b) {
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

}  // namespace llvmDetail
