/*
 * Scorpion / ESP32-S3 (Xtensa LX7) exception + interrupt plumbing.
 *
 * Xtensa dispatches through a VECBASE-relative table of fixed 64-byte
 * slots. Level-1 exceptions AND level-1 interrupts (everything we use)
 * land in the UserExceptionVector slot at VECBASE+0x340; the hardware
 * has already saved EPC1/EPS1/EXCCAUSE/EXCVADDR before the stub runs,
 * with EXCM=1 blocking further level-1 events while we work.
 *
 * _xt_trap_entry builds a TrapFrame (layout matches scorpion.h),
 * synthesizes an mcause — EXCCAUSE for exceptions, 0x80000000|<cpu int>
 * for interrupts, mirroring how RISC-V encodes interrupt causes — calls
 * the generic C trap_handler(), then restores and returns via `rfe`,
 * which atomically reloads PC←EPC1 and PS←EPS1. Because EXCM stays set
 * until rfe retires, the entire restore sequence is uninterruptible:
 * there is no window in which a nested tick can corrupt half-restored
 * registers.
 *
 * Built for call0: no window spill/fill ever happens (WOE off).
 */

    .section .vec.table, "ax"

/* ------------------------------------------------------------------ *
 * Vector table — must be 4 KB aligned; the linker script places this
 * section first in IRAM, so VECBASE == ORIGIN(IRAM).
 * ------------------------------------------------------------------ */
    .balign 4096
    .global _vector_table

_vector_table:
.org 0x000                         /* WindowOverflow4   (never: WOE off) */
    j       .
.org 0x040                         /* WindowUnderflow4                  */
    j       .
.org 0x080                         /* WindowOverflow8                   */
    j       .
.org 0x0C0                         /* WindowUnderflow8                  */
    j       .
.org 0x100                         /* WindowOverflow12                  */
    j       .
.org 0x140                         /* WindowUnderflow12                 */
    j       .
.org 0x180                         /* Level-2 interrupt                 */
    j       .
.org 0x1C0                         /* Level-3 interrupt                 */
    j       .
.org 0x200                         /* Level-4 interrupt                 */
    j       .
.org 0x240                         /* Level-5 interrupt                 */
    j       .
.org 0x280                         /* Debug                              */
    j       .
.org 0x2C0                         /* NMI                                */
    j       .
.org 0x300                         /* KernelException                    */
    j       .
.org 0x340                         /* UserException: level-1 exc + IRQs  */
    j       _xt_trap_entry
.org 0x380                         /* reserved                           */
    j       .
.org 0x3C0                         /* DoubleException                    */
    j       .

/* ------------------------------------------------------------------ *
 * Trap frame layout — must mirror TrapFrame in scorpion.h.
 * ------------------------------------------------------------------ */
#define TF_A(n)        ((n) * 4)
#define TF_SAR         (16 * 4)
#define TF_EXCVADDR    (17 * 4)
#define TF_PS          (18 * 4)
#define TF_PC          (19 * 4)
#define TF_CAUSE       (20 * 4)

#define FRAME_SIZE     96            /* 20 words used, pad to 16-align */

/* Synthesized mcause for "CPU interrupt 2 fired" (the only interrupt
 * Scorpion enables): 0x80000000 | 2. */
#define XT_CPU_INT_TIMER  0x80000002

    .section .text.trap, "ax"
    .align  4

    /* Tell GAS where to put any auto-generated literals (it defaults
     * to end-of-section, which the linker rejects as "placed after
     * use"). */
    .literal_position

    .global _xt_trap_entry

