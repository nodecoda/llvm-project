# Chapter 7 gate: %hi/%lo symbolic operands.  The immediate fields are zero
# before relocation, so .text must match the reference RISC-V assembler
# byte-for-byte; the relocation types are checked separately.
        .text
        lui     a0, %hi(g)
        addi    a0, a0, %lo(g)
        lw      a1, %lo(g)(a0)
        lb      a2, %lo(g)(a0)
        lhu     a3, %lo(g)(a0)
        sw      a4, %lo(g)(a0)
        sh      a5, %lo(g)(a0)
        sb      a6, %lo(g)(a0)
        lui     t0, %hi(g + 4)
        addi    t0, t0, %lo(g + 4)

# Chapter 8: the three-instruction direct call sequence.  Symbolic halves stay
# zero until relocation, so this must match the reference assembler too.
        lui     t1, %hi(other)
        addi    t1, t1, %lo(other)
        jalr    ra, 0(t1)
# Return: jalr zero, 0(ra).
        jalr    zero, 0(ra)

# Chapter 9 gate: what the type legalizer emits for narrow and 64-bit values.
# Narrow access uses the offset forms, sign extension is a shift pair, and the
# 64-bit carry is a `sltu` on the low halves.
        lbu     a0, 3(a1)
        lhu     a2, 6(a1)
        sb      a3, 7(a1)
        sh      a4, 8(a1)
        lw      a5, 12(a1)
        sw      a6, 16(a1)
        slli    a0, a0, 16
        srai    a0, a0, 16
        slli    a1, a1, 24
        srai    a1, a1, 24
        add     a0, a2, a3
        sltu    a1, a0, a2
        add     a1, a1, a4
        sltiu   a5, a0, 1
        andi    a6, a6, 255
