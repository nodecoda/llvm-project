# Chapter 03: the same handler, but the traps come from U-mode.
#
# mret drops to U; ecall (cause 8), ebreak (3) and an illegal word (2) are
# taken in M.  After the third trap the handler returns to M `done_m` and the
# host sees a0=0.

        .section .text.start,"ax",@progbits
        .globl  _start
_start:
        la      t0, handler
        csrw    mtvec, t0
        la      s1, expected
        li      s0, 0
        li      s2, 0x12345678
        la      t0, user_entry
        csrw    mepc, t0
        li      t0, 0x1800       # MPP = U
        csrc    mstatus, t0
        mret

user_entry:
        li      s3, 0x0badf00d
ecall_u:
        ecall
ebreak_u:
        ebreak
illegal_u:
        .word   0

done_m:
        csrw    mtvec, zero
        li      a0, 0
        ebreak

        .align  2
handler:
        csrr    t0, mcause
        csrr    t1, mepc
        csrr    t2, mtval
        lw      t3, 0(s1)
        bne     t0, t3, fail
        lw      t3, 4(s1)
        bne     t1, t3, fail
        lw      t3, 8(s1)
        bne     t2, t3, fail
        li      t3, 0x12345678
        bne     s2, t3, fail
        li      t3, 0x0badf00d
        bne     s3, t3, fail
        addi    s1, s1, 12
        addi    s0, s0, 1
        li      t3, 3
        beq     s0, t3, finish
        addi    t1, t1, 4
        csrw    mepc, t1
        mret
finish:
        la      t1, done_m
        csrw    mepc, t1
        li      t3, 0x1800       # return to M
        csrs    mstatus, t3
        mret
fail:
        csrw    mtvec, zero
        li      a0, 1
        ebreak

        .section .rodata
        .align  2
expected:
        .word   8, ecall_u, 0
        .word   3, ebreak_u, ebreak_u
        .word   2, illegal_u, 0

        .section .note.GNU-stack,"",@progbits
