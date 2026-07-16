#pragma once
#include "Diagnostic.hpp"
#include "Ir.hpp"
#include "Ownership.hpp"
#include "X64.hpp"
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

// The pure native backend: lowers the IR to x86-64 machine code and writes a
// standalone ELF — no g++, no linker, no libc. Each IR virtual register maps to
// a stack slot (a slot machine: load operands, compute, store result), so no
// register allocation is needed.
//
// Step 2 switches to TAGGED VALUES: every register/slot is a 16-byte pair
// [tag(8)][payload(8)], mirroring the interpreters' runtime Value so dynamic
// dispatch (getm/callm/opm/issub/index) works at runtime like the oracle.
// Tags follow the emit-C++ reference (CGen): 0 void, 1 int, 2 float, 3 bool,
// 4 string, 5 object, 6 array, 7 map, 8 none, 9 closure. Payload holds the
// int/bool value, or a heap pointer (string descriptor / object / array / map /
// closure). The calling convention returns a value in (RAX=tag, RDX=payload)
// and passes each argument as 16 bytes on the stack.
class X64Gen {
public:
    X64Gen(const IrModule& mod, DiagnosticSink& sink) : mod_(mod), sink_(sink) {}

    // Returns the ELF bytes, or "" on failure.
    std::string emit();

private:
    const IrModule& mod_;
    DiagnosticSink& sink_;
    Asm a_;
    bool ok_ = true;

    std::vector<size_t> funcOffset_;                  // code offset of each fn
    std::vector<std::pair<size_t, int>> callFixups_;  // (rel32 pos, fn index / helper)
    // data segment: [0..15] heap cursor cell, [16..23] empty string, then literals
    std::vector<uint8_t> data_;
    std::vector<std::pair<size_t, uint64_t>> dataFixups_;  // (imm64 code pos, data offset)
    std::unordered_map<std::string, uint64_t> strCache_;   // dedup literals -> data offset
    std::unordered_map<Symbol*, int> clsId_;               // class -> runtime id
    std::vector<bool> reachable_;                          // which IR fns are emitted
    std::vector<Symbol*> collClasses_;                     // Array/Map (in-language methods)
    std::vector<int> closureFns_;                          // lambda fn indices (CallValue dispatch)
    std::set<std::pair<int,int>> scopeOwned_;             // (fn,pc) allocs freed at frame exit (§15)
    size_t printIntOff_ = 0, printNlOff_ = 0, printBoolOff_ = 0, printStrOff_ = 0;
    size_t allocOff_ = 0, intToStrOff_ = 0, strConcatOff_ = 0, strEqOff_ = 0;
    size_t printValOff_ = 0;
    // object model (Step 2b): heap object = [classId(8)][fieldHead(8)]; each
    // field node = [next(8)][keyptr(8)][valtag(8)][valpay(8)] (keyptr interned,
    // so field lookup is pointer comparison).
    size_t mkObjOff_ = 0, getFieldOff_ = 0, setFieldOff_ = 0;
    size_t fieldCountOff_ = 0, fieldIndexOff_ = 0, capGetOff_ = 0, capSetOff_ = 0;
    size_t isValueClassOff_ = 0, copyValOff_ = 0;
    size_t hfreeOff_ = 0, retainOff_ = 0, releaseOff_ = 0, recursiveFreeOff_ = 0;   // §15 escaping-tier ARC
    size_t hallocOff_ = 0;                            // prefixed alloc (dense arrays)
    size_t vfreeOff_ = 0;                             // free a dead standalone value-struct copy
    size_t traceOff_ = 0;                             // debug ARC tracer (LANG_ARC_TRACE)
    bool arcTrace_ = false;                           // emit retain/release/free trace records
    size_t getmOff_ = 0, setmOff_ = 0, opmOff_ = 0;
    // collections (Step 2c): array = [len(8)][value(16)*len]; map =
    // [len(8)][entry(32)*len] with entry = [keytag,keypay,valtag,valpay].
    size_t arOff_ = 0, tsBuildOff_ = 0, mkArrOff_ = 0, mkMapOff_ = 0, arrAppendOff_ = 0;
    size_t arrSpreadOff_ = 0, arrFillOff_ = 0;
    size_t callmOff_ = 0, callNativeOff_ = 0, idxGetOff_ = 0, idxSetOff_ = 0, iterAtOff_ = 0;
    size_t callClosureOff_ = 0;
    // exceptions (Step 2e): globals in the data segment — g_throwing at [24],
    // g_thrown tag at [32], g_thrown payload at [40].
    size_t issubOff_ = 0, raiseOff_ = 0, raiseOobOff_ = 0, uncaughtOff_ = 0;
    // syscall runtime (Step 3): the std::sys floor as machine-code helpers.
    size_t cstrOff_ = 0, sysWriteOff_ = 0, sysReadLineOff_ = 0, sysReadOff_ = 0;
    size_t sysOpenOff_ = 0, sysCloseOff_ = 0, sysStatOff_ = 0;
    // event loop (Step 3b): a timer registry in the data segment at loopBase_.
    // Layout: [nextId(8)][pad(8)] then a fixed array of timer records
    // [active][id][due_ns][intervalMs][ticks][cb_tag][cb_pay] (56 bytes each).
    int loopBase_ = 0, watchBase_ = 0, pollBase_ = 0;
    static const int kMaxTimers = 64, kTimerRec = 56;
    static const int kMaxWatch = 64, kWatchRec = 40;   // [active][id][fd][cb_tag][cb_pay]
    // §15 escaping-tier ARC region (after the event-loop registries):
    //   [arcBase_+0]  live heap bytes  [arcBase_+8]  peak heap bytes
    //   [arcBase_+16 + class*8]  free-list head per power-of-two size class
    //   [arcBase_+16 + kSizeClasses*8]  heap mmap base (hfree's tier guard)
    int arcBase_ = 0;
    static const int kSizeClasses = 28;   // 2^4 .. 2^31
    static const int kHeapBytes = 0x8000000;   // the heap/arena mmap size (128 MiB)
    size_t nowNsOff_ = 0, timerAddOff_ = 0, timerCancelOff_ = 0;
    size_t loopStepOff_ = 0, hasWorkOff_ = 0, runLoopOff_ = 0;
    // sockets (Step 3c)
    size_t parseIpOff_ = 0, tcpConnectOff_ = 0, tcpListenOff_ = 0, acceptOff_ = 0;
    size_t sendOff_ = 0, recvOff_ = 0, watchAddOff_ = 0, watchCancelOff_ = 0;
    size_t strIndexOfOff_ = 0, strSubStrOff_ = 0, strToIntOff_ = 0, strTrimOff_ = 0;
    size_t strCaseOff_ = 0;                           // toUpper/toLower core (§15 strings tier)
    size_t floatToStrOff_ = 0;                        // "%f" renderer (float support)

