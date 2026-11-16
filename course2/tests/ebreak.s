//===-- ebreak.s - a breakpoint trap without a handler, chapter 08 -*- asm -*-===//

        .text
        .globl  main
        .p2align 2
main:
        ebreak                          // mcause must be 3, mtval == mepc
        addi    a0, zero, 0
        jalr    zero, 0(ra)
