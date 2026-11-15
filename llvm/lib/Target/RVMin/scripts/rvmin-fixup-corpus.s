# Chapter 5 gate: label fixups must produce the same bytes as the reference
# RISC-V assembler.  The instructions below exercise forward and backward
# branch/jump targets, including offsets larger than one page.
        .text
        .globl  fixups
fixups:
start:
        beq     a0, a1, fwd
        bne     a0, a1, fwd
        blt     a0, a1, back
        bge     a0, a1, back
        bltu    a0, a1, fwd
        bgeu    a0, a1, back
        jal     zero, fwd
        jal     ra, back
back:
        addi    a0, a0, 1
fwd:
        addi    a0, a0, 2
        beq     a0, a1, far
        jal     zero, far2
        .space  1024, 0
far:
        addi    a1, a1, 3
        jal     ra, fwd
        .space  8192, 0
far2:
        addi    a2, a2, 4
        jal     zero, start
