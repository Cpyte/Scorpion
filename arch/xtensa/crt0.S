/*
 * Scorpion / ESP32-S3 (Xtensa LX7) reset entry — call0 ABI.
 *
 * The boot ROM has already copied our loadable segments to their VMA
 * addresses in IRAM/DRAM and jumped here with all registers free.
 * We only need to:
 *   1. mask + clear every CPU interrupt
 *   2. point VECBASE at the kernel vector table
 *   3. zero .bss
 *   4. set up sp and call main()
 */

    .section .text._start, "ax"
    .align  4
    .global _start

_start:
    j       .Lreal_start
    .align  4
.Llit_vecbase:
    .word   _vector_table
.Llit_stack_top:
    .word   _stack_top
.Llit_bss_start:
    .word   _bss_start
.Llit_bss_end:
    .word   _bss_end
.Llit_main:
    .word   main

.Lreal_start:
    /* Disable every CPU interrupt source, then ack any stale pending
     * bits so nothing fires the moment we re-enable INTLEVEL 0. */
    movi    a0, 0
    wsr.intenable a0
    movi    a0, -1
    wsr.intclear a0

    /* PS = ring 0, EXCM=0, INTLEVEL=0, WOE off (call0 never uses it). */
    movi    a0, 0
    wsr.ps a0
    rsync

    l32r    a0, .Llit_vecbase
    wsr.vecbase a0
    rsync

    /* Zero .bss (a0 still holds 0). */
    l32r    a2, .Llit_bss_start
    l32r    a3, .Llit_bss_end
    beq     a2, a3, 2f
1:
    s32i    a0, a2, 0
    addi    a2, a2, 4
    blt     a2, a3, 1b
2:

    l32r    sp, .Llit_stack_top

    l32r    a0, .Llit_main
    callx0  a0                    /* main() never returns */

.Lhalt:
    waiti 0                       /* sleep until something happens */
    j       .Lhalt

    .size   _start, . - _start
