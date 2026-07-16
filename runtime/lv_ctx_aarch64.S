/* LA-30 S1 (fenced) — aarch64 AAPCS64 context switch (techdesign-01 §5).
 *
 * Same rules as the x86-64 leg (G4/G8 rationale in lv_ctx_x86_64.S): opaque
 * out-of-line .S, callee-saved regset only, no syscalls. Consumed by the M4
 * qemu cross lane iff that lane executes async corpus (overview §7).
 *
 * Saved-frame layout (what lv_task.c's stack seeding must reproduce; 160 B,
 * 16-aligned; low->high):
 *   sp+0   : x19   sp+8   : x20
 *   sp+16  : x21   sp+24  : x22
 *   sp+32  : x23   sp+40  : x24
 *   sp+48  : x25   sp+56  : x26
 *   sp+64  : x27   sp+72  : x28
 *   sp+80  : x29 (fp — a fresh task seeds 0: frame-chain terminator)
 *   sp+88  : x30 (lr — a fresh task seeds the trampoline; `ret` targets it)
 *   sp+96  : d8 .. sp+152 : d15
 */
#if defined(__aarch64__) && !defined(_WIN32) && !defined(LV_CTX_UCONTEXT)

    .text

/* void lv_ctx_switch(void** save_sp, void* load_sp) */
    .globl  lv_ctx_switch
    .type   lv_ctx_switch, %function
lv_ctx_switch:
    sub  sp, sp, #160
    stp  x19, x20, [sp, #0]
    stp  x21, x22, [sp, #16]
    stp  x23, x24, [sp, #32]
    stp  x25, x26, [sp, #48]
    stp  x27, x28, [sp, #64]
    stp  x29, x30, [sp, #80]
    stp  d8,  d9,  [sp, #96]
    stp  d10, d11, [sp, #112]
    stp  d12, d13, [sp, #128]
    stp  d14, d15, [sp, #144]
    mov  x2, sp
    str  x2, [x0]             /* *save_sp = sp */
    mov  sp, x1               /* sp = load_sp  */
    ldp  x19, x20, [sp, #0]
    ldp  x21, x22, [sp, #16]
    ldp  x23, x24, [sp, #32]
    ldp  x25, x26, [sp, #48]
    ldp  x27, x28, [sp, #64]
    ldp  x29, x30, [sp, #80]
    ldp  d8,  d9,  [sp, #96]
    ldp  d10, d11, [sp, #112]
    ldp  d12, d13, [sp, #128]
    ldp  d14, d15, [sp, #144]
    add  sp, sp, #160
    ret                       /* br x30 — into the target context */
    .size lv_ctx_switch, .-lv_ctx_switch

#endif /* __aarch64__ && !_WIN32 && !LV_CTX_UCONTEXT */

#if defined(__ELF__)
    .section .note.GNU-stack,"",%progbits
#endif
