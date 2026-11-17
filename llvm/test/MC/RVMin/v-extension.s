# RUN: split-file %s %t
# RUN: llvm-mc -triple=rvmin -mattr=+v -show-encoding %t/v.s | FileCheck %s
# RUN: llvm-mc -triple=rvmin -mcpu=generic-rv32-v -show-encoding %t/v.s | FileCheck %s --check-prefix=CPU
# RUN: llvm-mc -triple=rvmin -mattr=+v -filetype=obj %t/v.s -o %t.v.o
# RUN: llvm-objdump -d --triple=rvmin --mattr=+v %t.v.o | FileCheck %s --check-prefix=DIS
# RUN: not llvm-mc -triple=rvmin -filetype=obj %t/v.s -o /dev/null 2>&1 | FileCheck %s --check-prefix=MISSING
# RUN: not llvm-mc -triple=rvmin -mattr=+v -filetype=obj %t/badtype.s -o /dev/null 2>&1 | FileCheck %s --check-prefix=BADTYPE
# RUN: not llvm-mc -triple=rvmin -mattr=+v -filetype=obj %t/range.s -o /dev/null 2>&1 | FileCheck %s --check-prefix=RANGE
# RUN: not llvm-mc -triple=rvmin -mattr=+v -filetype=obj %t/range10.s -o /dev/null 2>&1 | FileCheck %s --check-prefix=RANGE10
# RUN: not llvm-mc -triple=rvmin -mattr=+v -filetype=obj %t/rangei.s -o /dev/null 2>&1 | FileCheck %s --check-prefix=RANGEI
# RUN: not llvm-mc -triple=rvmin -mattr=+v -filetype=obj %t/badmask.s -o /dev/null 2>&1 | FileCheck %s --check-prefix=BADMASK
# RUN: not llvm-mc -triple=rvmin -mattr=+v -filetype=obj %t/badmaskt.s -o /dev/null 2>&1 | FileCheck %s --check-prefix=BADMASKT
# RUN: not llvm-mc -triple=rvmin -mattr=+v -filetype=obj %t/badmerge.s -o /dev/null 2>&1 | FileCheck %s --check-prefix=BADMERGE
# RUN: not llvm-mc -triple=rvmin -mattr=+v -filetype=obj %t/badmerget.s -o /dev/null 2>&1 | FileCheck %s --check-prefix=BADMERGET
# RUN: not llvm-mc -triple=rvmin -mattr=+v -filetype=obj %t/badvd.s -o /dev/null 2>&1 | FileCheck %s --check-prefix=BADVD
# RUN: not llvm-mc -triple=rvmin -mattr=+v -filetype=obj %t/badvmerge.s -o /dev/null 2>&1 | FileCheck %s --check-prefix=BADVMERGE
#
# Chapter 47: the V-extension MC layer (Zve32x teaching subset).
# Chapter 52 adds the mask forms: vmerge.vvm and masked integer ALU.
#
# The thirty-four instructions below cover configuration, unit-stride 32-bit
# load/store, the vmv moves (including the scalar broadcast), integer
# add/sub/multiply with their .vx/.vi forms, the bitwise operations with their
# scalar forms, the vredsum.vs reduction, and the chapter-52 mask forms.
# Every encoding is checked byte for byte against
# `llvm-mc -triple=riscv32 -mattr=+v`; the corpus gate repeats that comparison
# over the whole file.
#
# The vtype operand is the interesting part: assembly spells it as the token
# group `e32, m1, ta, ma`, and the same numeric value (0xd0) must encode
# identically.  The printer always spells the tokens back, so the disassembler
# round trip is symmetric.

