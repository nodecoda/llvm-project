# Chapter 04: three CLINT timer interrupts in vectored mode.
#
# The handler validates mcause=0x80000007, mtval=0, mepc inside the wait loop,
# and monotonic mtime; it re-arms mtimecmp and returns.  The 8-entry vector
# table means a non-vectored mtvec would land on entry 0 and fail.

        .section .text.start,"ax",@progbits
        .globl  _start
_start:
        la      t0, vector
        ori     t0, t0, 1        # vectored mode
        csrw    mtvec, t0

        li      t0, 0x0200bff8   # mtime
        lw      t1, 0(t0)
        lw      t2, 4(t0)
        addi    t1, t1, 200
        li      t0, 0x02004000   # mtimecmp
        sw      t1, 0(t0)
        sw      t2, 4(t0)

        li      t0, 0x80         # MTIE
        csrs    mie, t0
        li      t0, 0x8          # MIE
        csrs    mstatus, t0
        li      s0, 0
        li      s1, 0
        li      s2, 0

wait:
        addi    s0, s0, 1
        j       wait

        .align  2
vector:
        .word   0, 0, 0, 0, 0, 0, 0
handler_timer:
        csrr    t0, mcause
        li      t1, 0x80000007
        bne     t0, t1, fail_cause
        csrr    t2, mtval
        bne     t2, zero, fail_tval
        csrr    t2, mepc
        la      t3, wait
        bltu    t2, t3, fail_epc

        li      t3, 0x0200bff8
        lw      t4, 0(t3)
        bltu    t4, s2, fail_time
        mv      s2, t4

        addi    s1, s1, 1
        li      t3, 0x02004000
        lw      t4, 0(t3)
        addi    t4, t4, 200
        sw      t4, 0(t3)

        li      t5, 3
        beq     s1, t5, finish
        mret

finish:
        csrw    mie, zero
        csrw    mstatus, zero
        csrw    mtvec, zero
        li      a0, 0
        ebreak

fail_cause:
        li      a0, 1
        j       fail
fail_tval:
        li      a0, 2
        j       fail
fail_epc:
        li      a0, 3
        j       fail
fail_time:
        li      a0, 4
fail:
        csrw    mtvec, zero
        ebreak

        .section .note.GNU-stack,"",@progbits
