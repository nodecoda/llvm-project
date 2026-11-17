# RV32C encoding corpus for the RVMin MC layer (chapter 43).
#
# Every line is an explicit compressed mnemonic accepted by the RVMin
# assembler with -mattr=+c,+f,+d.  check-rvmin-encodings.sh assembles this
# file with both -triple=rvmin and -triple=riscv32 and requires the 16-bit
# encodings to match one for one.
#
# Integer CI / CR / CSS / CIW / CL / CS / CB / CJ forms.
c.nop
c.addi a0, 1
c.li a0, -1
c.addi16sp sp, 16
c.lui a0, 1
c.slli a0, 3
c.lwsp a0, 8(sp)
c.swsp a0, 8(sp)
c.jr ra
c.mv a0, a1
c.ebreak
c.jalr ra
c.add a0, a1
c.addi4spn a0, sp, 16
c.lw a0, 8(a1)
c.sw a0, 8(a1)
c.srli a0, 3
c.srai a0, 3
c.andi a0, -1
c.sub a0, a1
c.xor a0, a1
c.or a0, a1
c.and a0, a1
c.beqz a0, 8
c.bnez a0, 8
c.j 8
c.jal 8
# Single-precision compressed loads/stores (RV32C + F).
c.flw fa0, 8(a1)
c.fsw fa0, 8(a1)
c.flwsp fa0, 8(sp)
c.fswsp fa0, 8(sp)
# Double-precision compressed loads/stores (RV32C + D).
c.fld fa0, 8(a1)
c.fsd fa0, 8(a1)
c.fldsp fa0, 8(sp)
c.fsdsp fa0, 8(sp)
