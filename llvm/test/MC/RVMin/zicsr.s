# RUN: split-file %s %t
# RUN: llvm-mc -triple=rvmin -mattr=+zicsr,+f -show-encoding %t/csr.s | FileCheck %s
# RUN: llvm-mc -triple=rvmin -mcpu=generic-rv32-zicsr -show-encoding %t/csr.s | FileCheck %s --check-prefix=CPU
# RUN: llvm-mc -triple=rvmin -mattr=+zifencei -show-encoding %t/zifencei.s | FileCheck %s --check-prefix=FENCEI
# RUN: llvm-mc -triple=rvmin -mattr=+zicsr,+f -filetype=obj %t/csr.s -o %t.csr.o
# RUN: llvm-objdump -d --triple=rvmin --mattr=+zicsr,+f %t.csr.o | FileCheck %s --check-prefix=DIS
# RUN: llvm-mc -triple=rvmin -mattr=+zifencei -filetype=obj %t/zifencei.s -o %t.fence.o
# RUN: llvm-objdump -d --triple=rvmin --mattr=+zifencei %t.fence.o | FileCheck %s --check-prefix=DISF
# RUN: not llvm-mc -triple=rvmin -mattr=+zifencei,+f -filetype=obj %t/csr.s -o /dev/null 2>&1 | FileCheck %s --check-prefix=MISSINGZICSR
# RUN: not llvm-mc -triple=rvmin -mattr=+zicsr -filetype=obj %t/zifencei.s -o /dev/null 2>&1 | FileCheck %s --check-prefix=MISSINGZIFENCEI
# RUN: not llvm-mc -triple=rvmin -mattr=+a -filetype=obj %t/zifencei.s -o /dev/null 2>&1 | FileCheck %s --check-prefix=MISSINGZIFENCEI
# RUN: not llvm-mc -triple=rvmin -mattr=+zicsr %t/bad.s -filetype=obj -o /dev/null 2>&1 | FileCheck %s --check-prefix=BADCSR
# RUN: not llvm-mc -triple=rvmin -mattr=+zicsr %t/range.s -filetype=obj -o /dev/null 2>&1 | FileCheck %s --check-prefix=RANGE
#
# Chapter 46: Zicsr and Zifencei.
#
# The six CSR instructions are plain I-type words with opcode SYSTEM.  The
# interesting part is the operand grammar: a CSR is a 12-bit number, but
# assembly spells it as a name (`mstatus`, `fflags`, ...).  The parser resolves
# names through the small table in MCTargetDesc/RVMinMCTargetDesc.cpp, so a
# name and its number produce identical bits, and the printer turns the number
# back into a name.
#
# The seven pseudo-instructions (csrr/csrw/csrs/csrc/csrwi/csrsi/csrci) are
# InstAlias records: they are accepted by the assembler and printed back by the
# asm writer, but they never reach the encoder as a separate opcode.
#
# fence.i moved out of A in this chapter: `+a` alone no longer accepts it, and
# `+zifencei` accepts it without atomics.  That is what makes rv32g and rv32gc
# legal in the driver.

#--- csr.s
csrrw a0, mstatus, a1
# CHECK: csrrw a0, mstatus, a1 # encoding: [0x73,0x95,0x05,0x30]
csrrs a0, mstatus, a1
# CHECK: csrrs a0, mstatus, a1 # encoding: [0x73,0xa5,0x05,0x30]
csrrc a0, mstatus, a1
# CHECK: csrrc a0, mstatus, a1 # encoding: [0x73,0xb5,0x05,0x30]
csrrwi a0, mstatus, 5
# CHECK: csrrw a0, mstatus, 5 # encoding: [0x73,0xd5,0x02,0x30]
csrrsi a0, mstatus, 5
# CHECK: csrrs a0, mstatus, 5 # encoding: [0x73,0xe5,0x02,0x30]
csrrci a0, mstatus, 5
# CHECK: csrrc a0, mstatus, 5 # encoding: [0x73,0xf5,0x02,0x30]
csrr a0, mstatus
# CHECK: csrr a0, mstatus # encoding: [0x73,0x25,0x00,0x30]
csrw mtvec, a1
# CHECK: csrw mtvec, a1 # encoding: [0x73,0x90,0x55,0x30]
csrs mepc, a1
# CHECK: csrs mepc, a1 # encoding: [0x73,0xa0,0x15,0x34]
csrc mcause, a1
# CHECK: csrc mcause, a1 # encoding: [0x73,0xb0,0x25,0x34]
csrwi mstatus, 0
# CHECK: csrwi mstatus, 0 # encoding: [0x73,0x50,0x00,0x30]
csrsi mstatus, 8
# CHECK: csrsi mstatus, 8 # encoding: [0x73,0x60,0x04,0x30]
csrci mstatus, 8
# CHECK: csrci mstatus, 8 # encoding: [0x73,0x70,0x04,0x30]
csrrw a0, fflags, a1
# CHECK: csrrw a0, fflags, a1 # encoding: [0x73,0x95,0x15,0x00]
csrrs a0, frm, a1
# CHECK: csrrs a0, frm, a1 # encoding: [0x73,0xa5,0x25,0x00]
csrrc a0, fcsr, a1
# CHECK: csrrc a0, fcsr, a1 # encoding: [0x73,0xb5,0x35,0x00]
csrr a0, fcsr
# CHECK: csrr a0, fcsr # encoding: [0x73,0x25,0x30,0x00]
csrw fflags, a1
# CHECK: csrw fflags, a1 # encoding: [0x73,0x90,0x15,0x00]
csrs frm, a1
# CHECK: csrs frm, a1 # encoding: [0x73,0xa0,0x25,0x00]
csrc fcsr, a1
# CHECK: csrc fcsr, a1 # encoding: [0x73,0xb0,0x35,0x00]
csrwi fflags, 31
# CHECK: csrwi fflags, 31 # encoding: [0x73,0xd0,0x1f,0x00]
# A numeric CSR must encode exactly like its name.
csrr a0, 0x300
# CHECK: csrr a0, mstatus # encoding: [0x73,0x25,0x00,0x30]
csrrw a0, 0x341, a1
# CHECK: csrrw a0, mepc, a1 # encoding: [0x73,0x95,0x15,0x34]

#--- zifencei.s
fence.i

#--- bad.s
csrr a0, not_a_csr

#--- range.s
csrr a0, 4096

# CPU: csrrw a0, mstatus, a1 # encoding: [0x73,0x95,0x05,0x30]
# FENCEI: fence.i # encoding: [0x0f,0x10,0x00,0x00]
# DIS: csrrw{{[[:space:]]+}}a0, mstatus, a1
# DIS: csrwi{{[[:space:]]+}}mstatus, 0
# DIS: csrci{{[[:space:]]+}}mstatus, 8
# DIS: csrr{{[[:space:]]+}}a0, fcsr
# DISF: fence.i
# MISSINGZICSR: error: instruction requires the following: 'Zicsr' (Control and Status Register Instructions)
# MISSINGZIFENCEI: error: instruction requires the following: 'Zifencei' (Instruction-Fetch Fence)
# BADCSR: error: unknown CSR name 'not_a_csr'
# RANGE: error: CSR number must be an integer in the range 0..4095
