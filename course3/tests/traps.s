# Chapter 03: six precise traps from M-mode, validated by the handler itself.
#
# The handler compares (mcause, mepc, mtval) against a table, checks that
# s2-s5 survived, advances mepc by 4 and returns.  The sixth trap returns to
# `done`, clears mtvec and lets the host see a0=0.

        .section .text.start,"ax",@progbits
        .globl  _start
_start:
        la      t0, handler
        csrw    mtvec, t0
        la      s1, expected
        li      s0, 0
        li      s2, 0x12345678
        li      s3, 0x0badf00d
        li      s4, 0x55aa55aa
        li      s5, 0x01020304

ecall_m:
        ecall
ebreak_m:
        ebreak
illegal_m:
        .word   0
        li      t0, 0x80000002
miss_load:
        lw      t1, 0(t0)
        li      t0, 0x80000002
miss_store:
        sw      t1, 0(t0)
        li      t0, 0x80000002
miss_jalr:
        jalr    ra, 0(t0)

done:
        csrw    mtvec, zero
        li      a0, 0
        ebreak

        .align  2
handler:
        csrr    t0, mcause
        csrr    t1, mepc
        csrr    t2, mtval
        li      s6, 1
        lw      t3, 0(s1)
        bne     t0, t3, fail
        li      s6, 2
        lw      t3, 4(s1)
        bne     t1, t3, fail
        li      s6, 3
        lw      t3, 8(s1)
        bne     t2, t3, fail
        li      t3, 0x12345678
        bne     s2, t3, fail
        li      t3, 0x0badf00d
        bne     s3, t3, fail
        li      t3, 0x55aa55aa
        bne     s4, t3, fail
        li      t3, 0x01020304
        bne     s5, t3, fail
        addi    s1, s1, 12
        addi    s0, s0, 1
        li      t3, 6
        beq     s0, t3, finish
        addi    t1, t1, 4
        csrw    mepc, t1
        mret
finish:
        la      t1, done
        csrw    mepc, t1
        li      t3, 0x1800       # MPP = M
        csrs    mstatus, t3
        mret
fail:
        csrw    mtvec, zero
        li      a0, 10
        mul     a0, s0, a0
        add     a0, a0, s6
        ebreak

        .section .rodata
        .align  2
expected:
        .word   11, ecall_m, 0
        .word   3, ebreak_m, ebreak_m
        .word   2, illegal_m, 0
        .word   4, miss_load, 0x80000002
        .word   6, miss_store, 0x80000002
        .word   0, miss_jalr, 0x80000002

        .section .note.GNU-stack,"",@progbits
