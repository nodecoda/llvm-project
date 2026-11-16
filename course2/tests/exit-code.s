//===-- exit-code.s - exit(2) must reach the host, chapter 05 ----*- asm -*-===//

        .text
        .globl  main
        .p2align 2
main:
        addi    a0, zero, 42            // the exit status the host must see
        addi    a7, zero, 93            // SYS_exit
        ecall
