//===-- trap-handler.s - install mtvec and validate both traps, ch 08 -*- asm -*-===//
//
// main() sets mtvec, trips an illegal instruction and then an ebreak.  The
// handler checks mcause/mepc/mtval for both, skips the faulting instruction,
// and mret's back.  Any mismatch exits 1; success exits 0.
//
// Assembled with clang for riscv32 (rv32i_zicsr) because the course-1 RVMin
// assembler has no Zicsr support: the encodings are standard, and chapter 03
// already proved our decoder agrees with the reference on every RV32I word.
//
//===----------------------------------------------------------------------===//

        .text
        .globl  main
        .p2align 2
main:
        addi    sp, sp, -16
        sw      ra, 12(sp)
        addi    s0, zero, 0             // trap counter
        lui     t0, %hi(.Lhandler)
        addi    t0, t0, %lo(.Lhandler)
        csrrw   zero, mtvec, t0
.Lbad:
        .word   0                       // trap 1: illegal instruction
.Lbrk:
        ebreak                          // trap 2: breakpoint
        addi    a0, zero, 0
        jal     zero, .Ldone
.Ldone:
        lw      ra, 12(sp)
        addi    sp, sp, 16
        jalr    zero, 0(ra)

.Lhandler:
        csrrs   t1, mcause, zero
        csrrs   t2, mepc, zero
        csrrs   t3, mtval, zero
        addi    s0, s0, 1
        addi    t4, zero, 1
        beq     s0, t4, .Ltrap1

        // trap 2: ebreak, mcause 3, mepc/mtval both the ebreak address
        addi    t4, zero, 3
        bne     t1, t4, .Lh_fail
        lui     t5, %hi(.Lbrk)
        addi    t5, t5, %lo(.Lbrk)
        bne     t2, t5, .Lh_fail
        bne     t3, t2, .Lh_fail
        jal     zero, .Lh_resume

.Ltrap1:
        // trap 1: illegal instruction, mcause 2, mtval the offending word
        addi    t4, zero, 2
        bne     t1, t4, .Lh_fail
        lui     t5, %hi(.Lbad)
        addi    t5, t5, %lo(.Lbad)
        bne     t2, t5, .Lh_fail
        bne     t3, zero, .Lh_fail

.Lh_resume:
        addi    t2, t2, 4               // skip the faulting instruction
        csrrw   zero, mepc, t2
        mret

.Lh_fail:
        addi    a0, zero, 1
        addi    a7, zero, 93
        ecall
