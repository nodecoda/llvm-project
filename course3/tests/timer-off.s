# Chapter 04 negative test: MIE+MTIE are on, but mtimecmp is never reached,
# so no interrupt may fire.  A 1000-iteration loop must finish normally.

        .section .text.start,"ax",@progbits
        .globl  _start
_start:
        la      t0, fail
        csrw    mtvec, t0
        li      t0, 0x02004000
        li      t1, -1
        sw      t1, 0(t0)
        sw      t1, 4(t0)
        li      t0, 0x80
        csrs    mie, t0
        li      t0, 0x8
        csrs    mstatus, t0
        li      s0, 1000
loop:
        addi    s0, s0, -1
        bne     s0, zero, loop
finish:
        csrw    mie, zero
        csrw    mstatus, zero
        csrw    mtvec, zero
        li      a0, 0
        ebreak
fail:
        csrw    mtvec, zero
        li      a0, 1
        ebreak

        .section .note.GNU-stack,"",@progbits
