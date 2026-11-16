//===-- misaligned-jump.s - a jalr to a 2 mod 4 target, chapter 04 -*- asm -*-===//

        .text
        .globl  main
        .p2align 2
main:
        lui     t0, 0x20000
        addi    t0, t0, 2
        jalr    ra, 0(t0)               // must trap with tval = 0x20000002
        addi    a0, zero, 0
        jalr    zero, 0(ra)
