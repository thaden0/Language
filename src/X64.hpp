#pragma once
#include <cstdint>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
//  A tiny x86-64 assembler + static ELF64 writer.
//
//  This is the pure, zero-dependency native backend: it emits raw machine code
//  and a complete executable — no g++, no assembler, no linker, no libc. The
//  produced binary talks to the kernel through the `syscall` instruction only.
//  (Enough of the ISA for the register-machine IR: a stack-slot model, i64
//  arithmetic, control flow, calls, and syscalls.)
// ---------------------------------------------------------------------------

enum Reg { RAX = 0, RCX = 1, RDX = 2, RBX = 3, RSP = 4, RBP = 5, RSI = 6, RDI = 7,
           R8 = 8, R9 = 9, R10 = 10 };

class Asm {
public:
    std::vector<uint8_t> code;

    size_t here() const { return code.size(); }
    void u8(uint8_t b) { code.push_back(b); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) u8((v >> (8 * i)) & 0xff); }
    void u64(uint64_t v) { for (int i = 0; i < 8; ++i) u8((v >> (8 * i)) & 0xff); }

    static uint8_t rex(bool w, int reg, int rm, int idx = 0) {
        return 0x40 | (w ? 8 : 0) | ((reg >= 8) ? 4 : 0) | ((idx >= 8) ? 2 : 0) |
               ((rm >= 8) ? 1 : 0);
    }
    // modrm for [rbp + disp32]: mod=10, rm=101
    void memRbp(int reg, int32_t disp) {
        u8(0x80 | ((reg & 7) << 3) | 5);
        u32((uint32_t)disp);
    }

    // mov r64, imm64
    void movImm(int r, uint64_t imm) { u8(rex(true, 0, r)); u8(0xB8 | (r & 7)); u64(imm); }
    // mov r64, [rbp+disp]
    void load(int r, int32_t disp) { u8(rex(true, r, RBP)); u8(0x8B); memRbp(r, disp); }
    // mov [rbp+disp], r64
    void store(int r, int32_t disp) { u8(rex(true, r, RBP)); u8(0x89); memRbp(r, disp); }
    // mov dst, src  (reg-reg)
    void movRR(int dst, int src) {
        u8(rex(true, src, dst)); u8(0x89); u8(0xC0 | ((src & 7) << 3) | (dst & 7));
    }
    // reg-reg ALU (MR form): add 0x01, sub 0x29, cmp 0x39, xor 0x31, and 0x21, or 0x09
    void alu(uint8_t op, int dst, int src) {
        u8(rex(true, src, dst)); u8(op); u8(0xC0 | ((src & 7) << 3) | (dst & 7));
    }
    void addRR(int d, int s) { alu(0x01, d, s); }
    void subRR(int d, int s) { alu(0x29, d, s); }
    void cmpRR(int d, int s) { alu(0x39, d, s); }
    void xorRR(int d, int s) { alu(0x31, d, s); }
    void andRR(int d, int s) { alu(0x21, d, s); }
    void orRR(int d, int s)  { alu(0x09, d, s); }
    // imul dst, src (RM form 0F AF)
    void imulRR(int dst, int src) {
        u8(rex(true, dst, src)); u8(0x0F); u8(0xAF); u8(0xC0 | ((dst & 7) << 3) | (src & 7));
    }
    void cqo() { u8(0x48); u8(0x99); }
    void idiv(int r) { u8(rex(true, 0, r)); u8(0xF7); u8(0xF8 | (r & 7)); }   // /7
    void neg(int r)  { u8(rex(true, 0, r)); u8(0xF7); u8(0xD8 | (r & 7)); }   // /3
    void incR(int r) { u8(rex(true, 0, r)); u8(0xFF); u8(0xC0 | (r & 7)); }
    void decR(int r) { u8(rex(true, 0, r)); u8(0xFF); u8(0xC8 | (r & 7)); }
    void testRR(int r) { u8(rex(true, r, r)); u8(0x85); u8(0xC0 | ((r & 7) << 3) | (r & 7)); }
    // setcc al ; movzx rax, al   (cc: e=0x94 ne=0x95 l=0x9C g=0x9F le=0x9E ge=0x9D)
    void setccAx(uint8_t cc) {
        u8(0x0F); u8(cc); u8(0xC0);                    // setcc al
        u8(0x48); u8(0x0F); u8(0xB6); u8(0xC0);        // movzx rax, al
    }
    void addImm(int r, int32_t imm) { u8(rex(true, 0, r)); u8(0x81); u8(0xC0 | (r & 7)); u32((uint32_t)imm); }
    void subImm(int r, int32_t imm) { u8(rex(true, 0, r)); u8(0x81); u8(0xE8 | (r & 7)); u32((uint32_t)imm); }
    // register-indirect (base must not be rsp/rbp/r12/r13): mov dst,[base] / mov [base],src
    void loadInd(int dst, int base) { u8(rex(true, dst, base)); u8(0x8B); u8(((dst & 7) << 3) | (base & 7)); }
    void storeInd(int base, int src) { u8(rex(true, src, base)); u8(0x89); u8(((src & 7) << 3) | (base & 7)); }
    // lea dst, [base + disp32]
    void lea(int dst, int base, int32_t disp) {
        u8(rex(true, dst, base)); u8(0x8D); u8(0x80 | ((dst & 7) << 3) | (base & 7)); u32((uint32_t)disp);
    }
    // mov dst, [base + disp32] / mov [base + disp32], src  (base low3 != 4, i.e. not rsp/r12)
    void loadMem(int dst, int base, int32_t disp) {
        u8(rex(true, dst, base)); u8(0x8B); u8(0x80 | ((dst & 7) << 3) | (base & 7)); u32((uint32_t)disp);
    }
    void storeMem(int base, int32_t disp, int src) {
        u8(rex(true, src, base)); u8(0x89); u8(0x80 | ((src & 7) << 3) | (base & 7)); u32((uint32_t)disp);
    }
    // cmp reg, imm32
    void cmpImm(int r, int32_t imm) { u8(rex(true, 0, r)); u8(0x81); u8(0xF8 | (r & 7)); u32((uint32_t)imm); }
    // shl reg, imm8 / shl reg, cl
    void shlImm(int r, uint8_t imm) { u8(rex(true, 0, r)); u8(0xC1); u8(0xE0 | (r & 7)); u8(imm); }
    void shlCl(int r) { u8(rex(true, 0, r)); u8(0xD3); u8(0xE0 | (r & 7)); }
    void shrImm(int r, uint8_t imm) { u8(rex(true, 0, r)); u8(0xC1); u8(0xE8 | (r & 7)); u8(imm); }
    // sar reg, cl  (arithmetic shift right by the count in CL; /7)
    void sarCl(int r) { u8(rex(true, 0, r)); u8(0xD3); u8(0xF8 | (r & 7)); }
    // movzx dst, byte [base+disp8]   (zero-extend a byte load)
    void loadByte(int dst, int base, int8_t disp) {
        u8(rex(true, dst, base)); u8(0x0F); u8(0xB6);
        u8(0x40 | ((dst & 7) << 3) | (base & 7)); u8((uint8_t)disp);
    }

    // --- SSE2 scalar-double support (floats are IEEE doubles in the payload) --
    // xmm registers are numbered 0-7 here (no REX.R extension needed); GPR
    // operands may be r8+ (REX.B). Mandatory prefix (66/F2) precedes REX.
    // movq xmm, r64  (66 REX.W 0F 6E /r): raw double bits GPR -> xmm
    void movqXmmR(int x, int r) {
        u8(0x66); u8(rex(true, x, r)); u8(0x0F); u8(0x6E);
        u8(0xC0 | ((x & 7) << 3) | (r & 7));
    }
    // movq r64, xmm  (66 REX.W 0F 7E /r): raw double bits xmm -> GPR
    void movqRXmm(int r, int x) {
        u8(0x66); u8(rex(true, x, r)); u8(0x0F); u8(0x7E);
        u8(0xC0 | ((x & 7) << 3) | (r & 7));
    }
    // addsd/subsd/mulsd/divsd xmm_d, xmm_s  (F2 0F 58/5C/59/5E /r)
    void sseArith(uint8_t op, int d, int s) {
        u8(0xF2); u8(0x0F); u8(op); u8(0xC0 | ((d & 7) << 3) | (s & 7));
    }
    void addsd(int d, int s) { sseArith(0x58, d, s); }
    void subsd(int d, int s) { sseArith(0x5C, d, s); }
    void mulsd(int d, int s) { sseArith(0x59, d, s); }
    void divsd(int d, int s) { sseArith(0x5E, d, s); }
    // comisd xmm_a, xmm_b  (66 0F 2F /r): ordered compare -> ZF/PF/CF
    void comisd(int a, int b) {
        u8(0x66); u8(0x0F); u8(0x2F); u8(0xC0 | ((a & 7) << 3) | (b & 7));
    }
    // cvtsi2sd xmm, r64  (F2 REX.W 0F 2A /r): signed int -> double
    void cvtsi2sd(int x, int r) {
        u8(0xF2); u8(rex(true, x, r)); u8(0x0F); u8(0x2A);
        u8(0xC0 | ((x & 7) << 3) | (r & 7));
    }
    // cvttsd2si r64, xmm  (F2 REX.W 0F 2C /r): double -> signed int (truncate)
    void cvttsd2si(int r, int x) {
        u8(0xF2); u8(rex(true, r, x)); u8(0x0F); u8(0x2C);
        u8(0xC0 | ((r & 7) << 3) | (x & 7));
    }
    // sqrtsd xmm_d, xmm_s (F2 0F 51 /r)
    void sqrtsd(int d, int s) { sseArith(0x51, d, s); }
    // roundsd xmm_d, xmm_s, imm8 (66 0F 3A 0B /r ib; SSE4.1) — imm bits
    // 0=nearest 1=floor(down) 2=ceil(up) 3=trunc(toward zero); no half-away-
    // from-zero mode exists in hardware (Track 06 problem #1) — `round()` is
    // composed from this (imm=3) plus a copysign(0.5,x) pre-add.
    void roundsd(int d, int s, uint8_t imm) {
        u8(0x66); u8(0x0F); u8(0x3A); u8(0x0B);
        u8(0xC0 | ((d & 7) << 3) | (s & 7));
        u8(imm);
    }
    // signed conditional jumps (l=0xC ge=0xD le=0xE g=0xF)
    size_t jl()  { return jcc(0xC); }
    size_t jge() { return jcc(0xD); }
    size_t jle() { return jcc(0xE); }
    size_t jg()  { return jcc(0xF); }
    void repMovsb() { u8(0xF3); u8(0xA4); }
    void repStosb() { u8(0xF3); u8(0xAA); }   // fill [rdi..rdi+rcx) with al
    void repeCmpsb() { u8(0xF3); u8(0xA6); }
    void push(int r) { if (r >= 8) u8(0x41); u8(0x50 | (r & 7)); }
    void pop(int r)  { if (r >= 8) u8(0x41); u8(0x58 | (r & 7)); }
    void ret() { u8(0xC3); }
    void syscall_() { u8(0x0F); u8(0x05); }
    void leave() { u8(0xC9); }

    // Jumps: emit with a 32-bit placeholder; returns the offset of the rel32
    // field so the caller can patch it once the target is known.
    size_t jmp() { u8(0xE9); size_t p = here(); u32(0); return p; }
    // conditional jump, cc: e=4 ne=5 s=8 ns=9 l=0xC ge=0xD le=0xE g=0xF
    size_t jcc(uint8_t cc) { u8(0x0F); u8(0x80 | cc); size_t p = here(); u32(0); return p; }
    size_t je()  { return jcc(4); }
    size_t jne() { return jcc(5); }
    size_t call(){ u8(0xE8); size_t p = here(); u32(0); return p; }
    void patchRel(size_t at, size_t target) {
        int32_t rel = (int32_t)((int64_t)target - (int64_t)(at + 4));
        for (int i = 0; i < 4; ++i) code[at + i] = (rel >> (8 * i)) & 0xff;
    }
};

// Write a static ELF64 executable: headers + code (+ optional data) mapped at a
// fixed base, entry at `entryOffset` within the code. No sections, no dynamic
// linking — the kernel maps one segment and jumps to _start.
std::string makeElf(const std::vector<uint8_t>& code, size_t entryOffset,
                    const std::vector<uint8_t>& data, uint64_t& dataVAddrOut);
