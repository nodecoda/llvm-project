//===-- illegal.s - one word that no instruction matches, chapter 08 -*- asm -*-===//

        .text
        .globl  main
        .p2align 2
main:
        .word   0                       // mcause must be 2, mepc must be main
