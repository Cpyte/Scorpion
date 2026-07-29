.section .text._start
.global _start
.type _start, @function

_start:
    li    t0, 0
    csrw  mstatus, t0
    csrw  mie, t0

    la    sp, _stack_top

    la    a0, _bss_start
    la    a1, _bss_end
    call  _clear_bss

    call  trap_init
    call  main

_halt:
    wfi
    j     _halt

_clear_bss:
    beq   a0, a1, 2f
1:
    sw    zero, 0(a0)
    addi  a0, a0, 4
    blt   a0, a1, 1b
2:
    ret
