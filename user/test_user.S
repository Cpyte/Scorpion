.section .text
.globl _start
_start:
    auipc a0, %pcrel_hi(msg)
    addi  a0, a0, %pcrel_lo(_start)
    li    a1, 20
    li    a7, 11
    ecall
loop:
    li    a7, 0
    ecall
    j     loop

.section .rodata
.balign 1
msg:
    .asciz "User process alive!\n"
