/* LA-30 S1 (fenced) — x86-64 SysV context switch (techdesign-01 §5).
 *
 * G4: this lives in a .S file precisely so it stays an OPAQUE, out-of-line,
 * non-leaf call — never inlined, never LTO-folded into a caller (asm files
 * are opaque to LTO; keep it that way; no "portable inline-asm rewrite").
 * A compiler that inlined an rsp-swap into a frame would corrupt both
 * contexts.
 *
 * G8: the x86-64 red zone is safe *because* this is an ordinary non-leaf
 * call — the caller may not keep live data below %rsp across any call. Do
 * not "optimize" the switch into a leaf-visible intrinsic.
 *
 * Callee-saved regset only: the compiler already spilled caller-saved state
 * around this (non-leaf) call. FP env (mxcsr + x87 cw) is saved: 2 cheap
 * insns buy immunity from any future native that toggles rounding modes.
 * ~12 insns, no syscalls (G1 — the reason ucontext is fallback-only).
 *
 * Saved-frame layout (what lv_task.c's stack seeding must reproduce; low->high):
 *   sp+0  : x87 control word (2 bytes) + pad, mxcsr at sp+4   (one 8-byte slot)
 *   sp+8  : r15
 *   sp+16 : r14
 *   sp+24 : r13
 *   sp+32 : r12
 *   sp+40 : rbx
 *   sp+48 : rbp
 *   sp+56 : return address (a fresh task seeds the trampoline here)
 */
#if defined(__x86_64__) && !defined(_WIN32) && !defined(LV_CTX_UCONTEXT)

    .text

/* void lv_ctx_switch(void** save_sp, void* load_sp) */
    .globl  lv_ctx_switch
    .type   lv_ctx_switch, @function
lv_ctx_switch:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    subq  $8, %rsp            /* mxcsr (4) + x87 cw (2) + pad */
    stmxcsr 4(%rsp)
    fnstcw  (%rsp)
    movq  %rsp, (%rdi)        /* *save_sp = sp */
    movq  %rsi, %rsp          /* sp = load_sp  */
    fldcw   (%rsp)
    ldmxcsr 4(%rsp)
    addq  $8, %rsp
    popq  %r15
    popq  %r14
    popq  %r13
    popq  %r12
    popq  %rbx
    popq  %rbp
    ret                       /* returns into the target context */
    .size lv_ctx_switch, .-lv_ctx_switch

/* void lv_ctx_fpenv(void* out8) — capture the CURRENT thread's fp env in the
 * exact 8-byte slot shape the switch restores (cw at +0, mxcsr at +4). The
 * stack seeder plants this in a fresh task's frame so the first switch-in
 * restores a VALID environment inherited from the spawner (pump parity: a
 * dispatched callback used to run under the caller's fp env). Kept in asm
 * beside the switch so the slot layout has exactly one owner. */
    .globl  lv_ctx_fpenv
    .type   lv_ctx_fpenv, @function
lv_ctx_fpenv:
    movq  $0, (%rdi)
    fnstcw  (%rdi)
    stmxcsr 4(%rdi)
    ret
    .size lv_ctx_fpenv, .-lv_ctx_fpenv

#endif /* __x86_64__ && !_WIN32 && !LV_CTX_UCONTEXT */

/* Non-executable stack marker (every hand-written .S needs one or the linker
 * flags the whole binary's stack executable). Outside the arch guard: harmless
 * and correct for any ELF target that assembles this file. */
#if defined(__ELF__)
    .section .note.GNU-stack,"",@progbits
#endif
