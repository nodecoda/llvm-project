# RV32I encoding corpus for RVMin.  One instruction per line; no labels, so the
# object produced from this file contains no relocations and its .text section
# can be compared byte-for-byte with the reference RISC-V assembler.
addi a0, zero, 1
addi a0, a0, 2047
addi a0, a0, -2048
addi x1, x2, 3
add a2, a0, a1
add t0, t1, t2
sub t0, t1, t2
sub s0, s1, s2
slt a3, a4, a5
sltu s0, s1, s2
xor a0, a1, a2
or a0, a1, a2
and a0, a1, a2
sll a0, a1, a2
srl a0, a1, a2
sra a0, a1, a2
sll x31, x30, x29
slti a0, a1, -5
slti a0, a1, 2047
sltiu a0, a1, 7
sltiu a0, a1, -2048
xori a0, a0, -1
ori a0, a0, 15
andi a0, a0, 255
andi a0, a0, -2048
slli a0, a1, 0
slli a0, a1, 3
slli a0, a1, 31
srli a0, a1, 0
srli a0, a1, 3
srli a0, a1, 31
srai a0, a1, 0
srai a0, a1, 3
srai a0, a1, 31
lui a1, 0x12345
lui a0, 0
lui a0, 0xfffff
auipc a2, 0x54321
auipc a0, 0
lw a0, 0(sp)
lw a0, 8(sp)
lw a1, -4(s0)
lw a2, 2047(sp)
lw a3, -2048(sp)
sw a2, 0(sp)
sw a2, 8(sp)
sw a3, -4(s0)
sw a4, 2047(sp)
sw a5, -2048(sp)
jal ra, 0
jal ra, 16
jal zero, -8
jal ra, 1048574
jal ra, -1048576
jalr ra, 0(ra)
jalr zero, 4(t0)
jalr a0, -2048(sp)
jalr a1, 2047(sp)
beq a0, a1, 8
beq a0, a1, 4094
beq a0, a1, -4096
bne a0, a1, -8
bne x1, x2, 2
blt a0, a1, 12
blt a0, a1, -2
bge a0, a1, -12
bge a0, a1, 4094
bltu a0, a1, 12
bltu a0, a1, -2
bgeu a0, a1, -12
bgeu a0, a1, 4094
