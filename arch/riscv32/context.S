/*
 * context_switch — Cooperative RISC-V context switch (RV32).
 *
 *   void context_switch(CpuContext *old, CpuContext *next);
 *
 *   a0 — pointer to the outgoing process context (saved)
 *   a1 — pointer to the incoming process context (restored)
 *
 * Saves/restores every register the RISC-V calling convention
 * marks callee-saved: ra, sp, gp, tp, s0–s11.
 *
 * On return execution resumes at the ra saved in *next.
 */

#define CTX_PC   0
#define CTX_RA   4
#define CTX_SP   8
#define CTX_GP   12
#define CTX_TP   16
#define CTX_S0   20
#define CTX_S1   24
#define CTX_S2   28
#define CTX_S3   32
#define CTX_S4   36
#define CTX_S5   40
#define CTX_S6   44
#define CTX_S7   48
#define CTX_S8   52
#define CTX_S9   56
#define CTX_S10  60
#define CTX_S11  64
#define CTX_MSTATUS 68

    .section .text
    .global context_switch
    .balign 4
    .type context_switch, @function

/*
 * void context_switch(CpuContext *old,
 *                     CpuContext *next);
 *
 * a0 = old context, or NULL
 * a1 = next context
 */

context_switch:

    /*
     * If old == NULL, this is a new process.
     * There is nothing to save.
     */
    beq   a0, zero, load_context


    /*
     * Save outgoing process.
     *
     * The return address in ra is the point at which
     * this process will resume when context_switch returns.
     */
    sw    ra,  CTX_PC(a0)
    sw    ra,  CTX_RA(a0)
    sw    sp,  CTX_SP(a0)
    sw    gp,  CTX_GP(a0)
    sw    tp,  CTX_TP(a0)

    sw    s0,  CTX_S0(a0)
    sw    s1,  CTX_S1(a0)
    sw    s2,  CTX_S2(a0)
    sw    s3,  CTX_S3(a0)
    sw    s4,  CTX_S4(a0)
    sw    s5,  CTX_S5(a0)
    sw    s6,  CTX_S6(a0)
    sw    s7,  CTX_S7(a0)
    sw    s8,  CTX_S8(a0)
    sw    s9,  CTX_S9(a0)
    sw    s10, CTX_S10(a0)
    sw    s11, CTX_S11(a0)

    csrr  t0, mstatus
    sw    t0,  CTX_MSTATUS(a0)


load_context:

    lw    t0,  CTX_MSTATUS(a1)
    csrw  mstatus, t0

    /*
     * Restore incoming process.
     */

    lw    ra,  CTX_RA(a1)
    lw    sp,  CTX_SP(a1)
    lw    gp,  CTX_GP(a1)
    lw    tp,  CTX_TP(a1)

    lw    s0,  CTX_S0(a1)
    lw    s1,  CTX_S1(a1)
    lw    s2,  CTX_S2(a1)
    lw    s3,  CTX_S3(a1)
    lw    s4,  CTX_S4(a1)
    lw    s5,  CTX_S5(a1)
    lw    s6,  CTX_S6(a1)
    lw    s7,  CTX_S7(a1)
    lw    s8,  CTX_S8(a1)
    lw    s9,  CTX_S9(a1)
    lw    s10, CTX_S10(a1)
    lw    s11, CTX_S11(a1)

    /*
     * Load the incoming PC into a temporary register
     * and jump to it.
     */
    lw    t0,  CTX_PC(a1)
    jr    t0