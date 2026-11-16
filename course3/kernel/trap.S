# Chapter 07: M-mode trap entry and the first mret into U-mode.
#
# mscratch holds the kernel stack pointer while U runs.  On entry the swap
# makes sp the kernel stack and mscratch the user stack; on the way out the
# same swap restores both.  All 31 registers are saved so a syscall can return
# to the user program without changing anything except a0.
        .section .text,"ax",@progbits
        .globl  trap_entry
trap_entry:
        csrrw   sp, mscratch, sp
        addi    sp, sp, -128
        sw      t0, 20(sp)            # save t0 before using it
        csrr    t0, mscratch
        sw      t0, 8(sp)             # x2 = user sp (for inspection)
        sw      ra, 4(sp)
        sw      gp, 12(sp)
        sw      tp, 16(sp)
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

        csrr    t0, mcause
        csrr    t1, mepc
        csrr    t2, mtval
        mv      a0, sp                # trap_frame *
        mv      a1, t0                # cause
        mv      a2, t1                # epc
        mv      a3, t2                # tval
        call    handle_trap

        lw      ra, 4(sp)
        lw      gp, 12(sp)
        lw      tp, 16(sp)
        lw      t0, 20(sp)
        lw      t1, 24(sp)
        lw      t2, 28(sp)
        lw      s0, 32(sp)
        lw      s1, 36(sp)
        lw      a0, 40(sp)
        lw      a1, 44(sp)
        lw      a2, 48(sp)
        lw      a3, 52(sp)
        lw      a4, 56(sp)
        lw      a5, 60(sp)
        lw      a6, 64(sp)
        lw      a7, 68(sp)
        lw      s2, 72(sp)
        lw      s3, 76(sp)
        lw      s4, 80(sp)
        lw      s5, 84(sp)
        lw      s6, 88(sp)
        lw      s7, 92(sp)
        lw      s8, 96(sp)
        lw      s9, 100(sp)
        lw      s10, 104(sp)
        lw      s11, 108(sp)
        lw      t3, 112(sp)
        lw      t4, 116(sp)
        lw      t5, 120(sp)
        lw      t6, 124(sp)

        addi    sp, sp, 128
        csrrw   sp, mscratch, sp
        mret

# enter_user(entry: a0, user_sp: a1, satp: a2) never returns to the kernel.
        .globl  enter_user
enter_user:
        csrw    satp, a2
        csrw    mepc, a0
        li      t0, 0x80100000
        csrw    mscratch, t0
        li      t0, 0x80            # MPIE=1: U-mode runs with MIE=1
        csrw    mstatus, t0
        mv      sp, a1
        mret

        .section .note.GNU-stack,"",@progbits