    void fail(const std::string& what);
    void genFunction(int index);
    void genPrintInt();
    void genPrintNl();
    void genPrintBool();
    void genPrintStr();
    void genPrintVal();
    void genAlloc();
    void genIntToStr();
    void genFloatToStr();
    void genStrConcat();
    void genStrEq();
    void genMkObj();
    void genGetField();
    void genSetField();
    void genCapGet();
    void genCapSet();
    void genFieldCount();
    void genFieldIndex();
    void genIsValueClass();
    void genCopyVal();
    void genHfree();
    void genHalloc();
    void genVFree();
    void genRetain();
    void genRelease();
    void genRecursiveFree();
    void genTrace();
    void emitTrace(int opChar);   // trace (op, RSI=ptr, RAX=count) if arcTrace_
    std::vector<std::string> fieldKeys(Symbol* cls) const;
    void genGetm(const std::vector<Symbol*>& classes);
    void genSetm(const std::vector<Symbol*>& classes);
    void genOpm(const std::vector<Symbol*>& classes);
    void genAr();
    void genTsBuild();
    void genMkArr();
    void genMkMap();
    void genArrAppend();
    void genArrSpread();
    void genArrFill();
    void genIterAt();
    void genIdxGet(const std::vector<Symbol*>& classes);
    void genIdxSet(const std::vector<Symbol*>& classes);
    void genCallM(const std::vector<Symbol*>& classes);
    void genCallNative();
    void genCallClosure();
    void genIsSub();
    void genRaise();
    void genRaiseOob();
    void genUncaught();
    void genCstr();
    void genSysWrite();
    void genSysReadLine();
    void genSysRead();
    void genSysOpen();
    void genSysClose();
    void genSysStat();
    void genNowNs();
    void genTimerAdd();
    void genTimerCancel();
    void genHasWork();
    void genLoopStep();
    void genRunLoop();
    void genParseIp();
    void genTcpConnect();
    void genTcpListen();
    void genAccept();
    void genSend();
    void genRecv();
    void genWatchAdd();
    void genWatchCancel();
    void genStrIndexOf();
    void genStrSubStr();
    void genStrToInt();
    void genStrTrim();
    void genStrCase();
    void emitStrTempFree(int reg);   // free the string in reg iff unowned (rc 0) + heap
    int clsId(Symbol* cls);
    int lookupClsId(const char* name);   // clsId of a prelude class by name (0 if absent)
    static int opCode(TokenKind k);
    void addrImm(int reg, uint64_t dataOffset);       // movImm(reg, dataVAddr+off), fixed up later
    uint64_t internString(const std::string& s);      // append a (deduped) descriptor, return offset

    // A register/slot is 16 bytes: [rbp+base] = tag, [rbp+base+8] = payload.
    static int slot(int reg) { return -16 * (reg + 1); }
    void loadVal(int tagReg, int payReg, int v) { a_.load(tagReg, slot(v)); a_.load(payReg, slot(v) + 8); }
    void loadPay(int payReg, int v) { a_.load(payReg, slot(v) + 8); }
    void storeVal(int v, int tagReg, int payReg) { a_.store(tagReg, slot(v)); a_.store(payReg, slot(v) + 8); }
    // store a value whose tag is a small immediate and payload is in a register.
    // Payload is written first so passing RAX as payReg is safe (setting the tag
    // clobbers RAX).
    void storeTagged(int v, int tag, int payReg) {
        a_.store(payReg, slot(v) + 8);
        a_.movImm(RAX, (uint64_t)tag); a_.store(RAX, slot(v));
    }
};
