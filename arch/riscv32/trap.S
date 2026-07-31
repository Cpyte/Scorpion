.equ TF_PC,   0
.equ TF_RA,   4
.equ TF_SP,   8
.equ TF_GP,   12
.equ TF_TP,   16
.equ TF_T0,   20
.equ TF_T1,   24
.equ TF_T2,   28
.equ TF_T3,   32
.equ TF_T4,   36
.equ TF_T5,   40
.equ TF_T6,   44
.equ TF_S0,   48
.equ TF_S1,   52
.equ TF_S2,   56
.equ TF_S3,   60
.equ TF_S4,   64
.equ TF_S5,   68
.equ TF_S6,   72
.equ TF_S7,   76
.equ TF_S8,   80
.equ TF_S9,   84
.equ TF_S10,  88
.equ TF_S11,  92
.equ TF_A0,   96
.equ TF_A1,   100
.equ TF_A2,   104
.equ TF_A3,   108
.equ TF_A4,   112
.equ TF_A5,   116
.equ TF_A6,   120
.equ TF_A7,   124
.equ TF_SIZE, 128
.equ TF_MCAUSE, 128
.equ TF_MEPC,   132
.equ TF_MSTATUS, 136
.equ TF_FULL,   140

    .section .text.trap
    .global trap_vector
    .global trap_init
    .balign 64

trap_vector:
    addi  sp, sp, -TF_FULL

    sw    x1, TF_RA(sp)
    addi  t0, sp, TF_FULL
    sw    t0, TF_SP(sp)
    sw    x3, TF_GP(sp)
    sw    x4, TF_TP(sp)

    sw    x5, TF_T0(sp)
    sw    x6, TF_T1(sp)
    sw    x7, TF_T2(sp)
    sw    x28, TF_T3(sp)
    sw    x29, TF_T4(sp)
    sw    x30, TF_T5(sp)
    sw    x31, TF_T6(sp)

    sw    x8,  TF_S0(sp)
    sw    x9,  TF_S1(sp)
    sw    x18, TF_S2(sp)
    sw    x19, TF_S3(sp)
    sw    x20, TF_S4(sp)
    sw    x21, TF_S5(sp)
    sw    x22, TF_S6(sp)
    sw    x23, TF_S7(sp)
    sw    x24, TF_S8(sp)
    sw    x25, TF_S9(sp)
    sw    x26, TF_S10(sp)
    sw    x27, TF_S11(sp)

    sw    x10, TF_A0(sp)
    sw    x11, TF_A1(sp)
    sw    x12, TF_A2(sp)
    sw    x13, TF_A3(sp)
    sw    x14, TF_A4(sp)
    sw    x15, TF_A5(sp)
    sw    x16, TF_A6(sp)
    sw    x17, TF_A7(sp)

    csrr  t0, mcause
    csrr  t1, mepc
    csrr  t2, mstatus
    sw    t1, TF_PC(sp)
    sw    t0, TF_MCAUSE(sp)
    sw    t1, TF_MEPC(sp)
    sw    t2, TF_MSTATUS(sp)

    mv    a0, sp
    call  trap_handler

    lw    t0, TF_PC(sp)
    lw    t1, TF_MSTATUS(sp)
    csrw  mepc,   t0
    csrw  mstatus, t1

    lw    x1,  TF_RA(sp)
    lw    x3,  TF_GP(sp)
    lw    x4,  TF_TP(sp)

    lw    x5,  TF_T0(sp)
    lw    x6,  TF_T1(sp)
    lw    x7,  TF_T2(sp)
    lw    x28, TF_T3(sp)
    lw    x29, TF_T4(sp)
    lw    x30, TF_T5(sp)
    lw    x31, TF_T6(sp)

    lw    x8,  TF_S0(sp)
    lw    x9,  TF_S1(sp)
    lw    x18, TF_S2(sp)
    lw    x19, TF_S3(sp)
    lw    x20, TF_S4(sp)
    lw    x21, TF_S5(sp)
    lw    x22, TF_S6(sp)
    lw    x23, TF_S7(sp)
    lw    x24, TF_S8(sp)
    lw    x25, TF_S9(sp)
    lw    x26, TF_S10(sp)
    lw    x27, TF_S11(sp)

    lw    x10, TF_A0(sp)
    lw    x11, TF_A1(sp)
    lw    x12, TF_A2(sp)
    lw    x13, TF_A3(sp)
    lw    x14, TF_A4(sp)
    lw    x15, TF_A5(sp)
    lw    x16, TF_A6(sp)
    lw    x17, TF_A7(sp)

    addi  sp, sp, TF_FULL
    mret

trap_init:
    la    t0, trap_vector
    csrw  mtvec, t0
    ret

    .section .text
    .global ecall_trigger
ecall_trigger:
    ecall
    ret
