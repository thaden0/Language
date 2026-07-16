#include "X64Gen.hpp"
#include <functional>
#include <cstdlib>

void X64Gen::fail(const std::string& what) {
    if (ok_) sink_.error({}, "native-elf backend: " + what);
    ok_ = false;
}

int X64Gen::clsId(Symbol* cls) {
    auto it = clsId_.find(cls);
    if (it != clsId_.end()) return it->second;
    int id = (int)clsId_.size() + 1;
    clsId_[cls] = id;
    return id;
}

// TokenKind -> operator index (matches the emit-C++ reference so operator
// methods dispatch identically): + == != - * / % < > <= >=.
int X64Gen::opCode(TokenKind k) {
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

// Strip a distinct "Source::name" key down to the bare member name.
static std::string stripColons(const std::string& s) {
    auto p = s.rfind("::");
    return p == std::string::npos ? s : s.substr(p + 2);
}

// A symbolic selector's operator index (same table as opCode).
static int symbolOp(const std::string& sym) {
    static const char* names[] = {"+","==","!=","-","*","/","%","<",">","<=",">=","&","|","<<",">>","^"};
    for (int k = 0; k < 16; ++k) if (sym == names[k]) return k;
    return -1;
}

// All accessor/method/operator members of a class, walking base classes.
static void collectMembers(Symbol* cls, std::vector<const Stmt*>& out) {
    if (!cls || !cls->decl) return;
    for (const StmtPtr& m : cls->decl->body)
        if (m->kind == StmtKind::Member && m->callable) out.push_back(m.get());
    for (const TypeRefPtr& b : cls->decl->bases) collectMembers(b->resolvedSymbol, out);
}

// Strings are heap descriptors: [i64 length][bytes]. A string value is a
// pointer to one. Literals live in the data segment; concat allocates.
void X64Gen::addrImm(int reg, uint64_t dataOffset) {
    a_.movImm(reg, 0);                                   // placeholder, fixed up later
    dataFixups_.push_back({a_.here() - 8, dataOffset});
}

uint64_t X64Gen::internString(const std::string& s) {
    if (s.empty()) return 16;                      // the shared empty-string descriptor
    auto it = strCache_.find(s);
    if (it != strCache_.end()) return it->second;  // dedup: identical content shares a descriptor
    uint64_t off = data_.size();
    uint64_t len = s.size();
    for (int i = 0; i < 8; ++i) data_.push_back((len >> (8 * i)) & 0xff);
    for (char c : s) data_.push_back((uint8_t)c);
    strCache_[s] = off;
    return off;
}

// alloc(rdi = size) -> rax = pointer. Arena (§15 scope tier): a raw bump of the
// arena cursor, bulk-freed at frame exit. Heap (escaping tier): the size is
// rounded up to its power-of-two class (2^4..2^31) and a dead block is popped
// from that class's free list when hfree has parked one there — re-zeroed,
// because callers rely on alloc returning zeroed memory (mkarr's void elements,
// mkobj before its own zeroing loop, etc.) — else carved from the bump cursor.
// live/peak accounting counts the ROUNDED size, mirroring hfree exactly.
void X64Gen::genAlloc() {
    allocOff_ = a_.here();
    addrImm(RCX, 56); a_.loadInd(RAX, RCX); a_.testRR(RAX);   // g_use_arena
    size_t useHeap = a_.je();
    addrImm(RCX, 48);                // arena: unrounded bump, no accounting
    a_.loadInd(RAX, RCX); a_.movRR(RDX, RAX); a_.addRR(RDX, RDI); a_.storeInd(RCX, RDX);
    a_.ret();
    a_.patchRel(useHeap, a_.here());
    // round up to the class size: rdx = 16 << class >= rdi, rcx = class
    a_.movImm(RDX, 16); a_.xorRR(RCX, RCX);
    size_t rl = a_.here();
    a_.cmpRR(RDX, RDI); size_t rdone = a_.jge();
    a_.cmpImm(RCX, kSizeClasses - 1); size_t rcap = a_.jge();
    a_.shlImm(RDX, 1); a_.addImm(RCX, 1);
    size_t rb = a_.jmp(); a_.patchRel(rb, rl);
    a_.patchRel(rdone, a_.here()); a_.patchRel(rcap, a_.here());
    // free-list pop: head at [arcBase_+16 + class*8]
    addrImm(R8, arcBase_ + 16); a_.movRR(R9, RCX); a_.shlImm(R9, 3); a_.addRR(R8, R9);
    a_.loadInd(RAX, R8); a_.testRR(RAX); size_t bump = a_.je();
    a_.loadInd(R9, RAX); a_.storeInd(R8, R9);          // head = next
    if (arcTrace_) {                                   // 'A': recycled-block pop
        a_.push(RAX); a_.push(RDX);
        a_.movRR(RSI, RAX); a_.movImm(RDI, 'A');       // rdx already = rounded size
        callFixups_.push_back({a_.call(), -68});
        a_.pop(RDX); a_.pop(RAX);
    }
    a_.push(RAX); a_.push(RDX);                        // re-zero the recycled block
    a_.movRR(RDI, RAX); a_.movRR(RCX, RDX); a_.xorRR(RAX, RAX); a_.repStosb();
    a_.pop(RDX); a_.pop(RAX);
    size_t acct = a_.jmp();
    a_.patchRel(bump, a_.here());                      // no free block: bump the cursor
    addrImm(RCX, 0); a_.loadInd(RAX, RCX); a_.movRR(R9, RAX); a_.addRR(R9, RDX); a_.storeInd(RCX, R9);
    a_.patchRel(acct, a_.here());
    // §15 accounting: live += rounded; peak = max(peak, live)
    a_.push(RAX);
    addrImm(RCX, arcBase_); a_.loadInd(RAX, RCX); a_.addRR(RAX, RDX); a_.storeInd(RCX, RAX);
    addrImm(RCX, arcBase_ + 8); a_.loadInd(R9, RCX); a_.cmpRR(RAX, R9); size_t noPeak = a_.jle();
    a_.storeInd(RCX, RAX);
    a_.patchRel(noPeak, a_.here());
    a_.pop(RAX);
    a_.ret();
}

// int_to_str(rdi = value) -> rax = string descriptor pointer.
void X64Gen::genIntToStr() {
    intToStrOff_ = a_.here();
    a_.push(RBP); a_.movRR(RBP, RSP); a_.subImm(RSP, 64);
    a_.movImm(RCX, 10);
    a_.lea(RSI, RBP, -1);
    a_.movRR(RAX, RDI);
    a_.xorRR(R8, R8);
    a_.testRR(RAX);
    size_t jns = a_.jcc(9);
    a_.neg(RAX); a_.movImm(R8, 1);
    a_.patchRel(jns, a_.here());
    // INT64_MIN is the one value two's-complement negation can't fix (it has
    // no positive counterpart — neg(RAX) leaves it negative, e.g. reachable
    // via `1 << 63`, Track 01 F1); an immortal data-segment literal is exactly
    // right here (same shape as float_to_str's nan/inf special cases) — the
    // ARC range check skips non-heap addresses, so no halloc/retain needed.
    a_.testRR(RAX); size_t notMin = a_.jcc(9);
    addrImm(RAX, internString("-9223372036854775808"));
    a_.leave(); a_.ret();
    a_.patchRel(notMin, a_.here());
    size_t loop = a_.here();
    a_.cqo(); a_.idiv(RCX);
    a_.u8(0x80); a_.u8(0xC2); a_.u8(0x30);       // add dl, '0'
    a_.u8(0x88); a_.u8(0x16);                     // mov [rsi], dl
    a_.decR(RSI);
    a_.testRR(RAX);
    size_t jnz = a_.jcc(5); a_.patchRel(jnz, loop);
    a_.testRR(R8);
    size_t jz = a_.jcc(4);
    a_.u8(0xC6); a_.u8(0x06); a_.u8(0x2D);        // mov byte [rsi], '-'
    a_.decR(RSI);
    a_.patchRel(jz, a_.here());
    a_.incR(RSI);                                 // rsi -> first char
    a_.movRR(R9, RBP); a_.subRR(R9, RSI);         // len
    // Bookkeeping slots live BELOW the digit buffer (which grows down from
    // [rbp-1]); an 8-byte store at [rbp-8] would otherwise clobber the digits.
    a_.store(RSI, -40); a_.store(R9, -48);
    a_.movRR(RDI, R9); a_.addImm(RDI, 8);
    callFixups_.push_back({a_.call(), -69});      // halloc: §15 ARC-prefixed heap string
    a_.load(RCX, -48); a_.storeInd(RAX, RCX);     // [ptr] = len
    a_.store(RAX, -56);
    a_.lea(RDI, RAX, 8);                          // dst = ptr+8
    a_.load(RSI, -40);                            // src = digits
    a_.load(RCX, -48);                            // count = len
    a_.repMovsb();
    a_.load(RAX, -56);
    a_.leave(); a_.ret();
}

// float_to_str(rdi = double bits) -> rax = string descriptor. Renders
// std::to_string(double)'s "%f" shape: sign, integer digits, '.', exactly six
// fraction digits (round-half-up at the 6th — engines agree on every corpus
// value; the half-even tie is not exercised). NaN/inf render "nan"/"inf"/
// "-inf". |x| >= 2^63 (integer part exceeds int64) renders "<float>" — a
// documented v1 limit of the pure backend's formatter.
void X64Gen::genFloatToStr() {
    floatToStrOff_ = a_.here();
    a_.push(RBP); a_.movRR(RBP, RSP); a_.subImm(RSP, 96);
    // NaN / inf: exponent bits all ones
    a_.movRR(RAX, RDI); a_.movImm(RCX, 0x7FF0000000000000ULL); a_.andRR(RAX, RCX);
    a_.cmpRR(RAX, RCX); size_t finite = a_.jne();
    a_.movRR(RAX, RDI); a_.movImm(RCX, 0x000FFFFFFFFFFFFFULL); a_.andRR(RAX, RCX);
    a_.testRR(RAX); size_t isInf = a_.je();
    addrImm(RAX, internString("nan")); a_.leave(); a_.ret();
    a_.patchRel(isInf, a_.here());
    a_.movRR(RAX, RDI); a_.shrImm(RAX, 63); a_.testRR(RAX); size_t posInf = a_.je();
    addrImm(RAX, internString("-inf")); a_.leave(); a_.ret();
    a_.patchRel(posInf, a_.here());
    addrImm(RAX, internString("inf")); a_.leave(); a_.ret();
    a_.patchRel(finite, a_.here());
    // sign flag -> r8; abs bits -> rdi (positive doubles order like their bits)
    a_.movRR(R8, RDI); a_.shrImm(R8, 63);
    a_.movImm(RCX, 0x7FFFFFFFFFFFFFFFULL); a_.andRR(RDI, RCX);
    a_.movImm(RCX, 0x43E0000000000000ULL);              // 2^63 as double
    a_.cmpRR(RDI, RCX); size_t inRange = a_.jl();
    addrImm(RAX, internString("<float>")); a_.leave(); a_.ret();
    a_.patchRel(inRange, a_.here());
    a_.movqXmmR(0, RDI);
    a_.cvttsd2si(RAX, 0); a_.store(RAX, -64);           // ipart (>= 0)
    a_.cvtsi2sd(1, RAX); a_.subsd(0, 1);                // frac in [0, 1)
    a_.movImm(RCX, 0x412E848000000000ULL);              // 1e6
    a_.movqXmmR(1, RCX); a_.mulsd(0, 1);
    a_.movImm(RCX, 0x3FE0000000000000ULL);              // 0.5 (round half up)
    a_.movqXmmR(1, RCX); a_.addsd(0, 1);
    a_.cvttsd2si(RCX, 0);                               // frac6
    a_.cmpImm(RCX, 1000000); size_t noCarry = a_.jl();  // 0.9999996 -> carry out
    a_.xorRR(RCX, RCX);
    a_.load(RAX, -64); a_.addImm(RAX, 1); a_.store(RAX, -64);
    a_.patchRel(noCarry, a_.here());
    a_.store(RCX, -72);
    // digits right-to-left from [rbp-1] (max 27 bytes; bookkeeping at -64.. is
    // clear): six fraction digits, '.', integer digits, sign.
    a_.lea(RSI, RBP, -1);
    a_.movImm(RCX, 10);
    a_.load(RAX, -72); a_.movImm(R9, 6);
    size_t floop = a_.here();
    a_.cqo(); a_.idiv(RCX);
    a_.u8(0x80); a_.u8(0xC2); a_.u8(0x30);              // add dl, '0'
    a_.u8(0x88); a_.u8(0x16);                           // mov [rsi], dl
    a_.decR(RSI);
    a_.decR(R9); a_.testRR(R9);
    size_t fnz = a_.jne(); a_.patchRel(fnz, floop);
    a_.u8(0xC6); a_.u8(0x06); a_.u8(0x2E);              // mov byte [rsi], '.'
    a_.decR(RSI);
    a_.load(RAX, -64);
    size_t iloop = a_.here();
    a_.cqo(); a_.idiv(RCX);
    a_.u8(0x80); a_.u8(0xC2); a_.u8(0x30);
    a_.u8(0x88); a_.u8(0x16);
    a_.decR(RSI);
    a_.testRR(RAX);
    size_t inz = a_.jne(); a_.patchRel(inz, iloop);
    a_.testRR(R8); size_t noSign = a_.je();
    a_.u8(0xC6); a_.u8(0x06); a_.u8(0x2D);              // mov byte [rsi], '-'
    a_.decR(RSI);
    a_.patchRel(noSign, a_.here());
    a_.incR(RSI);                                       // rsi -> first char
    a_.movRR(R9, RBP); a_.subRR(R9, RSI);               // len
    a_.store(RSI, -80); a_.store(R9, -88);
    a_.movRR(RDI, R9); a_.addImm(RDI, 8);
    callFixups_.push_back({a_.call(), -69});            // halloc (§15 prefixed)
    a_.load(RCX, -88); a_.storeInd(RAX, RCX);           // [ptr] = len
    a_.store(RAX, -64);
    a_.lea(RDI, RAX, 8);
    a_.load(RSI, -80); a_.load(RCX, -88);
    a_.repMovsb();
    a_.load(RAX, -64);
    a_.leave(); a_.ret();
}

// str_concat(rdi = A, rsi = B) -> rax = new descriptor.
void X64Gen::genStrConcat() {
    strConcatOff_ = a_.here();
    a_.push(RBP); a_.movRR(RBP, RSP); a_.subImm(RSP, 64);
    a_.store(RDI, -8); a_.store(RSI, -16);
    a_.loadInd(R8, RDI); a_.loadInd(R9, RSI);     // lenA, lenB
    a_.store(R8, -24); a_.store(R9, -32);
    a_.movRR(RDI, R8); a_.addRR(RDI, R9); a_.addImm(RDI, 8);
    callFixups_.push_back({a_.call(), -69});      // halloc -> rax (§15 prefixed)
    a_.store(RAX, -40);
    a_.load(RCX, -24); a_.load(RDX, -32); a_.addRR(RCX, RDX);
    a_.storeInd(RAX, RCX);                         // [ptr] = total len
    a_.lea(RDI, RAX, 8);                           // dst
    a_.load(RSI, -8); a_.addImm(RSI, 8);          // src = A+8
    a_.load(RCX, -24); a_.repMovsb();             // copy A (rdi advances)
    a_.load(RSI, -16); a_.addImm(RSI, 8);         // src = B+8
    a_.load(RCX, -32); a_.repMovsb();             // copy B
    // §15 strings tier: concat CONSUMES unowned operands. A refcount-0 heap
    // string fed to concat is a dropped temporary the instant its bytes are
    // copied (ts_build accumulator chains, int_to_str results) — free it here
    // so builder chains stay flat. Owned (rc>=1), literal (data segment), and
    // arena (rc -1) operands all fail emitStrTempFree's guards and pass
    // untouched. B==A (the same temp twice) is guarded: consume it once.
    a_.load(RCX, -8); emitStrTempFree(RCX);
    a_.load(RCX, -16); a_.load(RAX, -8); a_.cmpRR(RCX, RAX); size_t sameAB = a_.je();
    emitStrTempFree(RCX);
    a_.patchRel(sameAB, a_.here());
    a_.load(RAX, -40);
    a_.leave(); a_.ret();
}

// Free the string descriptor in `reg` iff it is an UNOWNED (refcount 0) heap
// block — the §15 strings-tier "dropped temporary" test. The range check runs
// FIRST: literals live in the data segment below the heap mmap (reading their
// [P-16] would read unrelated data), and arena strings live in the arena mmap
// (their -1 sentinel is belt and braces — the range check already excludes
// them). Clobbers RAX, RDI, RSI and reg itself.
void X64Gen::emitStrTempFree(int reg) {
    addrImm(RAX, arcBase_ + 16 + kSizeClasses * 8); a_.loadInd(RAX, RAX);   // heap base
    a_.cmpRR(reg, RAX); size_t lo = a_.jl();
    a_.addImm(RAX, kHeapBytes); a_.cmpRR(reg, RAX); size_t hi = a_.jge();
    a_.loadMem(RAX, reg, -16); a_.testRR(RAX); size_t owned = a_.jne();     // rc != 0 -> not a temp
    a_.loadMem(RSI, reg, -8); a_.movRR(RDI, reg); a_.subImm(RDI, 16);
    callFixups_.push_back({a_.call(), -64});                                // hfree(P-16, [P-8])
    a_.patchRel(lo, a_.here()); a_.patchRel(hi, a_.here()); a_.patchRel(owned, a_.here());
}

// str_eq(rdi = A, rsi = B) -> rax = 1 if equal (length + bytes), else 0.
void X64Gen::genStrEq() {
    strEqOff_ = a_.here();
    a_.loadInd(R8, RDI); a_.loadInd(R9, RSI);     // lenA, lenB
    a_.cmpRR(R8, R9);
    size_t neq = a_.jcc(5);                        // jne FALSE
    a_.testRR(R8);
    size_t bothEmpty = a_.je();                    // len 0 -> equal (avoid rep-cmpsb-with-0
                                                   // reading stale flags after the adds)
    a_.addImm(RDI, 8); a_.addImm(RSI, 8);
    a_.movRR(RCX, R8);
    a_.repeCmpsb();                                // compare rcx bytes
    size_t neq2 = a_.jcc(5);                       // jne FALSE (mismatch)
    a_.patchRel(bothEmpty, a_.here());
    a_.movImm(RAX, 1); a_.ret();
    a_.patchRel(neq, a_.here()); a_.patchRel(neq2, a_.here());
    a_.xorRR(RAX, RAX); a_.ret();
}

// print_str(rdi = descriptor): write its bytes to fd 1.
void X64Gen::genPrintStr() {
    printStrOff_ = a_.here();
    a_.loadInd(RDX, RDI);            // len
    a_.lea(RSI, RDI, 8);             // buffer
    a_.movImm(RAX, 1); a_.movImm(RDI, 1);
    a_.syscall_();
    a_.ret();
}

// mkobj(rdi = classId) -> rax = object pointer. Allocates [classId][fieldHead=0].
// Lever A: heap objects are PACKED — [classId(8)][fieldHead(8)][slot0(16)]...
// Declared fields live in fixed slots (index from the class shape); genuinely
// dynamic keys (closure captures) fall back to the fieldHead linked list.
std::vector<std::string> X64Gen::fieldKeys(Symbol* cls) const {
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

// mkobj(rdi = classId) -> rax = packed object; slots sized + zeroed from the
// class's declared field count.
void X64Gen::genMkObj() {
    mkObjOff_ = a_.here();
    a_.push(RDI);                          // classId
    callFixups_.push_back({a_.call(), -59});   // fieldcount(classId) -> rax
    a_.push(RAX);                          // save count
    // §15: a 16-byte ARC prefix precedes P — [P-16] refcount, [P-8] totalSize.
    // The object body [classId][fieldHead][slots] at P is unchanged.
    a_.movRR(RDI, RAX); a_.shlImm(RDI, 4); a_.addImm(RDI, 32);   // 16 prefix + 16 + count*16
    a_.push(RDI);                          // save totalSize
    callFixups_.push_back({a_.call(), -4});   // alloc -> rax = raw
    a_.pop(RDX); a_.storeMem(RAX, 8, RDX);    // [raw+8] = totalSize
    addrImm(RCX, 56); a_.loadInd(RCX, RCX); a_.testRR(RCX); size_t heap = a_.je();  // g_use_arena?
    a_.movImm(RDX, (uint64_t)-1); size_t setrc = a_.jmp();      // arena -> sentinel -1 (not refcounted)
    a_.patchRel(heap, a_.here()); a_.xorRR(RDX, RDX);           // heap -> refcount 0 (owned on first store)
    a_.patchRel(setrc, a_.here()); a_.storeMem(RAX, 0, RDX);    // [raw] = refcount
    a_.addImm(RAX, 16);                    // P = raw + 16
    a_.pop(RCX);                           // count
    a_.pop(RDX);                           // classId
    a_.storeInd(RAX, RDX);                 // [obj+0] = classId
    a_.xorRR(RDX, RDX); a_.storeMem(RAX, 8, RDX);   // [obj+8] = fieldHead (0)
    a_.lea(R8, RAX, 16);                    // ptr = &slot0
    size_t zl = a_.here();
    a_.testRR(RCX); size_t zdone = a_.je();
    a_.storeInd(R8, RDX); a_.storeMem(R8, 8, RDX);   // slot = (0,0)
    a_.addImm(R8, 16); a_.decR(RCX);
    size_t zb = a_.jmp(); a_.patchRel(zb, zl);
    a_.patchRel(zdone, a_.here());
    a_.ret();
}

// getfield(rdi = obj, rsi = keyptr) -> rax=tag, rdx=pay: packed slot by index,
// else the dynamic fallback (capget).
void X64Gen::genGetField() {
    getFieldOff_ = a_.here();
    a_.push(RDI); a_.push(RSI);
    a_.loadInd(RDI, RDI);                   // classId = [obj]
    callFixups_.push_back({a_.call(), -60});   // fieldindex(classId, keyptr)->rax (or -1)
    a_.pop(RSI); a_.pop(RDI);
    a_.cmpImm(RAX, 0); size_t dyn = a_.jl();   // < 0 -> dynamic fallback
    a_.shlImm(RAX, 4); a_.addImm(RAX, 16); a_.addRR(RAX, RDI);   // &slot
    a_.loadMem(RDX, RAX, 8);                // payload
    a_.loadMem(RAX, RAX, 0);                // tag
    a_.ret();
    a_.patchRel(dyn, a_.here());
    callFixups_.push_back({a_.call(), -57});   // capget (fallback list)
    a_.ret();
}

// setfield(rdi = obj, rsi = keyptr, rdx = valtag, rcx = valpay).
void X64Gen::genSetField() {
    setFieldOff_ = a_.here();
    a_.push(RDI); a_.push(RSI); a_.push(RDX); a_.push(RCX);
    a_.loadInd(RDI, RDI);                   // classId
    callFixups_.push_back({a_.call(), -60});   // fieldindex -> rax
    a_.pop(RCX); a_.pop(RDX); a_.pop(RSI); a_.pop(RDI);
    a_.cmpImm(RAX, 0); size_t dyn = a_.jl();
    a_.shlImm(RAX, 4); a_.addImm(RAX, 16); a_.addRR(RAX, RDI);   // &slot
    a_.storeMem(RAX, 0, RDX); a_.storeMem(RAX, 8, RCX);
    a_.ret();
    a_.patchRel(dyn, a_.here());
    callFixups_.push_back({a_.call(), -58});   // capset (fallback list)
    a_.ret();
}

// capget/capset: the dynamic-key fallback (a linked list at [obj+8]). Used for
// closure captures and any non-declared key.
void X64Gen::genCapGet() {
    capGetOff_ = a_.here();
    a_.loadMem(RCX, RDI, 8);               // node = fieldHead
    size_t loop = a_.here();
    a_.testRR(RCX); size_t jEnd = a_.je();
    a_.loadMem(RAX, RCX, 8); a_.cmpRR(RAX, RSI); size_t jHit = a_.je();
    a_.loadMem(RCX, RCX, 0); size_t jBack = a_.jmp(); a_.patchRel(jBack, loop);
    a_.patchRel(jHit, a_.here());
    a_.loadMem(RAX, RCX, 16); a_.loadMem(RDX, RCX, 24); a_.ret();
    a_.patchRel(jEnd, a_.here());
    a_.xorRR(RAX, RAX); a_.xorRR(RDX, RDX); a_.ret();
}
void X64Gen::genCapSet() {
    capSetOff_ = a_.here();
    a_.loadMem(R8, RDI, 8);
    size_t loop = a_.here();
    a_.testRR(R8); size_t jNew = a_.je();
    a_.loadMem(RAX, R8, 8); a_.cmpRR(RAX, RSI); size_t jHit = a_.je();
    a_.loadMem(R8, R8, 0); size_t jBack = a_.jmp(); a_.patchRel(jBack, loop);
    a_.patchRel(jHit, a_.here());
    a_.storeMem(R8, 16, RDX); a_.storeMem(R8, 24, RCX); a_.ret();
    a_.patchRel(jNew, a_.here());
    a_.push(RDI); a_.push(RSI); a_.push(RDX); a_.push(RCX);
    a_.movImm(RDI, 32);
    callFixups_.push_back({a_.call(), -4});
    a_.pop(RCX); a_.pop(RDX); a_.pop(RSI); a_.pop(RDI);
    a_.loadMem(R8, RDI, 8);
    a_.storeMem(RAX, 0, R8); a_.storeMem(RAX, 8, RSI);
    a_.storeMem(RAX, 16, RDX); a_.storeMem(RAX, 24, RCX);
    a_.storeMem(RDI, 8, RAX); a_.ret();
}

// fieldcount(rdi = classId) -> rax = declared field count (switch over classes).
void X64Gen::genFieldCount() {
    fieldCountOff_ = a_.here();
    std::vector<std::pair<Symbol*, int>> ids(clsId_.begin(), clsId_.end());
    std::vector<size_t> exits;
    for (auto& [sym, id] : ids) {
        int n = (int)fieldKeys(sym).size();
        if (n == 0) continue;
        a_.cmpImm(RDI, id); size_t nomatch = a_.jne();
        a_.movImm(RAX, (uint64_t)n); exits.push_back(a_.jmp());
        a_.patchRel(nomatch, a_.here());
    }
    a_.xorRR(RAX, RAX);                     // default 0
    for (size_t e : exits) a_.patchRel(e, a_.here());
    a_.ret();
}

// fieldindex(rdi = classId, rsi = keyptr) -> rax = slot index, or -1 (dynamic).
void X64Gen::genFieldIndex() {
    fieldIndexOff_ = a_.here();
    std::vector<std::pair<Symbol*, int>> ids(clsId_.begin(), clsId_.end());
    std::vector<size_t> exits;
    for (auto& [sym, id] : ids) {
        std::vector<std::string> keys = fieldKeys(sym);
        if (keys.empty()) continue;
        a_.cmpImm(RDI, id); size_t nextClass = a_.jne();
        for (size_t i = 0; i < keys.size(); ++i) {
            addrImm(RAX, internString(keys[i])); a_.cmpRR(RAX, RSI);
            size_t nk = a_.jne();
            a_.movImm(RAX, (uint64_t)i); exits.push_back(a_.jmp());
            a_.patchRel(nk, a_.here());
        }
        a_.movImm(RAX, (uint64_t)-1); exits.push_back(a_.jmp());   // in class, key not declared
        a_.patchRel(nextClass, a_.here());
    }
    a_.movImm(RAX, (uint64_t)-1);           // unknown class -> dynamic
    for (size_t e : exits) a_.patchRel(e, a_.here());
    a_.ret();
}

// isValueClass(rdi = classId) -> rax = 1 if a `struct` value type, else 0.
void X64Gen::genIsValueClass() {
    isValueClassOff_ = a_.here();
    std::vector<std::pair<Symbol*, int>> ids(clsId_.begin(), clsId_.end());
    std::vector<size_t> exits;
    for (auto& [sym, id] : ids) {
        if (!sym->isValue) continue;
        a_.cmpImm(RDI, id); size_t nomatch = a_.jne();
        a_.movImm(RAX, 1); exits.push_back(a_.jmp());
        a_.patchRel(nomatch, a_.here());
    }
    a_.xorRR(RAX, RAX);                      // default: not a value type
    for (size_t e : exits) a_.patchRel(e, a_.here());
    a_.ret();
}

// copyval(rdi = tag, rsi = pay) -> rax = tag, rdx = pay. A value struct is
// deep-copied (fresh object, each slot recursively copied); anything else —
// primitives, reference objects — passes through unchanged. Mirrors copyValue()
// in the interpreters and copyval() in emit-C++.
// hfree(rdi = RAW block base, rsi = requested size): return a dead heap block
// to the allocator. The size is rounded to its power-of-two class exactly as
// alloc rounds it (so the block re-enters the class it was carved for), the
// live counter is decremented by the rounded size, the block is pushed onto the
// class free list ([base] becomes the next pointer), and the payload is
// poisoned with 0xFE so a use-after-free reads loud garbage instead of
// stale-but-plausible data. Blocks outside the heap mmap (arena allocations
// that stray here, or a garbage pointer) are ignored entirely — the arena is
// bulk-freed at frame exit and must never feed the heap free lists.
// NOTE: callers pass the RAW base — for ARC-prefixed blocks that is P-16.
void X64Gen::genHfree() {
    hfreeOff_ = a_.here();
    // round: rdx = 16 << class >= rsi, rcx = class (capped — a garbage size
    // can't index past the free-list array)
    a_.movImm(RDX, 16); a_.xorRR(RCX, RCX);
    size_t rl = a_.here();
    a_.cmpRR(RDX, RSI); size_t rdone = a_.jge();
    a_.cmpImm(RCX, kSizeClasses - 1); size_t rcap = a_.jge();
    a_.shlImm(RDX, 1); a_.addImm(RCX, 1);
    size_t rb = a_.jmp(); a_.patchRel(rb, rl);
    a_.patchRel(rdone, a_.here()); a_.patchRel(rcap, a_.here());
    // tier guard: only blocks inside [heap base, heap base + kHeapBytes) join
    addrImm(R8, arcBase_ + 16 + kSizeClasses * 8); a_.loadInd(RAX, R8);
    a_.cmpRR(RDI, RAX); size_t skipLo = a_.jl();
    a_.movRR(R9, RAX); a_.addImm(R9, kHeapBytes);
    a_.cmpRR(RDI, R9); size_t skipHi = a_.jge();
    if (arcTrace_) {                                   // 'F': block pushed to the free list
        a_.push(RDI); a_.push(RSI); a_.push(RDX); a_.push(RCX);
        a_.movRR(RSI, RDI); a_.movImm(RDI, 'F');       // rdx = rounded size (already set)
        callFixups_.push_back({a_.call(), -68});
        a_.pop(RCX); a_.pop(RDX); a_.pop(RSI); a_.pop(RDI);
    }
    // live -= rounded (the alloc side added the rounded size)
    addrImm(R8, arcBase_); a_.loadInd(RAX, R8); a_.subRR(RAX, RDX); a_.storeInd(R8, RAX);
    // push: [base] = head; head = base
    addrImm(R8, arcBase_ + 16); a_.movRR(R9, RCX); a_.shlImm(R9, 3); a_.addRR(R8, R9);
    a_.loadInd(RAX, R8); a_.storeInd(RDI, RAX); a_.storeInd(R8, RDI);
    // poison everything past the next pointer
    a_.movRR(RCX, RDX); a_.subImm(RCX, 8); a_.addImm(RDI, 8);
    a_.movImm(RAX, 0xFE); a_.repStosb();
    a_.patchRel(skipLo, a_.here()); a_.patchRel(skipHi, a_.here());
    a_.ret();
}

// halloc(rdi = body size) -> rax = P: alloc body+16, write the 16-byte ARC prefix
// ([P-16] refcount 0/-1, [P-8] totalSize), return P = raw+16. Used by the dense
// value-struct array sites, which otherwise have no room for a refcount prefix.
void X64Gen::genHalloc() {
    hallocOff_ = a_.here();
    a_.addImm(RDI, 16);                                   // total = body + 16 (prefix)
    a_.push(RDI);
    callFixups_.push_back({a_.call(), -4});               // alloc(total) -> raw
    a_.pop(RDX); a_.storeMem(RAX, 8, RDX);               // [raw+8] = totalSize
    addrImm(RCX, 56); a_.loadInd(RCX, RCX); a_.testRR(RCX); size_t heap = a_.je();
    a_.movImm(RDX, (uint64_t)-1); size_t setrc = a_.jmp();
    a_.patchRel(heap, a_.here()); a_.xorRR(RDX, RDX);
    a_.patchRel(setrc, a_.here()); a_.storeMem(RAX, 0, RDX);   // [raw] = refcount
    a_.addImm(RAX, 16);                                   // P = raw + 16
    a_.ret();
}

// vfree(rdi=tag, rsi=pay): free a DEAD standalone value-struct copy — the
// return-site CopyVal of a struct-returning call, already copied out by the
// caller (Op::VFree). Value structs are uniquely owned (deep-copied at every
// bind), so once consumed the returned tree is provably unreachable. Guards:
// tag-5 value-struct objects with refcount 0 only ([P-16] — heap standalone;
// arena copies carry the -1 sentinel and are bulk-freed; Lower emits VFree
// only on in-language call results, never on pointers into dense buffers, so
// the prefix read is always a real prefix). Recurses into value-struct FIELDS
// (the deep copy allocated a fresh object per nested struct); reference-type
// fields are NOT released — struct copies share them uncounted, the original
// owner's counted ref outlives the copy.
void X64Gen::genVFree() {
    vfreeOff_ = a_.here();
    a_.cmpImm(RDI, 5); size_t notObj = a_.jne();
    a_.push(RBP); a_.movRR(RBP, RSP); a_.subImm(RSP, 32);
    a_.store(RSI, -8);                                   // P
    a_.loadInd(RDI, RSI);                                // classId
    callFixups_.push_back({a_.call(), -61});             // isValueClass
    a_.testRR(RAX); size_t notVal = a_.je();
    a_.load(RSI, -8); a_.loadMem(RAX, RSI, -16);         // refcount
    a_.testRR(RAX); size_t counted = a_.jne();           // != 0 -> arena/-1 or owned
    // free nested value-struct field copies first
    a_.loadInd(RDI, RSI);
    callFixups_.push_back({a_.call(), -59});             // fieldcount -> N
    a_.store(RAX, -16); a_.xorRR(RCX, RCX); a_.store(RCX, -24);
    size_t loop = a_.here();
    a_.load(RCX, -24); a_.load(RAX, -16); a_.cmpRR(RCX, RAX); size_t done = a_.jge();
    a_.load(RSI, -8); a_.load(RAX, -24); a_.shlImm(RAX, 4); a_.addRR(RAX, RSI); a_.addImm(RAX, 16);
    a_.loadMem(RDI, RAX, 0); a_.loadMem(RSI, RAX, 8);    // field (tag, pay)
    callFixups_.push_back({a_.call(), -70});             // vfree field (self-recursive)
    a_.load(RCX, -24); a_.addImm(RCX, 1); a_.store(RCX, -24);
    size_t back = a_.jmp(); a_.patchRel(back, loop);
    a_.patchRel(done, a_.here());
    a_.load(RDI, -8); a_.loadMem(RSI, RDI, -8); a_.subImm(RDI, 16);
    callFixups_.push_back({a_.call(), -64});             // hfree(P-16, [P-8])
    a_.patchRel(notVal, a_.here()); a_.patchRel(counted, a_.here());
    a_.leave(); a_.ret();
    a_.patchRel(notObj, a_.here());
    a_.ret();
}

// Debug ARC tracer: write a raw 24-byte record [op(8)][ptr(8)][count(8)] to fd 2.
// It allocates nothing, so it cannot perturb the refcounts it observes. rdi=op,
// rsi=ptr, rdx=count; preserves every register.
void X64Gen::genTrace() {
    traceOff_ = a_.here();
    a_.push(RAX); a_.push(RCX); a_.push(RDX); a_.push(RSI); a_.push(RDI);
    a_.push(R8); a_.push(R9); a_.push(R10);
    a_.subImm(RSP, 24);
    a_.movRR(R9, RSP);                    // R9 = record base (RSP can't be a ModRM base w/o SIB)
    a_.storeMem(R9, 0, RDI); a_.storeMem(R9, 8, RSI); a_.storeMem(R9, 16, RDX);
    a_.movImm(RAX, 1); a_.movImm(RDI, 2); a_.movRR(RSI, R9); a_.movImm(RDX, 24);
    a_.syscall_();
    a_.addImm(RSP, 24);
    a_.pop(R10); a_.pop(R9); a_.pop(R8);
    a_.pop(RDI); a_.pop(RSI); a_.pop(RDX); a_.pop(RCX); a_.pop(RAX);
    a_.ret();
}

// Emit a trace call assuming RSI = object ptr and RAX = new refcount.
void X64Gen::emitTrace(int opChar) {
    if (!arcTrace_) return;
    a_.movImm(RDI, (uint64_t)opChar);
    a_.movRR(RDX, RAX);
    callFixups_.push_back({a_.call(), -68});
}

// The ARC gate emitted inline into retain/release: fall through to the refcount
// slot only for a refcounted heap object. rdi=tag, rsi=pay; on skip jumps SKIP.
// (value structs / dense records / arena-sentinel objects are not refcounted.)

// retain(rdi=tag, rsi=pay): a new reference -> ++refcount, for heap objects only.
void X64Gen::genRetain() {
    retainOff_ = a_.here();
    a_.cmpImm(RDI, 4); size_t s1 = a_.jl();               // tag < 4: scalars, never counted
    size_t notStr = a_.jne();                             // tag > 4: the object/array/map/closure chain
    // §15 strings tier: a tag-4 payload is counted only if it lives in the heap
    // mmap. Literals (data segment) and arena strings sit outside the range —
    // and reading [P-16] from a literal would read unrelated data, so the
    // range check must come first.
    addrImm(RAX, arcBase_ + 16 + kSizeClasses * 8); a_.loadInd(RAX, RAX);   // heap base
    a_.cmpRR(RSI, RAX); size_t s5 = a_.jl();
    a_.addImm(RAX, kHeapBytes); a_.cmpRR(RSI, RAX); size_t s6 = a_.jge();
    size_t strCounted = a_.jmp();                         // heap string -> refcount
    a_.patchRel(notStr, a_.here());
    a_.cmpImm(RDI, 8); size_t s2 = a_.je();               // none
    a_.cmpImm(RDI, 6); size_t notArr = a_.jne();          // array (boxed or dense — both carry the prefix)
    size_t boxedArr = a_.jmp();                           // array -> refcount
    a_.patchRel(notArr, a_.here());
    a_.cmpImm(RDI, 5); size_t heap = a_.jne();            // not object -> (closure tag 9 excluded above) skip
    a_.loadInd(RAX, RSI);                                 // classId
    a_.push(RDI); a_.push(RSI); a_.movRR(RDI, RAX);
    callFixups_.push_back({a_.call(), -61});              // isValueClass
    a_.pop(RSI); a_.pop(RDI);
    a_.testRR(RAX); size_t s3 = a_.jne();                 // value struct/record -> skip
    a_.patchRel(heap, a_.here()); a_.patchRel(boxedArr, a_.here());
    a_.patchRel(strCounted, a_.here());
    a_.loadMem(RAX, RSI, -16); a_.testRR(RAX); size_t s4 = a_.jcc(8);   // JS: arena sentinel (<0)
    a_.addImm(RAX, 1); a_.storeMem(RSI, -16, RAX);
    emitTrace('R');
    a_.patchRel(s1, a_.here()); a_.patchRel(s2, a_.here());
    a_.patchRel(s3, a_.here()); a_.patchRel(s4, a_.here());
    a_.patchRel(s5, a_.here()); a_.patchRel(s6, a_.here());
    a_.ret();
}

// release(rdi=tag, rsi=pay): drop a reference -> --refcount; at 0, recursively
// free. Framed so the tag/pay survive the isValueClass and recursiveFree calls.
void X64Gen::genRelease() {
    releaseOff_ = a_.here();
    a_.push(RBP); a_.movRR(RBP, RSP); a_.subImm(RSP, 16);
    a_.store(RDI, -8); a_.store(RSI, -16);
    a_.load(RDI, -8); a_.cmpImm(RDI, 4); size_t s1 = a_.jl();    // tag < 4: scalars, never counted
    size_t notStr = a_.jne();                                    // tag > 4: the existing chain
    // §15 strings tier: count only heap-mmap strings (range check BEFORE any
    // [P-16] read — literals have no prefix; arena strings are out of range).
    a_.load(RSI, -16);
    addrImm(RAX, arcBase_ + 16 + kSizeClasses * 8); a_.loadInd(RAX, RAX);   // heap base
    a_.cmpRR(RSI, RAX); size_t s5 = a_.jl();
    a_.addImm(RAX, kHeapBytes); a_.cmpRR(RSI, RAX); size_t s6 = a_.jge();
    size_t strCounted = a_.jmp();
    a_.patchRel(notStr, a_.here());
    a_.cmpImm(RDI, 8); size_t s2 = a_.je();
    a_.cmpImm(RDI, 6); size_t notArr = a_.jne();                 // array (boxed or dense — both prefixed)
    size_t boxedArr = a_.jmp();
    a_.patchRel(notArr, a_.here());
    a_.cmpImm(RDI, 5); size_t heap = a_.jne();
    a_.load(RSI, -16); a_.loadInd(RAX, RSI); a_.movRR(RDI, RAX);
    callFixups_.push_back({a_.call(), -61});
    a_.testRR(RAX); size_t s3 = a_.jne();
    a_.patchRel(heap, a_.here()); a_.patchRel(boxedArr, a_.here());
    a_.patchRel(strCounted, a_.here());
    a_.load(RSI, -16); a_.loadMem(RAX, RSI, -16); a_.testRR(RAX); size_t s4 = a_.jle();  // <=0 skip
    a_.subImm(RAX, 1); a_.storeMem(RSI, -16, RAX);
    emitTrace('r');
    a_.testRR(RAX); size_t alive = a_.jne();              // still referenced
    a_.load(RDI, -8); a_.load(RSI, -16);
    callFixups_.push_back({a_.call(), -67});              // recursiveFree
    a_.patchRel(s1, a_.here()); a_.patchRel(s2, a_.here());
    a_.patchRel(s3, a_.here()); a_.patchRel(s4, a_.here()); a_.patchRel(alive, a_.here());
    a_.patchRel(s5, a_.here()); a_.patchRel(s6, a_.here());
    a_.leave(); a_.ret();
}

// recursiveFree(rdi=tag, rsi=pay): release the object's referenced contents, then
// return the block to the allocator (hfree). Dispatch by kind.
void X64Gen::genRecursiveFree() {
    recursiveFreeOff_ = a_.here();
    a_.push(RBP); a_.movRR(RBP, RSP); a_.subImm(RSP, 48);
    a_.store(RDI, -8); a_.store(RSI, -16);
    a_.load(RAX, -8);
    a_.cmpImm(RAX, 5); size_t isObj = a_.je();
    a_.cmpImm(RAX, 6); size_t isArr = a_.je();
    a_.cmpImm(RAX, 9); size_t isClo = a_.je();
    a_.cmpImm(RAX, 7); size_t isMap = a_.je();
    // string / other: no references -> hfree(P-16, [P-8])
    a_.load(RDI, -16); a_.loadMem(RSI, RDI, -8); a_.subImm(RDI, 16);
    callFixups_.push_back({a_.call(), -64}); a_.leave(); a_.ret();
    // object: release each declared field slot, then hfree
    a_.patchRel(isObj, a_.here());
    a_.load(RSI, -16); a_.loadInd(RDI, RSI); callFixups_.push_back({a_.call(), -59});   // fieldcount
    a_.store(RAX, -24); a_.xorRR(RCX, RCX); a_.store(RCX, -32);
    size_t oloop = a_.here();
    a_.load(RCX, -32); a_.load(RAX, -24); a_.cmpRR(RCX, RAX); size_t odone = a_.jge();
    a_.load(RSI, -16); a_.load(RAX, -32); a_.shlImm(RAX, 4); a_.addRR(RAX, RSI); a_.addImm(RAX, 16);
    a_.loadMem(RDI, RAX, 0); a_.loadMem(RSI, RAX, 8);
    callFixups_.push_back({a_.call(), -66});                                            // release field
    // §15: a value-struct field is EXCLUSIVELY owned (always a standalone copy)
    // and gate-excluded from release — vfree reclaims it (no-op for other tags,
    // and for a ref object release just freed: its poisoned classId fails the
    // isValueClass check).
    a_.load(RSI, -16); a_.load(RAX, -32); a_.shlImm(RAX, 4); a_.addRR(RAX, RSI); a_.addImm(RAX, 16);
    a_.loadMem(RDI, RAX, 0); a_.loadMem(RSI, RAX, 8);
    callFixups_.push_back({a_.call(), -70});                                            // vfree struct field
    a_.load(RCX, -32); a_.addImm(RCX, 1); a_.store(RCX, -32);
    size_t ob = a_.jmp(); a_.patchRel(ob, oloop);
    a_.patchRel(odone, a_.here());
    a_.load(RDI, -16); a_.load(RSI, -16); a_.loadMem(RSI, RSI, -8); a_.subImm(RDI, 16);
    callFixups_.push_back({a_.call(), -64}); a_.leave(); a_.ret();
    // array: boxed -> release each element; dense records are value structs (skip)
    a_.patchRel(isArr, a_.here());
    a_.load(RSI, -16); a_.loadInd(RAX, RSI); a_.testRR(RAX); size_t denseArr = a_.jcc(8);  // JS: dense marker
    a_.store(RAX, -24); a_.xorRR(RCX, RCX); a_.store(RCX, -32);                          // len, i
    size_t aloop = a_.here();
    a_.load(RCX, -32); a_.load(RAX, -24); a_.cmpRR(RCX, RAX); size_t adone = a_.jge();
    a_.load(RSI, -16); a_.load(RAX, -32); a_.shlImm(RAX, 4); a_.addRR(RAX, RSI); a_.addImm(RAX, 8);
    a_.loadMem(RDI, RAX, 0); a_.loadMem(RSI, RAX, 8);
    callFixups_.push_back({a_.call(), -66});                                            // release element
    a_.load(RSI, -16); a_.load(RAX, -32); a_.shlImm(RAX, 4); a_.addRR(RAX, RSI); a_.addImm(RAX, 8);
    a_.loadMem(RDI, RAX, 0); a_.loadMem(RSI, RAX, 8);
    callFixups_.push_back({a_.call(), -70});             // §15: vfree struct element (boxed edge)
    a_.load(RCX, -32); a_.addImm(RCX, 1); a_.store(RCX, -32);
    size_t ab = a_.jmp(); a_.patchRel(ab, aloop);
    a_.patchRel(adone, a_.here());
    // boxed hfree: 24 + 16*capacity ([P-8] = capacity)
    a_.load(RDI, -16); a_.loadMem(RSI, RDI, -8); a_.shlImm(RSI, 4); a_.addImm(RSI, 24);
    a_.subImm(RDI, 16);
    callFixups_.push_back({a_.call(), -64}); a_.leave(); a_.ret();
    // dense hfree: [P-8] = totalSize directly (inline value-struct records, no refs)
    a_.patchRel(denseArr, a_.here());
    a_.load(RDI, -16); a_.loadMem(RSI, RDI, -8); a_.subImm(RDI, 16);
    callFixups_.push_back({a_.call(), -64}); a_.leave(); a_.ret();
    // closure: walk the fieldHead capture list, release each value AND free the
    // 32-byte node (raw heap block [next][keyptr][valtag][valpay], no prefix)
    a_.patchRel(isClo, a_.here());
    a_.load(RSI, -16); a_.loadMem(RAX, RSI, 8); a_.store(RAX, -24);   // node = fieldHead
    size_t cloop = a_.here();
    a_.load(RAX, -24); a_.testRR(RAX); size_t cdone = a_.je();
    a_.loadMem(RCX, RAX, 0); a_.store(RCX, -32);                    // next = node->next (survives calls)
    a_.loadMem(RDI, RAX, 16); a_.loadMem(RSI, RAX, 24);            // node value (tag,pay)
    callFixups_.push_back({a_.call(), -66});                       // release value
    a_.load(RDI, -24); a_.movImm(RSI, 32);
    callFixups_.push_back({a_.call(), -64});                       // hfree the node (raw base, no prefix)
    a_.load(RAX, -32); a_.store(RAX, -24);                         // node = next
    size_t cb = a_.jmp(); a_.patchRel(cb, cloop);
    a_.patchRel(cdone, a_.here());
    a_.load(RDI, -16); a_.loadMem(RSI, RDI, -8); a_.subImm(RDI, 16);
    callFixups_.push_back({a_.call(), -64}); a_.leave(); a_.ret();
    // map: release each entry's key + value ([P+8+i*32] = key(16) + value(16)), then hfree
    a_.patchRel(isMap, a_.here());
    a_.load(RSI, -16); a_.loadInd(RAX, RSI); a_.store(RAX, -24);   // len
    a_.xorRR(RCX, RCX); a_.store(RCX, -32);                         // i = 0
    size_t mloop = a_.here();
    a_.load(RCX, -32); a_.load(RAX, -24); a_.cmpRR(RCX, RAX); size_t mdone = a_.jge();
    a_.load(RSI, -16); a_.load(RAX, -32); a_.shlImm(RAX, 5); a_.addRR(RAX, RSI); a_.addImm(RAX, 8);
    a_.store(RAX, -40);                                             // &entry[i]
    a_.loadMem(RDI, RAX, 0); a_.loadMem(RSI, RAX, 8);              // key (tag,pay)
    callFixups_.push_back({a_.call(), -66});                       // release key
    a_.load(RAX, -40); a_.loadMem(RDI, RAX, 16); a_.loadMem(RSI, RAX, 24);   // value
    callFixups_.push_back({a_.call(), -66});                       // release value
    a_.load(RAX, -40); a_.loadMem(RDI, RAX, 16); a_.loadMem(RSI, RAX, 24);
    callFixups_.push_back({a_.call(), -70});                       // §15: vfree struct value
    a_.load(RCX, -32); a_.addImm(RCX, 1); a_.store(RCX, -32);
    size_t mb = a_.jmp(); a_.patchRel(mb, mloop);
    a_.patchRel(mdone, a_.here());
    a_.load(RDI, -16); a_.load(RSI, -16); a_.loadMem(RSI, RSI, -8); a_.subImm(RDI, 16);
    callFixups_.push_back({a_.call(), -64}); a_.leave(); a_.ret();
}

void X64Gen::genCopyVal() {
    copyValOff_ = a_.here();
    a_.cmpImm(RDI, 5); size_t notObj = a_.jne();     // tag != object -> as-is
    a_.loadInd(RAX, RSI);                            // classId = [obj]
    a_.push(RSI);                                    // save obj across the call
    a_.movRR(RDI, RAX);
    callFixups_.push_back({a_.call(), -61});         // isValueClass(classId) -> rax
    a_.pop(RSI);
    a_.testRR(RAX); size_t notVal = a_.je();         // object but not a value struct

    // deep copy — RSI = src. Use an rbp frame for the loop locals.
    a_.push(RBP); a_.movRR(RBP, RSP); a_.subImm(RSP, 48);
    a_.store(RSI, -8);                               // [rbp-8]  = src
    a_.loadInd(RDI, RSI);                            // classId
    callFixups_.push_back({a_.call(), -10});         // mkobj(classId) -> rax = dst
    a_.store(RAX, -16);                              // [rbp-16] = dst
    a_.load(RDI, -8); a_.loadInd(RDI, RDI);          // classId (from src)
    callFixups_.push_back({a_.call(), -59});         // fieldcount -> rax = N
    a_.store(RAX, -24);                              // [rbp-24] = N
    a_.xorRR(RAX, RAX); a_.store(RAX, -32);          // [rbp-32] = i = 0

    size_t loop = a_.here();
    a_.load(RAX, -32); a_.load(RCX, -24);            // i, N
    a_.cmpRR(RAX, RCX); size_t done = a_.jge();      // i >= N -> done
    a_.load(RDX, -8);                                // src
    a_.movRR(RCX, RAX); a_.shlImm(RCX, 4); a_.addImm(RDX, 16); a_.addRR(RDX, RCX);
    a_.loadMem(RDI, RDX, 0);                         // slot tag
    a_.loadMem(RSI, RDX, 8);                         // slot pay
    callFixups_.push_back({a_.call(), -62});         // copyval (recursive) -> rax,rdx
    a_.load(RCX, -16);                               // dst
    a_.load(R8, -32); a_.shlImm(R8, 4); a_.addImm(RCX, 16); a_.addRR(RCX, R8);
    a_.storeMem(RCX, 0, RAX); a_.storeMem(RCX, 8, RDX);
    a_.load(RAX, -32); a_.addImm(RAX, 1); a_.store(RAX, -32);   // i++
    size_t back = a_.jmp(); a_.patchRel(back, loop);
    a_.patchRel(done, a_.here());
    a_.movImm(RAX, 5); a_.load(RDX, -16);            // return (object, dst)
    a_.leave(); a_.ret();

    a_.patchRel(notVal, a_.here());                  // object, not a value struct
    a_.movImm(RAX, 5); a_.movRR(RDX, RSI); a_.ret();
    a_.patchRel(notObj, a_.here());                  // non-object
    a_.movRR(RAX, RDI); a_.movRR(RDX, RSI); a_.ret();
}

// getm(rdi = objptr, rsi = nmptr(stripped), rdx = fullkeyptr) -> rax=tag, rdx=pay.
// Accessor-aware: per instantiated class, a getter whose name matches nm is
// called; otherwise falls back to a raw field lookup with the full key.
void X64Gen::genGetm(const std::vector<Symbol*>& classes) {
    getmOff_ = a_.here();
    a_.loadInd(RAX, RDI);                  // classId = [obj]
    std::vector<size_t> toFallback;
    for (Symbol* cls : classes) {
        std::vector<const Stmt*> mem; collectMembers(cls, mem);
        std::vector<std::pair<std::string, const Stmt*>> getters;
        for (const Stmt* m : mem)
            if (m->isGet && !m->selector.symbolic && std::string(m->name) != "[]" &&
                mod_.byDecl.count(m))
                getters.push_back({std::string(m->name), m});
        if (getters.empty()) continue;
        a_.cmpImm(RAX, clsId(cls));
        size_t jNext = a_.jne();
        for (auto& [name, m] : getters) {
            addrImm(R9, internString(name));
            a_.cmpRR(RSI, R9);
            size_t jSkip = a_.jne();
            a_.push(RDI);                  // getter(obj): push obj value (tag 5, objptr)
            a_.movImm(RAX, 5); a_.push(RAX);
            callFixups_.push_back({a_.call(), mod_.byDecl.at(m)});
            a_.addImm(RSP, 16);
            a_.ret();
            a_.patchRel(jSkip, a_.here());
        }
        toFallback.push_back(a_.jmp());    // no getter matched -> raw fallback
        a_.patchRel(jNext, a_.here());
    }
    size_t fb = a_.here();
    for (size_t j : toFallback) a_.patchRel(j, fb);
    a_.movRR(RSI, RDX);                    // key = full key
    callFixups_.push_back({a_.call(), -11});  // getfield
    a_.ret();
}

// setm(rdi=objptr, rsi=nmptr, rdx=fullkeyptr, rcx=valtag, r8=valpay).
void X64Gen::genSetm(const std::vector<Symbol*>& classes) {
    setmOff_ = a_.here();
    a_.loadInd(RAX, RDI);                  // classId
    std::vector<size_t> toFallback;
    for (Symbol* cls : classes) {
        std::vector<const Stmt*> mem; collectMembers(cls, mem);
        std::vector<std::pair<std::string, const Stmt*>> setters;
        for (const Stmt* m : mem)
            if (m->isSet && !m->selector.symbolic && std::string(m->name) != "[]" &&
                mod_.byDecl.count(m))
                setters.push_back({std::string(m->name), m});
        if (setters.empty()) continue;
        a_.cmpImm(RAX, clsId(cls));
        size_t jNext = a_.jne();
        for (auto& [name, m] : setters) {
            addrImm(R9, internString(name));
            a_.cmpRR(RSI, R9);
            size_t jSkip = a_.jne();
            // setter(obj, val): push val (arg1) then obj (arg0), payload then tag.
            a_.push(R8); a_.push(RCX);         // arg1 = val (pay, tag)
            a_.push(RDI);                      // arg0 = obj payload
            a_.movImm(RAX, 5); a_.push(RAX);   // arg0 tag
            callFixups_.push_back({a_.call(), mod_.byDecl.at(m)});
            a_.addImm(RSP, 32);
            a_.ret();
            a_.patchRel(jSkip, a_.here());
        }
        toFallback.push_back(a_.jmp());
        a_.patchRel(jNext, a_.here());
    }
    size_t fb = a_.here();
    for (size_t j : toFallback) a_.patchRel(j, fb);
    a_.movRR(RSI, RDX);                    // key = full key
    a_.movRR(RDX, RCX);                    // valtag
    a_.movRR(RCX, R8);                     // valpay
    callFixups_.push_back({a_.call(), -12});  // setfield
    a_.ret();
}

// opm(rdi=opcode, rsi=l_tag, rdx=l_pay, rcx=r_tag, r8=r_pay) -> rax=tag, rdx=pay.
// Operator-method dispatch on the left operand's class; derives != from ==.
void X64Gen::genOpm(const std::vector<Symbol*>& classes) {
    opmOff_ = a_.here();
    a_.loadInd(RAX, RDX);                  // classId = [l_pay]
    for (Symbol* cls : classes) {
        std::vector<const Stmt*> mem; collectMembers(cls, mem);
        std::vector<std::pair<int, const Stmt*>> ops;   // (opcode, fn)
        const Stmt* eq = nullptr;
        for (const Stmt* m : mem) {
            if (!m->selector.symbolic || !mod_.byDecl.count(m)) continue;
            int oc = symbolOp(std::string(m->selector.text));
            if (oc < 0) continue;
            ops.push_back({oc, m});
            if (oc == 1) eq = m;           // ==
        }
        if (ops.empty()) continue;
        a_.cmpImm(RAX, clsId(cls));
        size_t jNext = a_.jne();
        auto callOp = [&](const Stmt* m) {
            // op(l, r): push r (arg1) then l (arg0), payload then tag.
            a_.push(R8); a_.push(RCX);     // r (pay, tag)
            a_.push(RDX); a_.push(RSI);    // l (pay, tag)
            callFixups_.push_back({a_.call(), mod_.byDecl.at(m)});
            a_.addImm(RSP, 32);
        };
        bool hasNeq = false;
        for (auto& [oc, m] : ops) if (oc == 2) hasNeq = true;
        for (auto& [oc, m] : ops) {
            a_.cmpImm(RDI, oc);
            size_t jSkip = a_.jne();
            callOp(m);
            a_.ret();
            a_.patchRel(jSkip, a_.here());
        }
        if (eq && !hasNeq) {                // derived != : negate ==
            a_.cmpImm(RDI, 2);
            size_t jSkip = a_.jne();
            callOp(eq);
            a_.movRR(RCX, RDX);            // eq payload
            a_.testRR(RCX); a_.setccAx(0x94);  // rax = (eq==0) -> not equal
            a_.movRR(RDX, RAX);
            a_.movImm(RAX, 3);            // bool
            a_.ret();
            a_.patchRel(jSkip, a_.here());
        }
        a_.patchRel(jNext, a_.here());
    }
    a_.xorRR(RAX, RAX); a_.xorRR(RDX, RDX); a_.ret();   // no operator -> void
}

int X64Gen::lookupClsId(const char* name) {
    Symbol* s = mod_.sema->global->lookup(name);
    return s ? clsId(s) : 0;
}

// mkarr(rdi = len) -> rax: array = [len(8)][value(16) * len] (mmap heap is
// zero-filled, so fresh elements read as void until written).
// mkarr(rdi = len) -> rax = P (a boxed array). §15 COW: a 16-byte prefix sits
// BEFORE P — [P-16] = refcount (1), [P-8] = capacity (value slots) — so the array
// body [len][value*] at P is unchanged and every existing offset still works.
void X64Gen::genMkArr() {
    mkArrOff_ = a_.here();
    a_.push(RDI);                                          // len
    a_.movRR(RAX, RDI); a_.shlImm(RAX, 4); a_.addImm(RAX, 24); a_.movRR(RDI, RAX);   // 24 + 16*len
    callFixups_.push_back({a_.call(), -4});                // alloc -> rax = raw
    a_.pop(RCX);                                           // len
    // §15: boxed arrays join the ARC — refcount 0 (owned on first store), or the
    // -1 arena sentinel when scope-owned. (COW's ==1 uniqueness test still holds:
    // an array live in exactly one slot has been retained once.)
    addrImm(RDX, 56); a_.loadInd(RDX, RDX); a_.testRR(RDX); size_t heapArr = a_.je();
    a_.movImm(RDX, (uint64_t)-1); size_t setArr = a_.jmp();
    a_.patchRel(heapArr, a_.here()); a_.xorRR(RDX, RDX);
    a_.patchRel(setArr, a_.here()); a_.storeMem(RAX, 0, RDX);   // [raw] = refcount
    a_.storeMem(RAX, 8, RCX);                              // [raw+8] = capacity = len
    a_.addImm(RAX, 16);                                    // P = raw + 16 (past the prefix)
    a_.storeInd(RAX, RCX);                                 // [P] = len
    a_.ret();
}

// mkmap(rdi = len) -> rax: map = [len(8)][entry(32) * len].
void X64Gen::genMkMap() {
    mkMapOff_ = a_.here();
    a_.push(RDI);                                          // count
    // §15: 16-byte ARC prefix ([P-16] refcount, [P-8] totalSize) before P; the
    // body [len][entry(32)*len] at P is unchanged (map ops use P-relative addrs).
    a_.movRR(RAX, RDI); a_.shlImm(RAX, 5); a_.addImm(RAX, 24); a_.movRR(RDI, RAX);   // 16 + 8 + 32*count
    a_.push(RDI);                                          // totalSize
    callFixups_.push_back({a_.call(), -4});                // alloc -> raw
    a_.pop(RDX); a_.storeMem(RAX, 8, RDX);                // [raw+8] = totalSize
    addrImm(RCX, 56); a_.loadInd(RCX, RCX); a_.testRR(RCX); size_t heap = a_.je();
    a_.movImm(RDX, (uint64_t)-1); size_t setrc = a_.jmp();
    a_.patchRel(heap, a_.here()); a_.xorRR(RDX, RDX);
    a_.patchRel(setrc, a_.here()); a_.storeMem(RAX, 0, RDX);   // [raw] = refcount
    a_.addImm(RAX, 16);                                    // P = raw + 16
    a_.pop(RCX); a_.storeInd(RAX, RCX);                   // [P] = count (len)
    a_.ret();
}

// arr_append(rdi=arrptr, rsi=valtag, rdx=valpay) -> rax: pure append (new array).
// §9 B1: a boxed array is [len][value(16)*len]; when the first value-struct
// element is appended to an empty array it becomes DENSE — [len|1<<63][recBytes]
// [inline-record*len], records being inline objects. Boxed arrays are unchanged
// (the high bit is never set), so existing programs take the same path as before.
void X64Gen::genArrAppend() {
    arrAppendOff_ = a_.here();
    a_.push(RBP); a_.movRR(RBP, RSP); a_.subImm(RSP, 64);
    a_.store(RDI, -8); a_.store(RSI, -16); a_.store(RDX, -24);   // arrptr, valtag, valpay
    a_.loadInd(RAX, RDI); a_.store(RAX, -32);                    // rawlen
    a_.testRR(RAX); size_t denseAppend = a_.jcc(8);             // js -> already dense

    // boxed & non-empty, or non-struct element -> the plain boxed append below
    a_.testRR(RAX); size_t boxed1 = a_.jne();                   // oldlen != 0
    a_.load(RAX, -16); a_.cmpImm(RAX, 5); size_t boxed2 = a_.jne();   // tag != object
    a_.load(RDI, -24); a_.loadInd(RDI, RDI);                    // classId = [valpay]
    callFixups_.push_back({a_.call(), -61});                    // isValueClass
    a_.testRR(RAX); size_t boxed3 = a_.je();

    // GO DENSE: [1|1<<63][recBytes][record0], record0 = the element object inline
    a_.load(RDI, -24); a_.loadInd(RDI, RDI);
    callFixups_.push_back({a_.call(), -59});                    // fieldcount -> F
    a_.shlImm(RAX, 4); a_.addImm(RAX, 16); a_.store(RAX, -40);  // recBytes = 16 + 16F
    a_.addImm(RAX, 16); a_.movRR(RDI, RAX);
    callFixups_.push_back({a_.call(), -69});                    // halloc(body) -> P (prefixed)
    a_.store(RAX, -48);
    a_.movImm(RCX, 1); a_.storeMem(RAX, -16, RCX);             // §15: return owned (+1) — dk=2 transfer
    a_.movImm(RCX, 0x8000000000000001ULL); a_.storeInd(RAX, RCX);      // [new] = 1 | dense
    a_.load(RDX, -40); a_.storeMem(RAX, 8, RDX);               // [new+8] = recBytes
    a_.load(RSI, -24); a_.load(RDI, -48); a_.addImm(RDI, 16); a_.load(RCX, -40); a_.repMovsb();
    a_.load(RAX, -48); a_.leave(); a_.ret();

    // DENSE APPEND: copy the record buffer, inline the new element after it
    a_.patchRel(denseAppend, a_.here());
    a_.load(RAX, -32); a_.shlImm(RAX, 1); a_.shrImm(RAX, 1); a_.store(RAX, -56);   // cleanOldLen
    a_.load(RDI, -8); a_.loadMem(RCX, RDI, 8); a_.store(RCX, -40);                 // recBytes
    a_.load(RAX, -56); a_.addImm(RAX, 1); a_.imulRR(RAX, RCX); a_.addImm(RAX, 16); a_.movRR(RDI, RAX);
    callFixups_.push_back({a_.call(), -69}); a_.store(RAX, -48);                   // halloc -> newptr (prefixed)
    a_.movImm(RCX, 1); a_.storeMem(RAX, -16, RCX);                                 // §15: return owned (+1) — dk=2 transfer
    a_.load(RCX, -56); a_.addImm(RCX, 1); a_.movImm(RDX, 0x8000000000000000ULL); a_.orRR(RCX, RDX);
    a_.storeInd(RAX, RCX);                                                          // [new] = (len+1)|dense
    a_.load(RDX, -40); a_.storeMem(RAX, 8, RDX);
    a_.load(RCX, -56); a_.load(RAX, -40); a_.imulRR(RCX, RAX);                     // old records bytes
    a_.load(RSI, -8); a_.addImm(RSI, 16); a_.load(RDI, -48); a_.addImm(RDI, 16); a_.repMovsb();
    a_.load(RSI, -24); a_.load(RDI, -48); a_.addImm(RDI, 16);
    a_.load(RAX, -56); a_.load(RCX, -40); a_.imulRR(RAX, RCX); a_.addRR(RDI, RAX); // &record[oldLen]
    a_.load(RCX, -40); a_.repMovsb();                                              // inline new element
    a_.load(RAX, -48); a_.leave(); a_.ret();

    // BOXED APPEND — §15 COW: append in place when uniquely owned (refcount 1),
    // growing the buffer geometrically; otherwise copy (the pure-value fallback).
    a_.patchRel(boxed1, a_.here()); a_.patchRel(boxed2, a_.here()); a_.patchRel(boxed3, a_.here());
    a_.load(RSI, -8); a_.loadMem(RAX, RSI, -16);       // refcount
    a_.cmpImm(RAX, 1); size_t copy = a_.jne();         // shared -> copy
    a_.loadInd(RAX, RSI); a_.loadMem(RCX, RSI, -8);    // len, capacity
    a_.cmpRR(RAX, RCX); size_t realloc = a_.jge();     // len >= cap -> grow
    // spare capacity: write [arrptr + 8 + len*16] = value, len++
    a_.movRR(RDX, RAX); a_.shlImm(RDX, 4); a_.addImm(RDX, 8); a_.addRR(RDX, RSI);
    a_.load(RCX, -16); a_.storeMem(RDX, 0, RCX); a_.load(RCX, -24); a_.storeMem(RDX, 8, RCX);
    a_.addImm(RAX, 1); a_.storeInd(RSI, RAX);
    // §15: the buffer owns the stored element — retain it (recursiveFree releases it)
    a_.load(RDI, -16); a_.load(RSI, -24);
    callFixups_.push_back({a_.call(), -65});
    a_.load(RAX, -8); a_.leave(); a_.ret();
    // grow: newcap = max(4, 2*cap); fresh prefixed buffer; copy; append
    a_.patchRel(realloc, a_.here());
    a_.load(RSI, -8); a_.loadMem(RCX, RSI, -8); a_.shlImm(RCX, 1);       // 2*cap
    a_.cmpImm(RCX, 4); size_t big = a_.jge(); a_.movImm(RCX, 4); a_.patchRel(big, a_.here());
    a_.store(RCX, -56);                                                  // newcap
    a_.movRR(RDX, RCX); a_.shlImm(RDX, 4); a_.addImm(RDX, 24); a_.movRR(RDI, RDX);
    callFixups_.push_back({a_.call(), -4});                              // alloc(24 + 16*newcap)
    a_.movImm(RDX, 1); a_.storeMem(RAX, 0, RDX);                         // refcount 1
    a_.load(RCX, -56); a_.storeMem(RAX, 8, RCX);                         // capacity
    a_.addImm(RAX, 16); a_.store(RAX, -48);                              // P
    a_.load(RCX, -32); a_.storeInd(RAX, RCX);                            // [P] = oldlen
    a_.load(RCX, -32); a_.shlImm(RCX, 4);                                // oldlen*16
    a_.load(RSI, -8); a_.addImm(RSI, 8); a_.load(RDI, -48); a_.addImm(RDI, 8); a_.repMovsb();
    a_.load(RSI, -48); a_.load(RAX, -32);                               // P, oldlen
    a_.movRR(RDX, RAX); a_.shlImm(RDX, 4); a_.addImm(RDX, 8); a_.addRR(RDX, RSI);
    a_.load(RCX, -16); a_.storeMem(RDX, 0, RCX); a_.load(RCX, -24); a_.storeMem(RDX, 8, RCX);
    a_.load(RAX, -32); a_.addImm(RAX, 1); a_.storeInd(RSI, RAX);        // [P] = oldlen+1
    // §15: the old elements' refs TRANSFER to the new buffer (the old one dies
    // below without releasing them); only the appended element is a new ref.
    a_.load(RDI, -16); a_.load(RSI, -24);
    callFixups_.push_back({a_.call(), -65});                            // retain new element
    // §15: arr_append OWNS the receiver buffer's fate (the caller clears the
    // consumed receiver slot without releasing). The old buffer is unique and
    // fully copied -> dead; reclaim it here. New buffer returned owned (+1).
    a_.load(RDI, -8); a_.loadMem(RSI, RDI, -8); a_.shlImm(RSI, 4); a_.addImm(RSI, 24);
    a_.subImm(RDI, 16);
    callFixups_.push_back({a_.call(), -64});                            // hfree(old-16, 24+16*oldcap)
    a_.load(RAX, -48); a_.leave(); a_.ret();
    // COPY (shared): pure-value append via mkarr, returned at +1 (transfer)
    a_.patchRel(copy, a_.here());
    a_.load(RAX, -32); a_.movRR(RDI, RAX); a_.addImm(RDI, 1);
    callFixups_.push_back({a_.call(), -18});           // mkarr(oldlen+1) -> P at refcount 0
    a_.store(RAX, -48);
    a_.movImm(RCX, 1); a_.storeMem(RAX, -16, RCX);     // §15: return owned (+1)
    a_.load(RCX, -32); a_.shlImm(RCX, 4);
    a_.load(RSI, -8);  a_.addImm(RSI, 8);
    a_.load(RDI, -48); a_.addImm(RDI, 8);
    a_.repMovsb();
    a_.load(RAX, -48); a_.load(RCX, -32); a_.shlImm(RCX, 4); a_.addRR(RAX, RCX); a_.addImm(RAX, 8);
    a_.load(RDX, -16); a_.storeMem(RAX, 0, RDX);
    a_.load(RDX, -24); a_.storeMem(RAX, 8, RDX);
    // §15: the new buffer owns EVERY element — retain each (the copied refs and
    // the appended one). The old buffer stays live with its own counted refs;
    // when it dies its recursiveFree balances them. (Mirror of the map upsert.)
    a_.load(RSI, -48); a_.loadInd(RCX, RSI); a_.store(RCX, -56);   // newlen
    a_.xorRR(RCX, RCX); a_.store(RCX, -64);                         // i = 0
    size_t crl = a_.here();
    a_.load(RCX, -64); a_.load(RAX, -56); a_.cmpRR(RCX, RAX); size_t crdone = a_.jge();
    a_.load(RSI, -48); a_.load(RAX, -64); a_.shlImm(RAX, 4); a_.addRR(RAX, RSI); a_.addImm(RAX, 8);
    a_.loadMem(RDI, RAX, 0); a_.loadMem(RSI, RAX, 8);
    callFixups_.push_back({a_.call(), -65});                       // retain element
    a_.load(RCX, -64); a_.addImm(RCX, 1); a_.store(RCX, -64);
    size_t crb = a_.jmp(); a_.patchRel(crb, crl);
    a_.patchRel(crdone, a_.here());
    a_.load(RAX, -48);
    a_.leave(); a_.ret();
}

// arr_spread(rdi=arrptr, rsi=elem_tag, rdx=elem_pay) -> rax: append the element,
// or spread its values if it is a Range object (array-literal range elements).
void X64Gen::genArrSpread() {
    arrSpreadOff_ = a_.here();
    a_.push(RBP); a_.movRR(RBP, RSP); a_.subImm(RSP, 48);
    a_.store(RDI, -8); a_.store(RSI, -16); a_.store(RDX, -24);
    a_.cmpImm(RSI, 5); size_t plainA = a_.jne();
    a_.movRR(RDI, RDX); addrImm(RSI, internString("start"));
    callFixups_.push_back({a_.call(), -11});           // getfield start
    a_.testRR(RAX); size_t plainB = a_.je();
    a_.store(RDX, -32);                                 // curr = start
    a_.load(RDI, -24); addrImm(RSI, internString("end"));
    callFixups_.push_back({a_.call(), -11});
    a_.store(RDX, -40);                                 // end
    size_t rl = a_.here();
    a_.load(RAX, -32); a_.load(RCX, -40); a_.cmpRR(RAX, RCX); size_t rlend = a_.jg();
    a_.load(RDI, -8); a_.movImm(RSI, 1); a_.load(RDX, -32);
    callFixups_.push_back({a_.call(), -25}); a_.store(RAX, -8);   // arr = append(arr, int curr)
    a_.load(RAX, -32); a_.addImm(RAX, 1); a_.store(RAX, -32);
    size_t rlb = a_.jmp(); a_.patchRel(rlb, rl);
    a_.patchRel(rlend, a_.here());
    a_.load(RAX, -8); a_.leave(); a_.ret();
    a_.patchRel(plainA, a_.here()); a_.patchRel(plainB, a_.here());
    a_.load(RDI, -8); a_.load(RSI, -16); a_.load(RDX, -24);
    callFixups_.push_back({a_.call(), -25});           // arr_append
    a_.leave(); a_.ret();
}

// arr_fill(rdi=n, rsi=fillTag, rdx=fillPay) -> rax: an array of n copies of the
// fill value — dense inline records if it is a value struct, else boxed values.
void X64Gen::genArrFill() {
    arrFillOff_ = a_.here();
    a_.push(RBP); a_.movRR(RBP, RSP); a_.subImm(RSP, 64);
    a_.store(RDI, -8); a_.store(RSI, -16); a_.store(RDX, -24);   // n, fillTag, fillPay
    a_.load(RAX, -16); a_.cmpImm(RAX, 5); size_t boxed1 = a_.jne();
    a_.load(RDI, -24); a_.loadInd(RDI, RDI);
    callFixups_.push_back({a_.call(), -61}); a_.testRR(RAX); size_t boxed2 = a_.je();   // isValueClass

    // DENSE: alloc 16 + n*recBytes, memcpy the fill into each inline record
    a_.load(RDI, -24); a_.loadInd(RDI, RDI);
    callFixups_.push_back({a_.call(), -59});                     // fieldcount -> F
    a_.shlImm(RAX, 4); a_.addImm(RAX, 16); a_.store(RAX, -32);   // recBytes
    a_.load(RCX, -8); a_.imulRR(RCX, RAX); a_.addImm(RCX, 16); a_.movRR(RDI, RCX);
    callFixups_.push_back({a_.call(), -69}); a_.store(RAX, -40);  // halloc -> arr (prefixed)
    a_.load(RCX, -8); a_.movImm(RDX, 0x8000000000000000ULL); a_.orRR(RCX, RDX); a_.storeInd(RAX, RCX);
    a_.load(RDX, -32); a_.storeMem(RAX, 8, RDX);
    a_.xorRR(RCX, RCX); a_.store(RCX, -48);                      // i
    size_t fl = a_.here();
    a_.load(RCX, -48); a_.load(RAX, -8); a_.cmpRR(RCX, RAX); size_t fe = a_.jge();
    a_.load(RSI, -24); a_.load(RDI, -40); a_.addImm(RDI, 16);
    a_.load(RAX, -48); a_.load(RDX, -32); a_.imulRR(RAX, RDX); a_.addRR(RDI, RAX);
    a_.load(RCX, -32); a_.repMovsb();
    a_.load(RCX, -48); a_.addImm(RCX, 1); a_.store(RCX, -48);
    size_t fb = a_.jmp(); a_.patchRel(fb, fl);
    a_.patchRel(fe, a_.here());
    a_.load(RAX, -40); a_.leave(); a_.ret();

    // BOXED: mkarr(n) then store the fill value into each 16-byte slot
    a_.patchRel(boxed1, a_.here()); a_.patchRel(boxed2, a_.here());
    a_.load(RDI, -8); callFixups_.push_back({a_.call(), -18}); a_.store(RAX, -40);   // mkarr(n)
    a_.xorRR(RCX, RCX); a_.store(RCX, -48);
    size_t bl = a_.here();
    a_.load(RCX, -48); a_.load(RAX, -8); a_.cmpRR(RCX, RAX); size_t be = a_.jge();
    a_.load(RAX, -48); a_.shlImm(RAX, 4); a_.load(RDX, -40); a_.addRR(RAX, RDX); a_.addImm(RAX, 8);
    a_.load(RDX, -16); a_.storeMem(RAX, 0, RDX); a_.load(RDX, -24); a_.storeMem(RAX, 8, RDX);
    // §15: the buffer owns each stored copy of the fill value — retain it
    a_.load(RDI, -16); a_.load(RSI, -24);
    callFixups_.push_back({a_.call(), -65});
    a_.load(RCX, -48); a_.addImm(RCX, 1); a_.store(RCX, -48);
    size_t bb = a_.jmp(); a_.patchRel(bb, bl);
    a_.patchRel(be, a_.here());
    a_.load(RAX, -40); a_.leave(); a_.ret();
}

// ts_build(rdi=tag, rsi=payload) -> rax = string descriptor. The machine
// analogue of valueToString: recursive over arrays/maps; used by print and by
// the `+` string-concat path so both match the interpreters exactly.
void X64Gen::genTsBuild() {
    tsBuildOff_ = a_.here();
    a_.push(RBP); a_.movRR(RBP, RSP); a_.subImm(RSP, 64);
    // int
    a_.cmpImm(RDI, 1); size_t n1 = a_.jne();
    a_.movRR(RDI, RSI); callFixups_.push_back({a_.call(), -5}); a_.leave(); a_.ret();
    a_.patchRel(n1, a_.here());
    // bool
    a_.cmpImm(RDI, 3); size_t n3 = a_.jne();
    a_.testRR(RSI); size_t jf = a_.je();
    addrImm(RAX, internString("true")); a_.leave(); a_.ret();
    a_.patchRel(jf, a_.here());
    addrImm(RAX, internString("false")); a_.leave(); a_.ret();
    a_.patchRel(n3, a_.here());
    // string: self
    a_.cmpImm(RDI, 4); size_t n4 = a_.jne();
    a_.movRR(RAX, RSI); a_.leave(); a_.ret();
    a_.patchRel(n4, a_.here());
    // none
    a_.cmpImm(RDI, 8); size_t n8 = a_.jne();
    addrImm(RAX, internString("None")); a_.leave(); a_.ret();
    a_.patchRel(n8, a_.here());
    // object: Range ("start..end") else "<object>"
    a_.cmpImm(RDI, 5); size_t n5 = a_.jne();
    a_.store(RSI, -8);                                  // objptr
    a_.movRR(RDI, RSI); addrImm(RSI, internString("start"));
    callFixups_.push_back({a_.call(), -11});           // getfield start
    a_.testRR(RAX); size_t noStart = a_.je();
    a_.movRR(RDI, RDX); callFixups_.push_back({a_.call(), -5}); a_.store(RAX, -16);   // start str
    a_.load(RDI, -8); addrImm(RSI, internString("end"));
    callFixups_.push_back({a_.call(), -11});
    a_.movRR(RDI, RDX); callFixups_.push_back({a_.call(), -5}); a_.store(RAX, -24);   // end str
    a_.load(RDI, -16); addrImm(RSI, internString(".."));
    callFixups_.push_back({a_.call(), -6}); a_.store(RAX, -16);                       // start ".."
    a_.load(RDI, -16); a_.load(RSI, -24);
    callFixups_.push_back({a_.call(), -6}); a_.leave(); a_.ret();                     // + end
    a_.patchRel(noStart, a_.here());
    addrImm(RAX, internString("<object>")); a_.leave(); a_.ret();
    a_.patchRel(n5, a_.here());
    // array: "[" elems ", " "]"  — boxed values or dense inline records
    a_.cmpImm(RDI, 6); size_t n6 = a_.jne();
    a_.store(RSI, -8);                                  // arrptr
    addrImm(RAX, internString("[")); a_.store(RAX, -16);   // acc
    a_.xorRR(RAX, RAX); a_.store(RAX, -24);            // i
    a_.loadInd(RAX, RSI); a_.testRR(RAX); size_t tdense = a_.jcc(8);
    a_.store(RAX, -32);                                              // boxed: len, rec=16, off=8, dense=0
    a_.movImm(RCX, 16); a_.store(RCX, -40); a_.movImm(RCX, 8); a_.store(RCX, -48);
    a_.xorRR(RCX, RCX); a_.store(RCX, -56);
    size_t tld = a_.jmp();
    a_.patchRel(tdense, a_.here());
    a_.shlImm(RAX, 1); a_.shrImm(RAX, 1); a_.store(RAX, -32);        // dense: masked len
    a_.load(RSI, -8); a_.loadMem(RCX, RSI, 8); a_.store(RCX, -40);   // recBytes
    a_.movImm(RCX, 16); a_.store(RCX, -48); a_.movImm(RCX, 1); a_.store(RCX, -56);
    a_.patchRel(tld, a_.here());
    size_t aloop = a_.here();
    a_.load(RAX, -24); a_.load(RCX, -32); a_.cmpRR(RAX, RCX); size_t aend = a_.jge();
    a_.load(RAX, -24); a_.testRR(RAX); size_t anc = a_.je();
    a_.load(RDI, -16); addrImm(RSI, internString(", ")); callFixups_.push_back({a_.call(), -6}); a_.store(RAX, -16);
    a_.patchRel(anc, a_.here());
    a_.load(RCX, -8); a_.load(RAX, -24); a_.load(RDX, -40); a_.imulRR(RAX, RDX); a_.addRR(RAX, RCX);
    a_.load(RDX, -48); a_.addRR(RAX, RDX);              // &record[i]
    a_.load(RCX, -56); a_.testRR(RCX); size_t telemBoxed = a_.je();
    a_.movRR(RSI, RAX); a_.movImm(RDI, 5);              // dense: element is (object, &record)
    size_t telemDone = a_.jmp();
    a_.patchRel(telemBoxed, a_.here());
    a_.loadMem(RDI, RAX, 0); a_.loadMem(RSI, RAX, 8);   // boxed: (tag, pay)
    a_.patchRel(telemDone, a_.here());
    callFixups_.push_back({a_.call(), -17});           // ts_build(elem)
    a_.load(RDI, -16); a_.movRR(RSI, RAX); callFixups_.push_back({a_.call(), -6}); a_.store(RAX, -16);
    a_.load(RAX, -24); a_.addImm(RAX, 1); a_.store(RAX, -24);
    size_t ab = a_.jmp(); a_.patchRel(ab, aloop);
    a_.patchRel(aend, a_.here());
    a_.load(RDI, -16); addrImm(RSI, internString("]")); callFixups_.push_back({a_.call(), -6});
    a_.leave(); a_.ret();
    a_.patchRel(n6, a_.here());
    // map: "{" (k ": " v) ", " "}"
    a_.cmpImm(RDI, 7); size_t n7 = a_.jne();
    a_.store(RSI, -8);
    addrImm(RAX, internString("{")); a_.store(RAX, -16);
    a_.xorRR(RAX, RAX); a_.store(RAX, -24);
    a_.loadInd(RAX, RSI); a_.store(RAX, -32);
    size_t mloop = a_.here();
    a_.load(RAX, -24); a_.load(RCX, -32); a_.cmpRR(RAX, RCX); size_t mend = a_.jge();
    a_.load(RAX, -24); a_.testRR(RAX); size_t mnc = a_.je();
    a_.load(RDI, -16); addrImm(RSI, internString(", ")); callFixups_.push_back({a_.call(), -6}); a_.store(RAX, -16);
    a_.patchRel(mnc, a_.here());
    a_.load(RCX, -8); a_.load(RAX, -24); a_.shlImm(RAX, 5); a_.addRR(RAX, RCX); a_.addImm(RAX, 8);
    a_.store(RAX, -40);                                 // entryaddr
    a_.loadMem(RDI, RAX, 0); a_.loadMem(RSI, RAX, 8);   // key
    callFixups_.push_back({a_.call(), -17});
    a_.load(RDI, -16); a_.movRR(RSI, RAX); callFixups_.push_back({a_.call(), -6}); a_.store(RAX, -16);
    a_.load(RDI, -16); addrImm(RSI, internString(": ")); callFixups_.push_back({a_.call(), -6}); a_.store(RAX, -16);
    a_.load(RAX, -40); a_.loadMem(RDI, RAX, 16); a_.loadMem(RSI, RAX, 24);  // val
    callFixups_.push_back({a_.call(), -17});
    a_.load(RDI, -16); a_.movRR(RSI, RAX); callFixups_.push_back({a_.call(), -6}); a_.store(RAX, -16);
    a_.load(RAX, -24); a_.addImm(RAX, 1); a_.store(RAX, -24);
    size_t mb = a_.jmp(); a_.patchRel(mb, mloop);
    a_.patchRel(mend, a_.here());
    a_.load(RDI, -16); addrImm(RSI, internString("}")); callFixups_.push_back({a_.call(), -6});
    a_.leave(); a_.ret();
    a_.patchRel(n7, a_.here());
    // float: render via float_to_str (fresh unowned string, like int_to_str)
    a_.cmpImm(RDI, 2); size_t n2f = a_.jne();
    a_.movRR(RDI, RSI);
    callFixups_.push_back({a_.call(), -72});
    a_.leave(); a_.ret();
    a_.patchRel(n2f, a_.here());
    // void: empty string
    addrImm(RAX, 16); a_.leave(); a_.ret();
}

// ar(rdi=opcode, rsi=l_tag, rdx=l_pay, rcx=r_tag, r8=r_pay) -> rax=tag, rdx=pay.
// The scalar-core arithmetic/comparison/string dispatcher (mirrors arithPrim).
void X64Gen::genAr() {
    arOff_ = a_.here();
    a_.push(RBP); a_.movRR(RBP, RSP); a_.subImm(RSP, 48);
    a_.store(RDI, -8); a_.store(RSI, -16); a_.store(RDX, -24);
    a_.store(RCX, -32); a_.store(R8, -40);
    // None?
    a_.cmpImm(RSI, 8); size_t j1 = a_.je();
    a_.cmpImm(RCX, 8); size_t j2 = a_.je();
    // string?
    a_.cmpImm(RSI, 4); size_t s1 = a_.je();
    a_.cmpImm(RCX, 4); size_t s2 = a_.je();
    // float? (either side; the int side promotes)
    a_.cmpImm(RSI, 2); size_t f1 = a_.je();
    a_.cmpImm(RCX, 2); size_t f2 = a_.je();
    size_t toInt = a_.jmp();
    // --- None branch ---
    a_.patchRel(j1, a_.here()); a_.patchRel(j2, a_.here());
    a_.load(RDI, -8);
    a_.cmpImm(RDI, 1); size_t ne = a_.je();
    a_.cmpImm(RDI, 2); size_t nn = a_.je();
    a_.xorRR(RAX, RAX); a_.xorRR(RDX, RDX); a_.leave(); a_.ret();   // void
    a_.patchRel(ne, a_.here());
    a_.load(RAX, -16); a_.load(RCX, -32); a_.cmpRR(RAX, RCX); a_.setccAx(0x94);
    a_.movRR(RDX, RAX); a_.movImm(RAX, 3); a_.leave(); a_.ret();
    a_.patchRel(nn, a_.here());
    a_.load(RAX, -16); a_.load(RCX, -32); a_.cmpRR(RAX, RCX); a_.setccAx(0x95);
    a_.movRR(RDX, RAX); a_.movImm(RAX, 3); a_.leave(); a_.ret();
    // --- string branch ---
    a_.patchRel(s1, a_.here()); a_.patchRel(s2, a_.here());
    a_.load(RDI, -8);
    a_.cmpImm(RDI, 0); size_t sc = a_.je();
    a_.cmpImm(RDI, 1); size_t seq = a_.je();
    a_.cmpImm(RDI, 2); size_t sne = a_.je();
    // relational (<,>,<=,>= = opcodes 7,8,9,10): lexicographic compare (bug.md #2).
    // Both operands must be strings; otherwise void (a checker type error).
    a_.load(RAX, -16); a_.cmpImm(RAX, 4); size_t rbad0 = a_.jne();
    a_.load(RAX, -32); a_.cmpImm(RAX, 4); size_t rbad1 = a_.jne();
    a_.load(RAX, -24); a_.loadInd(R8, RAX);        // R8 = lenA  (len at [desc])
    a_.load(RCX, -40); a_.loadInd(R9, RCX);        // R9 = lenB
    a_.movRR(RCX, R8); a_.cmpRR(R8, R9); size_t aSmall = a_.jle();   // RCX = min(lenA,lenB)
    a_.movRR(RCX, R9);
    a_.patchRel(aSmall, a_.here());
    a_.load(RSI, -24); a_.addImm(RSI, 8);          // RSI = A bytes (cmpsb: [RSI]-[RDI]=A-B)
    a_.load(RDI, -40); a_.addImm(RDI, 8);          // RDI = B bytes
    a_.testRR(RCX); size_t minZero = a_.je();
    a_.repeCmpsb();
    size_t mism = a_.jne();                         // ZF=0 -> a differing byte
    a_.patchRel(minZero, a_.here());               // equal prefix -> compare lengths
    a_.movRR(RAX, R8); a_.subRR(RAX, R9);          // RAX = lenA - lenB
    size_t haveDiff = a_.jmp();
    a_.patchRel(mism, a_.here());                   // mismatch -> compare the bytes
    a_.loadByte(RAX, RSI, -1); a_.loadByte(RCX, RDI, -1);   // zero-extended
    a_.subRR(RAX, RCX);                             // RAX = Abyte - Bbyte (signed 3-way)
    a_.patchRel(haveDiff, a_.here());
    // RAX < 0: A<B; == 0: equal; > 0: A>B. Select the boolean by opcode.
    a_.load(RDI, -8);
    a_.cmpImm(RDI, 7); size_t n7 = a_.jne();
    a_.testRR(RAX); a_.setccAx(0x9C); size_t d7 = a_.jmp();     // <  setl
    a_.patchRel(n7, a_.here());
    a_.cmpImm(RDI, 8); size_t n8 = a_.jne();
    a_.testRR(RAX); a_.setccAx(0x9F); size_t d8 = a_.jmp();     // >  setg
    a_.patchRel(n8, a_.here());
    a_.cmpImm(RDI, 9); size_t n9 = a_.jne();
    a_.testRR(RAX); a_.setccAx(0x9E); size_t d9 = a_.jmp();     // <= setle
    a_.patchRel(n9, a_.here());
    a_.testRR(RAX); a_.setccAx(0x9D);                          // >= setge (opcode 10)
    a_.patchRel(d7, a_.here()); a_.patchRel(d8, a_.here()); a_.patchRel(d9, a_.here());
    a_.movRR(RDX, RAX); a_.movImm(RAX, 3); a_.leave(); a_.ret();   // bool result
    a_.patchRel(rbad0, a_.here()); a_.patchRel(rbad1, a_.here());
    a_.xorRR(RAX, RAX); a_.xorRR(RDX, RDX); a_.leave(); a_.ret();  // void (type error)
    a_.patchRel(sc, a_.here());                          // concat
    a_.load(RDI, -16); a_.load(RSI, -24); callFixups_.push_back({a_.call(), -17}); a_.store(RAX, -8);
    a_.load(RDI, -32); a_.load(RSI, -40); callFixups_.push_back({a_.call(), -17}); a_.movRR(RSI, RAX);
    a_.load(RDI, -8); callFixups_.push_back({a_.call(), -6});
    // §15 strings tier: Arith is a TRANSFER (dk=2) — hand the fresh concat to
    // the dest slot at +1. (concat consumed the ts_build temporaries itself.)
    a_.store(RAX, -8);
    a_.movImm(RDI, 4); a_.movRR(RSI, RAX);
    callFixups_.push_back({a_.call(), -65});
    a_.load(RDX, -8); a_.movImm(RAX, 4); a_.leave(); a_.ret();
    a_.patchRel(seq, a_.here());                          // ==
    a_.load(RAX, -16); a_.cmpImm(RAX, 4); size_t sq0 = a_.jne();
    a_.load(RAX, -32); a_.cmpImm(RAX, 4); size_t sq1 = a_.jne();
    a_.load(RDI, -24); a_.load(RSI, -40); callFixups_.push_back({a_.call(), -8});
    a_.movRR(RDX, RAX); a_.movImm(RAX, 3); a_.leave(); a_.ret();
    a_.patchRel(sq0, a_.here()); a_.patchRel(sq1, a_.here());
    a_.xorRR(RDX, RDX); a_.movImm(RAX, 3); a_.leave(); a_.ret();   // not equal
    a_.patchRel(sne, a_.here());                          // !=
    a_.load(RAX, -16); a_.cmpImm(RAX, 4); size_t sn0 = a_.jne();
    a_.load(RAX, -32); a_.cmpImm(RAX, 4); size_t sn1 = a_.jne();
    a_.load(RDI, -24); a_.load(RSI, -40); callFixups_.push_back({a_.call(), -8});
    a_.testRR(RAX); a_.setccAx(0x94);
    a_.movRR(RDX, RAX); a_.movImm(RAX, 3); a_.leave(); a_.ret();
    a_.patchRel(sn0, a_.here()); a_.patchRel(sn1, a_.here());
    a_.movImm(RDX, 1); a_.movImm(RAX, 3); a_.leave(); a_.ret();    // not equal -> true
    // --- float branch --- (either operand tag 2; an int operand promotes via
    // cvtsi2sd; any other operand kind -> void, mirroring arithPrim)
    a_.patchRel(f1, a_.here()); a_.patchRel(f2, a_.here());
    // l -> xmm0
    a_.load(RAX, -16); a_.load(RDX, -24);
    a_.cmpImm(RAX, 2); size_t lNotF = a_.jne();
    a_.movqXmmR(0, RDX); size_t lDone = a_.jmp();
    a_.patchRel(lNotF, a_.here());
    a_.cmpImm(RAX, 1); size_t fBad1 = a_.jne();
    a_.cvtsi2sd(0, RDX);
    a_.patchRel(lDone, a_.here());
    // r -> xmm1
    a_.load(RAX, -32); a_.load(RDX, -40);
    a_.cmpImm(RAX, 2); size_t rNotF = a_.jne();
    a_.movqXmmR(1, RDX); size_t rDone = a_.jmp();
    a_.patchRel(rNotF, a_.here());
    a_.cmpImm(RAX, 1); size_t fBad2 = a_.jne();
    a_.cvtsi2sd(1, RDX);
    a_.patchRel(rDone, a_.here());
    a_.load(RDI, -8);                                      // opcode
    auto retFloat = [&]() {
        a_.movqRXmm(RDX, 0); a_.movImm(RAX, 2); a_.leave(); a_.ret();
    };
    // comisd already ran; cc-jump taken -> true, fallthrough -> false. The ccs
    // used (ja=7, jae=3) read CF, which comisd sets on unordered (NaN), so NaN
    // compares false — matching IEEE and the C++ engines.
    auto retBoolCc = [&](uint8_t cc) {
        size_t t = a_.jcc(cc);
        a_.xorRR(RDX, RDX); a_.movImm(RAX, 3); a_.leave(); a_.ret();
        a_.patchRel(t, a_.here());
        a_.movImm(RDX, 1); a_.movImm(RAX, 3); a_.leave(); a_.ret();
    };
    a_.cmpImm(RDI, 0); size_t fo0 = a_.jne();
    a_.addsd(0, 1); retFloat();
    a_.patchRel(fo0, a_.here());
    a_.cmpImm(RDI, 3); size_t fo3 = a_.jne();
    a_.subsd(0, 1); retFloat();
    a_.patchRel(fo3, a_.here());
    a_.cmpImm(RDI, 4); size_t fo4 = a_.jne();
    a_.mulsd(0, 1); retFloat();
    a_.patchRel(fo4, a_.here());
    a_.cmpImm(RDI, 5); size_t fo5 = a_.jne();
    a_.divsd(0, 1); retFloat();                            // /0 -> IEEE inf/nan
    a_.patchRel(fo5, a_.here());
    a_.cmpImm(RDI, 1); size_t fc1 = a_.jne();              // == : NaN/differ -> false
    a_.comisd(0, 1);
    { size_t up = a_.jcc(0xA); size_t ne = a_.jne();
      a_.movImm(RDX, 1); a_.movImm(RAX, 3); a_.leave(); a_.ret();
      a_.patchRel(up, a_.here()); a_.patchRel(ne, a_.here());
      a_.xorRR(RDX, RDX); a_.movImm(RAX, 3); a_.leave(); a_.ret(); }
    a_.patchRel(fc1, a_.here());
    a_.cmpImm(RDI, 2); size_t fc2 = a_.jne();              // != : NaN/differ -> true
    a_.comisd(0, 1);
    { size_t up = a_.jcc(0xA); size_t ne = a_.jne();
      a_.xorRR(RDX, RDX); a_.movImm(RAX, 3); a_.leave(); a_.ret();
      a_.patchRel(up, a_.here()); a_.patchRel(ne, a_.here());
      a_.movImm(RDX, 1); a_.movImm(RAX, 3); a_.leave(); a_.ret(); }
    a_.patchRel(fc2, a_.here());
    a_.cmpImm(RDI, 7); size_t fc7 = a_.jne();              // <  == r > l
    a_.comisd(1, 0); retBoolCc(7);                         // ja
    a_.patchRel(fc7, a_.here());
    a_.cmpImm(RDI, 8); size_t fc8 = a_.jne();              // >
    a_.comisd(0, 1); retBoolCc(7);
    a_.patchRel(fc8, a_.here());
    a_.cmpImm(RDI, 9); size_t fc9 = a_.jne();              // <= == r >= l
    a_.comisd(1, 0); retBoolCc(3);                         // jae
    a_.patchRel(fc9, a_.here());
    a_.cmpImm(RDI, 10); size_t fc10 = a_.jne();            // >=
    a_.comisd(0, 1); retBoolCc(3);
    a_.patchRel(fc10, a_.here());
    // % / bitwise on floats, or a non-numeric operand: void (arithPrim default)
    a_.patchRel(fBad1, a_.here()); a_.patchRel(fBad2, a_.here());
    a_.xorRR(RAX, RAX); a_.xorRR(RDX, RDX); a_.leave(); a_.ret();
    // --- int branch ---
    a_.patchRel(toInt, a_.here());
    a_.load(RDI, -8);                                     // opcode
    auto reti = [&]() { a_.movRR(RDX, RAX); a_.movImm(RAX, 1); a_.leave(); a_.ret(); };
    auto cmpOp = [&](uint8_t cc) {
        a_.load(RAX, -24); a_.load(RCX, -40); a_.cmpRR(RAX, RCX); a_.setccAx(cc);
        a_.movRR(RDX, RAX); a_.movImm(RAX, 3); a_.leave(); a_.ret();
    };
    a_.cmpImm(RDI, 0); size_t o0 = a_.jne();
    a_.load(RAX, -24); a_.load(RCX, -40); a_.addRR(RAX, RCX); reti();
    a_.patchRel(o0, a_.here());
    a_.cmpImm(RDI, 3); size_t o3 = a_.jne();
    a_.load(RAX, -24); a_.load(RCX, -40); a_.subRR(RAX, RCX); reti();
    a_.patchRel(o3, a_.here());
    a_.cmpImm(RDI, 4); size_t o4 = a_.jne();
    a_.load(RAX, -24); a_.load(RCX, -40); a_.imulRR(RAX, RCX); reti();
    a_.patchRel(o4, a_.here());
    a_.cmpImm(RDI, 5); size_t o5 = a_.jne();
    a_.load(RAX, -24); a_.load(RCX, -40); a_.testRR(RCX); size_t z5 = a_.je();
    a_.cqo(); a_.idiv(RCX); reti();
    a_.patchRel(z5, a_.here()); a_.xorRR(RAX, RAX); reti();
    a_.patchRel(o5, a_.here());
    a_.cmpImm(RDI, 6); size_t o6 = a_.jne();
    a_.load(RAX, -24); a_.load(RCX, -40); a_.testRR(RCX); size_t z6 = a_.je();
    a_.cqo(); a_.idiv(RCX); a_.movRR(RAX, RDX); reti();
    a_.patchRel(z6, a_.here()); a_.xorRR(RAX, RAX); reti();
    a_.patchRel(o6, a_.here());
    a_.cmpImm(RDI, 11); size_t o11 = a_.jne();
    a_.load(RAX, -24); a_.load(RCX, -40); a_.andRR(RAX, RCX); reti();
    a_.patchRel(o11, a_.here());
    a_.cmpImm(RDI, 12); size_t o12 = a_.jne();
    a_.load(RAX, -24); a_.load(RCX, -40); a_.orRR(RAX, RCX); reti();
    a_.patchRel(o12, a_.here());
    // Shift count outside 0..63 throws (§3.7 loudness) rather than the x86
    // mask-to-6-bits default; within range, `shl`/`sar r, cl` are exactly the
    // left-shift / arithmetic-right-shift semantics the design specifies —
    // the count is already in RCX from the operand load, so CL just works.
    auto raiseShift = [&]() {
        addrImm(RDI, internString("shift count out of range"));
        callFixups_.push_back({a_.call(), -29});           // raise
        a_.xorRR(RAX, RAX); a_.xorRR(RDX, RDX); a_.leave(); a_.ret();
    };
    a_.cmpImm(RDI, 13); size_t o13 = a_.jne();              // <<
    a_.load(RAX, -24); a_.load(RCX, -40);
    a_.cmpImm(RCX, 0); size_t neg13 = a_.jl();
    a_.cmpImm(RCX, 63); size_t big13 = a_.jg();
    a_.shlCl(RAX); reti();
    a_.patchRel(neg13, a_.here()); a_.patchRel(big13, a_.here());
    raiseShift();
    a_.patchRel(o13, a_.here());
    a_.cmpImm(RDI, 14); size_t o14 = a_.jne();              // >> (arithmetic)
    a_.load(RAX, -24); a_.load(RCX, -40);
    a_.cmpImm(RCX, 0); size_t neg14 = a_.jl();
    a_.cmpImm(RCX, 63); size_t big14 = a_.jg();
    a_.sarCl(RAX); reti();
    a_.patchRel(neg14, a_.here()); a_.patchRel(big14, a_.here());
    raiseShift();
    a_.patchRel(o14, a_.here());
    a_.cmpImm(RDI, 15); size_t o15 = a_.jne();              // ^ (xor)
    a_.load(RAX, -24); a_.load(RCX, -40); a_.xorRR(RAX, RCX); reti();
    a_.patchRel(o15, a_.here());
    a_.cmpImm(RDI, 1); size_t c1 = a_.jne(); cmpOp(0x94); a_.patchRel(c1, a_.here());
    a_.cmpImm(RDI, 2); size_t c2 = a_.jne(); cmpOp(0x95); a_.patchRel(c2, a_.here());
    a_.cmpImm(RDI, 7); size_t c7 = a_.jne(); cmpOp(0x9C); a_.patchRel(c7, a_.here());
    a_.cmpImm(RDI, 8); size_t c8 = a_.jne(); cmpOp(0x9F); a_.patchRel(c8, a_.here());
    a_.cmpImm(RDI, 9); size_t c9 = a_.jne(); cmpOp(0x9E); a_.patchRel(c9, a_.here());
    cmpOp(0x9D);                                          // >= (opcode 10, fallthrough)
}

// iterat(rdi=it_tag, rsi=it_pay, rdx=index) -> rax=tag, rdx=pay. Array element,
// range value, or a Pair object for a map entry (mirrors CGen::iterat).
void X64Gen::genIterAt() {
    iterAtOff_ = a_.here();
    a_.push(RBP); a_.movRR(RBP, RSP); a_.subImm(RSP, 48);
    a_.cmpImm(RDI, 6); size_t na = a_.jne();
    a_.loadInd(RAX, RSI); a_.testRR(RAX); size_t denseIt = a_.jcc(8);       // dense array?
    a_.movRR(RAX, RDX); a_.shlImm(RAX, 4); a_.addRR(RAX, RSI); a_.addImm(RAX, 8);
    a_.loadMem(RCX, RAX, 8); a_.loadMem(RAX, RAX, 0); a_.movRR(RDX, RCX);   // tag=rax, pay=rdx
    a_.leave(); a_.ret();
    a_.patchRel(denseIt, a_.here());                                        // dense: pointer into buffer
    a_.loadMem(RCX, RSI, 8);                                                // recBytes
    a_.movRR(RAX, RDX); a_.imulRR(RAX, RCX); a_.addRR(RAX, RSI); a_.addImm(RAX, 16);
    a_.movRR(RDX, RAX); a_.movImm(RAX, 5);                                  // (object, &record[idx])
    a_.leave(); a_.ret();
    a_.patchRel(na, a_.here());
    a_.cmpImm(RDI, 5); size_t nr = a_.jne();               // range: start + index
    a_.store(RDX, -8);                                     // index
    a_.movRR(RDI, RSI); addrImm(RSI, internString("start"));
    callFixups_.push_back({a_.call(), -11});               // getfield -> rax,rdx (start)
    a_.load(RCX, -8); a_.addRR(RDX, RCX); a_.movImm(RAX, 1); a_.leave(); a_.ret();
    a_.patchRel(nr, a_.here());
    a_.cmpImm(RDI, 7); size_t nm = a_.jne();               // map: Pair(first,second)
    a_.movRR(RAX, RDX); a_.shlImm(RAX, 5); a_.addRR(RAX, RSI); a_.addImm(RAX, 8);
    a_.store(RAX, -8);                                     // entryaddr
    a_.movImm(RDI, (uint64_t)lookupClsId("Pair"));
    callFixups_.push_back({a_.call(), -10});               // mkobj -> rax = pair
    a_.store(RAX, -16);                                    // pairptr
    a_.load(RCX, -8); a_.loadMem(RDX, RCX, 0); a_.loadMem(R8, RCX, 8);   // key -> first
    a_.movRR(RDI, RAX); addrImm(RSI, internString("first")); a_.movRR(RCX, R8);
    callFixups_.push_back({a_.call(), -12});               // setfield(pair,"first",tag,pay)
    // §15: the Pair's fields OWN their refs (raw setfield bypasses the
    // SetMember hook) — retain each, or the Pair's death releases the map's
    // own entry refs unbalanced.
    a_.load(RAX, -8); a_.loadMem(RDI, RAX, 0); a_.loadMem(RSI, RAX, 8);
    callFixups_.push_back({a_.call(), -65});               // retain first (the key)
    a_.load(RAX, -8); a_.loadMem(RDX, RAX, 16); a_.loadMem(RCX, RAX, 24); // val -> second
    a_.load(RDI, -16); addrImm(RSI, internString("second"));
    callFixups_.push_back({a_.call(), -12});
    a_.load(RAX, -8); a_.loadMem(RDI, RAX, 16); a_.loadMem(RSI, RAX, 24);
    callFixups_.push_back({a_.call(), -65});               // retain second (the value)
    a_.load(RAX, -16); a_.movImm(RDX, 0); a_.movRR(RDX, RAX); a_.movImm(RAX, 5);
    a_.leave(); a_.ret();
    a_.patchRel(nm, a_.here());
    a_.xorRR(RAX, RAX); a_.xorRR(RDX, RDX); a_.leave(); a_.ret();
}

// idxget(rdi=base_tag, rsi=base_pay, rdx=idx_tag, rcx=idx_pay) -> rax,rdx.
// Object ([]) accessor, native array element, or native map lookup.
void X64Gen::genIdxGet(const std::vector<Symbol*>& classes) {
    idxGetOff_ = a_.here();
    a_.push(RBP); a_.movRR(RBP, RSP); a_.subImm(RSP, 48);
    a_.store(RDI, -8); a_.store(RSI, -16); a_.store(RDX, -24); a_.store(RCX, -32);
    // object: dispatch to the class's ([]) getter
    a_.cmpImm(RDI, 5); size_t no = a_.jne();
    a_.loadInd(RAX, RSI);                                  // classId
    for (Symbol* cls : classes) {
        std::vector<const Stmt*> mem; collectMembers(cls, mem);
        const Stmt* g = nullptr;
        for (const Stmt* m : mem)
            if (m->isGet && std::string(m->name) == "[]" && mod_.byDecl.count(m)) g = m;
        if (!g) continue;
        a_.cmpImm(RAX, clsId(cls)); size_t jn = a_.jne();
        // getter(obj, idx): push idx (arg1) then obj (arg0)
        a_.load(RCX, -32); a_.push(RCX); a_.load(RDX, -24); a_.push(RDX);   // idx pay, tag
        a_.load(RSI, -16); a_.push(RSI); a_.movImm(RAX, 5); a_.push(RAX);   // obj pay, tag
        callFixups_.push_back({a_.call(), mod_.byDecl.at(g)});
        a_.addImm(RSP, 32); a_.leave(); a_.ret();
        a_.patchRel(jn, a_.here());
    }
    a_.xorRR(RAX, RAX); a_.xorRR(RDX, RDX); a_.leave(); a_.ret();   // object, no ([]) getter
    a_.patchRel(no, a_.here());
    // array: element at idx (void if out of range)
    a_.cmpImm(RDI, 6); size_t nar = a_.jne();
    a_.load(RSI, -16); a_.loadInd(RAX, RSI); a_.testRR(RAX); size_t denseGet = a_.jcc(8);   // dense?
    // BOXED (unchanged): bounds vs len, element value at +8 + idx*16
    a_.load(RCX, -32); a_.load(RSI, -16); a_.loadInd(RAX, RSI); a_.cmpRR(RCX, RAX); size_t oob = a_.jge();
    a_.load(RCX, -32); a_.testRR(RCX); size_t neg = a_.jcc(8);   // js -> negative idx
    a_.movRR(RAX, RCX); a_.shlImm(RAX, 4); a_.load(RSI, -16); a_.addRR(RAX, RSI); a_.addImm(RAX, 8);
    a_.loadMem(RCX, RAX, 8); a_.loadMem(RAX, RAX, 0); a_.movRR(RDX, RCX);
    a_.leave(); a_.ret();
    // DENSE: bounds vs clean len, element = POINTER into the buffer (no materialize)
    a_.patchRel(denseGet, a_.here());
    a_.load(RCX, -32); a_.load(RSI, -16); a_.loadInd(RAX, RSI); a_.shlImm(RAX, 1); a_.shrImm(RAX, 1);
    a_.cmpRR(RCX, RAX); size_t doob = a_.jge();
    a_.testRR(RCX); size_t dneg = a_.jcc(8);
    a_.load(RSI, -16); a_.loadMem(RDX, RSI, 8);          // recBytes
    a_.load(RAX, -32); a_.imulRR(RAX, RDX);              // idx * recBytes
    a_.addRR(RAX, RSI); a_.addImm(RAX, 16);              // &record[idx]
    a_.movRR(RDX, RAX); a_.movImm(RAX, 5);               // (object, &record)
    a_.leave(); a_.ret();
    a_.patchRel(oob, a_.here()); a_.patchRel(neg, a_.here());
    a_.patchRel(doob, a_.here()); a_.patchRel(dneg, a_.here());
    a_.load(RDI, -32); a_.load(RSI, -16); a_.loadInd(RSI, RSI); a_.shlImm(RSI, 1); a_.shrImm(RSI, 1);
    callFixups_.push_back({a_.call(), -30});                      // raise_oob(index, clean length)
    a_.xorRR(RAX, RAX); a_.xorRR(RDX, RDX); a_.leave(); a_.ret();
    a_.patchRel(nar, a_.here());
    // map: linear scan by key equality (void if absent). Strings compare by
    // CONTENT (str_eq), mirroring keyEquals in the interpreters — pointer
    // identity only holds for interned literals, not built keys.
    a_.cmpImm(RDI, 7); size_t nmp = a_.jne();
    a_.load(RSI, -16); a_.loadInd(RAX, RSI); a_.store(RAX, -40);        // len
    a_.xorRR(RAX, RAX); a_.store(RAX, -48);                             // i
    size_t ml = a_.here();
    a_.load(RAX, -48); a_.load(RCX, -40); a_.cmpRR(RAX, RCX); size_t mdone = a_.jge();
    a_.load(RAX, -48); a_.shlImm(RAX, 5); a_.load(RSI, -16); a_.addRR(RAX, RSI); a_.addImm(RAX, 8);
    // compare key (entry[0..1]) with idx (-24,-32)
    a_.loadMem(RCX, RAX, 0); a_.load(RDX, -24); a_.cmpRR(RCX, RDX); size_t kt = a_.jne();
    a_.cmpImm(RCX, 4); size_t kStr = a_.je();
    a_.loadMem(RCX, RAX, 8); a_.load(RDX, -32); a_.cmpRR(RCX, RDX); size_t kp = a_.jne();
    size_t kHit = a_.jmp();
    a_.patchRel(kStr, a_.here());
    a_.loadMem(RDI, RAX, 8); a_.load(RSI, -32);
    callFixups_.push_back({a_.call(), -8});                             // str_eq(key, probe)
    a_.testRR(RAX); size_t kp2 = a_.je();
    a_.load(RAX, -48); a_.shlImm(RAX, 5); a_.load(RSI, -16); a_.addRR(RAX, RSI); a_.addImm(RAX, 8);
    a_.patchRel(kHit, a_.here());
    a_.loadMem(RCX, RAX, 24); a_.loadMem(RAX, RAX, 16); a_.movRR(RDX, RCX);  // value
    a_.leave(); a_.ret();
    a_.patchRel(kt, a_.here()); a_.patchRel(kp, a_.here()); a_.patchRel(kp2, a_.here());
    a_.load(RAX, -48); a_.addImm(RAX, 1); a_.store(RAX, -48);
    size_t mlb = a_.jmp(); a_.patchRel(mlb, ml);
    a_.patchRel(mdone, a_.here());
    a_.patchRel(nmp, a_.here());
    a_.xorRR(RAX, RAX); a_.xorRR(RDX, RDX); a_.leave(); a_.ret();
}

// idxset(rdi=base_tag, rsi=base_pay, rdx=idx_tag, rcx=idx_pay, r8=val_tag,
// r9=val_pay) -> rax,rdx (new base). Object ([]) setter (identity result),
// pure array store, or pure map upsert.
void X64Gen::genIdxSet(const std::vector<Symbol*>& classes) {
    idxSetOff_ = a_.here();
    a_.push(RBP); a_.movRR(RBP, RSP); a_.subImm(RSP, 96);
    a_.store(RDI, -8); a_.store(RSI, -16); a_.store(RDX, -24); a_.store(RCX, -32);
    a_.store(R8, -40); a_.store(R9, -48);
    // -56 newbase, -64 oldlen, -72 foundIdx
    // object: call ([]) setter, return base unchanged
    a_.cmpImm(RDI, 5); size_t no = a_.jne();
    a_.loadInd(RAX, RSI);
    for (Symbol* cls : classes) {
        std::vector<const Stmt*> mem; collectMembers(cls, mem);
        const Stmt* s = nullptr;
        for (const Stmt* m : mem)
            if (m->isSet && std::string(m->name) == "[]" && mod_.byDecl.count(m)) s = m;
        if (!s) continue;
        a_.cmpImm(RAX, clsId(cls)); size_t jn = a_.jne();
        a_.load(RAX, -48); a_.push(RAX); a_.load(RAX, -40); a_.push(RAX);   // val (pay, tag)
        a_.load(RAX, -32); a_.push(RAX); a_.load(RAX, -24); a_.push(RAX);   // idx (pay, tag)
        a_.load(RAX, -16); a_.push(RAX); a_.movImm(RAX, 5); a_.push(RAX);   // obj (pay, tag)
        callFixups_.push_back({a_.call(), mod_.byDecl.at(s)});
        a_.addImm(RSP, 48);
        a_.load(RDX, -16); a_.movImm(RAX, 5); a_.leave(); a_.ret();
        a_.patchRel(jn, a_.here());
    }
    a_.load(RDX, -16); a_.movImm(RAX, 5); a_.leave(); a_.ret();
    a_.patchRel(no, a_.here());
    // array: copy, set [idx]=val
    a_.cmpImm(RDI, 6); size_t nar = a_.jne();
    a_.load(RSI, -16); a_.loadInd(RAX, RSI); a_.testRR(RAX); size_t denseSet = a_.jcc(8);   // dense?
    // §15 COW: a uniquely-owned boxed array (refcount 1) can be mutated in place —
    // no copy, no leftover. Aliased arrays fall through to the pure-value copy.
    a_.load(RSI, -16); a_.loadMem(RAX, RSI, -16); a_.cmpImm(RAX, 1); size_t sharedCopy = a_.jne();
    a_.load(RCX, -32); a_.loadInd(RAX, RSI); a_.cmpRR(RCX, RAX); size_t ipOob = a_.jge();   // idx>=len
    a_.testRR(RCX); size_t ipNeg = a_.jcc(8);
    a_.movRR(RAX, RCX); a_.shlImm(RAX, 4); a_.addRR(RAX, RSI); a_.addImm(RAX, 8);           // &slot[idx]
    a_.store(RAX, -88);
    // §15: the slot owns its element — release the overwritten ref, retain the new
    a_.loadMem(RDI, RAX, 0); a_.loadMem(RSI, RAX, 8);
    callFixups_.push_back({a_.call(), -66});
    a_.load(RAX, -88);
    a_.load(RDX, -40); a_.storeMem(RAX, 0, RDX); a_.load(RDX, -48); a_.storeMem(RAX, 8, RDX);
    a_.load(RDI, -40); a_.load(RSI, -48);
    callFixups_.push_back({a_.call(), -65});
    a_.patchRel(ipOob, a_.here()); a_.patchRel(ipNeg, a_.here());
    a_.load(RDX, -16); a_.movImm(RAX, 6); a_.leave(); a_.ret();                             // same array
    // BOXED shared: copy the value buffer, overwrite the 16-byte slot
    a_.patchRel(sharedCopy, a_.here());
    a_.load(RSI, -16); a_.loadInd(RAX, RSI);
    a_.movRR(RDI, RAX); callFixups_.push_back({a_.call(), -18}); a_.store(RAX, -56);  // mkarr(len)
    a_.load(RSI, -16); a_.loadInd(RCX, RSI); a_.shlImm(RCX, 4);
    a_.load(RSI, -16); a_.addImm(RSI, 8); a_.load(RDI, -56); a_.addImm(RDI, 8); a_.repMovsb();
    a_.load(RCX, -32); a_.load(RSI, -56); a_.loadInd(RAX, RSI); a_.cmpRR(RCX, RAX); size_t oob = a_.jge();
    a_.load(RAX, -32); a_.shlImm(RAX, 4); a_.load(RSI, -56); a_.addRR(RAX, RSI); a_.addImm(RAX, 8);
    a_.load(RDX, -40); a_.storeMem(RAX, 0, RDX); a_.load(RDX, -48); a_.storeMem(RAX, 8, RDX);
    a_.patchRel(oob, a_.here());
    // §15: the new buffer owns every element — retain each (the old shared
    // buffer keeps its own counted refs; mirror of the map upsert below)
    a_.load(RSI, -56); a_.loadInd(RCX, RSI); a_.store(RCX, -88);   // newlen
    a_.xorRR(RCX, RCX); a_.store(RCX, -96);                         // i = 0
    size_t srl = a_.here();
    a_.load(RCX, -96); a_.load(RAX, -88); a_.cmpRR(RCX, RAX); size_t srdone = a_.jge();
    a_.load(RSI, -56); a_.load(RAX, -96); a_.shlImm(RAX, 4); a_.addRR(RAX, RSI); a_.addImm(RAX, 8);
    a_.loadMem(RDI, RAX, 0); a_.loadMem(RSI, RAX, 8);
    callFixups_.push_back({a_.call(), -65});                       // retain element
    a_.load(RCX, -96); a_.addImm(RCX, 1); a_.store(RCX, -96);
    size_t srb = a_.jmp(); a_.patchRel(srb, srl);
    a_.patchRel(srdone, a_.here());
    a_.load(RDX, -56); a_.movImm(RAX, 6); a_.leave(); a_.ret();
    a_.patchRel(denseSet, a_.here());
    // §15 COW: a uniquely-owned dense array (refcount 1) copies the record into
    // place — same buffer, no allocation. Records are inline value structs, so
    // there is no element ref to release/retain. Aliased arrays fall through.
    a_.load(RSI, -16); a_.loadMem(RAX, RSI, -16); a_.cmpImm(RAX, 1); size_t dShared = a_.jne();
    a_.loadInd(RAX, RSI); a_.shlImm(RAX, 1); a_.shrImm(RAX, 1);        // cleanLen
    a_.load(RCX, -32); a_.cmpRR(RCX, RAX); size_t dIpOob = a_.jge();   // idx >= len
    a_.testRR(RCX); size_t dIpNeg = a_.jcc(8);                          // idx < 0
    a_.loadMem(RCX, RSI, 8);                                            // recBytes
    a_.load(RAX, -32); a_.imulRR(RAX, RCX);                             // idx*recBytes
    a_.movRR(RDI, RSI); a_.addImm(RDI, 16); a_.addRR(RDI, RAX);         // &record[idx]
    a_.load(RSI, -48);                                                  // value payload
    a_.repMovsb();
    a_.patchRel(dIpOob, a_.here()); a_.patchRel(dIpNeg, a_.here());
    a_.load(RDX, -16); a_.movImm(RAX, 6); a_.leave(); a_.ret();         // same array
    a_.patchRel(dShared, a_.here());
    // DENSE shared: copy the record buffer, inline the (already deep-copied) value into record[idx]
    a_.load(RSI, -16); a_.loadInd(RAX, RSI); a_.shlImm(RAX, 1); a_.shrImm(RAX, 1); a_.store(RAX, -64);  // cleanLen
    a_.load(RSI, -16); a_.loadMem(RCX, RSI, 8); a_.store(RCX, -72);                                     // recBytes
    a_.load(RAX, -64); a_.imulRR(RAX, RCX); a_.addImm(RAX, 16); a_.movRR(RDI, RAX);
    callFixups_.push_back({a_.call(), -69}); a_.store(RAX, -56);                                        // halloc -> newbase (prefixed)
    a_.load(RCX, -64); a_.movImm(RDX, 0x8000000000000000ULL); a_.orRR(RCX, RDX); a_.storeInd(RAX, RCX);
    a_.load(RDX, -72); a_.storeMem(RAX, 8, RDX);
    a_.load(RCX, -64); a_.load(RAX, -72); a_.imulRR(RCX, RAX);
    a_.load(RSI, -16); a_.addImm(RSI, 16); a_.load(RDI, -56); a_.addImm(RDI, 16); a_.repMovsb();
    a_.load(RCX, -32); a_.load(RAX, -64); a_.cmpRR(RCX, RAX); size_t doob = a_.jge();
    a_.load(RCX, -32); a_.testRR(RCX); size_t dneg = a_.jcc(8);
    a_.load(RSI, -48); a_.load(RDI, -56); a_.addImm(RDI, 16);
    a_.load(RAX, -32); a_.load(RCX, -72); a_.imulRR(RAX, RCX); a_.addRR(RDI, RAX);
    a_.load(RCX, -72); a_.repMovsb();
    a_.patchRel(doob, a_.here()); a_.patchRel(dneg, a_.here());
    a_.load(RDX, -56); a_.movImm(RAX, 6); a_.leave(); a_.ret();
    a_.patchRel(nar, a_.here());
    // map: pure upsert — find the key, then copy and overwrite (or append).
    // String keys match by CONTENT (str_eq), mirroring keyEquals.
    a_.cmpImm(RDI, 7); size_t nmp = a_.jne();
    a_.load(RSI, -16); a_.loadInd(RAX, RSI); a_.store(RAX, -64);   // oldlen
    a_.movImm(RAX, (uint64_t)-1); a_.store(RAX, -72);              // foundIdx = -1
    a_.xorRR(RAX, RAX); a_.store(RAX, -80);                        // i = 0 (slot: str_eq clobbers regs)
    size_t fl = a_.here();
    a_.load(RCX, -80); a_.load(RAX, -64); a_.cmpRR(RCX, RAX); size_t fdone = a_.jge();
    a_.load(RAX, -80); a_.shlImm(RAX, 5); a_.load(RSI, -16); a_.addRR(RAX, RSI); a_.addImm(RAX, 8);
    a_.loadMem(RCX, RAX, 0); a_.load(RDX, -24); a_.cmpRR(RCX, RDX); size_t kt = a_.jne();
    a_.cmpImm(RCX, 4); size_t kStr = a_.je();
    a_.loadMem(RCX, RAX, 8); a_.load(RDX, -32); a_.cmpRR(RCX, RDX); size_t kp = a_.jne();
    size_t kHit = a_.jmp();
    a_.patchRel(kStr, a_.here());
    a_.loadMem(RDI, RAX, 8); a_.load(RSI, -32);
    callFixups_.push_back({a_.call(), -8});                        // str_eq(key, probe)
    a_.testRR(RAX); size_t kp2 = a_.je();
    a_.patchRel(kHit, a_.here());
    a_.load(RAX, -80); a_.store(RAX, -72); size_t ffound = a_.jmp();   // foundIdx = i; stop
    a_.patchRel(kt, a_.here()); a_.patchRel(kp, a_.here()); a_.patchRel(kp2, a_.here());
    a_.load(RAX, -80); a_.addImm(RAX, 1); a_.store(RAX, -80);
    size_t flb = a_.jmp(); a_.patchRel(flb, fl);
    a_.patchRel(fdone, a_.here()); a_.patchRel(ffound, a_.here());
    // §15 COW: a uniquely-owned map (refcount 1) with an EXISTING key updates the
    // value in place — release the overwritten ref, store + retain the new one,
    // return the same map. A missing key still takes the pure copy below: halloc
    // records the requested size, so there is no spare capacity to append into.
    a_.load(RSI, -16); a_.loadMem(RAX, RSI, -16); a_.cmpImm(RAX, 1); size_t mShared = a_.jne();
    a_.load(RAX, -72); a_.cmpImm(RAX, -1); size_t mAbsent = a_.je();
    a_.shlImm(RAX, 5); a_.addRR(RAX, RSI); a_.addImm(RAX, 8); a_.store(RAX, -80);   // &entry
    a_.loadMem(RDI, RAX, 16); a_.loadMem(RSI, RAX, 24);
    callFixups_.push_back({a_.call(), -66});                   // release the old value
    a_.load(RAX, -80);
    a_.load(RDX, -40); a_.storeMem(RAX, 16, RDX);
    a_.load(RDX, -48); a_.storeMem(RAX, 24, RDX);
    a_.load(RDI, -40); a_.load(RSI, -48);
    callFixups_.push_back({a_.call(), -65});                   // the entry owns the new value
    a_.load(RDX, -16); a_.movImm(RAX, 7); a_.leave(); a_.ret();   // same map
    a_.patchRel(mShared, a_.here()); a_.patchRel(mAbsent, a_.here());
    // newlen = found ? oldlen : oldlen+1
    a_.load(RAX, -72); a_.cmpImm(RAX, -1); size_t wasFound = a_.jne();
    a_.load(RDI, -64); a_.addImm(RDI, 1); size_t mk = a_.jmp();
    a_.patchRel(wasFound, a_.here()); a_.load(RDI, -64);
    a_.patchRel(mk, a_.here());
    callFixups_.push_back({a_.call(), -19}); a_.store(RAX, -56);   // mkmap(newlen)
    a_.load(RCX, -64); a_.shlImm(RCX, 5); a_.load(RSI, -16); a_.addImm(RSI, 8);
    a_.load(RDI, -56); a_.addImm(RDI, 8); a_.repMovsb();           // copy old entries
    // target = found ? foundIdx : oldlen
    a_.load(RAX, -72); a_.cmpImm(RAX, -1); size_t tFound = a_.jne();
    a_.load(RAX, -64); a_.patchRel(tFound, a_.here());
    a_.shlImm(RAX, 5); a_.load(RSI, -56); a_.addRR(RAX, RSI); a_.addImm(RAX, 8);
    a_.load(RDX, -24); a_.storeMem(RAX, 0, RDX);
    a_.load(RDX, -32); a_.storeMem(RAX, 8, RDX);
    a_.load(RDX, -40); a_.storeMem(RAX, 16, RDX);
    a_.load(RDX, -48); a_.storeMem(RAX, 24, RDX);
    // §15: the new map owns every entry — retain each key + value (covers both the
    // copied old entries and the new one; the old map is released by IndexStore's
    // op-hook, whose recursiveFree balances these retains).
    a_.load(RSI, -56); a_.loadInd(RCX, RSI); a_.store(RCX, -64);   // newlen
    a_.xorRR(RCX, RCX); a_.store(RCX, -72);                         // i = 0
    size_t rloop = a_.here();
    a_.load(RCX, -72); a_.load(RAX, -64); a_.cmpRR(RCX, RAX); size_t rdone = a_.jge();
    a_.load(RSI, -56); a_.load(RAX, -72); a_.shlImm(RAX, 5); a_.addRR(RAX, RSI); a_.addImm(RAX, 8);
    a_.store(RAX, -80);
    a_.loadMem(RDI, RAX, 0); a_.loadMem(RSI, RAX, 8);
    callFixups_.push_back({a_.call(), -65});                       // retain key
    a_.load(RAX, -80); a_.loadMem(RDI, RAX, 16); a_.loadMem(RSI, RAX, 24);
    callFixups_.push_back({a_.call(), -65});                       // retain value
    a_.load(RCX, -72); a_.addImm(RCX, 1); a_.store(RCX, -72);
    size_t rb = a_.jmp(); a_.patchRel(rb, rloop);
    a_.patchRel(rdone, a_.here());
    a_.load(RDX, -56); a_.movImm(RAX, 7); a_.leave(); a_.ret();
    a_.patchRel(nmp, a_.here());
    a_.load(RDX, -16); a_.load(RAX, -8); a_.leave(); a_.ret();     // unknown base: return as-is
}

// callnative(rdi=self_tag, rsi=self_pay, rdx=nameptr, rcx=argc, r8=argptr) ->
// rax,rdx. The native cores: string/int/bool + Array/Map primitives, mirroring
// RuntimeNatives. argptr points at the first arg (tag at [r8+16k], pay +8).
void X64Gen::genCallNative() {
    callNativeOff_ = a_.here();
    a_.push(RBP); a_.movRR(RBP, RSP); a_.subImm(RSP, 48);
    a_.store(RDI, -8); a_.store(RSI, -16); a_.store(RDX, -24); a_.store(R8, -40);
    auto nameIs = [&](const char* n) {                 // rax=1 if nameptr==intern(n)
        a_.load(RAX, -24); addrImm(RCX, internString(n)); a_.cmpRR(RAX, RCX);
    };
    auto retInt = [&]() { a_.movImm(RAX, 1); a_.leave(); a_.ret(); };   // rdx=payload preset
    // string ----------------------------------------------------------------
    a_.load(RAX, -8); a_.cmpImm(RAX, 4); size_t nStr = a_.jne();
    nameIs("length"); size_t s1 = a_.jne();
    a_.load(RSI, -16); a_.loadInd(RDX, RSI); retInt();
    a_.patchRel(s1, a_.here());
    nameIs("isEmpty"); size_t s2 = a_.jne();
    a_.load(RSI, -16); a_.loadInd(RAX, RSI); a_.testRR(RAX); a_.setccAx(0x94);
    a_.movRR(RDX, RAX); a_.movImm(RAX, 3); a_.leave(); a_.ret();
    a_.patchRel(s2, a_.here());
    nameIs("toString"); size_t s3 = a_.jne();
    // §15: returns the RECEIVER — retain it (borrowed -> +1 transfer contract;
    // frame-exit release would otherwise free the caller's own string).
    a_.movImm(RDI, 4); a_.load(RSI, -16);
    callFixups_.push_back({a_.call(), -65});
    a_.load(RDX, -16); a_.movImm(RAX, 4); a_.leave(); a_.ret();
    a_.patchRel(s3, a_.here());
    nameIs("indexOf"); size_t s4 = a_.jne();
    a_.load(RDI, -16); a_.load(R8, -40); a_.loadMem(RSI, R8, 8);
    callFixups_.push_back({a_.call(), -53}); a_.movRR(RDX, RAX); a_.movImm(RAX, 1); a_.leave(); a_.ret();
    a_.patchRel(s4, a_.here());
    nameIs("contains"); size_t s5 = a_.jne();
    a_.load(RDI, -16); a_.load(R8, -40); a_.loadMem(RSI, R8, 8);
    callFixups_.push_back({a_.call(), -53});
    a_.xorRR(RCX, RCX); a_.cmpRR(RAX, RCX); a_.setccAx(0x9D);   // idx >= 0
    a_.movRR(RDX, RAX); a_.movImm(RAX, 3); a_.leave(); a_.ret();
    a_.patchRel(s5, a_.here());
    // §15: every fresh-string native below returns at +1 (the CallDyn transfer
    // contract): store the unowned result, retain it, then hand it over.
    auto retStrOwned = [&]() {
        a_.store(RAX, -32);
        a_.movImm(RDI, 4); a_.movRR(RSI, RAX);
        callFixups_.push_back({a_.call(), -65});
        a_.load(RDX, -32); a_.movImm(RAX, 4); a_.leave(); a_.ret();
    };
    nameIs("subStr"); size_t s6 = a_.jne();
    a_.load(RDI, -16); a_.load(R8, -40); a_.loadMem(RSI, R8, 8); a_.loadMem(RDX, R8, 24);
    callFixups_.push_back({a_.call(), -54}); retStrOwned();
    a_.patchRel(s6, a_.here());
    nameIs("toInt"); size_t s7 = a_.jne();
    a_.load(RDI, -16); callFixups_.push_back({a_.call(), -55});
    a_.movRR(RDX, RAX); a_.movImm(RAX, 1); a_.leave(); a_.ret();
    a_.patchRel(s7, a_.here());
    nameIs("trim"); size_t s8 = a_.jne();
    a_.load(RDI, -16); callFixups_.push_back({a_.call(), -56});
    retStrOwned();
    a_.patchRel(s8, a_.here());
    nameIs("charAt"); size_t s9 = a_.jne();
    // charAt(i) == subStr(i, 1): "" when out of range, else the 1-char string.
    a_.load(RDI, -16); a_.load(R8, -40); a_.loadMem(RSI, R8, 8); a_.movImm(RDX, 1);
    callFixups_.push_back({a_.call(), -54}); retStrOwned();
    a_.patchRel(s9, a_.here());
    nameIs("toUpper"); size_t s10 = a_.jne();
    a_.load(RDI, -16); a_.xorRR(RSI, RSI);               // mode 0 = upper
    size_t caseJoin = a_.jmp();
    a_.patchRel(s10, a_.here());
    nameIs("toLower"); size_t s11 = a_.jne();
    a_.load(RDI, -16); a_.movImm(RSI, 1);                // mode 1 = lower
    a_.patchRel(caseJoin, a_.here());
    callFixups_.push_back({a_.call(), -71});             // str_case -> fresh copy
    retStrOwned();
    a_.patchRel(s11, a_.here());
    // startsWith/endsWith: probe = subStr(s, a, plen), then bytewise equality.
    nameIs("startsWith"); size_t s12 = a_.jne();
    a_.load(R8, -40); a_.loadMem(RCX, R8, 8);            // pattern
    a_.loadInd(RDX, RCX);                                // n = plen
    a_.xorRR(RSI, RSI);                                  // a = 0
    size_t fixJoin = a_.jmp();
    a_.patchRel(s12, a_.here());
    nameIs("endsWith"); size_t s13 = a_.jne();
    a_.load(R8, -40); a_.loadMem(RCX, R8, 8);            // pattern
    a_.loadInd(RDX, RCX);                                // n = plen
    a_.load(RSI, -16); a_.loadInd(RSI, RSI);             // slen
    a_.subRR(RSI, RDX);                                  // a = slen - plen (neg -> substr yields "")
    a_.patchRel(fixJoin, a_.here());
    a_.load(RDI, -16);
    callFixups_.push_back({a_.call(), -54});             // probe (fresh, freed below)
    a_.store(RAX, -32);
    a_.movRR(RDI, RAX); a_.load(R8, -40); a_.loadMem(RSI, R8, 8);
    callFixups_.push_back({a_.call(), -8});              // str_eq(probe, pattern)
    a_.store(RAX, -48);
    a_.load(RCX, -32); emitStrTempFree(RCX);
    a_.load(RDX, -48); a_.movImm(RAX, 3); a_.leave(); a_.ret();
    a_.patchRel(s13, a_.here());
    a_.xorRR(RAX, RAX); a_.xorRR(RDX, RDX); a_.leave(); a_.ret();
    a_.patchRel(nStr, a_.here());
    // array -----------------------------------------------------------------
    a_.load(RAX, -8); a_.cmpImm(RAX, 6); size_t nArr = a_.jne();
    nameIs("length"); size_t a1 = a_.jne();
    a_.load(RSI, -16); a_.loadInd(RDX, RSI); a_.shlImm(RDX, 1); a_.shrImm(RDX, 1); retInt();  // mask dense marker
    a_.patchRel(a1, a_.here());
    nameIs("at"); size_t a2 = a_.jne();
    a_.load(R8, -40); a_.loadMem(RCX, R8, 8);          // index payload (arg0)
    a_.load(RSI, -16); a_.loadInd(RAX, RSI);           // len
    a_.cmpRR(RCX, RAX); size_t aoob = a_.jge();
    a_.testRR(RCX); size_t aneg = a_.jcc(8);           // js
    a_.movRR(RAX, RCX); a_.shlImm(RAX, 4); a_.load(RSI, -16); a_.addRR(RAX, RSI); a_.addImm(RAX, 8);
    a_.loadMem(RDX, RAX, 8); a_.loadMem(RAX, RAX, 0);
    // §15: a CallDyn result is a TRANSFER (+1) — the buffer's element ref is
    // borrowed, so retain it for the caller (frame exit releases it).
    a_.store(RAX, -32); a_.store(RDX, -48);
    a_.movRR(RDI, RAX); a_.movRR(RSI, RDX);
    callFixups_.push_back({a_.call(), -65});
    a_.load(RAX, -32); a_.load(RDX, -48); a_.leave(); a_.ret();
    a_.patchRel(aoob, a_.here()); a_.patchRel(aneg, a_.here());
    a_.load(R8, -40); a_.loadMem(RDI, R8, 8); a_.load(RSI, -16); a_.loadInd(RSI, RSI);
    callFixups_.push_back({a_.call(), -30});                      // raise_oob
    a_.xorRR(RAX, RAX); a_.xorRR(RDX, RDX); a_.leave(); a_.ret();
    a_.patchRel(a2, a_.here());
    nameIs("add"); size_t a3 = a_.jne();
    a_.load(RDI, -16); a_.load(R8, -40); a_.loadMem(RSI, R8, 0); a_.loadMem(RDX, R8, 8);
    callFixups_.push_back({a_.call(), -25});           // arr_append -> rax
    a_.movRR(RDX, RAX); a_.movImm(RAX, 6); a_.leave(); a_.ret();
    a_.patchRel(a3, a_.here());
    a_.xorRR(RAX, RAX); a_.xorRR(RDX, RDX); a_.leave(); a_.ret();
    a_.patchRel(nArr, a_.here());
    // map -------------------------------------------------------------------
    a_.load(RAX, -8); a_.cmpImm(RAX, 7); size_t nMap = a_.jne();
    nameIs("length"); size_t m1 = a_.jne();
    a_.load(RSI, -16); a_.loadInd(RDX, RSI); retInt();
    a_.patchRel(m1, a_.here());
    nameIs("at"); size_t m2 = a_.jne();                // delegate to idxget (map lookup)
    a_.movImm(RDI, 7); a_.load(RSI, -16);
    a_.load(R8, -40); a_.loadMem(RDX, R8, 0); a_.loadMem(RCX, R8, 8);
    callFixups_.push_back({a_.call(), -22});
    // §15: idxget returns a borrowed entry ref — retain to honor the CallDyn
    // transfer contract (mirrors the array `at` above).
    a_.store(RAX, -32); a_.store(RDX, -48);
    a_.movRR(RDI, RAX); a_.movRR(RSI, RDX);
    callFixups_.push_back({a_.call(), -65});
    a_.load(RAX, -32); a_.load(RDX, -48); a_.leave(); a_.ret();
    a_.patchRel(m2, a_.here());
    nameIs("has"); size_t m3 = a_.jne();               // scan for key -> bool
    // String keys match by CONTENT (str_eq), mirroring keyEquals; i lives in a
    // slot because str_eq clobbers the loop registers.
    a_.xorRR(RAX, RAX); a_.store(RAX, -32);            // i
    size_t hl = a_.here();
    a_.load(RCX, -32); a_.load(RSI, -16); a_.loadInd(R9, RSI);
    a_.cmpRR(RCX, R9); size_t hdone = a_.jge();
    a_.movRR(RAX, RCX); a_.shlImm(RAX, 5); a_.addRR(RAX, RSI); a_.addImm(RAX, 8);
    a_.load(R8, -40);
    a_.loadMem(RDX, RAX, 0); a_.loadMem(RSI, R8, 0); a_.cmpRR(RDX, RSI); size_t hkt = a_.jne();
    a_.cmpImm(RDX, 4); size_t hStr = a_.je();
    a_.loadMem(RDX, RAX, 8); a_.loadMem(RSI, R8, 8); a_.cmpRR(RDX, RSI); size_t hkp = a_.jne();
    size_t hHit = a_.jmp();
    a_.patchRel(hStr, a_.here());
    a_.loadMem(RDI, RAX, 8); a_.loadMem(RSI, R8, 8);
    callFixups_.push_back({a_.call(), -8});            // str_eq(key, probe)
    a_.testRR(RAX); size_t hkp2 = a_.je();
    a_.patchRel(hHit, a_.here());
    a_.movImm(RDX, 1); a_.movImm(RAX, 3); a_.leave(); a_.ret();     // found
    a_.patchRel(hkt, a_.here()); a_.patchRel(hkp, a_.here()); a_.patchRel(hkp2, a_.here());
    a_.load(RCX, -32); a_.addImm(RCX, 1); a_.store(RCX, -32);
    size_t hlb = a_.jmp(); a_.patchRel(hlb, hl);
    a_.patchRel(hdone, a_.here());
    a_.xorRR(RDX, RDX); a_.movImm(RAX, 3); a_.leave(); a_.ret();    // not found
    a_.patchRel(m3, a_.here());
    nameIs("keys"); size_t m4 = a_.jne();
    // fallthrough handled below with values via a shared builder
    a_.movImm(RDI, 0); size_t doKeys = a_.jmp();       // keys: offset 0 in entry
    a_.patchRel(m4, a_.here());
    nameIs("values"); size_t m5 = a_.jne();
    a_.movImm(RDI, 16);                                 // values: offset 16 in entry
    a_.patchRel(doKeys, a_.here());
    // build array from map entries at [entry + rdi]
    a_.store(RDI, -32);                                 // field offset (0 keys / 16 values)
    a_.load(RSI, -16); a_.loadInd(RDI, RSI);
    callFixups_.push_back({a_.call(), -18}); a_.store(RAX, -48);    // mkarr(len)  (frame has room: -48)
    a_.load(RSI, -16); a_.loadInd(R9, RSI); a_.xorRR(RCX, RCX);     // len, i
    size_t kl = a_.here();
    a_.cmpRR(RCX, R9); size_t kdone = a_.jge();
    a_.movRR(RAX, RCX); a_.shlImm(RAX, 5); a_.load(RSI, -16); a_.addRR(RAX, RSI); a_.addImm(RAX, 8);
    a_.load(RDX, -32); a_.addRR(RAX, RDX);             // entry + fieldoff
    a_.loadMem(RSI, RAX, 0); a_.loadMem(RDX, RAX, 8);  // element tag, pay
    // dst = arr+8+16*i
    a_.movRR(RAX, RCX); a_.shlImm(RAX, 4); a_.load(RDI, -48); a_.addRR(RAX, RDI); a_.addImm(RAX, 8);
    a_.storeMem(RAX, 0, RSI); a_.storeMem(RAX, 8, RDX);
    a_.addImm(RCX, 1); size_t klb = a_.jmp(); a_.patchRel(klb, kl);
    a_.patchRel(kdone, a_.here());
    // §15: honor the CallDyn transfer contract — the built array is owned (+1)
    // and owns each element ref it copied out of the map entries (retain each).
    a_.load(RAX, -48); a_.movImm(RCX, 1); a_.storeMem(RAX, -16, RCX);
    a_.xorRR(RCX, RCX); a_.store(RCX, -32);                        // i = 0 (fieldoff done)
    size_t krl = a_.here();
    a_.load(RCX, -32); a_.load(RSI, -48); a_.loadInd(RAX, RSI); a_.cmpRR(RCX, RAX); size_t krdone = a_.jge();
    a_.load(RAX, -32); a_.shlImm(RAX, 4); a_.load(RSI, -48); a_.addRR(RAX, RSI); a_.addImm(RAX, 8);
    a_.loadMem(RDI, RAX, 0); a_.loadMem(RSI, RAX, 8);
    callFixups_.push_back({a_.call(), -65});                       // retain element
    a_.load(RCX, -32); a_.addImm(RCX, 1); a_.store(RCX, -32);
    size_t krb = a_.jmp(); a_.patchRel(krb, krl);
    a_.patchRel(krdone, a_.here());
    a_.load(RDX, -48); a_.movImm(RAX, 6); a_.leave(); a_.ret();
    a_.patchRel(m5, a_.here());
    a_.xorRR(RAX, RAX); a_.xorRR(RDX, RDX); a_.leave(); a_.ret();
    a_.patchRel(nMap, a_.here());
    // int / bool toString ---------------------------------------------------
    a_.load(RAX, -8); a_.cmpImm(RAX, 1); size_t nInt = a_.jne();
    nameIs("toString"); size_t i1 = a_.jne();
    a_.load(RDI, -16); callFixups_.push_back({a_.call(), -5});
    retStrOwned();                                       // §15: fresh int_to_str -> +1
    a_.patchRel(i1, a_.here());
    // Track 06: int.toFloat() — exact cvtsi2sd, no coverage split needed.
    nameIs("toFloat"); size_t i2 = a_.jne();
    a_.load(RDX, -16); a_.cvtsi2sd(0, RDX); a_.movqRXmm(RDX, 0);
    a_.movImm(RAX, 2); a_.leave(); a_.ret();
    a_.patchRel(i2, a_.here());
    a_.xorRR(RAX, RAX); a_.xorRR(RDX, RDX); a_.leave(); a_.ret();
    a_.patchRel(nInt, a_.here());
    a_.load(RAX, -8); a_.cmpImm(RAX, 3); size_t nBool = a_.jne();
    nameIs("toString"); size_t b1 = a_.jne();
    a_.load(RSI, -16); a_.testRR(RSI); size_t bf = a_.je();
    addrImm(RDX, internString("true")); a_.movImm(RAX, 4); a_.leave(); a_.ret();
    a_.patchRel(bf, a_.here());
    addrImm(RDX, internString("false")); a_.movImm(RAX, 4); a_.leave(); a_.ret();
    a_.patchRel(b1, a_.here());
    a_.xorRR(RAX, RAX); a_.xorRR(RDX, RDX); a_.leave(); a_.ret();
    a_.patchRel(nBool, a_.here());
    // float toString --------------------------------------------------------
    a_.load(RAX, -8); a_.cmpImm(RAX, 2); size_t nFloat = a_.jne();
    nameIs("toString"); size_t f1 = a_.jne();
    a_.load(RDI, -16); callFixups_.push_back({a_.call(), -72});
    retStrOwned();                                       // §15: fresh string -> +1
    a_.patchRel(f1, a_.here());
    // Track 06: floor/ceil/trunc are a single roundsd each (imm 1/2/3 — the
    // hardware has no half-away-from-zero mode, so `round` alone composes it
    // from trunc + a copysign(0.5,x) pre-add, problem #1 of the design).
    auto retFloatXmm0 = [&]() { a_.movqRXmm(RDX, 0); a_.movImm(RAX, 2); a_.leave(); a_.ret(); };
    nameIs("floor"); size_t ff1 = a_.jne();
    a_.load(RDX, -16); a_.movqXmmR(0, RDX); a_.roundsd(0, 0, 0x1); retFloatXmm0();
    a_.patchRel(ff1, a_.here());
    nameIs("ceil"); size_t ff2 = a_.jne();
    a_.load(RDX, -16); a_.movqXmmR(0, RDX); a_.roundsd(0, 0, 0x2); retFloatXmm0();
    a_.patchRel(ff2, a_.here());
    nameIs("trunc"); size_t ff3 = a_.jne();
    a_.load(RDX, -16); a_.movqXmmR(0, RDX); a_.roundsd(0, 0, 0x3); retFloatXmm0();
    a_.patchRel(ff3, a_.here());
    nameIs("round"); size_t ff4 = a_.jne();
    a_.load(RDX, -16); a_.movqXmmR(0, RDX);         // xmm0 = x
    a_.movRR(RAX, RDX);
    a_.movImm(RCX, 0x8000000000000000ULL); a_.andRR(RAX, RCX);    // sign bit of x
    a_.movImm(RCX, 0x3FE0000000000000ULL); a_.orRR(RAX, RCX);     // copysign(0.5, x)
    a_.movqXmmR(1, RAX); a_.addsd(0, 1);            // x + copysign(0.5, x)
    a_.roundsd(0, 0, 0x3);                          // trunc toward zero
    retFloatXmm0();
    a_.patchRel(ff4, a_.here());
    nameIs("sqrt"); size_t ff5 = a_.jne();
    a_.load(RDX, -16); a_.movqXmmR(0, RDX); a_.sqrtsd(0, 0); retFloatXmm0();   // neg -> NaN (IEEE)
    a_.patchRel(ff5, a_.here());
    // toInt(): truncation; NaN/±inf/out-of-int64-range -> RuntimeException
    // (loud). Range-checked via comisd against the exact int64 double bounds
    // BEFORE the convert — cvttsd2si's own "integer indefinite" sentinel
    // (INT64_MIN) is ambiguous with a genuinely-valid -2^63 input, and
    // comisd's unordered result (CF=1) rejects NaN for free, same trick as
    // the in-language isInfinite()/isNaN() reformulation (see kPrelude).
    nameIs("toInt"); size_t ff6 = a_.jne();
    a_.load(RDX, -16); a_.movqXmmR(0, RDX);
    a_.movImm(RAX, 0xC3E0000000000000ULL); a_.movqXmmR(1, RAX);   // -2^63
    a_.comisd(0, 1); size_t lowOk = a_.jcc(0x3);                   // jae: x>=-2^63 (ordered)
    addrImm(RDI, internString("float is not finite or out of int64 range for toInt()"));
    callFixups_.push_back({a_.call(), -29});                      // raise
    a_.xorRR(RAX, RAX); a_.xorRR(RDX, RDX); a_.leave(); a_.ret();
    a_.patchRel(lowOk, a_.here());
    a_.movImm(RAX, 0x43E0000000000000ULL); a_.movqXmmR(2, RAX);   // 2^63
    a_.comisd(0, 2); size_t highOk = a_.jcc(0x2);                  // jb: x<2^63
    addrImm(RDI, internString("float is not finite or out of int64 range for toInt()"));
    callFixups_.push_back({a_.call(), -29});
    a_.xorRR(RAX, RAX); a_.xorRR(RDX, RDX); a_.leave(); a_.ret();
    a_.patchRel(highOk, a_.here());
    a_.cvttsd2si(RAX, 0); a_.movRR(RDX, RAX); a_.movImm(RAX, 1); a_.leave(); a_.ret();
    a_.patchRel(ff6, a_.here());
    a_.xorRR(RAX, RAX); a_.xorRR(RDX, RDX); a_.leave(); a_.ret();
    a_.patchRel(nFloat, a_.here());
    a_.xorRR(RAX, RAX); a_.xorRR(RDX, RDX); a_.leave(); a_.ret();
}

// callm(rdi=recv_tag, rsi=recv_pay, rdx=nameptr, rcx=argc; args at [rbp+16+16k])
// -> rax,rdx. Name-based dynamic dispatch: in-language methods of the receiver's
// class, else the native cores.
void X64Gen::genCallM(const std::vector<Symbol*>& classes) {
    callmOff_ = a_.here();
    a_.push(RBP); a_.movRR(RBP, RSP); a_.subImm(RSP, 48);
    a_.store(RDI, -8); a_.store(RSI, -16); a_.store(RDX, -24); a_.store(RCX, -32);
    // classId -> slot -40 (survives across the per-class blocks)
    a_.movRR(RAX, RDI); a_.cmpImm(RAX, 5); size_t c5 = a_.jne();
    a_.loadInd(RAX, RSI); size_t have = a_.jmp();
    a_.patchRel(c5, a_.here());
    a_.xorRR(RAX, RAX);                                 // unknown -> 0
    std::vector<size_t> haves;
    auto tagToId = [&](int tag, const char* clsName) {
        a_.movRR(RCX, RDI); a_.cmpImm(RCX, tag); size_t jn = a_.jne();
        a_.movImm(RAX, (uint64_t)lookupClsId(clsName)); haves.push_back(a_.jmp());
        a_.patchRel(jn, a_.here());
    };
    tagToId(6, "Array"); tagToId(7, "Map"); tagToId(4, "string");
    tagToId(1, "int"); tagToId(3, "bool"); tagToId(2, "float");
    a_.patchRel(have, a_.here());
    for (size_t h : haves) a_.patchRel(h, a_.here());
    a_.store(RAX, -40);                                 // classId
    for (Symbol* cls : classes) {
        std::vector<const Stmt*> methods;
        std::vector<const Stmt*> mem; collectMembers(cls, mem);
        for (const Stmt* m : mem) {
            if (!m->callable || m->isCtor || m->isGet || m->isSet || m->selector.symbolic) continue;
            if (!mod_.byDecl.count(m)) continue;
            int fi = mod_.byDecl.at(m);
            if (fi >= (int)reachable_.size() || !reachable_[fi]) continue;
            methods.push_back(m);
        }
        if (methods.empty()) continue;
        a_.load(RAX, -40); a_.cmpImm(RAX, clsId(cls)); size_t jNext = a_.jne();
        for (const Stmt* m : methods) {
            int fi = mod_.byDecl.at(m);
            int np = mod_.functions[fi].nparams;         // incl. receiver
            a_.load(RDX, -24); addrImm(R9, internString(std::string(m->name)));
            a_.cmpRR(RDX, R9); size_t jSkip = a_.jne();
            for (int k = np - 2; k >= 0; --k) {          // forward args (right-to-left)
                a_.load(RAX, 16 + 16 * k + 8); a_.push(RAX);
                a_.load(RAX, 16 + 16 * k);     a_.push(RAX);
            }
            a_.load(RAX, -16); a_.push(RAX);             // recv payload
            a_.load(RAX, -8);  a_.push(RAX);             // recv tag
            callFixups_.push_back({a_.call(), fi});
            a_.addImm(RSP, 16 * np);
            a_.leave(); a_.ret();
            a_.patchRel(jSkip, a_.here());
        }
        a_.patchRel(jNext, a_.here());
    }
    // native fallback
    a_.load(RDI, -8); a_.load(RSI, -16); a_.load(RDX, -24); a_.load(RCX, -32);
    a_.lea(R8, RBP, 16);                                // argptr = &args
    callFixups_.push_back({a_.call(), -21});           // callnative
    a_.leave(); a_.ret();
}

// callclosure(rdi=clo_pay, rcx=argc; args at [rbp+16+16k]) -> rax,rdx. Dispatch
// on the closure's function id: call f<id>(closure, args...).
void X64Gen::genCallClosure() {
    callClosureOff_ = a_.here();
    a_.push(RBP); a_.movRR(RBP, RSP); a_.subImm(RSP, 16);
    a_.store(RDI, -8);                                  // clo_pay
    a_.loadInd(RAX, RDI);                               // fnId = [clo]
    for (int fi : closureFns_) {
        int np = mod_.functions[fi].nparams;            // incl. the closure (r0)
        a_.cmpImm(RAX, fi); size_t jn = a_.jne();
        for (int k = np - 2; k >= 0; --k) {             // forward args
            a_.load(RDX, 16 + 16 * k + 8); a_.push(RDX);
            a_.load(RDX, 16 + 16 * k);     a_.push(RDX);
        }
        a_.load(RDX, -8); a_.push(RDX);                 // closure payload
        a_.movImm(RDX, 9); a_.push(RDX);                // closure tag
        callFixups_.push_back({a_.call(), fi});
        a_.addImm(RSP, 16 * np);
        a_.leave(); a_.ret();
        a_.patchRel(jn, a_.here());
        a_.loadInd(RAX, RDI);                            // reload fnId (rdi preserved)
    }
    a_.xorRR(RAX, RAX); a_.xorRR(RDX, RDX); a_.leave(); a_.ret();
}

// issub(rdi=a, rsi=b) -> rax: is class a the same as, or a subclass/implementor
// of, b (bases include interfaces). Generated from the class hierarchy.
void X64Gen::genIsSub() {
    issubOff_ = a_.here();
    a_.push(RBP); a_.movRR(RBP, RSP); a_.subImm(RSP, 16);
    a_.store(RDI, -8); a_.store(RSI, -16);
    a_.cmpRR(RDI, RSI); size_t ne = a_.jne();
    a_.movImm(RAX, 1); a_.leave(); a_.ret();
    a_.patchRel(ne, a_.here());
    for (const auto& [sym, id] : clsId_) {
        if (!sym->decl || sym->decl->bases.empty()) continue;
        a_.load(RAX, -8); a_.cmpImm(RAX, id); size_t skip = a_.jne();
        for (const TypeRefPtr& b : sym->decl->bases) {
            if (!b->resolvedSymbol) continue;
            a_.movImm(RDI, (uint64_t)clsId(b->resolvedSymbol)); a_.load(RSI, -16);
            callFixups_.push_back({a_.call(), -28});   // issub(base, b)
            a_.testRR(RAX); size_t jz = a_.je();
            a_.movImm(RAX, 1); a_.leave(); a_.ret();
            a_.patchRel(jz, a_.here());
        }
        a_.xorRR(RAX, RAX); a_.leave(); a_.ret();
        a_.patchRel(skip, a_.here());
    }
    a_.xorRR(RAX, RAX); a_.leave(); a_.ret();
}

// raise(rdi=msgptr): throw a fresh RuntimeException carrying the message
// (used by the native cores for runtime errors, e.g. array bounds).
void X64Gen::genRaise() {
    raiseOff_ = a_.here();
    a_.push(RBP); a_.movRR(RBP, RSP); a_.subImm(RSP, 16);
    a_.store(RDI, -8);                                  // msgptr
    a_.movImm(RDI, (uint64_t)lookupClsId("RuntimeException"));
    callFixups_.push_back({a_.call(), -10});           // mkobj -> rax
    a_.store(RAX, -16);                                 // exc
    a_.movRR(RDI, RAX); addrImm(RSI, internString("message"));
    a_.movImm(RDX, 4); a_.load(RCX, -8);
    callFixups_.push_back({a_.call(), -12});           // setfield message
    // §15 strings tier: the field OWNS the message (raw setfield bypasses the
    // SetMember hook) — retain it so the exception's recursiveFree balances.
    a_.movImm(RDI, 4); a_.load(RSI, -8);
    callFixups_.push_back({a_.call(), -65});
    addrImm(RCX, 24); a_.movImm(RAX, 1); a_.storeInd(RCX, RAX);   // g_throwing = 1
    addrImm(RCX, 32); a_.movImm(RAX, 5); a_.storeInd(RCX, RAX);   // g_thrown tag
    addrImm(RCX, 40); a_.load(RAX, -16); a_.storeInd(RCX, RAX);   // g_thrown pay
    // §15: g_thrown owns the raised exception (+1), consumed by the catch
    // bind's frame-exit release (mirrors Op::Throw).
    a_.movImm(RDI, 5); a_.load(RSI, -16);
    callFixups_.push_back({a_.call(), -65});
    a_.leave(); a_.ret();
}

// raise_oob(rdi=index, rsi=length): raise "index N out of bounds (length M)".
void X64Gen::genRaiseOob() {
    raiseOobOff_ = a_.here();
    a_.push(RBP); a_.movRR(RBP, RSP); a_.subImm(RSP, 16);
    a_.store(RSI, -8);                                  // length
    callFixups_.push_back({a_.call(), -5});            // int_to_str(index) -> rax
    a_.movRR(RSI, RAX); addrImm(RDI, internString("index "));
    callFixups_.push_back({a_.call(), -6}); a_.store(RAX, -16);   // "index N"
    a_.load(RDI, -16); addrImm(RSI, internString(" out of bounds (length "));
    callFixups_.push_back({a_.call(), -6}); a_.store(RAX, -16);
    a_.load(RDI, -8); callFixups_.push_back({a_.call(), -5}); a_.movRR(RSI, RAX);
    a_.load(RDI, -16); callFixups_.push_back({a_.call(), -6}); a_.store(RAX, -16);
    a_.load(RDI, -16); addrImm(RSI, internString(")"));
    callFixups_.push_back({a_.call(), -6});
    a_.movRR(RDI, RAX); callFixups_.push_back({a_.call(), -29});   // raise
    a_.leave(); a_.ret();
}

// uncaught(): if a throw propagated to the top, report "Uncaught <cls>: <msg>".
void X64Gen::genUncaught() {
    uncaughtOff_ = a_.here();
    a_.push(RBP); a_.movRR(RBP, RSP); a_.subImm(RSP, 32);
    addrImm(RCX, 24); a_.loadInd(RAX, RCX); a_.testRR(RAX); size_t done = a_.je();
    // class name -> -8
    addrImm(RCX, 32); a_.loadInd(RAX, RCX); a_.cmpImm(RAX, 5); size_t valcase = a_.jne();
    addrImm(RCX, 40); a_.loadInd(RAX, RCX); a_.loadInd(RAX, RAX);   // classId
    std::vector<size_t> named;
    for (const auto& [sym, id] : clsId_) {
        a_.cmpImm(RAX, id); size_t skip = a_.jne();
        addrImm(RAX, internString(std::string(sym->name))); named.push_back(a_.jmp());
        a_.patchRel(skip, a_.here());
    }
    addrImm(RAX, internString("object")); size_t j0 = a_.jmp();
    a_.patchRel(valcase, a_.here());
    addrImm(RAX, internString("value"));
    a_.patchRel(j0, a_.here());
    for (size_t n : named) a_.patchRel(n, a_.here());
    a_.store(RAX, -8);                                  // clsname
    // message -> -16 (empty if absent)
    addrImm(RCX, 32); a_.loadInd(RAX, RCX); a_.cmpImm(RAX, 5); size_t nomsg = a_.jne();
    addrImm(RCX, 40); a_.loadInd(RDI, RCX); addrImm(RSI, internString("message"));
    callFixups_.push_back({a_.call(), -11});           // getfield
    a_.cmpImm(RAX, 4); size_t nomsg2 = a_.jne();
    a_.store(RDX, -16); size_t hasmsg = a_.jmp();
    a_.patchRel(nomsg, a_.here()); a_.patchRel(nomsg2, a_.here());
    a_.movImm(RAX, 16); a_.store(RAX, -16);            // empty string
    a_.patchRel(hasmsg, a_.here());
    // acc = "Uncaught " + clsname
    addrImm(RDI, internString("Uncaught ")); a_.load(RSI, -8);
    callFixups_.push_back({a_.call(), -6}); a_.store(RAX, -24);
    // if message length > 0: acc += ": " + msg
    a_.load(RCX, -16); a_.loadInd(RAX, RCX); a_.testRR(RAX); size_t skipmsg = a_.je();
    a_.load(RDI, -24); addrImm(RSI, internString(": "));
    callFixups_.push_back({a_.call(), -6}); a_.store(RAX, -24);
    a_.load(RDI, -24); a_.load(RSI, -16);
    callFixups_.push_back({a_.call(), -6}); a_.store(RAX, -24);
    a_.patchRel(skipmsg, a_.here());
    // acc += "\n"; print; drop the rendered line (§15: it is an unowned temp)
    a_.load(RDI, -24); addrImm(RSI, internString("\n"));
    callFixups_.push_back({a_.call(), -6});
    a_.store(RAX, -24);
    a_.movRR(RDI, RAX); callFixups_.push_back({a_.call(), -7});   // print_str
    a_.load(RCX, -24); emitStrTempFree(RCX);
    a_.patchRel(done, a_.here());
    a_.leave(); a_.ret();
}

// ---------------------------------------------------------------------------
//  Step 3: the std::sys floor as machine-code helpers issuing real Linux
//  syscalls (x86-64 numbers). Strings are our heap descriptors [len][bytes];
//  paths are copied to NUL-terminated C strings for the path-taking calls.
//  The reference semantics are the tree-walk interpreter (RuntimeNatives),
//  which does real I/O — the ELF output is diffed against `lang --run`.
// ---------------------------------------------------------------------------

// cstr(rdi=strptr) -> rax = NUL-terminated copy on the heap.
void X64Gen::genCstr() {
    cstrOff_ = a_.here();
    a_.push(RBP); a_.movRR(RBP, RSP); a_.subImm(RSP, 16);
    a_.store(RDI, -8);
    a_.loadInd(RCX, RDI); a_.movRR(RDI, RCX); a_.addImm(RDI, 1);   // len+1
    callFixups_.push_back({a_.call(), -69});                       // halloc: prefixed so callers can hfree it
    a_.store(RAX, -16);                                            // dst
    a_.load(RSI, -8); a_.loadInd(RCX, RSI); a_.addImm(RSI, 8);     // src bytes, len
    a_.movRR(RDI, RAX); a_.repMovsb();                             // copy len bytes
    a_.movImm(RCX, 0); a_.u8(0x88); a_.u8(0x0F);                   // mov [rdi], cl (NUL)
    a_.load(RAX, -16); a_.leave(); a_.ret();
}

// sysWrite(rdi=fd, rsi=strptr) -> rax = bytes written.
void X64Gen::genSysWrite() {
    sysWriteOff_ = a_.here();
    a_.loadInd(RDX, RSI);                       // len
    a_.addImm(RSI, 8);                          // buf
    a_.movImm(RAX, 1);                          // write
    a_.syscall_();
    a_.ret();
}

// sysReadLine(rdi=fd) -> rax = string (line without the newline; "" at EOF).
void X64Gen::genSysReadLine() {
    sysReadLineOff_ = a_.here();
    a_.push(RBP); a_.movRR(RBP, RSP); a_.subImm(RSP, 4128);
    a_.store(RDI, -8);                          // fd
    a_.xorRR(RAX, RAX); a_.store(RAX, -16);     // count
    size_t loop = a_.here();
    a_.xorRR(RAX, RAX); a_.store(RAX, -24);     // zero the byte slot (read fills low byte only)
    a_.movImm(RAX, 0); a_.load(RDI, -8); a_.lea(RSI, RBP, -24); a_.movImm(RDX, 1);
    a_.syscall_();                              // read(fd, &c, 1) -> rax
    a_.testRR(RAX); size_t eof = a_.jle();      // <=0 -> EOF/end
    a_.load(RAX, -24); a_.cmpImm(RAX, 10); size_t nl = a_.je();   // '\n'
    a_.load(RCX, -16); a_.cmpImm(RCX, 4096); size_t full = a_.jge();
    a_.lea(RDX, RBP, -4128); a_.addRR(RDX, RCX);
    a_.load(RAX, -24); a_.u8(0x88); a_.u8(0x02);   // mov [rdx], al
    a_.patchRel(full, a_.here());
    a_.load(RCX, -16); a_.addImm(RCX, 1); a_.store(RCX, -16);
    size_t back = a_.jmp(); a_.patchRel(back, loop);
    a_.patchRel(eof, a_.here()); a_.patchRel(nl, a_.here());
    a_.load(RDI, -16); a_.addImm(RDI, 8); callFixups_.push_back({a_.call(), -69});   // halloc (§15 prefixed)
    a_.load(RCX, -16); a_.storeInd(RAX, RCX);      // [ptr] = count
    a_.store(RAX, -32);
    a_.lea(RSI, RBP, -4128); a_.movRR(RDI, RAX); a_.addImm(RDI, 8);
    a_.load(RCX, -16); a_.repMovsb();
    a_.load(RAX, -32); a_.leave(); a_.ret();
}

// sysRead(rdi=fd, rsi=max) -> rax = string of the bytes actually read.
void X64Gen::genSysRead() {
    sysReadOff_ = a_.here();
    a_.push(RBP); a_.movRR(RBP, RSP); a_.subImm(RSP, 16);
    a_.store(RDI, -8); a_.store(RSI, -16);
    a_.movRR(RDI, RSI); a_.addImm(RDI, 8); callFixups_.push_back({a_.call(), -69});  // halloc(max+8) (§15 prefixed)
    a_.load(RDI, -8); a_.movRR(RSI, RAX); a_.addImm(RSI, 8); a_.load(RDX, -16);
    a_.push(RAX);                               // save descriptor
    a_.movImm(RAX, 0); a_.syscall_();           // read -> rax = n
    a_.testRR(RAX); size_t ok = a_.jcc(9); a_.xorRR(RAX, RAX); a_.patchRel(ok, a_.here());  // n<0 -> 0
    a_.pop(RCX); a_.storeInd(RCX, RAX);         // [ptr] = n
    a_.movRR(RAX, RCX); a_.leave(); a_.ret();
}

// sysOpen(rdi=pathstr, rsi=flagbits: 1 read 2 write 4 append) -> rax = fd.
void X64Gen::genSysOpen() {
    sysOpenOff_ = a_.here();
    a_.push(RBP); a_.movRR(RBP, RSP); a_.subImm(RSP, 16);
    a_.store(RSI, -16);                         // flagbits
    callFixups_.push_back({a_.call(), -32});    // cstr(path) -> rax
    a_.store(RAX, -8);                          // cpath
    // compute open flags from bits: O_RDONLY=0 WRONLY=1 RDWR=2 CREAT=0x40
    //   TRUNC=0x200 APPEND=0x400
    a_.load(RSI, -16);
    a_.movRR(RAX, RSI); a_.andRR(RAX, RAX);     // (rax = bits) nop keep
    a_.movRR(RCX, RSI); a_.movImm(RDX, 3); a_.andRR(RCX, RDX);   // rd|wr in low 2 bits
    // default flags -> rdx
    a_.movImm(RDX, 0);                          // O_RDONLY
    a_.movRR(RAX, RSI); a_.movImm(R8, 3); a_.andRR(RAX, R8); a_.cmpImm(RAX, 3);
    size_t notrw = a_.jne();
    a_.movImm(RDX, 0x42);                       // O_RDWR|O_CREAT
    size_t haveflags = a_.jmp();
    a_.patchRel(notrw, a_.here());
    a_.movRR(RAX, RSI); a_.movImm(R8, 2); a_.andRR(RAX, R8); a_.testRR(RAX);
    size_t notw = a_.je();
    a_.movImm(RDX, 0x241);                      // O_WRONLY|O_CREAT|O_TRUNC
    a_.patchRel(notw, a_.here());
    a_.patchRel(haveflags, a_.here());
    // append: if bits&4 -> flags = O_APPEND|O_CREAT | (rd? O_RDWR:O_WRONLY)
    a_.movRR(RAX, RSI); a_.movImm(R8, 4); a_.andRR(RAX, R8); a_.testRR(RAX);
    size_t noap = a_.je();
    a_.movRR(RAX, RSI); a_.movImm(R8, 1); a_.andRR(RAX, R8); a_.testRR(RAX);
    size_t apRd = a_.jne();
    a_.movImm(RDX, 0x441);                      // O_APPEND|O_CREAT|O_WRONLY
    size_t apDone = a_.jmp();
    a_.patchRel(apRd, a_.here());
    a_.movImm(RDX, 0x442);                      // O_APPEND|O_CREAT|O_RDWR
    a_.patchRel(apDone, a_.here());
    a_.patchRel(noap, a_.here());
    a_.load(RDI, -8);                           // cpath
    a_.movRR(RSI, RDX);                         // flags
    a_.movImm(RDX, 0x1A4);                      // mode 0644
    a_.movImm(RAX, 2); a_.syscall_();           // open -> rax = fd
    a_.store(RAX, -16);                          // fd (flagbits slot is dead)
    a_.load(RCX, -8); emitStrTempFree(RCX);      // §15: drop the transient C-path copy
    a_.load(RAX, -16);
    a_.leave(); a_.ret();
}

// sysClose(rdi=fd) -> rax.
void X64Gen::genSysClose() {
    sysCloseOff_ = a_.here();
    a_.movImm(RAX, 3); a_.syscall_(); a_.ret();
}

// sysStat(rdi=pathstr, rsi=field: 0 exists, 1 size, 2 mtime) -> rax.
void X64Gen::genSysStat() {
    sysStatOff_ = a_.here();
    a_.push(RBP); a_.movRR(RBP, RSP); a_.subImm(RSP, 160);
    a_.store(RSI, -8);                          // field
    callFixups_.push_back({a_.call(), -32});    // cstr(path) -> rax
    a_.movRR(RDI, RAX); a_.lea(RSI, RBP, -160); // &statbuf
    a_.movImm(RAX, 4); a_.syscall_();           // stat -> rax (0 ok)
    a_.store(RAX, -16);                          // stat rc
    a_.movRR(RCX, RDI);                          // cpath survives the syscall (only rax/rcx/r11 clobbered)
    emitStrTempFree(RCX);                        // §15: drop the transient C-path copy
    a_.load(RCX, -8); a_.testRR(RCX); size_t notField0 = a_.jne();
    a_.load(RAX, -16); a_.testRR(RAX); a_.setccAx(0x94);   // exists = (rc==0)
    a_.leave(); a_.ret();
    a_.patchRel(notField0, a_.here());
    a_.load(RAX, -16); a_.testRR(RAX); size_t okStat = a_.je();
    a_.movImm(RAX, (uint64_t)-1); a_.leave(); a_.ret();     // missing -> -1
    a_.patchRel(okStat, a_.here());
    a_.load(RCX, -8); a_.cmpImm(RCX, 1); size_t notSize = a_.jne();
    a_.loadMem(RAX, RBP, -160 + 48); a_.leave(); a_.ret();  // st_size
    a_.patchRel(notSize, a_.here());
    a_.load(RCX, -8); a_.cmpImm(RCX, 2); size_t notMtime = a_.jne();
    a_.loadMem(RAX, RBP, -160 + 88); a_.leave(); a_.ret();  // st_mtim.tv_sec
    a_.patchRel(notMtime, a_.here());
    a_.movImm(RAX, (uint64_t)-1); a_.leave(); a_.ret();
}

// ---------------------------------------------------------------------------
//  Step 3b: the event loop. A timer registry lives in the data segment; the
//  loop (run after @main, and pumped by await) sleeps until the earliest timer
//  is due, then fires due timers in (due, id) order — invoking their stored
//  callback closure with the tick number — re-arming intervals and dropping
//  one-shots. Mirrors RuntimeLoop; time via clock_gettime(MONOTONIC), sleep
//  via nanosleep.
// ---------------------------------------------------------------------------

// now_ns() -> rax = CLOCK_MONOTONIC in nanoseconds.
void X64Gen::genNowNs() {
    nowNsOff_ = a_.here();
    a_.push(RBP); a_.movRR(RBP, RSP); a_.subImm(RSP, 16);
    a_.movImm(RDI, 1); a_.lea(RSI, RBP, -16); a_.movImm(RAX, 228); a_.syscall_();
    a_.load(RAX, -16); a_.movImm(RCX, 1000000000); a_.imulRR(RAX, RCX);
    a_.load(RCX, -8); a_.addRR(RAX, RCX);
    a_.leave(); a_.ret();
}

// timer_add(rdi=delayMs, rsi=intervalMs, rdx=cb_tag, rcx=cb_pay) -> rax=id.
void X64Gen::genTimerAdd() {
    timerAddOff_ = a_.here();
    a_.push(RBP); a_.movRR(RBP, RSP); a_.subImm(RSP, 48);
    a_.store(RDI, -8); a_.store(RSI, -16); a_.store(RDX, -24); a_.store(RCX, -32);
    callFixups_.push_back({a_.call(), -39});               // now_ns
    a_.load(RCX, -8); a_.movImm(RDX, 1000000); a_.imulRR(RCX, RDX); a_.addRR(RAX, RCX);
    a_.store(RAX, -40);                                    // due_ns
    addrImm(RDI, loopBase_ + 0); a_.loadInd(R8, RDI);      // id = nextId
    a_.movRR(RAX, R8); a_.addImm(RAX, 1); a_.storeInd(RDI, RAX);
    addrImm(RDI, loopBase_ + 16);                          // timers base
    a_.xorRR(RCX, RCX);
    size_t find = a_.here();
    a_.cmpImm(RCX, kMaxTimers); size_t full = a_.jge();
    a_.movRR(RAX, RCX); a_.movImm(RDX, kTimerRec); a_.imulRR(RAX, RDX); a_.addRR(RAX, RDI);
    a_.loadMem(RDX, RAX, 0); a_.testRR(RDX); size_t got = a_.je();
    a_.addImm(RCX, 1); size_t fb = a_.jmp(); a_.patchRel(fb, find);
    a_.patchRel(got, a_.here());
    a_.movImm(RDX, 1); a_.storeMem(RAX, 0, RDX);           // active
    a_.storeMem(RAX, 8, R8);                               // id
    a_.load(RDX, -40); a_.storeMem(RAX, 16, RDX);          // due
    a_.load(RDX, -16); a_.storeMem(RAX, 24, RDX);          // interval
    a_.xorRR(RDX, RDX); a_.storeMem(RAX, 32, RDX);         // ticks
    a_.load(RDX, -24); a_.storeMem(RAX, 40, RDX);          // cb_tag
    a_.load(RDX, -32); a_.storeMem(RAX, 48, RDX);          // cb_pay
    // §15: the registry owns the callback until cancel / one-shot completion —
    // the registering frame's ref dies with that frame.
    a_.push(R8);
    a_.load(RDI, -24); a_.load(RSI, -32);
    callFixups_.push_back({a_.call(), -65});
    a_.pop(R8);
    a_.patchRel(full, a_.here());
    a_.movRR(RAX, R8); a_.leave(); a_.ret();
}

// timer_cancel(rdi=id): deactivate the matching timer.
void X64Gen::genTimerCancel() {
    timerCancelOff_ = a_.here();
    addrImm(RSI, loopBase_ + 16); a_.xorRR(RCX, RCX);
    size_t loop = a_.here();
    a_.cmpImm(RCX, kMaxTimers); size_t done = a_.jge();
    a_.movRR(RAX, RCX); a_.movImm(RDX, kTimerRec); a_.imulRR(RAX, RDX); a_.addRR(RAX, RSI);
    a_.loadMem(RDX, RAX, 0); a_.testRR(RDX); size_t nx = a_.je();
    a_.loadMem(RDX, RAX, 8); a_.cmpRR(RDX, RDI); size_t nx2 = a_.jne();
    a_.xorRR(RDX, RDX); a_.storeMem(RAX, 0, RDX);          // active = 0
    // §15: drop the registry's callback ref (retained by timer_add)
    a_.loadMem(RDI, RAX, 40); a_.loadMem(RSI, RAX, 48);
    a_.xorRR(RDX, RDX); a_.storeMem(RAX, 40, RDX); a_.storeMem(RAX, 48, RDX);
    callFixups_.push_back({a_.call(), -66});
    a_.ret();
    a_.patchRel(nx, a_.here()); a_.patchRel(nx2, a_.here());
    a_.addImm(RCX, 1); size_t b = a_.jmp(); a_.patchRel(b, loop);
    a_.patchRel(done, a_.here());
    a_.ret();
}

// has_work() -> rax = 1 if any timer or fd-watch is active.
void X64Gen::genHasWork() {
    hasWorkOff_ = a_.here();
    addrImm(RSI, loopBase_ + 16); a_.xorRR(RCX, RCX);
    size_t tl = a_.here();
    a_.cmpImm(RCX, kMaxTimers); size_t chkW = a_.jge();
    a_.movRR(RAX, RCX); a_.movImm(RDX, kTimerRec); a_.imulRR(RAX, RDX); a_.addRR(RAX, RSI);
    a_.loadMem(RDX, RAX, 0); a_.testRR(RDX); size_t yes = a_.jne();
    a_.addImm(RCX, 1); size_t tb = a_.jmp(); a_.patchRel(tb, tl);
    a_.patchRel(chkW, a_.here());
    addrImm(RSI, watchBase_); a_.xorRR(RCX, RCX);
    size_t wl = a_.here();
    a_.cmpImm(RCX, kMaxWatch); size_t none = a_.jge();
    a_.movRR(RAX, RCX); a_.movImm(RDX, kWatchRec); a_.imulRR(RAX, RDX); a_.addRR(RAX, RSI);
    a_.loadMem(RDX, RAX, 0); a_.testRR(RDX); size_t yes2 = a_.jne();
    a_.addImm(RCX, 1); size_t wb = a_.jmp(); a_.patchRel(wb, wl);
    a_.patchRel(yes, a_.here()); a_.patchRel(yes2, a_.here()); a_.movImm(RAX, 1); a_.ret();
    a_.patchRel(none, a_.here()); a_.xorRR(RAX, RAX); a_.ret();
}

// loop_step(): poll watched fds up to the earliest timer's deadline, fire ready
// watch callbacks (with the fd) then all due timer callbacks (with the tick).
// Mirrors RuntimeLoop::nextBatch. Frame slots: -8 min_due/now, -16 found,
// -24 timeoutMs, -32 pickaddr, -40 pickdue, -48 nwatch, -56/-64 timespec, -72 i.
void X64Gen::genLoopStep() {
    loopStepOff_ = a_.here();
    a_.push(RBP); a_.movRR(RBP, RSP); a_.subImm(RSP, 96);
    // --- find earliest active timer ---
    a_.xorRR(RAX, RAX); a_.store(RAX, -16);
    addrImm(RDI, loopBase_ + 16); a_.xorRR(RCX, RCX);
    size_t p1 = a_.here();
    a_.cmpImm(RCX, kMaxTimers); size_t p1done = a_.jge();
    a_.movRR(RAX, RCX); a_.movImm(RDX, kTimerRec); a_.imulRR(RAX, RDX); a_.addRR(RAX, RDI);
    a_.loadMem(RDX, RAX, 0); a_.testRR(RDX); size_t p1next = a_.je();
    a_.loadMem(RDX, RAX, 16);
    a_.load(R8, -16); a_.testRR(R8); size_t p1set = a_.je();
    a_.load(R9, -8); a_.cmpRR(RDX, R9); size_t p1skip = a_.jge();
    a_.patchRel(p1set, a_.here());
    a_.store(RDX, -8); a_.movImm(R8, 1); a_.store(R8, -16);
    a_.patchRel(p1skip, a_.here());
    a_.patchRel(p1next, a_.here());
    a_.addImm(RCX, 1); size_t p1b = a_.jmp(); a_.patchRel(p1b, p1);
    a_.patchRel(p1done, a_.here());
    // --- gather active watches into pfds@pollBase_, ids@pollBase_+kMaxWatch*8 ---
    a_.xorRR(RAX, RAX); a_.store(RAX, -48); a_.store(RAX, -72);   // nwatch, i
    size_t w1 = a_.here();
    a_.load(RCX, -72); a_.cmpImm(RCX, kMaxWatch); size_t w1done = a_.jge();
    addrImm(RDI, watchBase_); a_.movRR(RAX, RCX); a_.movImm(RDX, kWatchRec);
    a_.imulRR(RAX, RDX); a_.addRR(RAX, RDI);                       // wslot
    a_.loadMem(RDX, RAX, 0); a_.testRR(RDX); size_t w1n = a_.je();
    a_.load(R8, -48);                                              // n
    addrImm(RDI, pollBase_); a_.movRR(R9, R8); a_.shlImm(R9, 3); a_.addRR(R9, RDI);  // &pfd[n]
    a_.loadMem(RDX, RAX, 16);                                      // fd
    a_.movImm(RCX, 1); a_.shlImm(RCX, 32); a_.orRR(RDX, RCX); a_.storeMem(R9, 0, RDX);  // fd|events
    a_.loadMem(RDX, RAX, 8);                                       // id
    addrImm(RDI, pollBase_ + kMaxWatch * 8); a_.movRR(R9, R8); a_.shlImm(R9, 3); a_.addRR(R9, RDI);
    a_.storeMem(R9, 0, RDX);                                       // ids[n]
    a_.load(R8, -48); a_.addImm(R8, 1); a_.store(R8, -48);
    a_.patchRel(w1n, a_.here());
    a_.load(RCX, -72); a_.addImm(RCX, 1); a_.store(RCX, -72); size_t w1b = a_.jmp(); a_.patchRel(w1b, w1);
    a_.patchRel(w1done, a_.here());
    // --- nothing to do? ---
    a_.load(RAX, -16); a_.testRR(RAX); size_t hw1 = a_.jne();
    a_.load(RAX, -48); a_.testRR(RAX); size_t hw2 = a_.jne();
    a_.leave(); a_.ret();
    a_.patchRel(hw1, a_.here()); a_.patchRel(hw2, a_.here());
    // --- timeoutMs from earliest timer, else -1 ---
    a_.load(RAX, -16); a_.testRR(RAX); size_t noTimer = a_.je();
    callFixups_.push_back({a_.call(), -39}); a_.movRR(RCX, RAX);   // now
    a_.load(RAX, -8); a_.subRR(RAX, RCX);
    a_.testRR(RAX); size_t posd = a_.jcc(9); a_.xorRR(RAX, RAX); a_.patchRel(posd, a_.here());
    a_.movImm(RCX, 1000000); a_.cqo(); a_.idiv(RCX); a_.store(RAX, -24);
    size_t haveTo = a_.jmp();
    a_.patchRel(noTimer, a_.here());
    a_.movImm(RAX, (uint64_t)-1); a_.store(RAX, -24);
    a_.patchRel(haveTo, a_.here());
    // --- poll (if watches) else sleep (if a timer with timeout>0) ---
    a_.load(RAX, -48); a_.testRR(RAX); size_t noPoll = a_.je();
    addrImm(RDI, pollBase_); a_.load(RSI, -48); a_.load(RDX, -24); a_.movImm(RAX, 7); a_.syscall_();
    a_.xorRR(RAX, RAX); a_.store(RAX, -72);                        // i = 0
    size_t pl = a_.here();
    a_.load(RCX, -72); a_.load(RDX, -48); a_.cmpRR(RCX, RDX); size_t pldone = a_.jge();
    addrImm(RDI, pollBase_); a_.movRR(RAX, RCX); a_.shlImm(RAX, 3); a_.addRR(RAX, RDI);
    a_.loadMem(RDX, RAX, 0); a_.shrImm(RDX, 48);                   // revents
    a_.movImm(RCX, 0x19); a_.andRR(RDX, RCX); a_.testRR(RDX); size_t pln = a_.je();
    a_.load(RCX, -72); addrImm(RDI, pollBase_ + kMaxWatch * 8);
    a_.movRR(RAX, RCX); a_.shlImm(RAX, 3); a_.addRR(RAX, RDI); a_.loadMem(R8, RAX, 0);   // id
    addrImm(RDI, watchBase_); a_.xorRR(RCX, RCX);
    size_t fw = a_.here();
    a_.cmpImm(RCX, kMaxWatch); size_t fwdone = a_.jge();
    a_.movRR(RAX, RCX); a_.movImm(RDX, kWatchRec); a_.imulRR(RAX, RDX); a_.addRR(RAX, RDI);
    a_.loadMem(RDX, RAX, 0); a_.testRR(RDX); size_t fwn = a_.je();
    a_.loadMem(RDX, RAX, 8); a_.cmpRR(RDX, R8); size_t fwn2 = a_.jne();
    a_.loadMem(RDX, RAX, 16);                                      // fd (arg)
    a_.push(RDX); a_.movImm(RCX, 1); a_.push(RCX);
    a_.loadMem(RDI, RAX, 32); a_.movImm(RCX, 1);
    callFixups_.push_back({a_.call(), -27});                       // callclosure(cb, fd)
    a_.addImm(RSP, 16);
    size_t fwbreak = a_.jmp();
    a_.patchRel(fwn, a_.here()); a_.patchRel(fwn2, a_.here());
    a_.addImm(RCX, 1); size_t fwb = a_.jmp(); a_.patchRel(fwb, fw);
    a_.patchRel(fwdone, a_.here()); a_.patchRel(fwbreak, a_.here());
    addrImm(RCX, 24); a_.loadInd(RAX, RCX); a_.testRR(RAX); size_t plstop = a_.jne();
    a_.patchRel(pln, a_.here());
    a_.load(RCX, -72); a_.addImm(RCX, 1); a_.store(RCX, -72); size_t plb = a_.jmp(); a_.patchRel(plb, pl);
    a_.patchRel(plstop, a_.here());
    a_.leave(); a_.ret();
    a_.patchRel(pldone, a_.here());
    size_t toFire = a_.jmp();
    a_.patchRel(noPoll, a_.here());
    a_.load(RAX, -24); a_.testRR(RAX); size_t toFire2 = a_.jle();   // timeout<=0 -> fire now
    a_.movImm(RCX, 1000); a_.cqo(); a_.idiv(RCX); a_.store(RAX, -56);   // sec = ms/1000
    a_.movRR(RAX, RDX); a_.movImm(RCX, 1000000); a_.imulRR(RAX, RCX); a_.store(RAX, -64);  // nsec
    a_.movImm(RAX, 35); a_.lea(RDI, RBP, -56); a_.xorRR(RSI, RSI); a_.syscall_();
    a_.patchRel(toFire, a_.here()); a_.patchRel(toFire2, a_.here());
    // --- fire due timers ---
    callFixups_.push_back({a_.call(), -39}); a_.store(RAX, -8);    // now (snapshot)
    size_t fireloop = a_.here();
    a_.xorRR(RAX, RAX); a_.store(RAX, -16);
    addrImm(RDI, loopBase_ + 16); a_.xorRR(RCX, RCX);
    size_t f1 = a_.here();
    a_.cmpImm(RCX, kMaxTimers); size_t f1done = a_.jge();
    a_.movRR(RAX, RCX); a_.movImm(RDX, kTimerRec); a_.imulRR(RAX, RDX); a_.addRR(RAX, RDI);
    a_.loadMem(RDX, RAX, 0); a_.testRR(RDX); size_t f1next = a_.je();
    a_.loadMem(RDX, RAX, 16); a_.load(R9, -8); a_.cmpRR(RDX, R9); size_t f1skip = a_.jg();
    a_.load(R8, -16); a_.testRR(R8); size_t f1pick = a_.je();
    a_.load(R9, -40); a_.cmpRR(RDX, R9); size_t f1skip2 = a_.jge();
    a_.patchRel(f1pick, a_.here());
    a_.store(RDX, -40); a_.store(RAX, -32); a_.movImm(R8, 1); a_.store(R8, -16);
    a_.patchRel(f1skip, a_.here()); a_.patchRel(f1skip2, a_.here());
    a_.patchRel(f1next, a_.here());
    a_.addImm(RCX, 1); size_t f1b = a_.jmp(); a_.patchRel(f1b, f1);
    a_.patchRel(f1done, a_.here());
    a_.load(RAX, -16); a_.testRR(RAX); size_t fire = a_.jne();
    a_.leave(); a_.ret();                                  // nothing due -> step done
    a_.patchRel(fire, a_.here());
    a_.load(RSI, -32);                                     // picked slot
    a_.loadMem(RAX, RSI, 32); a_.addImm(RAX, 1); a_.storeMem(RSI, 32, RAX);   // ticks++
    a_.push(RAX); a_.movImm(RAX, 1); a_.push(RAX);         // arg = tick (pay, tag int)
    a_.load(RSI, -32); a_.loadMem(RDI, RSI, 48); a_.movImm(RCX, 1);
    callFixups_.push_back({a_.call(), -27});               // callclosure(cb, tick)
    a_.addImm(RSP, 16);
    a_.load(RSI, -32); a_.loadMem(RAX, RSI, 24); a_.testRR(RAX); size_t oneshot = a_.je();
    a_.movImm(RDX, 1000000); a_.imulRR(RAX, RDX);
    a_.loadMem(RDX, RSI, 16); a_.addRR(RAX, RDX); a_.storeMem(RSI, 16, RAX);  // due += interval
    size_t armed = a_.jmp();
    a_.patchRel(oneshot, a_.here());
    a_.xorRR(RAX, RAX); a_.storeMem(RSI, 0, RAX);          // active = 0
    // §15: one-shot done — drop the registry's callback ref (retained by timer_add)
    a_.loadMem(RDI, RSI, 40); a_.loadMem(RSI, RSI, 48);
    callFixups_.push_back({a_.call(), -66});
    a_.patchRel(armed, a_.here());
    addrImm(RCX, 24); a_.loadInd(RAX, RCX); a_.testRR(RAX); size_t stop = a_.jne();
    size_t fb = a_.jmp(); a_.patchRel(fb, fireloop);
    a_.patchRel(stop, a_.here());
    a_.leave(); a_.ret();
}

// run_loop(): pump loop_step while there is work (and no pending throw).
void X64Gen::genRunLoop() {
    runLoopOff_ = a_.here();
    a_.push(RBP); a_.movRR(RBP, RSP); a_.subImm(RSP, 16);
    size_t loop = a_.here();
    callFixups_.push_back({a_.call(), -43}); a_.testRR(RAX); size_t done = a_.je();
    addrImm(RCX, 24); a_.loadInd(RAX, RCX); a_.testRR(RAX); size_t thrown = a_.jne();
    callFixups_.push_back({a_.call(), -42});               // loop_step
    size_t b = a_.jmp(); a_.patchRel(b, loop);
    a_.patchRel(done, a_.here()); a_.patchRel(thrown, a_.here());
    a_.leave(); a_.ret();
}

// ---------------------------------------------------------------------------
//  Step 3c: sockets over raw syscalls, plus fd-watch polling in the loop.
//  Syscall numbers: socket 41, connect 42, accept 43, sendto 44, recvfrom 45,
//  bind 49, listen 50, setsockopt 54, fcntl 72, poll 7. The syscall ABI passes
//  the 4th argument in r10 (not rcx).
// ---------------------------------------------------------------------------

// parse_ip(rdi=strptr) -> rax = IPv4 address as a little-endian u32 whose bytes
// are the octets in order (i.e. ready to drop into sin_addr).
void X64Gen::genParseIp() {
    parseIpOff_ = a_.here();
    a_.push(RBP); a_.movRR(RBP, RSP); a_.subImm(RSP, 16);
    a_.loadInd(RCX, RDI); a_.lea(RSI, RDI, 8); a_.addRR(RCX, RSI); a_.store(RCX, -8);  // end
    a_.xorRR(R8, R8); a_.xorRR(R9, R9);        // result, shift
    size_t oct = a_.here();
    a_.xorRR(R10, R10);                         // octet
    size_t dig = a_.here();
    a_.load(RDX, -8); a_.cmpRR(RSI, RDX); size_t octdone = a_.jge();
    a_.loadByte(RAX, RSI, 0); a_.cmpImm(RAX, 46); size_t isDot = a_.je();
    a_.movImm(RCX, 10); a_.imulRR(R10, RCX); a_.subImm(RAX, 48); a_.addRR(R10, RAX);
    a_.addImm(RSI, 1); size_t db = a_.jmp(); a_.patchRel(db, dig);
    a_.patchRel(octdone, a_.here()); a_.patchRel(isDot, a_.here());
    a_.movRR(RAX, R10); a_.movRR(RCX, R9); a_.shlCl(RAX); a_.orRR(R8, RAX);
    a_.addImm(R9, 8);
    a_.load(RDX, -8); a_.cmpRR(RSI, RDX); size_t done = a_.jge();
    a_.addImm(RSI, 1); a_.cmpImm(R9, 32); size_t more = a_.jl(); a_.patchRel(more, oct);
    a_.patchRel(done, a_.here());
    a_.movRR(RAX, R8); a_.leave(); a_.ret();
}

// Build the 8-byte word for sockaddr_in[0..7]: family(2)=AF_INET | htons(port)
// in [16..31] | sin_addr in [32..63]. rdi=port, rsi=addr -> rax = the word.
static void emitSockaddrWord(Asm& a) {
    // htons(port): rax = ((port & 0xff) << 8) | ((port >> 8) & 0xff)
    a.movImm(RCX, 0xffff); a.andRR(RDI, RCX);
    a.movRR(RAX, RDI); a.movImm(RCX, 0xff); a.andRR(RAX, RCX); a.shlImm(RAX, 8);  // lo<<8
    a.shrImm(RDI, 8);                                                              // hi
    a.orRR(RAX, RDI);                          // htons(port)
    a.shlImm(RAX, 16); a.movImm(RCX, 2); a.orRR(RAX, RCX);   // | AF_INET
    a.shlImm(RSI, 32); a.orRR(RAX, RSI);       // | addr<<32
}

// sysTcpConnect(rdi=hoststr, rsi=port) -> rax = fd (or -1).
void X64Gen::genTcpConnect() {
    tcpConnectOff_ = a_.here();
    a_.push(RBP); a_.movRR(RBP, RSP); a_.subImm(RSP, 64);
    a_.store(RSI, -8);                          // port
    callFixups_.push_back({a_.call(), -45});    // parse_ip(host) -> rax
    a_.store(RAX, -16);                          // addr
    a_.movImm(RDI, 2); a_.movImm(RSI, 1); a_.xorRR(RDX, RDX); a_.movImm(RAX, 41); a_.syscall_();
    a_.testRR(RAX); size_t failNoFd = a_.jcc(8);   // js
    a_.store(RAX, -24);                          // fd
    a_.xorRR(RAX, RAX); a_.store(RAX, -48); a_.store(RAX, -40);   // zero sockaddr
    a_.load(RDI, -8); a_.load(RSI, -16); emitSockaddrWord(a_); a_.store(RAX, -48);
    a_.load(RDI, -24); a_.lea(RSI, RBP, -48); a_.movImm(RDX, 16); a_.movImm(RAX, 42); a_.syscall_();
    a_.testRR(RAX); size_t failClose = a_.jcc(8);
    a_.load(RDI, -24); a_.movImm(RSI, 4); a_.movImm(RDX, 0x800); a_.movImm(RAX, 72); a_.syscall_();
    a_.load(RAX, -24); a_.leave(); a_.ret();
    a_.patchRel(failClose, a_.here());
    a_.load(RDI, -24); a_.movImm(RAX, 3); a_.syscall_();
    a_.patchRel(failNoFd, a_.here());
    a_.movImm(RAX, (uint64_t)-1); a_.leave(); a_.ret();
}

// sysTcpListen(rdi=port) -> rax = fd (or -1). Binds 127.0.0.1:port, nonblocking.
void X64Gen::genTcpListen() {
    tcpListenOff_ = a_.here();
    a_.push(RBP); a_.movRR(RBP, RSP); a_.subImm(RSP, 64);
    a_.store(RDI, -8);                           // port
    a_.movImm(RDI, 2); a_.movImm(RSI, 1); a_.xorRR(RDX, RDX); a_.movImm(RAX, 41); a_.syscall_();
    a_.testRR(RAX); size_t failNoFd = a_.jcc(8);
    a_.store(RAX, -24);                          // fd
    a_.movImm(RAX, 1); a_.store(RAX, -32);       // one (SO_REUSEADDR)
    a_.load(RDI, -24); a_.movImm(RSI, 1); a_.movImm(RDX, 2); a_.lea(R10, RBP, -32);
    a_.movImm(R8, 4); a_.movImm(RAX, 54); a_.syscall_();
    a_.xorRR(RAX, RAX); a_.store(RAX, -48); a_.store(RAX, -40);
    a_.load(RDI, -8); a_.movImm(RSI, 0x0100007f); emitSockaddrWord(a_); a_.store(RAX, -48);  // 127.0.0.1
    a_.load(RDI, -24); a_.lea(RSI, RBP, -48); a_.movImm(RDX, 16); a_.movImm(RAX, 49); a_.syscall_();
    a_.testRR(RAX); size_t failClose = a_.jcc(8);
    a_.load(RDI, -24); a_.movImm(RSI, 16); a_.movImm(RAX, 50); a_.syscall_();
    a_.testRR(RAX); size_t failClose2 = a_.jcc(8);
    a_.load(RDI, -24); a_.movImm(RSI, 4); a_.movImm(RDX, 0x800); a_.movImm(RAX, 72); a_.syscall_();
    a_.load(RAX, -24); a_.leave(); a_.ret();
    a_.patchRel(failClose, a_.here()); a_.patchRel(failClose2, a_.here());
    a_.load(RDI, -24); a_.movImm(RAX, 3); a_.syscall_();
    a_.patchRel(failNoFd, a_.here());
    a_.movImm(RAX, (uint64_t)-1); a_.leave(); a_.ret();
}

// sysAccept(rdi=fd) -> rax = client fd (or -1), nonblocking.
void X64Gen::genAccept() {
    acceptOff_ = a_.here();
    a_.push(RBP); a_.movRR(RBP, RSP); a_.subImm(RSP, 16);
    a_.xorRR(RSI, RSI); a_.xorRR(RDX, RDX); a_.movImm(RAX, 43); a_.syscall_();
    a_.testRR(RAX); size_t none = a_.jcc(8);
    a_.store(RAX, -8);
    a_.movRR(RDI, RAX); a_.movImm(RSI, 4); a_.movImm(RDX, 0x800); a_.movImm(RAX, 72); a_.syscall_();
    a_.load(RAX, -8);
    a_.patchRel(none, a_.here());
    a_.leave(); a_.ret();
}

// sysSend(rdi=fd, rsi=strptr) -> rax = bytes sent.
void X64Gen::genSend() {
    sendOff_ = a_.here();
    a_.loadInd(RDX, RSI); a_.addImm(RSI, 8);     // len, buf
    a_.movImm(R10, 0x4000); a_.xorRR(R8, R8); a_.xorRR(R9, R9);   // MSG_NOSIGNAL
    a_.movImm(RAX, 44); a_.syscall_();
    a_.ret();
}

// sysRecv(rdi=fd, rsi=max) -> rax=tag, rdx=pay. None (tag8) on close, "" on
// would-block, else the received bytes.
void X64Gen::genRecv() {
    recvOff_ = a_.here();
    a_.push(RBP); a_.movRR(RBP, RSP); a_.subImm(RSP, 32);
    a_.store(RDI, -8); a_.store(RSI, -24);       // fd, max
    a_.movRR(RDI, RSI); a_.addImm(RDI, 8); callFixups_.push_back({a_.call(), -69}); a_.store(RAX, -16);   // halloc (§15 prefixed)
    a_.load(RDI, -8); a_.load(RSI, -16); a_.addImm(RSI, 8); a_.load(RDX, -24);
    a_.xorRR(R10, R10); a_.xorRR(R8, R8); a_.xorRR(R9, R9);
    a_.movImm(RAX, 45); a_.syscall_();           // recvfrom -> rax = n
    a_.testRR(RAX); size_t isNone = a_.je(); size_t isEmpty = a_.jcc(8);   // 0 / <0
    a_.load(RCX, -16); a_.storeInd(RCX, RAX); a_.movRR(RDX, RCX); a_.movImm(RAX, 4);
    a_.leave(); a_.ret();
    a_.patchRel(isNone, a_.here());
    a_.load(RCX, -16); emitStrTempFree(RCX);      // §15: drop the unused recv buffer
    a_.movImm(RAX, 8); a_.xorRR(RDX, RDX); a_.leave(); a_.ret();
    a_.patchRel(isEmpty, a_.here());
    a_.load(RCX, -16); emitStrTempFree(RCX);      // §15: would-block polls must not accrete
    a_.movImm(RAX, 4); addrImm(RDX, 16); a_.leave(); a_.ret();   // empty string
}

// watch_add(rdi=fd, rsi=cb_tag, rdx=cb_pay) -> rax=id.
void X64Gen::genWatchAdd() {
    watchAddOff_ = a_.here();
    a_.push(RBP); a_.movRR(RBP, RSP); a_.subImm(RSP, 32);
    a_.store(RDI, -8); a_.store(RSI, -16); a_.store(RDX, -24);
    addrImm(RDI, loopBase_ + 0); a_.loadInd(R8, RDI);
    a_.movRR(RAX, R8); a_.addImm(RAX, 1); a_.storeInd(RDI, RAX);
    addrImm(RDI, watchBase_); a_.xorRR(RCX, RCX);
    size_t find = a_.here();
    a_.cmpImm(RCX, kMaxWatch); size_t full = a_.jge();
    a_.movRR(RAX, RCX); a_.movImm(RDX, kWatchRec); a_.imulRR(RAX, RDX); a_.addRR(RAX, RDI);
    a_.loadMem(RDX, RAX, 0); a_.testRR(RDX); size_t got = a_.je();
    a_.addImm(RCX, 1); size_t fb = a_.jmp(); a_.patchRel(fb, find);
    a_.patchRel(got, a_.here());
    a_.movImm(RDX, 1); a_.storeMem(RAX, 0, RDX);
    a_.storeMem(RAX, 8, R8);
    a_.load(RDX, -8); a_.storeMem(RAX, 16, RDX);      // fd
    a_.load(RDX, -16); a_.storeMem(RAX, 24, RDX);     // cb_tag
    a_.load(RDX, -24); a_.storeMem(RAX, 32, RDX);     // cb_pay
    // §15: the registry owns the callback until watch_cancel (mirror timer_add)
    a_.push(R8);
    a_.load(RDI, -16); a_.load(RSI, -24);
    callFixups_.push_back({a_.call(), -65});
    a_.pop(R8);
    a_.patchRel(full, a_.here());
    a_.movRR(RAX, R8); a_.leave(); a_.ret();
}

// watch_cancel(rdi=id): deactivate the matching watch.
void X64Gen::genWatchCancel() {
    watchCancelOff_ = a_.here();
    addrImm(RSI, watchBase_); a_.xorRR(RCX, RCX);
    size_t loop = a_.here();
    a_.cmpImm(RCX, kMaxWatch); size_t done = a_.jge();
    a_.movRR(RAX, RCX); a_.movImm(RDX, kWatchRec); a_.imulRR(RAX, RDX); a_.addRR(RAX, RSI);
    a_.loadMem(RDX, RAX, 0); a_.testRR(RDX); size_t nx = a_.je();
    a_.loadMem(RDX, RAX, 8); a_.cmpRR(RDX, RDI); size_t nx2 = a_.jne();
    a_.xorRR(RDX, RDX); a_.storeMem(RAX, 0, RDX);     // active = 0
    // §15: drop the registry's callback ref (retained by watch_add)
    a_.loadMem(RDI, RAX, 24); a_.loadMem(RSI, RAX, 32);
    a_.xorRR(RDX, RDX); a_.storeMem(RAX, 24, RDX); a_.storeMem(RAX, 32, RDX);
    callFixups_.push_back({a_.call(), -66});
    a_.ret();
    a_.patchRel(nx, a_.here()); a_.patchRel(nx2, a_.here());
    a_.addImm(RCX, 1); size_t b = a_.jmp(); a_.patchRel(b, loop);
    a_.patchRel(done, a_.here());
    a_.ret();
}

// str_indexof(rdi=s, rsi=needle) -> rax = first index of needle in s, or -1.
void X64Gen::genStrIndexOf() {
    strIndexOfOff_ = a_.here();
    a_.push(RBP); a_.movRR(RBP, RSP); a_.subImm(RSP, 64);
    a_.store(RDI, -24); a_.store(RSI, -32);
    a_.loadInd(RAX, RDI); a_.store(RAX, -8);           // slen
    a_.loadInd(RAX, RSI); a_.store(RAX, -16);          // nlen
    a_.testRR(RAX); size_t ne = a_.jne();
    a_.xorRR(RAX, RAX); a_.leave(); a_.ret();          // empty needle -> 0
    a_.patchRel(ne, a_.here());
    a_.xorRR(RAX, RAX); a_.store(RAX, -40);            // i = 0
    size_t iloop = a_.here();
    a_.load(RAX, -40); a_.load(RCX, -16); a_.addRR(RAX, RCX); a_.load(RDX, -8);
    a_.cmpRR(RAX, RDX); size_t notfound = a_.jg();     // i+nlen > slen
    a_.xorRR(RAX, RAX); a_.store(RAX, -48);            // j = 0
    size_t jloop = a_.here();
    a_.load(RAX, -48); a_.load(RCX, -16); a_.cmpRR(RAX, RCX); size_t matched = a_.jge();
    a_.load(RDI, -24); a_.load(RAX, -40); a_.load(RCX, -48); a_.addRR(RAX, RCX); a_.addImm(RAX, 8);
    a_.addRR(RDI, RAX); a_.loadByte(RDX, RDI, 0);      // sc
    a_.load(RSI, -32); a_.load(RAX, -48); a_.addImm(RAX, 8); a_.addRR(RSI, RAX); a_.loadByte(RCX, RSI, 0);
    a_.cmpRR(RDX, RCX); size_t jbreak = a_.jne();
    a_.load(RAX, -48); a_.addImm(RAX, 1); a_.store(RAX, -48); size_t jb = a_.jmp(); a_.patchRel(jb, jloop);
    a_.patchRel(jbreak, a_.here());
    a_.load(RAX, -40); a_.addImm(RAX, 1); a_.store(RAX, -40); size_t ib = a_.jmp(); a_.patchRel(ib, iloop);
    a_.patchRel(matched, a_.here());
    a_.load(RAX, -40); a_.leave(); a_.ret();
    a_.patchRel(notfound, a_.here());
    a_.movImm(RAX, (uint64_t)-1); a_.leave(); a_.ret();
}

// str_substr(rdi=s, rsi=a, rdx=n) -> rax = s[a .. a+n) (clamped), a new string.
void X64Gen::genStrSubStr() {
    strSubStrOff_ = a_.here();
    a_.push(RBP); a_.movRR(RBP, RSP); a_.subImm(RSP, 64);
    a_.store(RDI, -8); a_.loadInd(RAX, RDI); a_.store(RAX, -16);   // s, slen
    a_.store(RSI, -24); a_.store(RDX, -32);                        // a, n
    a_.load(RAX, -24); a_.testRR(RAX); size_t empty = a_.jcc(8);   // a<0
    a_.load(RAX, -24); a_.load(RCX, -16); a_.cmpRR(RAX, RCX); size_t empty2 = a_.jg();  // a>slen
    a_.load(RAX, -16); a_.load(RCX, -24); a_.subRR(RAX, RCX);      // avail = slen-a
    a_.load(RCX, -32); a_.cmpRR(RCX, RAX); size_t useAvail = a_.jge();
    a_.movRR(RAX, RCX);                                            // take = n (< avail)
    a_.patchRel(useAvail, a_.here());
    a_.testRR(RAX); size_t tk = a_.jcc(9); a_.xorRR(RAX, RAX); a_.patchRel(tk, a_.here());
    a_.store(RAX, -40);                                            // take
    a_.movRR(RDI, RAX); a_.addImm(RDI, 8); callFixups_.push_back({a_.call(), -69});   // halloc (§15 prefixed)
    a_.store(RAX, -48);                                            // new
    a_.load(RCX, -40); a_.storeInd(RAX, RCX);
    a_.load(RSI, -8); a_.addImm(RSI, 8); a_.load(RCX, -24); a_.addRR(RSI, RCX);    // src = s+8+a
    a_.load(RDI, -48); a_.addImm(RDI, 8); a_.load(RCX, -40); a_.repMovsb();
    a_.load(RAX, -48); a_.leave(); a_.ret();
    a_.patchRel(empty, a_.here()); a_.patchRel(empty2, a_.here());
    addrImm(RAX, 16); a_.leave(); a_.ret();
}

// str_toint(rdi=s) -> rax (atoll-style: optional leading spaces, sign, digits).
void X64Gen::genStrToInt() {
    strToIntOff_ = a_.here();
    a_.push(RBP); a_.movRR(RBP, RSP); a_.subImm(RSP, 16);
    a_.loadInd(RCX, RDI); a_.lea(RSI, RDI, 8); a_.addRR(RCX, RSI); a_.store(RCX, -8);   // end
    a_.xorRR(R8, R8); a_.xorRR(R9, R9);                            // result, neg
    size_t sk = a_.here();
    a_.load(RDX, -8); a_.cmpRR(RSI, RDX); size_t done = a_.jge();
    a_.loadByte(RAX, RSI, 0); a_.cmpImm(RAX, 32); size_t noSp = a_.jne();
    a_.addImm(RSI, 1); size_t skb = a_.jmp(); a_.patchRel(skb, sk);
    a_.patchRel(noSp, a_.here());
    a_.loadByte(RAX, RSI, 0); a_.cmpImm(RAX, 45); size_t nosign = a_.jne();
    a_.movImm(R9, 1); a_.addImm(RSI, 1);
    a_.patchRel(nosign, a_.here());
    size_t dloop = a_.here();
    a_.load(RDX, -8); a_.cmpRR(RSI, RDX); size_t done2 = a_.jge();
    a_.loadByte(RAX, RSI, 0); a_.cmpImm(RAX, 48); size_t done3 = a_.jl();
    a_.cmpImm(RAX, 57); size_t done4 = a_.jg();
    a_.movImm(RCX, 10); a_.imulRR(R8, RCX); a_.subImm(RAX, 48); a_.addRR(R8, RAX);
    a_.addImm(RSI, 1); size_t db = a_.jmp(); a_.patchRel(db, dloop);
    a_.patchRel(done, a_.here()); a_.patchRel(done2, a_.here());
    a_.patchRel(done3, a_.here()); a_.patchRel(done4, a_.here());
    a_.movRR(RAX, R8); a_.testRR(R9); size_t pos = a_.je(); a_.neg(RAX); a_.patchRel(pos, a_.here());
    a_.leave(); a_.ret();
}

// str_trim(rdi=s) -> rax = s with leading/trailing whitespace removed.
void X64Gen::genStrTrim() {
    strTrimOff_ = a_.here();
    a_.push(RBP); a_.movRR(RBP, RSP); a_.subImm(RSP, 32);
    a_.store(RDI, -8); a_.loadInd(RAX, RDI); a_.store(RAX, -16);   // s, len
    auto isWs = [&](int reg, size_t& j32, size_t& j9, size_t& j10, size_t& j13) {
        a_.cmpImm(reg, 32); j32 = a_.je();
        a_.cmpImm(reg, 9);  j9  = a_.je();
        a_.cmpImm(reg, 10); j10 = a_.je();
        a_.cmpImm(reg, 13); j13 = a_.je();
    };
    a_.xorRR(RCX, RCX);                                            // a
    size_t la = a_.here();
    a_.load(RAX, -16); a_.cmpRR(RCX, RAX); size_t allws = a_.jge();
    a_.load(RDI, -8); a_.movRR(RAX, RCX); a_.addImm(RAX, 8); a_.addRR(RDI, RAX); a_.loadByte(RDX, RDI, 0);
    size_t a32, a9, a10, a13; isWs(RDX, a32, a9, a10, a13);
    size_t foundA = a_.jmp();
    a_.patchRel(a32, a_.here()); a_.patchRel(a9, a_.here());
    a_.patchRel(a10, a_.here()); a_.patchRel(a13, a_.here());
    a_.addImm(RCX, 1); size_t lab = a_.jmp(); a_.patchRel(lab, la);
    a_.patchRel(allws, a_.here());
    addrImm(RAX, 16); a_.leave(); a_.ret();                        // all whitespace -> ""
    a_.patchRel(foundA, a_.here());
    a_.store(RCX, -24);                                            // a
    a_.load(RCX, -16); a_.subImm(RCX, 1);                          // b = len-1
    size_t lb = a_.here();
    a_.load(RDI, -8); a_.movRR(RAX, RCX); a_.addImm(RAX, 8); a_.addRR(RDI, RAX); a_.loadByte(RDX, RDI, 0);
    size_t b32, b9, b10, b13; isWs(RDX, b32, b9, b10, b13);
    size_t foundB = a_.jmp();
    a_.patchRel(b32, a_.here()); a_.patchRel(b9, a_.here());
    a_.patchRel(b10, a_.here()); a_.patchRel(b13, a_.here());
    a_.subImm(RCX, 1); size_t lbb = a_.jmp(); a_.patchRel(lbb, lb);
    a_.patchRel(foundB, a_.here());
    a_.load(RDI, -8); a_.load(RSI, -24); a_.movRR(RDX, RCX); a_.load(RAX, -24);
    a_.subRR(RDX, RAX); a_.addImm(RDX, 1);                         // n = b-a+1
    callFixups_.push_back({a_.call(), -54});                       // str_substr
    a_.leave(); a_.ret();
}

// str_case(rdi=s, rsi=mode: 0 upper / 1 lower) -> rax = a fresh copy with
// ASCII case applied (mirrors RuntimeNatives toUpper/toLower).
void X64Gen::genStrCase() {
    strCaseOff_ = a_.here();
    a_.push(RBP); a_.movRR(RBP, RSP); a_.subImm(RSP, 32);
    a_.store(RDI, -8); a_.store(RSI, -16);
    a_.loadInd(RCX, RDI); a_.store(RCX, -24);            // len
    a_.movRR(RDI, RCX); a_.addImm(RDI, 8);
    callFixups_.push_back({a_.call(), -69});             // halloc(len+8) (§15 prefixed)
    a_.store(RAX, -32);
    a_.load(RCX, -24); a_.storeInd(RAX, RCX);            // [dst] = len
    a_.load(RSI, -8); a_.addImm(RSI, 8);                 // src bytes
    a_.movRR(RDI, RAX); a_.addImm(RDI, 8);               // dst bytes
    a_.load(RCX, -24);
    size_t loop = a_.here();
    a_.testRR(RCX); size_t caseDone = a_.je();
    a_.loadByte(RAX, RSI, 0);
    a_.load(RDX, -16); a_.testRR(RDX); size_t toLower = a_.jne();
    a_.cmpImm(RAX, 'a'); size_t sk1 = a_.jl();
    a_.cmpImm(RAX, 'z'); size_t sk2 = a_.jg();
    a_.subImm(RAX, 32);
    a_.patchRel(sk1, a_.here()); a_.patchRel(sk2, a_.here());
    size_t caseJoin = a_.jmp();
    a_.patchRel(toLower, a_.here());
    a_.cmpImm(RAX, 'A'); size_t sk3 = a_.jl();
    a_.cmpImm(RAX, 'Z'); size_t sk4 = a_.jg();
    a_.addImm(RAX, 32);
    a_.patchRel(sk3, a_.here()); a_.patchRel(sk4, a_.here());
    a_.patchRel(caseJoin, a_.here());
    a_.u8(0x88); a_.u8(0x07);                            // mov [rdi], al
    a_.incR(RSI); a_.incR(RDI); a_.decR(RCX);
    size_t back = a_.jmp(); a_.patchRel(back, loop);
    a_.patchRel(caseDone, a_.here());
    a_.load(RAX, -32); a_.leave(); a_.ret();
}

// print_val(rdi = tag, rsi = payload): runtime print — delegates to ts_build,
// the machine analogue of valueToString (recursive over arrays/maps).
void X64Gen::genPrintVal() {
    printValOff_ = a_.here();
    a_.push(RBP); a_.movRR(RBP, RSP); a_.subImm(RSP, 16);
    callFixups_.push_back({a_.call(), -17});   // ts_build(tag, pay) -> rax = string
    a_.store(RAX, -8);
    a_.movRR(RDI, RAX);
    callFixups_.push_back({a_.call(), -7});    // print_str
    // §15 strings tier: ts_build's result is an unowned fresh string when the
    // value had to be rendered (ints, arrays, maps) and a borrow when the value
    // IS a string; free exactly the unowned case now that it is printed.
    a_.load(RCX, -8); emitStrTempFree(RCX);
    a_.leave(); a_.ret();
}

// print_int(rdi = value): write the decimal representation to fd 1.
void X64Gen::genPrintInt() {
    printIntOff_ = a_.here();
    a_.push(RBP); a_.movRR(RBP, RSP); a_.subImm(RSP, 32);
    a_.movImm(RCX, 10);
    // lea rsi, [rbp-1]
    a_.u8(0x48); a_.u8(0x8D); a_.u8(0x75); a_.u8(0xFF);
    a_.movRR(RAX, RDI);
    a_.xorRR(R8, R8);                       // sign flag
    a_.testRR(RAX);
    size_t jns = a_.jcc(9);                 // jns POS
    a_.neg(RAX);
    a_.movImm(R8, 1);
    a_.patchRel(jns, a_.here());            // POS:
    size_t loop = a_.here();
    a_.cqo();
    a_.idiv(RCX);                           // rax=quot, rdx=rem
    a_.u8(0x80); a_.u8(0xC2); a_.u8(0x30);  // add dl, '0'
    a_.u8(0x88); a_.u8(0x16);               // mov [rsi], dl
    a_.u8(0x48); a_.u8(0xFF); a_.u8(0xCE);  // dec rsi
    a_.testRR(RAX);
    size_t jnz = a_.jcc(5);                 // jne loop
    a_.patchRel(jnz, loop);
    a_.testRR(R8);
    size_t jz = a_.jcc(4);                  // jz NOSIGN
    a_.u8(0xC6); a_.u8(0x06); a_.u8(0x2D);  // mov byte [rsi], '-'
    a_.u8(0x48); a_.u8(0xFF); a_.u8(0xCE);  // dec rsi
    a_.patchRel(jz, a_.here());             // NOSIGN:
    a_.u8(0x48); a_.u8(0xFF); a_.u8(0xC6);  // inc rsi  (rsi -> first char)
    a_.movRR(RDX, RBP); a_.subRR(RDX, RSI); // len = rbp - rsi
    a_.movImm(RAX, 1); a_.movImm(RDI, 1);   // write(1, rsi, len)
    a_.syscall_();
    a_.leave(); a_.ret();
}

// print_nl(): write a single newline to fd 1 (buffer on the stack).
void X64Gen::genPrintNl() {
    printNlOff_ = a_.here();
    a_.subImm(RSP, 8);
    a_.u8(0xC6); a_.u8(0x04); a_.u8(0x24); a_.u8(0x0A);   // mov byte [rsp], 10
    a_.movImm(RAX, 1); a_.movImm(RDI, 1); a_.movRR(RSI, RSP); a_.movImm(RDX, 1);
    a_.syscall_();
    a_.addImm(RSP, 8);
    a_.ret();
}

// print_bool(rdi): write "true" or "false" to fd 1 (bytes staged on the stack).
void X64Gen::genPrintBool() {
    printBoolOff_ = a_.here();
    a_.subImm(RSP, 8);
    a_.testRR(RDI);
    size_t jz = a_.jcc(4);                                  // jz FALSE
    a_.u8(0xC7); a_.u8(0x04); a_.u8(0x24); a_.u32(0x65757274);   // mov dword [rsp], "true"
    a_.movImm(RAX, 1); a_.movImm(RDI, 1); a_.movRR(RSI, RSP); a_.movImm(RDX, 4);
    a_.syscall_();
    size_t jd = a_.jmp();                                   // jmp DONE
    a_.patchRel(jz, a_.here());                             // FALSE:
    a_.u8(0xC7); a_.u8(0x04); a_.u8(0x24); a_.u32(0x736C6166);   // mov dword [rsp], "fals"
    a_.u8(0xC6); a_.u8(0x44); a_.u8(0x24); a_.u8(0x04); a_.u8(0x65);  // mov byte [rsp+4], 'e'
    a_.movImm(RAX, 1); a_.movImm(RDI, 1); a_.movRR(RSI, RSP); a_.movImm(RDX, 5);
    a_.syscall_();
    a_.patchRel(jd, a_.here());                             // DONE:
    a_.addImm(RSP, 8);
    a_.ret();
}

void X64Gen::genFunction(int index) {
    const IrFunction& fn = mod_.functions[index];
    funcOffset_[index] = a_.here();
    // +2 slots: the saved arena cursor (§15: scope-owned allocations live in an
    // arena reset when this frame returns) and an ARC scratch cell that holds a
    // slot's old (tag,pay) across an op so release-old runs AFTER the op wrote
    // the new value — safe for `b = b.next` (dest is also a source) and for
    // throwing ops (on throw the after-hook is skipped; unwind reclaims the slot).
    int frame = (((fn.nregs + 2) * 16) + 15) & ~15;
    int arenaSlot = slot(fn.nregs);
    int arcScratch = slot(fn.nregs + 1);

    a_.push(RBP); a_.movRR(RBP, RSP); a_.subImm(RSP, frame);
    // §15 ARC: zero the register slots so release-old on a not-yet-written slot
    // reads tag 0 (void) and is a no-op — never a deref of stack garbage. The IR
    // writes every reg before reading it, so this changes no observable output.
    if (fn.nregs > 0) {
        a_.xorRR(RAX, RAX);
        a_.lea(RDX, RBP, slot(fn.nregs - 1));   // &lowest reg slot
        a_.movImm(RCX, (uint64_t)fn.nregs * 2);  // one qword per tag/payload half
        size_t zl = a_.here();
        a_.storeInd(RDX, RAX); a_.addImm(RDX, 8); a_.decR(RCX);
        size_t zb = a_.jne(); a_.patchRel(zb, zl);
    }
    for (int p = 0; p < fn.nparams; ++p) {            // params: [rbp+16+16k] (tag,pay) -> slots
        a_.load(RAX, 16 + 16 * p);     a_.store(RAX, slot(p));
        a_.load(RAX, 16 + 16 * p + 8); a_.store(RAX, slot(p) + 8);
    }
    // §15 ARC: the callee owns its parameter slots -> retain each; frame-exit
    // releaseAllRegs balances it. (Args arrive at +0 on the stack; the caller's
    // own reg keeps its count, so the value is counted once per live slot.)
    for (int p = 0; p < fn.nparams; ++p) {
        loadVal(RDI, RSI, p);
        callFixups_.push_back({a_.call(), -65});       // retain
    }
    // §9/§15: $init builds `this`'s FIELDS — everything it allocates must outlive
    // it (a bare value-struct field auto-construct allocates in this's tier, and
    // an arena inner must live exactly as long as the arena outer). So $init
    // adopts the CONSTRUCTING frame's arena window: no save, no reset.
    bool isInit = false;
    for (const auto& [icls, ifn] : mod_.initByClass)
        if (ifn == index) { isInit = true; break; }
    if (!isInit) { addrImm(RCX, 48); a_.loadInd(RAX, RCX); a_.store(RAX, arenaSlot); }   // save arena cursor

    // Free the frame's scope-owned allocations by resetting the arena cursor.
    auto restoreArena = [&]() {
        if (isInit) return;
        a_.load(RAX, arenaSlot); addrImm(RCX, 48); a_.storeInd(RCX, RAX);
    };
    // §15 ARC: at every frame exit, release every register slot (drop this frame's
    // references to escaping-tier objects; arena objects have refcount -1 and are
    // skipped). A returned value is retained by Ret first, so it survives this.
    auto releaseAllRegs = [&]() {
        for (int r = 0; r < fn.nregs; ++r) {
            loadVal(RDI, RSI, r);
            callFixups_.push_back({a_.call(), -66});   // release
        }
    };
    // Route a scope-owned allocation to the arena: set/clear the use-arena flag
    // around the alloc call (result must be stored before the clear).
    auto beginAlloc = [&](int pc) {
        if (scopeOwned_.count({index, pc})) { addrImm(RCX, 56); a_.movImm(RAX, 1); a_.storeInd(RCX, RAX); }
    };
    auto endAlloc = [&](int pc) {
        if (scopeOwned_.count({index, pc})) { addrImm(RCX, 56); a_.xorRR(RAX, RAX); a_.storeInd(RCX, RAX); }
    };

    std::vector<size_t> pcOff(fn.code.size() + 1, 0);
    std::vector<std::pair<size_t, int>> jumps;        // (rel32 pos, target pc)

    // After anything that can throw: if a throw is pending, dispatch to the
    // first covering handler whose clause type matches (issub), binding the
    // value; else jump to the unwind point (the function's void return).
    auto throwCheck = [&](int pc) {
        addrImm(RCX, 24); a_.loadInd(RAX, RCX); a_.testRR(RAX); size_t skip = a_.je();
        for (const Handler& h : fn.handlers) {
            if (pc < h.start || pc >= h.end) continue;
            addrImm(RCX, 32); a_.loadInd(RAX, RCX); a_.cmpImm(RAX, 5); size_t nt = a_.jne();
            size_t nt2 = 0;
            if (h.type) {
                addrImm(RCX, 40); a_.loadInd(RAX, RCX); a_.loadInd(RDI, RAX);   // classId
                a_.movImm(RSI, (uint64_t)clsId(h.type));
                callFixups_.push_back({a_.call(), -28});   // issub
                a_.testRR(RAX); nt2 = a_.je();
            }
            addrImm(RCX, 24); a_.xorRR(RAX, RAX); a_.storeInd(RCX, RAX);        // clear throwing
            // §15: a catch in a loop REBINDS — release the previously bound
            // exception (its g_thrown +1 was transferred here; prologue zeroing
            // makes the first bind a void no-op).
            loadVal(RDI, RSI, h.bindReg);
            callFixups_.push_back({a_.call(), -66});
            addrImm(RCX, 32); a_.loadInd(RAX, RCX); a_.store(RAX, slot(h.bindReg));
            addrImm(RCX, 40); a_.loadInd(RAX, RCX); a_.store(RAX, slot(h.bindReg) + 8);
            jumps.push_back({a_.jmp(), h.handlerPc});
            a_.patchRel(nt, a_.here());
            if (h.type) a_.patchRel(nt2, a_.here());
        }
        jumps.push_back({a_.jmp(), (int)fn.code.size()});   // UNWIND -> void return
        a_.patchRel(skip, a_.here());
    };

    // §15 ARC slot-write discipline. destKind classifies an op by what it does to
    // its dest slot in.a: 0 = no reg dest (or handled specially: Ret*); 1 = writes
    // a value the slot now OWNS -> retain after (fresh alloc at +0, or a borrow/
    // co-reference); 2 = writes a value already at +1 -> TRANSFER, no retain (an
    // in-language call/opm result, or MoveClear). Both 1 and 2 release the slot's
    // OLD reference (IndexStore releases it BEFORE the op — see the loop hook).
    auto destKind = [](Op op) -> int {
        switch (op) {
            case Op::Jump: case Op::JumpIfFalse: case Op::JumpIfTrue:
            case Op::Print: case Op::PrintNl: case Op::Ret: case Op::RetVoid:
            case Op::SetMember: case Op::RawSet:   // IndexStore is dk=1 (writes the new container into in.a)
            case Op::StoreGlobal: case Op::CaptureVar: case Op::Throw:
            case Op::VFree:      // frees in.a's value; clears the slot itself
                return 0;
            case Op::Call: case Op::CallDyn: case Op::CallValue:
            case Op::CallNativeFn:   // native helpers return +1 (arr_append etc.) — transfer
            case Op::Arith: case Op::MoveClear:
            case Op::NewObject:   // retains itself internally (before $init) — transfer
                return 2;
            default:
                return 1;   // every other op writes an owned/borrowed value into in.a
        }
    };

    if (std::getenv("LANG_IR_DUMP")) {
        fprintf(stderr, "== fn %s (nregs %d)\n", fn.name.c_str(), fn.nregs);
        for (size_t pc = 0; pc < fn.code.size(); ++pc) {
            const Inst& in = fn.code[pc];
            fprintf(stderr, "  @%zu op=%d a=%d b=%d c=%d d=%d %s\n", pc, (int)in.op,
                    in.a, in.b, in.c, in.d, in.sname.c_str());
        }
    }
    // Jump-target map for the IndexStore rebind-chain peephole below: the
    // chain's stale-temp release is only valid on straight-line code.
    std::vector<bool> jumpTarget(fn.code.size() + 1, false);
    for (const Inst& ji : fn.code) {
        if (ji.op == Op::Jump && ji.a >= 0 && ji.a <= (int)fn.code.size())
            jumpTarget[ji.a] = true;
        if ((ji.op == Op::JumpIfFalse || ji.op == Op::JumpIfTrue) &&
            ji.b >= 0 && ji.b <= (int)fn.code.size())
            jumpTarget[ji.b] = true;
    }
    for (const Handler& h : fn.handlers)
        if (h.handlerPc >= 0 && h.handlerPc <= (int)fn.code.size())
            jumpTarget[h.handlerPc] = true;

    for (size_t pc = 0; pc < fn.code.size() && ok_; ++pc) {
        pcOff[pc] = a_.here();
        const Inst& in = fn.code[pc];
        int dk = destKind(in.op);
        if (dk && in.op == Op::IndexStore) {
            // §11/§15 COW: the dest temp still holds the PREVIOUS IndexStore
            // result — in an `arr[i] = v` loop that stale +1 makes a uniquely-
            // owned base look shared (rc 2), defeating idxset's in-place path
            // and turning the loop O(n^2). Release it BEFORE the op instead of
            // stashing. Safe: if an op input aliases it, that input's own slot
            // holds a counted ref too (every reg write retains), so rc >= 2 and
            // this release cannot free a live input. The post-hook then sees a
            // void old-ref (release no-op) and only retains the result.
            loadVal(RDI, RSI, in.a);
            callFixups_.push_back({a_.call(), -66});          // release stale dest
            a_.xorRR(RAX, RAX);
            a_.store(RAX, slot(in.a)); a_.store(RAX, slot(in.a) + 8);
            a_.store(RAX, arcScratch); a_.store(RAX, arcScratch + 8);
            // lowerAssign's rebind chain `CopyVal t<-nb; Move L<-t` parks ONE
            // more stale +1 in t (registers are never reused, so t is written
            // only by that CopyVal). Release it here too — otherwise the base
            // still reads rc 2 at the check, one temp behind, forever. Only on
            // straight-line code (no jump may land between the chain's ops).
            if (pc + 2 < fn.code.size()) {
                const Inst& n1 = fn.code[pc + 1];
                const Inst& n2 = fn.code[pc + 2];
                if (n1.op == Op::CopyVal && n1.b == in.a &&
                    n2.op == Op::Move && n2.b == n1.a &&
                    n1.a != in.b && n1.a != in.c && n1.a != in.d &&
                    !jumpTarget[pc + 1] && !jumpTarget[pc + 2]) {
                    loadVal(RDI, RSI, n1.a);
                    callFixups_.push_back({a_.call(), -66});  // release stale chain temp
                    a_.xorRR(RAX, RAX);
                    a_.store(RAX, slot(n1.a)); a_.store(RAX, slot(n1.a) + 8);
                }
            }
        } else if (dk) {   // stash the dest slot's OLD (tag,pay); released AFTER the op runs
            loadVal(RAX, RCX, in.a);
            a_.store(RAX, arcScratch); a_.store(RCX, arcScratch + 8);
        }
        switch (in.op) {
            case Op::LoadConst: {
                const Value& c = fn.consts[in.b];
                if (c.kind == VKind::String) {
                    addrImm(RAX, internString(c.s)); storeTagged(in.a, 4, RAX);
                } else if (c.kind == VKind::Int) {
                    a_.movImm(RAX, (uint64_t)c.i); storeTagged(in.a, 1, RAX);
                } else if (c.kind == VKind::Bool) {
                    a_.movImm(RAX, c.b ? 1 : 0); storeTagged(in.a, 3, RAX);
                } else if (c.kind == VKind::Float) {
                    uint64_t bits; double d = c.f; __builtin_memcpy(&bits, &d, 8);
                    a_.movImm(RAX, bits); storeTagged(in.a, 2, RAX);   // float bits in payload
                } else if (c.kind == VKind::None) {
                    a_.xorRR(RAX, RAX); storeTagged(in.a, 8, RAX);
                } else {
                    a_.xorRR(RAX, RAX); storeTagged(in.a, 0, RAX);     // void/other
                }
                break;
            }
            case Op::Default: {
                const std::string& t = in.sname;
                if (t == "string") { addrImm(RAX, 16); storeTagged(in.a, 4, RAX); }
                else if (t == "int") { a_.xorRR(RAX, RAX); storeTagged(in.a, 1, RAX); }
                else if (t == "bool") { a_.xorRR(RAX, RAX); storeTagged(in.a, 3, RAX); }
                else if (t == "float") { a_.xorRR(RAX, RAX); storeTagged(in.a, 2, RAX); }
                else if (t == "None" ||
                         (t.find(" | ") != std::string::npos && t.find("None") != std::string::npos)) {
                    a_.xorRR(RAX, RAX); storeTagged(in.a, 8, RAX);
                } else if (t.rfind("Array", 0) == 0) {
                    a_.movImm(RDI, 0); callFixups_.push_back({a_.call(), -18}); storeTagged(in.a, 6, RAX);
                } else if (t.rfind("Map", 0) == 0) {
                    a_.movImm(RDI, 0); callFixups_.push_back({a_.call(), -19}); storeTagged(in.a, 7, RAX);
                } else { a_.xorRR(RAX, RAX); storeTagged(in.a, 0, RAX); }  // unmodeled -> void
                break;
            }
            case Op::MoveClear:   // ownership transfer: copy without retaining, then CLEAR
                a_.load(RAX, slot(in.b));     a_.store(RAX, slot(in.a));
                a_.load(RAX, slot(in.b) + 8); a_.store(RAX, slot(in.a) + 8);
                // §15: the source gives up its reference (transfer) — clear it so
                // frame-exit / reuse does not release the value a second time.
                a_.xorRR(RAX, RAX); a_.store(RAX, slot(in.b));
                break;
            case Op::Move: {
                a_.load(RAX, slot(in.b));     a_.store(RAX, slot(in.a));       // tag
                a_.load(RDX, slot(in.b) + 8); a_.store(RDX, slot(in.a) + 8);   // payload
                break;   // §15: boxed-array retain is now the uniform op-hook (dk=1)
            }
            case Op::CopyVal:
                beginAlloc((int)pc);            // §15: a scope-owned value-struct copy -> arena
                a_.load(RDI, slot(in.b));       // tag
                a_.load(RSI, slot(in.b) + 8);   // payload
                callFixups_.push_back({a_.call(), -62});   // copyval -> rax=tag, rdx=pay
                a_.store(RAX, slot(in.a));
                a_.store(RDX, slot(in.a) + 8);
                endAlloc((int)pc);
                break;
            case Op::Arith: {
                // Fully runtime-dispatched (like the emit-C++ reference): an
                // object left operand goes to the operator-method dispatcher
                // (opm); everything else to the scalar core (ar). This is
                // tag-driven, so prelude method bodies (whose operand kinds the
                // checker never typed) concatenate/compare correctly.
                a_.load(RSI, slot(in.b)); a_.load(RDX, slot(in.b) + 8);   // l tag, pay
                a_.load(RCX, slot(in.c)); a_.load(R8, slot(in.c) + 8);    // r tag, pay
                a_.movImm(RDI, (uint64_t)opCode(in.tk));
                a_.cmpImm(RSI, 5); size_t notObj = a_.jne();
                a_.cmpImm(RCX, 8); size_t noneCmp = a_.je();   // obj vs None -> ar (tag compare)
                callFixups_.push_back({a_.call(), -15});                  // opm
                storeVal(in.a, RAX, RDX);
                size_t doneObj = a_.jmp();
                a_.patchRel(notObj, a_.here()); a_.patchRel(noneCmp, a_.here());
                callFixups_.push_back({a_.call(), -16});                  // ar
                storeVal(in.a, RAX, RDX);
                a_.patchRel(doneObj, a_.here());
                throwCheck((int)pc);        // ar can now raise (shift out of range)
                break;
            }
            case Op::Not:
                loadPay(RAX, in.b); a_.testRR(RAX); a_.setccAx(0x94);   // ==0
                storeTagged(in.a, 3, RAX);
                break;
            case Op::Neg: {
                a_.load(RCX, slot(in.b));                     // tag
                loadPay(RAX, in.b);
                a_.cmpImm(RCX, 2); size_t negInt = a_.jne();
                a_.movImm(RDX, 0x8000000000000000ULL);        // float: flip the sign bit
                a_.xorRR(RAX, RDX); storeTagged(in.a, 2, RAX);
                size_t negDone = a_.jmp();
                a_.patchRel(negInt, a_.here());
                a_.neg(RAX); storeTagged(in.a, 1, RAX);
                a_.patchRel(negDone, a_.here());
                break;
            }
            case Op::Jump:
                jumps.push_back({a_.jmp(), in.a});
                break;
            case Op::JumpIfFalse:
                loadPay(RAX, in.a); a_.testRR(RAX);
                jumps.push_back({a_.je(), in.b});
                break;
            case Op::JumpIfTrue:
                loadPay(RAX, in.a); a_.testRR(RAX);
                jumps.push_back({a_.jne(), in.b});
                break;
            case Op::Call: {
                for (int k = in.d - 1; k >= 0; --k) {  // push 16-byte args right-to-left
                    a_.load(RAX, slot(in.c + k) + 8); a_.push(RAX);   // payload (higher addr)
                    a_.load(RAX, slot(in.c + k));     a_.push(RAX);   // tag (lower addr)
                }
                callFixups_.push_back({a_.call(), in.b});
                if (in.d > 0) a_.addImm(RSP, 16 * in.d);
                storeVal(in.a, RAX, RDX);              // result: RAX=tag, RDX=payload
                throwCheck((int)pc);
                break;
            }
            case Op::Ret:
                // §15: return at +1 — retain the value so releaseAllRegs (which
                // releases the callee's slots, including the one holding it) nets
                // the caller a single owning reference. The caller stores the
                // result by TRANSFER (no retain), balancing this.
                loadVal(RDI, RSI, in.a);
                callFixups_.push_back({a_.call(), -65});   // retain returned value
                releaseAllRegs();
                restoreArena();                        // free scope-owned before returning
                loadVal(RAX, RDX, in.a); a_.leave(); a_.ret();
                break;
            case Op::RetVoid:
                releaseAllRegs();
                restoreArena();
                a_.xorRR(RAX, RAX); a_.xorRR(RDX, RDX); a_.leave(); a_.ret();
                break;
            case Op::Print:
                a_.load(RDI, slot(in.a)); a_.load(RSI, slot(in.a) + 8);
                callFixups_.push_back({a_.call(), -9});   // print_val (runtime tag dispatch)
                break;
            case Op::PrintNl:
                callFixups_.push_back({a_.call(), -2});   // -2 => print_nl
                break;
            case Op::NewObject: {
                if (in.c == 1) {
                    // §9: bare value-struct FIELD auto-construct (emitted only
                    // inside $init, r0 = this) — allocate in THIS's tier: an
                    // arena outer gets arena inners (they die together), a heap
                    // outer gets heap inners (recursiveFree vfrees them).
                    addrImm(RCX, 56); a_.loadInd(RAX, RCX); a_.push(RAX);   // save flag
                    loadPay(RAX, 0); a_.loadMem(RAX, RAX, -16);             // this's refcount
                    a_.shrImm(RAX, 63);                                     // -1 sentinel -> 1, else 0
                    addrImm(RCX, 56); a_.storeInd(RCX, RAX);
                    a_.movImm(RDI, (uint64_t)(in.sym ? clsId(in.sym) : 0));
                    callFixups_.push_back({a_.call(), -10});                // mkobj -> rax
                    storeTagged(in.a, 5, RAX);
                    a_.pop(RAX); addrImm(RCX, 56); a_.storeInd(RCX, RAX);   // restore flag
                } else {
                beginAlloc((int)pc);
                a_.movImm(RDI, (uint64_t)(in.sym ? clsId(in.sym) : 0));
                callFixups_.push_back({a_.call(), -10});   // mkobj -> rax
                storeTagged(in.a, 5, RAX);
                endAlloc((int)pc);                         // clear BEFORE $init (its frame owns its arena)
                }
                // §15: own the object at +1 BEFORE $init runs. $init takes `this`
                // as a borrowed param (retain on entry, release at frame exit); if
                // the object were still at +0 that release would free it mid-init.
                // NewObject is TRANSFER (dk=2), so this retain is the dest's one ref.
                loadVal(RDI, RSI, in.a);
                callFixups_.push_back({a_.call(), -65});   // retain
                if (in.b >= 0) {                           // run the class's $init(this)
                    a_.load(RAX, slot(in.a) + 8); a_.push(RAX);   // payload
                    a_.load(RAX, slot(in.a));     a_.push(RAX);   // tag
                    callFixups_.push_back({a_.call(), in.b});
                    a_.addImm(RSP, 16);
                }
                break;
            }
            case Op::GetMember:
                loadPay(RDI, in.b);
                addrImm(RSI, internString(stripColons(in.sname)));
                addrImm(RDX, internString(in.sname));
                callFixups_.push_back({a_.call(), -13});   // getm -> rax,rdx
                storeVal(in.a, RAX, RDX);
                break;
            case Op::SetMember:
                // §15: the field owns its value — release the field's OLD ref and
                // retain the new one (so a read that borrows+drops it can't free it).
                loadPay(RDI, in.b); addrImm(RSI, internString(in.sname));
                callFixups_.push_back({a_.call(), -11});   // getfield -> old (rax tag, rdx pay)
                a_.store(RAX, arcScratch); a_.store(RDX, arcScratch + 8);
                loadPay(RDI, in.b);
                addrImm(RSI, internString(stripColons(in.sname)));
                addrImm(RDX, internString(in.sname));
                a_.load(RCX, slot(in.a)); a_.load(R8, slot(in.a) + 8);   // val (tag, pay)
                callFixups_.push_back({a_.call(), -14});   // setm
                a_.load(RDI, arcScratch); a_.load(RSI, arcScratch + 8);
                callFixups_.push_back({a_.call(), -66});   // release old field ref
                loadVal(RDI, RSI, in.a);
                callFixups_.push_back({a_.call(), -65});   // retain new field ref
                break;
            case Op::RawGet:
                if (in.d > 0) {                            // §7: compile-time packed slot
                    int off = 16 + (in.d - 1) * 16;
                    loadPay(RAX, in.b);
                    a_.loadMem(RDX, RAX, off + 8);         // payload
                    a_.loadMem(RAX, RAX, off);            // tag
                    storeVal(in.a, RAX, RDX);
                } else {
                    loadPay(RDI, in.b);
                    addrImm(RSI, internString(in.sname));
                    callFixups_.push_back({a_.call(), -11});   // getfield -> rax,rdx
                    storeVal(in.a, RAX, RDX);
                }
                break;
            case Op::RawSet:
                if (in.d > 0) {                            // §7: direct store to the slot
                    int off = 16 + (in.d - 1) * 16;
                    loadPay(RAX, in.b);
                    a_.loadMem(RDI, RAX, off); a_.loadMem(RSI, RAX, off + 8);   // §15 old field ref
                    a_.store(RDI, arcScratch); a_.store(RSI, arcScratch + 8);
                    a_.load(RDX, slot(in.a)); a_.load(RCX, slot(in.a) + 8);
                    a_.storeMem(RAX, off, RDX); a_.storeMem(RAX, off + 8, RCX);
                } else {
                    loadPay(RDI, in.b);
                    addrImm(RSI, internString(in.sname));
                    callFixups_.push_back({a_.call(), -11});   // §15 read old ref
                    a_.store(RAX, arcScratch); a_.store(RDX, arcScratch + 8);
                    loadPay(RDI, in.b);
                    addrImm(RSI, internString(in.sname));
                    a_.load(RDX, slot(in.a)); a_.load(RCX, slot(in.a) + 8);  // valtag, valpay
                    callFixups_.push_back({a_.call(), -12});   // setfield
                }
                a_.load(RDI, arcScratch); a_.load(RSI, arcScratch + 8);
                callFixups_.push_back({a_.call(), -66});       // release old field ref
                loadVal(RDI, RSI, in.a);
                callFixups_.push_back({a_.call(), -65});       // retain new field ref
                break;
            case Op::CallDyn: {
                if (in.decl && mod_.byDecl.count(in.decl)) {   // resolved: direct call
                    for (int k = in.d - 1; k >= 0; --k) {
                        a_.load(RAX, slot(in.c + k) + 8); a_.push(RAX);
                        a_.load(RAX, slot(in.c + k));     a_.push(RAX);
                    }
                    callFixups_.push_back({a_.call(), mod_.byDecl.at(in.decl)});
                    if (in.d > 0) a_.addImm(RSP, 16 * in.d);
                    storeVal(in.a, RAX, RDX);
                    throwCheck((int)pc);
                } else if (in.sname == "pow") {
                    // Track 06 §2: float.pow(float) is the one native-METHOD
                    // (not std::math free-function) in the deferred
                    // transcendental group — genCallNative's dynamic dispatch
                    // is a single shared runtime routine generated once for
                    // every program, so an unmatched-name row there silently
                    // returns void instead of failing (a pre-existing gap:
                    // string.byteAt/toFloat and Array.concatAll have the same
                    // hole today, unrelated to this track — filed as bug.md
                    // #18). This per-callsite check is the only place a
                    // clean, program-scoped coverage diagnostic can land
                    // without breaking every OTHER program that never calls
                    // pow(), matching the existing "native-elf backend: ..."
                    // pattern run_elf.sh already treats as a clean skip.
                    fail("float native 'pow' not yet emitted (ELF: transcendental math deferred)");
                } else {                                   // unresolved: name-based callm
                    for (int k = in.d - 1; k >= 1; --k) {  // push args (window[1..])
                        a_.load(RAX, slot(in.c + k) + 8); a_.push(RAX);
                        a_.load(RAX, slot(in.c + k));     a_.push(RAX);
                    }
                    a_.load(RDI, slot(in.c)); a_.load(RSI, slot(in.c) + 8);   // receiver
                    addrImm(RDX, internString(in.sname));
                    a_.movImm(RCX, (uint64_t)(in.d - 1));  // argc
                    callFixups_.push_back({a_.call(), -20});   // callm
                    if (in.d > 1) a_.addImm(RSP, 16 * (in.d - 1));
                    storeVal(in.a, RAX, RDX);
                    throwCheck((int)pc);
                }
                // §15: a consumed receiver (COW self-append, in.b=1) had its buffer's
                // fate taken by the callee — clear the window slot WITHOUT releasing.
                if (in.b) { a_.xorRR(RAX, RAX); a_.store(RAX, slot(in.c)); }
                break;
            }
            case Op::NewArray:
                beginAlloc((int)pc);
                a_.movImm(RDI, 0);
                callFixups_.push_back({a_.call(), -18});   // mkarr(0)
                storeTagged(in.a, 6, RAX);
                for (int k = 0; k < in.d; ++k) {
                    loadPay(RDI, in.a);
                    a_.load(RSI, slot(in.c + k)); a_.load(RDX, slot(in.c + k) + 8);
                    callFixups_.push_back({a_.call(), -26});   // arr_spread
                    storeTagged(in.a, 6, RAX);
                }
                endAlloc((int)pc);
                break;
            case Op::NewArraySized:
                beginAlloc((int)pc);
                if (in.d == 2) {
                    loadPay(RDI, in.c);                        // n
                    a_.load(RSI, slot(in.c + 1)); a_.load(RDX, slot(in.c + 1) + 8);  // fill tag, pay
                    callFixups_.push_back({a_.call(), -63});   // arr_fill (dense if struct)
                    storeTagged(in.a, 6, RAX);
                } else {
                    a_.movImm(RDI, 0);
                    callFixups_.push_back({a_.call(), -18});
                    storeTagged(in.a, 6, RAX);
                }
                endAlloc((int)pc);
                break;
            case Op::NewMap:
                beginAlloc((int)pc);
                a_.movImm(RDI, 0);
                callFixups_.push_back({a_.call(), -19});   // mkmap(0)
                storeTagged(in.a, 7, RAX);
                endAlloc((int)pc);
                break;
            case Op::MakeRange:
                beginAlloc((int)pc);
                a_.movImm(RDI, (uint64_t)lookupClsId("Range"));
                callFixups_.push_back({a_.call(), -10});   // mkobj
                storeTagged(in.a, 5, RAX);
                loadPay(RDI, in.a); addrImm(RSI, internString("start"));
                a_.load(RDX, slot(in.b)); a_.load(RCX, slot(in.b) + 8);
                callFixups_.push_back({a_.call(), -12});   // setfield
                loadPay(RDI, in.a); addrImm(RSI, internString("end"));
                a_.load(RDX, slot(in.c)); a_.load(RCX, slot(in.c) + 8);
                callFixups_.push_back({a_.call(), -12});
                endAlloc((int)pc);
                break;
            case Op::GetIndex:
                a_.load(RDI, slot(in.b)); a_.load(RSI, slot(in.b) + 8);
                a_.load(RDX, slot(in.c)); a_.load(RCX, slot(in.c) + 8);
                callFixups_.push_back({a_.call(), -22});   // idxget
                storeVal(in.a, RAX, RDX);
                throwCheck((int)pc);
                break;
            case Op::IndexStore:
                a_.load(RDI, slot(in.b)); a_.load(RSI, slot(in.b) + 8);
                a_.load(RDX, slot(in.c)); a_.load(RCX, slot(in.c) + 8);
                a_.load(R8, slot(in.d)); a_.load(R9, slot(in.d) + 8);
                callFixups_.push_back({a_.call(), -23});   // idxset -> new base
                storeVal(in.a, RAX, RDX);
                throwCheck((int)pc);
                break;
            case Op::IterLen: {
                a_.load(RAX, slot(in.b));
                a_.cmpImm(RAX, 5); size_t rng = a_.je();
                a_.cmpImm(RAX, 6); size_t ar1 = a_.je();
                a_.cmpImm(RAX, 7); size_t ar2 = a_.je();
                a_.xorRR(RAX, RAX); storeTagged(in.a, 1, RAX);   // non-iterable -> 0
                size_t d1 = a_.jmp();
                a_.patchRel(ar1, a_.here()); a_.patchRel(ar2, a_.here());
                loadPay(RCX, in.b); a_.loadInd(RAX, RCX);
                a_.shlImm(RAX, 1); a_.shrImm(RAX, 1);            // mask the dense-array marker bit
                storeTagged(in.a, 1, RAX);
                size_t d2 = a_.jmp();
                a_.patchRel(rng, a_.here());
                loadPay(RDI, in.b); addrImm(RSI, internString("start"));
                callFixups_.push_back({a_.call(), -11}); a_.movRR(R10, RDX);   // start
                loadPay(RDI, in.b); addrImm(RSI, internString("end"));
                callFixups_.push_back({a_.call(), -11});                       // rdx=end
                a_.movRR(RAX, RDX); a_.subRR(RAX, R10); a_.addImm(RAX, 1);
                a_.testRR(RAX); size_t pos = a_.jcc(9);   // jns (>=0)
                a_.xorRR(RAX, RAX);
                a_.patchRel(pos, a_.here());
                storeTagged(in.a, 1, RAX);
                a_.patchRel(d1, a_.here()); a_.patchRel(d2, a_.here());
                break;
            }
            case Op::IterAt:
                a_.load(RDI, slot(in.b)); a_.load(RSI, slot(in.b) + 8);
                a_.load(RDX, slot(in.c) + 8);              // index payload
                callFixups_.push_back({a_.call(), -24});   // iterat
                storeVal(in.a, RAX, RDX);
                break;
            case Op::MakeClosure:
                beginAlloc((int)pc);
                a_.movImm(RDI, (uint64_t)in.b);            // fnId stored at [clo+0]
                callFixups_.push_back({a_.call(), -10});   // mkobj (reused: word0 = fnId)
                storeTagged(in.a, 9, RAX);
                endAlloc((int)pc);
                break;
            case Op::CaptureVar:
                loadPay(RDI, in.a); addrImm(RSI, internString(in.sname));
                a_.load(RDX, slot(in.b)); a_.load(RCX, slot(in.b) + 8);
                callFixups_.push_back({a_.call(), -58});   // capset (dynamic key)
                loadVal(RDI, RSI, in.b);                   // §15: the capture node owns the value
                callFixups_.push_back({a_.call(), -65});   // retain (balanced by recursiveFree)
                break;
            case Op::LoadCapture:
                loadPay(RDI, 0);                           // r0 = the closure
                addrImm(RSI, internString(in.sname));
                callFixups_.push_back({a_.call(), -57});   // capget (dynamic key)
                storeVal(in.a, RAX, RDX);
                break;
            case Op::CallValue: {
                for (int k = in.d - 1; k >= 1; --k) {      // push args (window[1..])
                    a_.load(RAX, slot(in.c + k) + 8); a_.push(RAX);
                    a_.load(RAX, slot(in.c + k));     a_.push(RAX);
                }
                loadPay(RDI, in.c);                        // closure payload (window[0])
                a_.movImm(RCX, (uint64_t)(in.d - 1));      // argc
                callFixups_.push_back({a_.call(), -27});   // callclosure
                if (in.d > 1) a_.addImm(RSP, 16 * (in.d - 1));
                storeVal(in.a, RAX, RDX);
                throwCheck((int)pc);
                break;
            }
            case Op::LoadGlobal:
                addrImm(RCX, 64 + 16 * in.b);
                a_.loadMem(RAX, RCX, 0); a_.loadMem(RDX, RCX, 8);
                storeVal(in.a, RAX, RDX);
                break;
            case Op::StoreGlobal:
                addrImm(RCX, 64 + 16 * in.b);
                a_.load(RAX, slot(in.a)); a_.storeMem(RCX, 0, RAX);
                a_.load(RAX, slot(in.a) + 8); a_.storeMem(RCX, 8, RAX);
                loadVal(RDI, RSI, in.a);                   // §15: the global owns a reference
                callFixups_.push_back({a_.call(), -65});   // retain
                break;
            case Op::CallNativeFn: {
                const std::string& n = in.sname;
                if (n == "sysWrite") {
                    loadPay(RDI, in.c); loadPay(RSI, in.c + 1);
                    callFixups_.push_back({a_.call(), -33}); storeTagged(in.a, 1, RAX);
                } else if (n == "sysReadLine") {
                    loadPay(RDI, in.c);
                    callFixups_.push_back({a_.call(), -34}); storeTagged(in.a, 4, RAX);
                    loadVal(RDI, RSI, in.a);                   // §15: fresh heap string -> +1 (transfer)
                    callFixups_.push_back({a_.call(), -65});
                } else if (n == "sysRead") {
                    loadPay(RDI, in.c); loadPay(RSI, in.c + 1);
                    callFixups_.push_back({a_.call(), -35}); storeTagged(in.a, 4, RAX);
                    loadVal(RDI, RSI, in.a);                   // §15: fresh heap string -> +1 (transfer)
                    callFixups_.push_back({a_.call(), -65});
                } else if (n == "sysOpen") {
                    loadPay(RDI, in.c); loadPay(RSI, in.c + 1);
                    callFixups_.push_back({a_.call(), -36}); storeTagged(in.a, 1, RAX);
                } else if (n == "sysClose") {
                    loadPay(RDI, in.c);
                    callFixups_.push_back({a_.call(), -37}); storeTagged(in.a, 1, RAX);
                } else if (n == "sysStat") {
                    loadPay(RDI, in.c); loadPay(RSI, in.c + 1);
                    callFixups_.push_back({a_.call(), -38}); storeTagged(in.a, 1, RAX);
                } else if (n == "sysTimerStart") {
                    loadPay(RDI, in.c); loadPay(RSI, in.c + 1);
                    a_.load(RDX, slot(in.c + 2)); a_.load(RCX, slot(in.c + 2) + 8);  // cb
                    callFixups_.push_back({a_.call(), -40}); storeTagged(in.a, 1, RAX);
                } else if (n == "sysTimerCancel") {
                    loadPay(RDI, in.c);
                    callFixups_.push_back({a_.call(), -41});
                    a_.xorRR(RAX, RAX); storeTagged(in.a, 1, RAX);
                } else if (n == "sysTcpConnect") {
                    loadPay(RDI, in.c); loadPay(RSI, in.c + 1);
                    callFixups_.push_back({a_.call(), -46}); storeTagged(in.a, 1, RAX);
                } else if (n == "sysTcpListen") {
                    // Track 10: the SO_REUSEPORT overload (sysTcpListen/2) is a
                    // worker primitive — LLVM/interpreters only. On the frozen
                    // ELF backend it is a loud diagnostic, never a silent drop
                    // of reusePort (design §5, the no-silent-fallback rule).
                    if (in.d == 2) { fail("threads: unsupported on the ELF backend (use LLVM)"); break; }
                    loadPay(RDI, in.c);
                    callFixups_.push_back({a_.call(), -47}); storeTagged(in.a, 1, RAX);
                } else if (n == "sysAccept") {
                    loadPay(RDI, in.c);
                    callFixups_.push_back({a_.call(), -48}); storeTagged(in.a, 1, RAX);
                } else if (n == "sysSend") {
                    loadPay(RDI, in.c); loadPay(RSI, in.c + 1);
                    callFixups_.push_back({a_.call(), -49}); storeTagged(in.a, 1, RAX);
                } else if (n == "sysRecv") {
                    loadPay(RDI, in.c); loadPay(RSI, in.c + 1);
                    callFixups_.push_back({a_.call(), -50}); storeVal(in.a, RAX, RDX);
                    loadVal(RDI, RSI, in.a);                   // §15: fresh recv string -> +1
                    callFixups_.push_back({a_.call(), -65});   // (None / "" literal: gate skips)
                } else if (n == "sysWatch") {
                    loadPay(RDI, in.c);
                    a_.load(RSI, slot(in.c + 1)); a_.load(RDX, slot(in.c + 1) + 8);   // cb
                    callFixups_.push_back({a_.call(), -51}); storeTagged(in.a, 1, RAX);
                } else if (n == "sysUnwatch") {
                    loadPay(RDI, in.c);
                    callFixups_.push_back({a_.call(), -52});
                    a_.xorRR(RAX, RAX); storeTagged(in.a, 1, RAX);
                } else {
                    fail("native floor function '" + n + "'");
                }
                break;
            }
            case Op::Await: {
                // Pump the loop until the promise resolves; yield its value.
                a_.load(RAX, slot(in.b)); a_.cmpImm(RAX, 5); size_t isObj = a_.je();
                a_.load(RAX, slot(in.b)); a_.load(RDX, slot(in.b) + 8);
                storeVal(in.a, RAX, RDX); size_t notObj = a_.jmp();   // non-object: pass through
                a_.patchRel(isObj, a_.here());
                size_t awloop = a_.here();
                loadPay(RDI, in.b); addrImm(RSI, internString("ready"));
                callFixups_.push_back({a_.call(), -11});             // getfield ready
                a_.testRR(RDX); size_t ready = a_.jne();
                addrImm(RCX, 24); a_.loadInd(RAX, RCX); a_.testRR(RAX); size_t thrown = a_.jne();
                callFixups_.push_back({a_.call(), -43}); a_.testRR(RAX); size_t noWork = a_.je();
                callFixups_.push_back({a_.call(), -42});             // loop_step
                size_t ab = a_.jmp(); a_.patchRel(ab, awloop);
                a_.patchRel(ready, a_.here()); a_.patchRel(thrown, a_.here());
                a_.patchRel(noWork, a_.here());
                loadPay(RDI, in.b); addrImm(RSI, internString("value"));
                callFixups_.push_back({a_.call(), -11});             // getfield value
                storeVal(in.a, RAX, RDX);
                a_.patchRel(notObj, a_.here());
                break;
            }
            case Op::IsType: {
                std::vector<std::string> members;
                if (in.sname.find(" | ") != std::string::npos) members = unionMembersOf(in.sname);
                else members.push_back(in.sname);
                auto primTag = [](const std::string& m) -> int {
                    if (m == "None") return 8; if (m == "int") return 1;
                    if (m == "bool") return 3; if (m == "float") return 2;
                    if (m == "string") return 4;
                    if (m.rfind("Array", 0) == 0) return 6;
                    if (m.rfind("Map", 0) == 0) return 7;
                    return -1;   // a class
                };
                std::vector<size_t> matches;
                for (const std::string& m : members) {
                    int t = primTag(m);
                    if (t >= 0) {
                        a_.load(RAX, slot(in.b)); a_.cmpImm(RAX, t); matches.push_back(a_.je());
                    } else if (in.sym) {
                        a_.load(RAX, slot(in.b)); a_.cmpImm(RAX, 5); size_t nt = a_.jne();
                        loadPay(RDI, in.b); a_.loadInd(RDI, RDI);   // classId
                        a_.movImm(RSI, (uint64_t)clsId(in.sym));
                        callFixups_.push_back({a_.call(), -28});    // issub
                        a_.testRR(RAX); matches.push_back(a_.jne());
                        a_.patchRel(nt, a_.here());
                    }
                }
                a_.xorRR(RAX, RAX); storeTagged(in.a, 3, RAX);      // no match -> false
                size_t end = a_.jmp();
                for (size_t mm : matches) a_.patchRel(mm, a_.here());
                a_.movImm(RAX, 1); storeTagged(in.a, 3, RAX);
                a_.patchRel(end, a_.here());
                break;
            }
            case Op::Throw:
                addrImm(RCX, 24); a_.movImm(RAX, 1); a_.storeInd(RCX, RAX);        // g_throwing = 1
                a_.load(RAX, slot(in.a)); addrImm(RCX, 32); a_.storeInd(RCX, RAX); // g_thrown tag
                a_.load(RAX, slot(in.a) + 8); addrImm(RCX, 40); a_.storeInd(RCX, RAX);
                // §15: g_thrown owns the in-flight exception (+1) — every frame
                // unwinding between here and the catch releases its own refs, so
                // without this the exception is freed mid-flight. The catch bind
                // takes the copy uncounted; its frame-exit release consumes this.
                a_.load(RDI, slot(in.a)); a_.load(RSI, slot(in.a) + 8);
                callFixups_.push_back({a_.call(), -65});
                throwCheck((int)pc);
                break;
            case Op::VFree:
                // §15: free the dead standalone value-struct copy in in.a, then
                // clear the slot so no later hook/exit path sees the freed value.
                a_.load(RDI, slot(in.a)); a_.load(RSI, slot(in.a) + 8);
                callFixups_.push_back({a_.call(), -70});
                a_.xorRR(RAX, RAX);
                a_.store(RAX, slot(in.a)); a_.store(RAX, slot(in.a) + 8);
                break;
            default:
                fail("this op is not yet covered by the pure ELF backend (Step 2 in progress)");
                break;
        }
        if (dk) {   // release the stashed OLD dest ref; skipped if the op threw
            // If the op left the SAME (tag,pay) in the dest — a self-assign, or an
            // in-place COW (`a = a.add(x)` returns a's own buffer) — ownership is
            // unchanged, so skip the release/retain: releasing then re-retaining the
            // same pointer would free it (refcount 1->0) and resurrect it (UAF).
            a_.load(RAX, arcScratch); a_.load(RDX, slot(in.a)); a_.cmpRR(RAX, RDX);
            size_t diffTag = a_.jne();
            a_.load(RAX, arcScratch + 8); a_.load(RDX, slot(in.a) + 8); a_.cmpRR(RAX, RDX);
            size_t sameVal = a_.je();
            a_.patchRel(diffTag, a_.here());
            a_.load(RDI, arcScratch); a_.load(RSI, arcScratch + 8);
            callFixups_.push_back({a_.call(), -66});          // release old
            if (dk == 1) {                                    // owned/borrowed result -> retain new
                loadVal(RDI, RSI, in.a);
                callFixups_.push_back({a_.call(), -65});      // retain new
            }
            a_.patchRel(sameVal, a_.here());
        }
    }
    pcOff[fn.code.size()] = a_.here();
    releaseAllRegs();                                         // §15: drop all frame refs on fall-through/unwind
    restoreArena();
    a_.xorRR(RAX, RAX); a_.xorRR(RDX, RDX); a_.leave(); a_.ret();   // safety fall-through

    for (auto& [pos, tpc] : jumps) a_.patchRel(pos, pcOff[tpc]);
}

std::string X64Gen::emit() {
    arcTrace_ = std::getenv("LANG_ARC_TRACE") != nullptr;   // debug retain/release/free log
    // Reachability from the entry: direct calls, constructor $init, resolved
    // dynamic dispatch; every instantiated class pulls in its members (so the
    // generated getm/setm/opm dispatchers can reach the accessor/operator fns).
    std::vector<bool> reachable(mod_.functions.size(), false);
    std::vector<int> work{mod_.entry};
    reachable[mod_.entry] = true;
    if (mod_.ginit >= 0) { reachable[mod_.ginit] = true; work.push_back(mod_.ginit); }
    std::vector<Symbol*> instClasses;
    auto mark = [&](int idx) {
        if (idx >= 0 && idx < (int)mod_.functions.size() && !reachable[idx]) {
            reachable[idx] = true; work.push_back(idx);
        }
    };
    auto scan = [&](int fi) {
        for (const Inst& in : mod_.functions[fi].code) {
            if (in.op == Op::Call) mark(in.b);
            else if (in.op == Op::NewObject) {
                mark(in.b);                            // $init
                if (in.sym) {
                    bool seen = false;
                    for (Symbol* c : instClasses) if (c == in.sym) seen = true;
                    if (!seen) {
                        instClasses.push_back(in.sym);
                        std::vector<const Stmt*> mem; collectMembers(in.sym, mem);
                        for (const Stmt* m : mem)
                            if (mod_.byDecl.count(m)) mark(mod_.byDecl.at(m));
                    }
                }
            } else if (in.op == Op::CallDyn && in.decl && mod_.byDecl.count(in.decl))
                mark(mod_.byDecl.at(in.decl));
            else if (in.op == Op::MakeClosure) {
                mark(in.b);                            // the lambda function
                bool seen = false;
                for (int f : closureFns_) if (f == in.b) seen = true;
                if (!seen) closureFns_.push_back(in.b);
            }
        }
    };
    while (!work.empty()) { int fi = work.back(); work.pop_back(); scan(fi); }

    // If the program uses collections, seed the in-language Array/Map methods
    // (map/where/reduce/...) so name-based dispatch (callm) can reach them.
    bool usesColl = false;
    for (size_t i = 0; i < mod_.functions.size(); ++i)
        if (reachable[i])
            for (const Inst& in : mod_.functions[i].code)
                switch (in.op) {
                    case Op::NewArray: case Op::NewArraySized: case Op::NewMap:
                    case Op::GetIndex: case Op::IndexStore: case Op::IterAt: case Op::MakeRange:
                        usesColl = true; break;
                    case Op::Default:
                        // A defaulted Array/Map field (e.g. `Array items;` inside a
                        // prelude class) uses collections even when no literal /
                        // index op appears in user code.
                        if (in.sname.rfind("Array", 0) == 0 || in.sname.rfind("Map", 0) == 0)
                            usesColl = true;
                        break;
                    default: break;
                }
    if (usesColl) {
        for (const char* cn : {"Array", "Map"}) {
            Symbol* c = mod_.sema->global->lookup(cn);
            if (!c || !c->decl) continue;
            collClasses_.push_back(c);
            for (const StmtPtr& m : c->decl->body)
                if (m->kind == StmtKind::Member && m->callable && !m->isCtor &&
                    mod_.byDecl.count(m.get()))
                    mark(mod_.byDecl.at(m.get()));
        }
        while (!work.empty()) { int fi = work.back(); work.pop_back(); scan(fi); }
    }
    reachable_ = reachable;

    // callm dispatches to in-language methods of instantiated classes, the
    // collection classes, and the primitive masks.
    std::vector<Symbol*> callmClasses = instClasses;
    for (Symbol* c : collClasses_) callmClasses.push_back(c);
    for (const char* p : {"string", "int", "bool", "float"}) {
        Symbol* c = mod_.sema->global->lookup(p);
        if (c) callmClasses.push_back(c);
    }

    OwnershipInfo own = analyzeOwnership(mod_);   // §15: which allocs are scope-owned
    scopeOwned_ = own.scopeOwned;
    funcOffset_.assign(mod_.functions.size(), 0);
    // [0..15] heap cursor; [16..23] empty string; [24] g_throwing;
    // [32] g_thrown tag; [40] g_thrown payload; [48] arena cursor (§15
    // scope-owned tier); [56] use-arena flag; [64..] the globals array (16
    // bytes each); then the event-loop timer registry at loopBase_.
    loopBase_ = 64 + 16 * (mod_.nglobals > 0 ? mod_.nglobals : 0);   // [48] arena cursor, [56] use-arena flag
    watchBase_ = loopBase_ + 16 + kMaxTimers * kTimerRec;      // timer registry then watches
    pollBase_ = watchBase_ + kMaxWatch * kWatchRec;            // then pollfd + id scratch
    arcBase_ = pollBase_ + kMaxWatch * 16;                     // then the ARC region (§15 escaping tier)
    data_.assign(arcBase_ + 16 + kSizeClasses * 8 + 8, 0);     // + heap-base slot (hfree tier guard)

    // _start: mmap a heap, store the cursor, call @main, exit(0).
    size_t startOff = a_.here();
    a_.movImm(RAX, 9);                // mmap
    a_.xorRR(RDI, RDI);
    a_.movImm(RSI, kHeapBytes);       // 128 MiB
    a_.movImm(RDX, 3);                // PROT_READ|PROT_WRITE
    a_.movImm(R10, 0x22);            // MAP_PRIVATE|MAP_ANONYMOUS
    a_.movImm(R8, (uint64_t)-1);
    a_.movImm(R9, 0);
    a_.syscall_();
    addrImm(RCX, 0); a_.storeInd(RCX, RAX);       // heap_cursor = mmap base
    addrImm(RCX, arcBase_ + 16 + kSizeClasses * 8); a_.storeInd(RCX, RAX);   // heap base (hfree tier guard)
    a_.movImm(RAX, 9); a_.xorRR(RDI, RDI); a_.movImm(RSI, kHeapBytes);   // arena mmap (128 MiB)
    a_.movImm(RDX, 3); a_.movImm(R10, 0x22); a_.movImm(R8, (uint64_t)-1); a_.movImm(R9, 0);
    a_.syscall_();
    addrImm(RCX, 48); a_.storeInd(RCX, RAX);      // arena_cursor = arena base (§15 scope tier)
    addrImm(RCX, loopBase_ + 0); a_.movImm(RAX, 1); a_.storeInd(RCX, RAX);   // timer nextId = 1
    if (mod_.ginit >= 0)
        callFixups_.push_back({a_.call(), mod_.ginit});   // initialize prelude globals
    size_t startCall = a_.call();
    callFixups_.push_back({a_.call(), -44});      // run the event loop (timers/watches)
    callFixups_.push_back({a_.call(), -31});      // report any uncaught throw
    // §15 accounting: report escaping-tier peak/live heap bytes to stderr (stdout
    // is untouched, so the differential is unaffected). live > 0 at exit == leak.
    addrImm(RSI, internString("[heap] escaping-tier peak=")); a_.movImm(RDI, 2);
    callFixups_.push_back({a_.call(), -33});
    addrImm(RDI, arcBase_ + 8); a_.loadInd(RDI, RDI);
    callFixups_.push_back({a_.call(), -5});       // int_to_str(peak) -> rax = descriptor
    a_.movRR(RSI, RAX); a_.push(RAX); a_.movImm(RDI, 2); callFixups_.push_back({a_.call(), -33});
    a_.pop(RCX); emitStrTempFree(RCX);            // §15: free the meter's own temp BEFORE reading live
    addrImm(RSI, internString(" live-at-exit=")); a_.movImm(RDI, 2);
    callFixups_.push_back({a_.call(), -33});
    addrImm(RDI, arcBase_ + 0); a_.loadInd(RDI, RDI);
    callFixups_.push_back({a_.call(), -5});
    a_.movRR(RSI, RAX); a_.movImm(RDI, 2); callFixups_.push_back({a_.call(), -33});
    addrImm(RSI, internString(" bytes\n")); a_.movImm(RDI, 2);
    callFixups_.push_back({a_.call(), -33});
    a_.movImm(RAX, 60); a_.xorRR(RDI, RDI); a_.syscall_();

    genPrintInt();
    genPrintNl();
    genPrintBool();
    genPrintStr();
    genAlloc();
    genIntToStr();
    genStrConcat();
    genStrEq();
    genPrintVal();
    genMkObj();
    genGetField();
    genSetField();
    genCapGet();
    genCapSet();
    genGetm(instClasses);
    genSetm(instClasses);
    genOpm(instClasses);
    genMkArr();
    genMkMap();
    genArrAppend();
    genArrSpread();
    genArrFill();
    genTsBuild();
    genAr();
    genIterAt();
    genIdxGet(instClasses);
    genIdxSet(instClasses);
    genCallNative();
    genCallM(callmClasses);
    genCallClosure();
    for (size_t i = 0; i < mod_.functions.size() && ok_; ++i)
        if (reachable[i]) genFunction((int)i);
    if (!ok_) return "";

    // Exceptions: assign class ids across the whole hierarchy (so issub and the
    // uncaught-report class-name table are complete), then emit the helpers.
    std::function<void(Symbol*)> idChain = [&](Symbol* c) {
        if (!c || !c->decl) return;
        clsId(c);
        for (const TypeRefPtr& b : c->decl->bases) idChain(b->resolvedSymbol);
    };
    std::vector<Symbol*> known;
    for (auto& [s, i] : clsId_) known.push_back(s);
    for (Symbol* s : known) idChain(s);
    if (Symbol* rte = mod_.sema->global->lookup("RuntimeException")) idChain(rte);
    genFieldCount();
    genFieldIndex();
    genIsValueClass();
    genCopyVal();
    genHfree();
    genHalloc();
    genTrace();
    genRetain();
    genRelease();
    genRecursiveFree();
    genIsSub();
    genRaise();
    genRaiseOob();
    genUncaught();
    genCstr();
    genSysWrite();
    genSysReadLine();
    genSysRead();
    genSysOpen();
    genSysClose();
    genSysStat();
    genNowNs();
    genTimerAdd();
    genTimerCancel();
    genHasWork();
    genLoopStep();
    genRunLoop();
    genParseIp();
    genTcpConnect();
    genTcpListen();
    genAccept();
    genSend();
    genRecv();
    genWatchAdd();
    genWatchCancel();
    genVFree();
    genStrIndexOf();
    genStrSubStr();
    genStrToInt();
    genStrTrim();
    genStrCase();
    genFloatToStr();

    // resolve calls (negative indices are runtime helpers)
    a_.patchRel(startCall, funcOffset_[mod_.entry]);
    for (auto& [pos, fi] : callFixups_) {
        size_t target = fi == -1 ? printIntOff_  : fi == -2 ? printNlOff_
                       : fi == -3 ? printBoolOff_ : fi == -4 ? allocOff_
                       : fi == -5 ? intToStrOff_  : fi == -6 ? strConcatOff_
                       : fi == -7 ? printStrOff_  : fi == -8 ? strEqOff_
                       : fi == -9 ? printValOff_  : fi == -10 ? mkObjOff_
                       : fi == -11 ? getFieldOff_ : fi == -12 ? setFieldOff_
                       : fi == -13 ? getmOff_     : fi == -14 ? setmOff_
                       : fi == -15 ? opmOff_      : fi == -16 ? arOff_
                       : fi == -17 ? tsBuildOff_  : fi == -18 ? mkArrOff_
                       : fi == -19 ? mkMapOff_    : fi == -20 ? callmOff_
                       : fi == -21 ? callNativeOff_ : fi == -22 ? idxGetOff_
                       : fi == -23 ? idxSetOff_   : fi == -24 ? iterAtOff_
                       : fi == -25 ? arrAppendOff_ : fi == -26 ? arrSpreadOff_
                       : fi == -27 ? callClosureOff_ : fi == -28 ? issubOff_
                       : fi == -29 ? raiseOff_    : fi == -30 ? raiseOobOff_
                       : fi == -31 ? uncaughtOff_ : fi == -32 ? cstrOff_
                       : fi == -33 ? sysWriteOff_ : fi == -34 ? sysReadLineOff_
                       : fi == -35 ? sysReadOff_  : fi == -36 ? sysOpenOff_
                       : fi == -37 ? sysCloseOff_ : fi == -38 ? sysStatOff_
                       : fi == -39 ? nowNsOff_    : fi == -40 ? timerAddOff_
                       : fi == -41 ? timerCancelOff_ : fi == -42 ? loopStepOff_
                       : fi == -43 ? hasWorkOff_  : fi == -44 ? runLoopOff_
                       : fi == -45 ? parseIpOff_  : fi == -46 ? tcpConnectOff_
                       : fi == -47 ? tcpListenOff_ : fi == -48 ? acceptOff_
                       : fi == -49 ? sendOff_     : fi == -50 ? recvOff_
                       : fi == -51 ? watchAddOff_ : fi == -52 ? watchCancelOff_
                       : fi == -53 ? strIndexOfOff_ : fi == -54 ? strSubStrOff_
                       : fi == -55 ? strToIntOff_ : fi == -56 ? strTrimOff_
                       : fi == -57 ? capGetOff_   : fi == -58 ? capSetOff_
                       : fi == -59 ? fieldCountOff_ : fi == -60 ? fieldIndexOff_
                       : fi == -61 ? isValueClassOff_ : fi == -62 ? copyValOff_
                       : fi == -63 ? arrFillOff_       : fi == -64 ? hfreeOff_
                       : fi == -65 ? retainOff_        : fi == -66 ? releaseOff_
                       : fi == -67 ? recursiveFreeOff_ : fi == -68 ? traceOff_
                       : fi == -69 ? hallocOff_
                       : fi == -70 ? vfreeOff_
                       : fi == -71 ? strCaseOff_
                       : fi == -72 ? floatToStrOff_
                       : funcOffset_[fi];
        a_.patchRel(pos, target);
    }

    // Patch data-address immediates now that the data segment's vaddr is known.
    const uint64_t BASE = 0x400000, codeOff = 64 + 56;
    uint64_t dataVAddr = BASE + codeOff + a_.code.size();
    for (auto& [pos, off] : dataFixups_) {
        uint64_t v = dataVAddr + off;
        for (int i = 0; i < 8; ++i) a_.code[pos + i] = (v >> (8 * i)) & 0xff;
    }

    uint64_t dv = 0;
    return makeElf(a_.code, startOff, data_, dv);
}
