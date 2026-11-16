# Chapter 08: process B prints 40 lines "B\n" and exits with 0.
        .section .text.start,"ax",@progbits
        .globl  _start
_start:
        li      s0, 0
        li      s1, 200
.Lloop:
        li      a0, 1
        la      a1, line
        li      a2, 2
        li      a7, 64              # SYS_write
        ecall
        addi    s0, s0, 1
        blt     s0, s1, .Lloop

        li      a0, 0
        li      a7, 93              # SYS_exit
        ecall
1:      j       1b

        .section .rodata
line:   .ascii  "B\n"

        .section .note.GNU-stack,"",@progbits