#--- v.s
vsetivli zero, 4, e32, m1, ta, ma
# CHECK: vsetivli zero, 4, e32, m1, ta, ma # encoding: [0x57,0x70,0x02,0xcd]
vsetvli a0, a1, e32, m1, ta, ma
# CHECK: vsetvli a0, a1, e32, m1, ta, ma # encoding: [0x57,0xf5,0x05,0x0d]
vsetvl a0, a1, a2
# CHECK: vsetvl a0, a1, a2 # encoding: [0x57,0xf5,0xc5,0x80]
vle32.v v8, (a0)
# CHECK: vle32.v v8, (a0) # encoding: [0x07,0x64,0x05,0x02]
vse32.v v8, (a0)
# CHECK: vse32.v v8, (a0) # encoding: [0x27,0x64,0x05,0x02]
vmv.v.i v8, 5
# CHECK: vmv.v.i v8, 5 # encoding: [0x57,0xb4,0x02,0x5e]
vmv.v.x v8, a0
# CHECK: vmv.v.x v8, a0 # encoding: [0x57,0x44,0x05,0x5e]
vmv.v.v v8, v9
# CHECK: vmv.v.v v8, v9 # encoding: [0x57,0x84,0x04,0x5e]
vmv.x.s a0, v8
# CHECK: vmv.x.s a0, v8 # encoding: [0x57,0x25,0x80,0x42]
vmv.s.x v8, a0
# CHECK: vmv.s.x v8, a0 # encoding: [0x57,0x64,0x05,0x42]
vadd.vv v8, v9, v10
# CHECK: vadd.vv v8, v9, v10 # encoding: [0x57,0x04,0x95,0x02]
vadd.vx v8, v9, a0
# CHECK: vadd.vx v8, v9, a0 # encoding: [0x57,0x44,0x95,0x02]
vadd.vi v8, v9, 5
# CHECK: vadd.vi v8, v9, 5 # encoding: [0x57,0xb4,0x92,0x02]
vsub.vv v8, v9, v10
# CHECK: vsub.vv v8, v9, v10 # encoding: [0x57,0x04,0x95,0x0a]
vsub.vx v8, v9, a0
# CHECK: vsub.vx v8, v9, a0 # encoding: [0x57,0x44,0x95,0x0a]
vmul.vv v8, v9, v10
# CHECK: vmul.vv v8, v9, v10 # encoding: [0x57,0x24,0x95,0x96]
vmul.vx v8, v9, a0
# CHECK: vmul.vx v8, v9, a0 # encoding: [0x57,0x64,0x95,0x96]
vand.vv v8, v9, v10
# CHECK: vand.vv v8, v9, v10 # encoding: [0x57,0x04,0x95,0x26]
vand.vx v8, v9, a0
# CHECK: vand.vx v8, v9, a0 # encoding: [0x57,0x44,0x95,0x26]
vand.vi v8, v9, 5
# CHECK: vand.vi v8, v9, 5 # encoding: [0x57,0xb4,0x92,0x26]
vor.vv v8, v9, v10
# CHECK: vor.vv v8, v9, v10 # encoding: [0x57,0x04,0x95,0x2a]
vor.vx v8, v9, a0
# CHECK: vor.vx v8, v9, a0 # encoding: [0x57,0x44,0x95,0x2a]
vor.vi v8, v9, 5
# CHECK: vor.vi v8, v9, 5 # encoding: [0x57,0xb4,0x92,0x2a]
vxor.vv v8, v9, v10
# CHECK: vxor.vv v8, v9, v10 # encoding: [0x57,0x04,0x95,0x2e]
vxor.vx v8, v9, a0
# CHECK: vxor.vx v8, v9, a0 # encoding: [0x57,0x44,0x95,0x2e]
vxor.vi v8, v9, 5
# CHECK: vxor.vi v8, v9, 5 # encoding: [0x57,0xb4,0x92,0x2e]
vredsum.vs v8, v9, v10
# CHECK: vredsum.vs v8, v9, v10 # encoding: [0x57,0x24,0x95,0x02]
# A numeric vtype must encode exactly like the token group.
vsetivli zero, 4, 0xd0
# CHECK: vsetivli zero, 4, e32, m1, ta, ma # encoding: [0x57,0x70,0x02,0xcd]
# v0-v31 are all ordinary vector registers in this subset.
vadd.vv v0, v31, v1
# CHECK: vadd.vv v0, v31, v1 # encoding: [0x57,0x80,0xf0,0x03]

# Chapter 52: masks.  vmerge spells the mask as plain v0, the masked ALU
# forms spell it as v0.t.  The mask value is bit i of element i of v0.
vmerge.vvm v8, v9, v10, v0
# CHECK: vmerge.vvm v8, v9, v10, v0 # encoding: [0x57,0x04,0x95,0x5c]
vadd.vv v8, v9, v10, v0.t
# CHECK: vadd.vv v8, v9, v10, v0.t # encoding: [0x57,0x04,0x95,0x00]
vsub.vv v8, v9, v10, v0.t
# CHECK: vsub.vv v8, v9, v10, v0.t # encoding: [0x57,0x04,0x95,0x08]
vmul.vv v8, v9, v10, v0.t
# CHECK: vmul.vv v8, v9, v10, v0.t # encoding: [0x57,0x24,0x95,0x94]
vand.vv v8, v9, v10, v0.t
# CHECK: vand.vv v8, v9, v10, v0.t # encoding: [0x57,0x04,0x95,0x24]
vor.vv v8, v9, v10, v0.t
# CHECK: vor.vv v8, v9, v10, v0.t # encoding: [0x57,0x04,0x95,0x28]
vxor.vv v8, v9, v10, v0.t
# CHECK: vxor.vv v8, v9, v10, v0.t # encoding: [0x57,0x04,0x95,0x2c]
# The mask register may be a source, just not the destination.
vadd.vv v8, v0, v10, v0.t
# CHECK: vadd.vv v8, v0, v10, v0.t # encoding: [0x57,0x04,0x05,0x00]
vadd.vv v8, v9, v0, v0.t
# CHECK: vadd.vv v8, v9, v0, v0.t # encoding: [0x57,0x04,0x90,0x00]

#--- badtype.s
vsetivli zero, 4, e33, m1, ta, ma

#--- range.s
vsetivli zero, 4, 4096

#--- range10.s
vsetivli zero, 4, 1024

#--- rangei.s
vadd.vi v8, v9, 16

#--- badmask.s
vadd.vv v8, v9, v10, v1.t

#--- badmaskt.s
vadd.vv v8, v9, v10, v0

#--- badmerge.s
vmerge.vvm v8, v9, v10, v1

#--- badmerget.s
vmerge.vvm v8, v9, v10, v0.t

#--- badvd.s
vadd.vv v0, v31, v1, v0.t

#--- badvmerge.s
vmerge.vvm v0, v1, v2, v0

# CPU: vsetivli zero, 4, e32, m1, ta, ma # encoding: [0x57,0x70,0x02,0xcd]
# DIS: vsetivli{{[[:space:]]+}}zero, 4, e32, m1, ta, ma
# DIS: vsetvli{{[[:space:]]+}}a0, a1, e32, m1, ta, ma
# DIS: vsetvl{{[[:space:]]+}}a0, a1, a2
# DIS: vle32.v{{[[:space:]]+}}v8, (a0)
# DIS: vse32.v{{[[:space:]]+}}v8, (a0)
# DIS: vmv.v.i{{[[:space:]]+}}v8, 5
# DIS: vmv.v.x{{[[:space:]]+}}v8, a0
# DIS: vmv.v.v{{[[:space:]]+}}v8, v9
# DIS: vmv.x.s{{[[:space:]]+}}a0, v8
# DIS: vmv.s.x{{[[:space:]]+}}v8, a0
# DIS: vadd.vv{{[[:space:]]+}}v8, v9, v10
# DIS: vadd.vx{{[[:space:]]+}}v8, v9, a0
# DIS: vadd.vi{{[[:space:]]+}}v8, v9, 5
# DIS: vmul.vv{{[[:space:]]+}}v8, v9, v10
# DIS: vand.vx{{[[:space:]]+}}v8, v9, a0
# DIS: vand.vi{{[[:space:]]+}}v8, v9, 5
# DIS: vor.vx{{[[:space:]]+}}v8, v9, a0
# DIS: vor.vi{{[[:space:]]+}}v8, v9, 5
# DIS: vxor.vv{{[[:space:]]+}}v8, v9, v10
# DIS: vxor.vx{{[[:space:]]+}}v8, v9, a0
# DIS: vxor.vi{{[[:space:]]+}}v8, v9, 5
# DIS: vredsum.vs{{[[:space:]]+}}v8, v9, v10
# DIS: vmerge.vvm{{[[:space:]]+}}v8, v9, v10, v0
# DIS: vadd.vv{{[[:space:]]+}}v8, v9, v10, v0.t
# DIS: vsub.vv{{[[:space:]]+}}v8, v9, v10, v0.t
# DIS: vmul.vv{{[[:space:]]+}}v8, v9, v10, v0.t
# DIS: vand.vv{{[[:space:]]+}}v8, v9, v10, v0.t
# DIS: vor.vv{{[[:space:]]+}}v8, v9, v10, v0.t
# DIS: vxor.vv{{[[:space:]]+}}v8, v9, v10, v0.t
# BADMASK: error: operand must be v0.t
# BADMASKT: error: operand must be v0.t
# BADMERGE: error: operand must be v0
# BADMERGET: error: operand must be v0
# BADVD: error: the destination vector register group cannot overlap the mask register
# BADVMERGE: error: the destination vector register group cannot be V0
# MISSING: error: instruction requires the following: 'V' (Vector Extension)
# BADTYPE: error: unknown vtype SEW 'e33'
# RANGE: error: vtype must be an integer in the range 0..2047
# RANGE10: error: expected a vtype (e32, m1, ta, ma) or an integer
# RANGEI: error: invalid operand for instruction
