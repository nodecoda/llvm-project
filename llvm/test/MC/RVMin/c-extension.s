# RUN: split-file %s %t
# RUN: llvm-mc -triple=rvmin -mattr=+c,+f,+d -show-encoding %t/all.s | FileCheck %s
# RUN: not llvm-mc -triple=rvmin -filetype=obj %t/all.s -o /dev/null 2>&1 | FileCheck %s --check-prefix=MISSING
# RUN: not llvm-mc -triple=rvmin -mattr=+c -filetype=obj %t/all.s -o /dev/null 2>&1 | FileCheck %s --check-prefix=MISSINGF
# RUN: not llvm-mc -triple=rvmin -mattr=+c,+f -filetype=obj %t/all.s -o /dev/null 2>&1 | FileCheck %s --check-prefix=MISSINGD
# RUN: llvm-mc -triple=rvmin -mattr=+c,+f,+d -filetype=obj %t/all.s -o %t.o
# RUN: llvm-objdump -d --triple=rvmin --mattr=+c,+f,+d %t.o | FileCheck %s --check-prefix=DIS
# RUN: llvm-mc -triple=rvmin -mattr=+c -filetype=obj %t/labels.s -o %t.labels.o
# RUN: llvm-objdump -d --triple=rvmin --mattr=+c %t.labels.o | FileCheck %s --check-prefix=LABEL
# RUN: not llvm-mc -triple=rvmin -mattr=+c -filetype=obj %t/range.s -o /dev/null 2>&1 | FileCheck %s --check-prefix=RANGE
# RUN: not llvm-mc -triple=rvmin -mattr=+c -filetype=obj %t/odd.s -o /dev/null 2>&1 | FileCheck %s --check-prefix=ODD
#
# Chapter 43: the RV32C MC layer.  Every 16-bit encoding below is
# byte-identical to the reference RISC-V assembler
# (`llvm-mc -triple=riscv32 -mattr=+c,+f,+d`), and
# scripts/check-rvmin-encodings.sh re-checks all 35 of them on every run.
#
# The compressed formats are CR/CI/CSS/CIW/CL/CS/CB/CJ.  Three details make
# them different from the 32-bit formats:
#
#   * the instruction word is 16 bits, so the emitter writes two bytes and
#     the disassembler inspects bits [1:0] first to pick a decoder table;
#   * GPRC operands only encode x8-x15 in three bits, and FS/FT fields in
#     CL/CS/CSS encode f8-f15;
#   * branch and jump offsets are scattered across the word and resolved by
#     dedicated fixups (`fixup_rvmin_rvc_branch` / `_rvc_jump`).
#
# The negative runs pin the feature diagnostics and the fixup range checks.

#--- all.s
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
c.flw fa0, 8(a1)
c.fsw fa0, 8(a1)
c.flwsp fa0, 8(sp)
c.fswsp fa0, 8(sp)
c.fld fa0, 8(a1)
c.fsd fa0, 8(a1)
c.fldsp fa0, 8(sp)
c.fsdsp fa0, 8(sp)

# CHECK: c.nop                        # encoding: [0x01,0x00]
# CHECK-NEXT: c.addi a0, 1            # encoding: [0x05,0x05]
# CHECK-NEXT: c.li a0, -1             # encoding: [0x7d,0x55]
# CHECK-NEXT: c.addi16sp sp, 16       # encoding: [0x41,0x61]
# CHECK-NEXT: c.lui a0, 1             # encoding: [0x05,0x65]
# CHECK-NEXT: c.slli a0, 3            # encoding: [0x0e,0x05]
# CHECK-NEXT: c.lwsp a0, 8(sp)        # encoding: [0x22,0x45]
# CHECK-NEXT: c.swsp a0, 8(sp)        # encoding: [0x2a,0xc4]
# CHECK-NEXT: c.jr ra                 # encoding: [0x82,0x80]
# CHECK-NEXT: c.mv a0, a1             # encoding: [0x2e,0x85]
# CHECK-NEXT: c.ebreak                # encoding: [0x02,0x90]
# CHECK-NEXT: c.jalr ra               # encoding: [0x82,0x90]
# CHECK-NEXT: c.add a0, a1            # encoding: [0x2e,0x95]
# CHECK-NEXT: c.addi4spn a0, sp, 16   # encoding: [0x08,0x08]
# CHECK-NEXT: c.lw a0, 8(a1)          # encoding: [0x88,0x45]
# CHECK-NEXT: c.sw a0, 8(a1)          # encoding: [0x88,0xc5]
# CHECK-NEXT: c.srli a0, 3            # encoding: [0x0d,0x81]
# CHECK-NEXT: c.srai a0, 3            # encoding: [0x0d,0x85]
# CHECK-NEXT: c.andi a0, -1           # encoding: [0x7d,0x99]
# CHECK-NEXT: c.sub a0, a1            # encoding: [0x0d,0x8d]
# CHECK-NEXT: c.xor a0, a1            # encoding: [0x2d,0x8d]
# CHECK-NEXT: c.or a0, a1             # encoding: [0x4d,0x8d]
# CHECK-NEXT: c.and a0, a1            # encoding: [0x6d,0x8d]
# CHECK-NEXT: c.beqz a0, 8            # encoding: [0x01,0xc5]
# CHECK-NEXT: c.bnez a0, 8            # encoding: [0x01,0xe5]
# CHECK-NEXT: c.j 8                   # encoding: [0x21,0xa0]
# CHECK-NEXT: c.jal 8                 # encoding: [0x21,0x20]
# CHECK-NEXT: c.flw fa0, 8(a1)        # encoding: [0x88,0x65]
# CHECK-NEXT: c.fsw fa0, 8(a1)        # encoding: [0x88,0xe5]
# CHECK-NEXT: c.flwsp fa0, 8(sp)      # encoding: [0x22,0x65]
# CHECK-NEXT: c.fswsp fa0, 8(sp)      # encoding: [0x2a,0xe4]
# CHECK-NEXT: c.fld fa0, 8(a1)        # encoding: [0x88,0x25]
# CHECK-NEXT: c.fsd fa0, 8(a1)        # encoding: [0x88,0xa5]
# CHECK-NEXT: c.fldsp fa0, 8(sp)      # encoding: [0x22,0x25]
# CHECK-NEXT: c.fsdsp fa0, 8(sp)      # encoding: [0x2a,0xa4]

