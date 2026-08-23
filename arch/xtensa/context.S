/*
 * Scorpion thread switcher — Xtensa LX7, call0 ABI.
 *
 * Under call0 the compiler keeps callee-saved state in a12–a15 plus sp;
 * everything else (a2–a11) is caller-saved and dead across a C call.
 * The return address lives in a0, which is exactly what we need for a
 * "pc". A context is therefore just { pc, sp, a12..a15 } — see the
 * CpuContext definition in scorpion.h.
 *
 * void context_switch(CpuContext *old, CpuContext *next);
 *        args:            a2            a3
 */

    .section .text.context_switch, "ax"
    .align  4
    .global context_switch

/* Offsets into struct CpuContext */
#define CTX_PC   0
#define CTX_SP   4
#define CTX_S0   8                     /* s[0] == a12 */

context_switch:
    /* old == NULL means "first switch in": nothing to save. */
    beqz    a2, .Lload_next

    s32i    a0,  a2, CTX_PC         /* return address = resume pc      */
    s32i    a1,  a2, CTX_SP
    s32i    a12, a2, CTX_S0 + 0*4
    s32i    a13, a2, CTX_S0 + 1*4
    s32i    a14, a2, CTX_S0 + 2*4
    s32i    a15, a2, CTX_S0 + 3*4

.Lload_next:
    /* Load next's state. a3 stays valid while we pull a12–a15 because
     * those loads do not write a2/a3; sp must move last but one, with
     * the resume pc fetched after sp so no frame memory is referenced
     * once sp has changed. */
    l32i    a12, a3, CTX_S0 + 0*4
    l32i    a13, a3, CTX_S0 + 1*4
    l32i    a14, a3, CTX_S0 + 2*4
    l32i    a15, a3, CTX_S0 + 3*4
    l32i    a1,  a3, CTX_SP
    l32i    a0,  a3, CTX_PC

    jx      a0                      /* resume next */

    .size   context_switch, . - context_switch
