# RUN: llvm-mc -triple=minirv64 -show-encoding %s | FileCheck %s
# RUN: llvm-mc -triple=minirv64 -filetype=obj %s -o %t.o
# RUN: llvm-objdump -d --triple=minirv64 %t.o | FileCheck --check-prefix=DIS %s

# The whole instruction set of this target is generated from
# llvm/lib/Target/MiniRV/auto-td by auto-td-gen; this test pins the encodings
# that the opcode database produced.

# CHECK: add x1, x2, x3
# CHECK-SAME: encoding: [0xb3,0x00,0x31,0x00]
add x1, x2, x3

# CHECK: sub x5, x6, x7
# CHECK-SAME: encoding: [0xb3,0x02,0x73,0x40]
sub x5, x6, x7

# CHECK: sra x10, x11, x12
# CHECK-SAME: encoding: [0x33,0xd5,0xc5,0x40]
sra x10, x11, x12

# CHECK: addi x1, x2, -1
# CHECK-SAME: encoding: [0x93,0x00,0xf1,0xff]
addi x1, x2, -1

# CHECK: xori x28, x29, 2047
# CHECK-SAME: encoding: [0x13,0xce,0xfe,0x7f]
xori x28, x29, 2047

# CHECK: lui x1, 74565
# CHECK-SAME: encoding: [0xb7,0x50,0x34,0x12]
lui x1, 0x12345

# CHECK: auipc x2, 1048575
# CHECK-SAME: encoding: [0x17,0xf1,0xff,0xff]
auipc x2, 0xfffff

# DIS: add x1, x2, x3
# DIS: sub x5, x6, x7
# DIS: sra x10, x11, x12
# DIS: addi x1, x2, -1
# DIS: xori x28, x29, 2047
# DIS: lui x1, 74565
# DIS: auipc x2, 1048575
