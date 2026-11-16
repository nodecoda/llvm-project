# Chapter 08: process A prints 60 lines "A\n" and exits with 7.
# The loop counter lives in s0/s1, so a lost callee-saved register across a
# timer switch would show up as a wrong line count or an endless loop.
        .section .text.start,"ax",@progbits
        .globl  _start
_start:
        li      s0, 0
        li      s1, 300
.Lloop:
        li      a0, 1
        la      a1, line
        li      a2, 2
        li      a7, 64              # SYS_write
        ecall
        addi    s0, s0, 1
        blt     s0, s1, .Lloop

        li      a0, 7
        li      a7, 93              # SYS_exit
        ecall
1:      j       1b

        .section .rodata
line:   .ascii  "A\n"

        .section .note.GNU-stack,"",@progbits
