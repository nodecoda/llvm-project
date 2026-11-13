# RUN: llvm-mc -triple=minirv64 -show-encoding %s | FileCheck %s
# RUN: llvm-mc -triple=minirv64 -filetype=obj %s -o %t.o
# RUN: llvm-objdump -d --triple=minirv64 %t.o | FileCheck --check-prefix=DIS %s

# The load/store and branch records come from the same auto-td corpus as the
# arithmetic ones.  The encodings are pinned against the RISC-V opcode
# database, and the branch offsets exercise the target fixups: a branch
# immediate is not contiguous in the instruction word, so the assembler has to
# scatter it itself.  A branch to a label cannot be encoded while streaming, so
# its bytes are checked through the object below.

# CHECK: ld x1, 8(x2)
# CHECK-SAME: encoding: [0x83,0x30,0x81,0x00]
ld x1, 8(x2)

# CHECK: sd x3, -16(x4)
# CHECK-SAME: encoding: [0x23,0x38,0x32,0xfe]
sd x3, -16(x4)

# CHECK: lb x5, 0(x6)
# CHECK-SAME: encoding: [0x83,0x02,0x03,0x00]
lb x5, 0(x6)

# CHECK: lwu x7, 2047(x8)
# CHECK-SAME: encoding: [0x83,0x63,0xf4,0x7f]
lwu x7, 2047(x8)

# CHECK: sb x9, -2048(x10)
# CHECK-SAME: encoding: [0x23,0x00,0x95,0x80]
sb x9, -2048(x10)

# CHECK: slli x11, x12, 63
# CHECK-SAME: encoding: [0x93,0x15,0xf6,0x03]
slli x11, x12, 63

# CHECK: srai x13, x14, 1
# CHECK-SAME: encoding: [0x93,0x56,0x17,0x40]
srai x13, x14, 1

# CHECK: slt x15, x16, x17
# CHECK-SAME: encoding: [0xb3,0x27,0x18,0x01]
slt x15, x16, x17

# CHECK: sltiu x18, x19, -1
# CHECK-SAME: encoding: [0x13,0xb9,0xf9,0xff]
sltiu x18, x19, -1

# CHECK: jalr x0, 0(x1)
# CHECK-SAME: encoding: [0x67,0x80,0x00,0x00]
jalr x0, 0(x1)

start:
  addi x1, x1, 1
# CHECK: beq x1, x2, start
  beq x1, x2, start
# CHECK: bne x1, x0, forward
  bne x1, x0, forward
  addi x2, x2, 2
forward:
# CHECK: jal x3, start
  jal x3, start
# CHECK: jal x0, forward
  jal x0, forward

# DIS: ld x1, 8(x2)
# DIS: sd x3, -16(x4)
# DIS: lb x5, 0(x6)
# DIS: lwu x7, 2047(x8)
# DIS: sb x9, -2048(x10)
# DIS: slli x11, x12, 63
# DIS: srai x13, x14, 1
# DIS: slt x15, x16, x17
# DIS: sltiu x18, x19, -1
# DIS: jalr x0, 0(x1)
# DIS: beq x1, x2, -4
# DIS: bne x1, x0, 8
# DIS: jal x3, -16
# DIS: jal x0, -4
