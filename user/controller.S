.section .text
.globl _start
_start:
1:
    auipc a0, %pcrel_hi(user_sexec_start)
    addi  a0, a0, %pcrel_lo(1b)
2:
    auipc a1, %pcrel_hi(user_sexec_end)
    addi  a1, a1, %pcrel_lo(2b)
3:
    auipc a2, %pcrel_hi(user_sexec_start)
    addi  a2, a2, %pcrel_lo(3b)
    sub   a1, a1, a2
    li    a2, 100
    li    a7, 12
    ecall

loop:
    li    a7, 0
    ecall
    j     loop

.section .rodata
.balign 4
user_sexec_start:
    .incbin "test_user.sexec"
user_sexec_end:
