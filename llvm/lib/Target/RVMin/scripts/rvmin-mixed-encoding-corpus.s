# Mixed 16/32-bit corpus for the RVMin disassembler (chapter 43).
#
# A decoder that always reads four bytes would consume the word after a
# compressed instruction, so this stream alternates widths and the gate
# requires the whole .text section to survive objdump -> asm -> obj
# byte-for-byte.
addi a0, a0, 1
c.addi a0, 2
lw a1, 8(sp)
c.lw a1, 8(a0)
c.j 6
addi a2, a2, 3
