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
