//===-- unknown-syscall.s - a syscall we do not implement, chapter 05 -*- asm -*-===//

        .text
        .globl  main
        .p2align 2
main:
        addi    a7, zero, 999           // no such service
        ecall                           // must stop with status 127
        addi    a0, zero, 0
        jalr    zero, 0(ra)
