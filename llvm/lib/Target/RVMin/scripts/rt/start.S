//===-- start.S - entry point and exit for the RVMin demo -------*- asm -*-===//
//
// Chapter 12: run a linked RVMin program under QEMU's Linux user mode.
// `_start` calls main, prints "rvmin: ok" or "rvmin: fail", and exits with
// main's return value.
//
//===----------------------------------------------------------------------===//

        .section .text.start,"ax",@progbits
        .globl  _start
_start:
        addi    sp, sp, -16
        sw      ra, 12(sp)
        jal     ra, main
        addi    t0, a0, 0                // remember main's result

        lui     a1, %hi(ok_msg)
        addi    a1, a1, %lo(ok_msg)
        addi    a2, zero, 10             // "rvmin: ok\n"
        beq     t0, zero, .Lprint
        lui     a1, %hi(fail_msg)
        addi    a1, a1, %lo(fail_msg)
        addi    a2, zero, 12             // "rvmin: fail\n"
.Lprint:
        addi    a0, zero, 1              // fd = stdout
        addi    a7, zero, 64             // SYS_write
        ecall

        addi    a0, t0, 0                // exit(main())
        addi    a7, zero, 93             // SYS_exit
        ecall
        jal     zero, .Lprint            // not reached

        .section .rodata
ok_msg:
        .ascii  "rvmin: ok\n"
fail_msg:
        .ascii  "rvmin: fail\n"

        .section .note.GNU-stack,"",@progbits