_xt_trap_entry:
    addi    sp, sp, -FRAME_SIZE

    /* Save the register file. a1 is stored after the stack adjustment,
     * so the frame carries the frame pointer rather than the exact
     * interrupted sp — nothing ever trap-returns to a suspended sp
     * under Scorpion (threads resume via context_switch), and rfe
     * reloads EPC1/EPS1 directly, so this is cosmetic. */
    s32i    a0,  sp, TF_A(0)
    s32i    a1,  sp, TF_A(1)
    s32i    a2,  sp, TF_A(2)
    s32i    a3,  sp, TF_A(3)
    s32i    a4,  sp, TF_A(4)
    s32i    a5,  sp, TF_A(5)
    s32i    a6,  sp, TF_A(6)
    s32i    a7,  sp, TF_A(7)
    s32i    a8,  sp, TF_A(8)
    s32i    a9,  sp, TF_A(9)
    s32i    a10, sp, TF_A(10)
    s32i    a11, sp, TF_A(11)
    s32i    a12, sp, TF_A(12)
    s32i    a13, sp, TF_A(13)
    s32i    a14, sp, TF_A(14)
    s32i    a15, sp, TF_A(15)

    rsr.sar      a2
    s32i         a2, sp, TF_SAR
    rsr.excvaddr a2
    s32i         a2, sp, TF_EXCVADDR
    rsr.epc1     a2
    s32i         a2, sp, TF_PC
    rsr.ps       a2
    s32i         a2, sp, TF_PS

    /* Exception or interrupt? Any bit set in INTERRUPT means the trap
     * was caused by a peripheral (only CPU int 2 is ever unmasked). */
    rsr.interrupt a3
    bnez       a3, .Lint
    rsr.exccause a3
    j          .Lcause_done
.Lint:
    /* 0x80000002 without relying on wide immediates: movi only takes
     * 12-bit signed values, so build 1<<31 | 2. */
    movi       a3, 1
    slli       a3, a3, 31
    addi       a3, a3, XT_CPU_INT_TIMER & 0xFFFF
.Lcause_done:
    s32i       a3, sp, TF_CAUSE

    mov  a2, sp                       /* arg0 = TrapFrame * */
    call0 trap_handler                /* may adjust frame->mepc */

    /* ---- restore ----
     * EXCM=1 keeps level-1 interrupts masked throughout, so this block
     * is atomic up to and including rfe. */
    l32i    a4,  sp, TF_A(4)
    l32i    a5,  sp, TF_A(5)
    l32i    a6,  sp, TF_A(6)
    l32i    a7,  sp, TF_A(7)
    l32i    a8,  sp, TF_A(8)
    l32i    a9,  sp, TF_A(9)
    l32i    a10, sp, TF_A(10)
    l32i    a11, sp, TF_A(11)
    l32i    a12, sp, TF_A(12)
    l32i    a13, sp, TF_A(13)
    l32i    a14, sp, TF_A(14)
    l32i    a15, sp, TF_A(15)

    l32i    a3,  sp, TF_SAR           /* borrow a3 */
    wsr.sar a3
    l32i    a3,  sp, TF_PC            /* borrow a3: possibly-adjusted pc */
    wsr.epc1 a3
    l32i    a3,  sp, TF_A(3)          /* real a3 */
    l32i    a2,  sp, TF_A(2)          /* real a2 */
    l32i    a0,  sp, TF_A(0)          /* real a0 */

    /* Pop the frame last: every register above was loaded relative to
     * it. (Negative post-pop loads are avoided deliberately — GAS
     * rewrites those into l32r sequences.) */

    addi    sp, sp, FRAME_SIZE

    rfe                               /* PC<-EPC1, PS<-EPS1, EXCM=0 */

    .size   _xt_trap_entry, . - _xt_trap_entry

/* ------------------------------------------------------------------ *
 * void ecall_trigger(void) — raises the syscall exception from kernel
 * code (used by tests / early bring-up to exercise the trap path).
 * ------------------------------------------------------------------ */
    .section .text.ecall_trigger, "ax"
    .align  4
    .global ecall_trigger

ecall_trigger:
    syscall
    ret.n

    .size   ecall_trigger, . - ecall_trigger
