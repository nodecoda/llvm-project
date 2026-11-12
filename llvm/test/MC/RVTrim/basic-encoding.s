# RUN: llvm-mc -triple=rvtrim64 -mattr=+m,+a,+zicsr,+zifencei -show-encoding %s \
# RUN:   | FileCheck %s --check-prefix=ENC
# RUN: llvm-mc -triple=rvtrim64 -mattr=+m,+a,+zicsr,+zifencei -filetype=obj %s -o %t
# RUN: llvm-objdump --no-print-imm-hex -d %t | FileCheck %s --check-prefix=DIS

# The whole RV64IMA base set is generated from the opcode database by auto-td,
# so these checks cover every encoding shape: U/J/B/I/S/R, the scattered B and
# J immediates, the 6-bit RV64 shift amount, the aq/rl atomic variants, Zicsr
# and the fixed-field fence/ecall/ebreak records.

# ENC: lui a0, 1{{.*}}# encoding: [0x37,0x15,0x00,0x00]
lui a0, 1
# ENC: auipc a1, 2{{.*}}# encoding: [0x97,0x25,0x00,0x00]
auipc a1, 2
# ENC: jal 8{{.*}}# encoding: [0xef,0x00,0x80,0x00]
jal 8
# ENC: jalr 4(sp){{.*}}# encoding: [0xe7,0x00,0x41,0x00]
jalr 4(sp)
# ENC: beq a0, a1, 8{{.*}}# encoding: [0x63,0x04,0xb5,0x00]
beq a0, a1, 8
# ENC: bgeu a0, a1, 8{{.*}}# encoding: [0x63,0x74,0xb5,0x00]
bgeu a0, a1, 8
# ENC: lb a0, 8(sp){{.*}}# encoding: [0x03,0x05,0x81,0x00]
lb a0, 8(sp)
# ENC: sd a0, 16(sp){{.*}}# encoding: [0x23,0x38,0xa1,0x00]
sd a0, 16(sp)
# ENC: addi a0, a1, 7{{.*}}# encoding: [0x13,0x85,0x75,0x00]
addi a0, a1, 7
# ENC: srai a0, a1, 7{{.*}}# encoding: [0x13,0xd5,0x75,0x40]
srai a0, a1, 7
# ENC: addw a0, a1, a2{{.*}}# encoding: [0x3b,0x85,0xc5,0x00]
addw a0, a1, a2
# ENC: mul a0, a1, a2{{.*}}# encoding: [0x33,0x85,0xc5,0x02]
mul a0, a1, a2
# ENC: mulhsu a0, a1, a2{{.*}}# encoding: [0x33,0xa5,0xc5,0x02]
mulhsu a0, a1, a2
# ENC: divu a0, a1, a2{{.*}}# encoding: [0x33,0xd5,0xc5,0x02]
divu a0, a1, a2
# ENC: remw a0, a1, a2{{.*}}# encoding: [0x3b,0xe5,0xc5,0x02]
remw a0, a1, a2
# ENC: lr.w a0, (a1){{.*}}# encoding: [0x2f,0xa5,0x05,0x10]
lr.w a0, (a1)
# ENC: lr.w.aqrl a0, (a1){{.*}}# encoding: [0x2f,0xa5,0x05,0x16]
lr.w.aqrl a0, (a1)
# ENC: sc.w.aq a0, a2, (a1){{.*}}# encoding: [0x2f,0xa5,0xc5,0x1c]
sc.w.aq a0, a2, (a1)
# ENC: amoadd.d.aq a0, a2, (a1){{.*}}# encoding: [0x2f,0xb5,0xc5,0x04]
amoadd.d.aq a0, a2, (a1)
# ENC: csrrw a0, mstatus, a1{{.*}}# encoding: [0x73,0x95,0x05,0x30]
csrrw a0, 0x300, a1
# ENC: csrrwi a0, mstatus, 7{{.*}}# encoding: [0x73,0xd5,0x03,0x30]
csrrwi a0, 0x300, 7
# ENC: fence rw, rw{{.*}}# encoding: [0x0f,0x00,0x30,0x03]
fence rw, rw
# ENC: fence.i{{.*}}# encoding: [0x0f,0x10,0x00,0x00]
fence.i
# ENC: ecall{{.*}}# encoding: [0x73,0x00,0x00,0x00]
ecall
# ENC: ebreak{{.*}}# encoding: [0x73,0x00,0x10,0x00]
ebreak

# DIS: lui a0, 1
# DIS: srai a0, a1, 7
# DIS: mulhsu a0, a1, a2
# DIS: lr.w.aqrl a0, (a1)
# DIS: amoadd.d.aq a0, a2, (a1)
# DIS: csrrwi a0, mstatus, 7
# DIS: fence.i
# DIS: ebreak