#--- labels.s
c.beqz a0, target
c.bnez a1, target
c.j end
c.jal target
target:
  c.nop
end:
  c.ebreak

# LABEL: c.beqz a0, 8
# LABEL: c.bnez a1, 6
# LABEL: c.j 6
# LABEL: c.jal 2
# LABEL: <target>:
# LABEL: <end>:

#--- range.s
c.beqz a0, far
.space 4096
far:
  c.nop

# RANGE: error: compressed branch target out of range

#--- odd.s
c.j 3

# ODD: error: invalid operand for instruction

# Without +c no compressed mnemonic may assemble, and once +c is present the
# floating-point variants must still name the missing F or D feature.
# MISSING: error: instruction requires the following: 'C' (Compressed)
# MISSINGF: error: instruction requires the following: 'F' (Single-Precision Floating-Point)
# MISSINGD: error: instruction requires the following: 'D' (Double-Precision Floating-Point)

# The disassembler selects the 16-bit table from bits [1:0] and advances by
# two bytes, so a compressed instruction never swallows the word after it.
# DIS: 0: 01 00{{.*}}c.nop
# DIS-NEXT: 2: 05 05{{.*}}c.addi a0, 1
# DIS-NEXT: 4: 7d 55{{.*}}c.li a0, -1
# DIS-NEXT: 6: 41 61{{.*}}c.addi16sp sp, 16
# DIS-NEXT: 8: 05 65{{.*}}c.lui a0, 1
# DIS-NEXT: a: 0e 05{{.*}}c.slli a0, 3
# DIS-NEXT: c: 22 45{{.*}}c.lwsp a0, 8(sp)
# DIS-NEXT: e: 2a c4{{.*}}c.swsp a0, 8(sp)
# DIS-NEXT: 10: 82 80{{.*}}c.jr ra
# DIS-NEXT: 12: 2e 85{{.*}}c.mv a0, a1
# DIS-NEXT: 14: 02 90{{.*}}c.ebreak
# DIS-NEXT: 16: 82 90{{.*}}c.jalr ra
# DIS-NEXT: 18: 2e 95{{.*}}c.add a0, a1
# DIS-NEXT: 1a: 08 08{{.*}}c.addi4spn a0, sp, 16
# DIS-NEXT: 1c: 88 45{{.*}}c.lw a0, 8(a1)
# DIS-NEXT: 1e: 88 c5{{.*}}c.sw a0, 8(a1)
# DIS-NEXT: 20: 0d 81{{.*}}c.srli a0, 3
# DIS-NEXT: 22: 0d 85{{.*}}c.srai a0, 3
# DIS-NEXT: 24: 7d 99{{.*}}c.andi a0, -1
# DIS-NEXT: 26: 0d 8d{{.*}}c.sub a0, a1
# DIS-NEXT: 28: 2d 8d{{.*}}c.xor a0, a1
# DIS-NEXT: 2a: 4d 8d{{.*}}c.or a0, a1
# DIS-NEXT: 2c: 6d 8d{{.*}}c.and a0, a1
# DIS-NEXT: 2e: 01 c5{{.*}}c.beqz a0, 8
# DIS-NEXT: 30: 01 e5{{.*}}c.bnez a0, 8
# DIS-NEXT: 32: 21 a0{{.*}}c.j 8
# DIS-NEXT: 34: 21 20{{.*}}c.jal 8
# DIS-NEXT: 36: 88 65{{.*}}c.flw fa0, 8(a1)
# DIS-NEXT: 38: 88 e5{{.*}}c.fsw fa0, 8(a1)
# DIS-NEXT: 3a: 22 65{{.*}}c.flwsp fa0, 8(sp)
# DIS-NEXT: 3c: 2a e4{{.*}}c.fswsp fa0, 8(sp)
# DIS-NEXT: 3e: 88 25{{.*}}c.fld fa0, 8(a1)
# DIS-NEXT: 40: 88 a5{{.*}}c.fsd fa0, 8(a1)
# DIS-NEXT: 42: 22 25{{.*}}c.fldsp fa0, 8(sp)
# DIS-NEXT: 44: 2a a4{{.*}}c.fsdsp fa0, 8(sp)

