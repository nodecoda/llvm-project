# Chapter 10 acceptance: all 31 GPRs survive U ecall -> M handler -> mret.
#
# We use sp itself as the array base so no scratch register is needed while
# recording the register file.  The handler saves t0/t1 (the only temporaries
# it touches) on the stack below `before`, then restores them.
        .section .text.start,"ax",@progbits
        .globl  _start
_start:
        la      t0, handler
        csrw    mtvec, t0

        la      sp, before
        li      ra, 0x10000001
        li      gp, 0x10000003
        li      tp, 0x10000004
        li      t0, 0x10000005
        li      t1, 0x10000006
        li      t2, 0x10000007
        li      s0, 0x10000008
        li      s1, 0x10000009
        li      a0, 0x1000000a
        li      a1, 0x1000000b
        li      a2, 0x1000000c
        li      a3, 0x1000000d
        li      a4, 0x1000000e
        li      a5, 0x1000000f
        li      a6, 0x10000010
        li      a7, 0x10000011
        li      s2, 0x10000012
        li      s3, 0x10000013
        li      s4, 0x10000014
        li      s5, 0x10000015
        li      s6, 0x10000016
        li      s7, 0x10000017
        li      s8, 0x10000018
        li      s9, 0x10000019
        li      s10, 0x1000001a
        li      s11, 0x1000001b
        li      t3, 0x1000001c
        li      t4, 0x1000001d
        li      t5, 0x1000001e
        li      t6, 0x1000001f

        # before[i] = x[i], with sp as the base (sp itself lands in slot 2).
        sw      ra, 4(sp)
        sw      sp, 8(sp)
        sw      gp, 12(sp)
        sw      tp, 16(sp)
        sw      t0, 20(sp)
        sw      t1, 24(sp)
        sw      t2, 28(sp)
        sw      s0, 32(sp)
        sw      s1, 36(sp)
        sw      a0, 40(sp)
        sw      a1, 44(sp)
        sw      a2, 48(sp)
        sw      a3, 52(sp)
        sw      a4, 56(sp)
        sw      a5, 60(sp)
        sw      a6, 64(sp)
        sw      a7, 68(sp)
        sw      s2, 72(sp)
        sw      s3, 76(sp)
        sw      s4, 80(sp)
        sw      s5, 84(sp)
        sw      s6, 88(sp)
        sw      s7, 92(sp)
        sw      s8, 96(sp)
        sw      s9, 100(sp)
        sw      s10, 104(sp)
        sw      s11, 108(sp)
        sw      t3, 112(sp)
        sw      t4, 116(sp)
        sw      t5, 120(sp)
        sw      t6, 124(sp)

        la      t0, user_entry
        csrw    mepc, t0
        li      t0, 0x1800          # MPP = U
        csrc    mstatus, t0
        li      t0, 0x10000005      # restore t0's test value
        mret

user_entry:
        ecall                       # handler returns to user_entry+4

        # after[i] = x[i], 256 bytes past before.
        sw      ra, 260(sp)
        sw      sp, 264(sp)
        sw      gp, 268(sp)
        sw      tp, 272(sp)
        sw      t0, 276(sp)
        sw      t1, 280(sp)
        sw      t2, 284(sp)
        sw      s0, 288(sp)
        sw      s1, 292(sp)
        sw      a0, 296(sp)
        sw      a1, 300(sp)
        sw      a2, 304(sp)
        sw      a3, 308(sp)
        sw      a4, 312(sp)
        sw      a5, 316(sp)
        sw      a6, 320(sp)
        sw      a7, 324(sp)
        sw      s2, 328(sp)
        sw      s3, 332(sp)
        sw      s4, 336(sp)
        sw      s5, 340(sp)
        sw      s6, 344(sp)
        sw      s7, 348(sp)
        sw      s8, 352(sp)
        sw      s9, 356(sp)
        sw      s10, 360(sp)
        sw      s11, 364(sp)
        sw      t3, 368(sp)
        sw      t4, 372(sp)
        sw      t5, 376(sp)
        sw      t6, 380(sp)

        # Compare the two 31-word snapshots; t0..t4 are already saved.
        li      t0, 1
        li      t1, 32
1:
        slli    t2, t0, 2
        add     t2, sp, t2
        lw      t3, 0(t2)
        lw      t4, 256(t2)
        bne     t3, t4, fail
        addi    t0, t0, 1
        blt     t0, t1, 1b

        li      t0, 0x00100000
        li      t1, 0x00005555
        sw      t1, 0(t0)
2:      j       2b

fail:
        li      t0, 0x00100000
        li      t1, 0x00007777
        sw      t1, 0(t0)
3:      j       3b

        .align  2
handler:
        addi    sp, sp, -8
        sw      t0, 0(sp)
        sw      t1, 4(sp)
        csrr    t0, mcause
        li      t1, 8
        bne     t0, t1, handler_fail
        csrr    t0, mepc
        addi    t0, t0, 4
        csrw    mepc, t0
        lw      t0, 0(sp)
        lw      t1, 4(sp)
        addi    sp, sp, 8
        mret
handler_fail:
        lw      t0, 0(sp)
        lw      t1, 4(sp)
        addi    sp, sp, 8
        j       fail

        .section .bss
        .align  4
guard:  .space  16
before: .space  128
after:  .space  128

        .section .note.GNU-stack,"",@progbits
