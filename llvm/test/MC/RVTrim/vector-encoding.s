# RUN: llvm-mc -triple=rvtrim64 -mattr=+zve64x -show-encoding %s \
# RUN:   | FileCheck %s --check-prefix=ENC
# RUN: llvm-mc -triple=rvtrim64 -mattr=+zve64x -filetype=obj %s -o %t
# RUN: llvm-objdump --no-print-imm-hex -d %t | FileCheck %s --check-prefix=DIS

# The RV64IMA_Zve64x profile reuses the V instruction definitions copied from
# the RISC-V backend, so these checks pin the encodings of every operand shape:
# vtype immediates, unit/strided/indexed memory, .vv/.vx/.vi ALU forms, masked
# forms, mask-producing compares, reductions, permutes and whole-register moves.

# ENC: vsetvli t0, zero, e32, m1, ta, ma{{.*}}# encoding: [0xd7,0x72,0x00,0x0d]
vsetvli t0, zero, e32, m1, ta, ma
# ENC: vsetivli t0, 4, e32, m1, ta, ma{{.*}}# encoding: [0xd7,0x72,0x02,0xcd]
vsetivli t0, 4, e32, m1, ta, ma
# ENC: vle32.v v1, (a0){{.*}}# encoding: [0x87,0x60,0x05,0x02]
vle32.v v1, (a0)
# ENC: vle32.v v1, (a0), v0.t{{.*}}# encoding: [0x87,0x60,0x05,0x00]
vle32.v v1, (a0), v0.t
# ENC: vse32.v v1, (a0){{.*}}# encoding: [0xa7,0x60,0x05,0x02]
vse32.v v1, (a0)
# ENC: vse32.v v1, (a0), v0.t{{.*}}# encoding: [0xa7,0x60,0x05,0x00]
vse32.v v1, (a0), v0.t
# ENC: vlse32.v v1, (a0), a1{{.*}}# encoding: [0x87,0x60,0xb5,0x0a]
vlse32.v v1, (a0), a1
# ENC: vsse32.v v1, (a0), a1{{.*}}# encoding: [0xa7,0x60,0xb5,0x0a]
vsse32.v v1, (a0), a1
# ENC: vluxei32.v v1, (a0), v2{{.*}}# encoding: [0x87,0x60,0x25,0x06]
vluxei32.v v1, (a0), v2
# ENC: vsuxei32.v v1, (a0), v2{{.*}}# encoding: [0xa7,0x60,0x25,0x06]
vsuxei32.v v1, (a0), v2
# ENC: vadd.vv v1, v2, v3{{.*}}# encoding: [0xd7,0x80,0x21,0x02]
vadd.vv v1, v2, v3
# ENC: vadd.vv v1, v2, v3, v0.t{{.*}}# encoding: [0xd7,0x80,0x21,0x00]
vadd.vv v1, v2, v3, v0.t
# ENC: vadd.vx v1, v2, a0{{.*}}# encoding: [0xd7,0x40,0x25,0x02]
vadd.vx v1, v2, a0
# ENC: vadd.vi v1, v2, 3{{.*}}# encoding: [0xd7,0xb0,0x21,0x02]
vadd.vi v1, v2, 3
# ENC: vsub.vv v1, v2, v3{{.*}}# encoding: [0xd7,0x80,0x21,0x0a]
vsub.vv v1, v2, v3
# ENC: vmul.vv v1, v2, v3{{.*}}# encoding: [0xd7,0xa0,0x21,0x96]
vmul.vv v1, v2, v3
# ENC: vand.vv v1, v2, v3{{.*}}# encoding: [0xd7,0x80,0x21,0x26]
vand.vv v1, v2, v3
# ENC: vor.vv v1, v2, v3{{.*}}# encoding: [0xd7,0x80,0x21,0x2a]
vor.vv v1, v2, v3
# ENC: vxor.vv v1, v2, v3{{.*}}# encoding: [0xd7,0x80,0x21,0x2e]
vxor.vv v1, v2, v3
# ENC: vmin.vv v1, v2, v3{{.*}}# encoding: [0xd7,0x80,0x21,0x16]
vmin.vv v1, v2, v3
# ENC: vmax.vv v1, v2, v3{{.*}}# encoding: [0xd7,0x80,0x21,0x1e]
vmax.vv v1, v2, v3
# ENC: vmseq.vv v1, v2, v3{{.*}}# encoding: [0xd7,0x80,0x21,0x62]
vmseq.vv v1, v2, v3
# ENC: vmslt.vv v1, v2, v3{{.*}}# encoding: [0xd7,0x80,0x21,0x6e]
vmslt.vv v1, v2, v3
# ENC: vmerge.vvm v1, v2, v3, v0{{.*}}# encoding: [0xd7,0x80,0x21,0x5c]
vmerge.vvm v1, v2, v3, v0
# ENC: vredsum.vs v1, v2, v3{{.*}}# encoding: [0xd7,0xa0,0x21,0x02]
vredsum.vs v1, v2, v3
# ENC: vrgather.vv v1, v2, v3{{.*}}# encoding: [0xd7,0x80,0x21,0x32]
vrgather.vv v1, v2, v3
# ENC: vslideup.vx v1, v2, a0{{.*}}# encoding: [0xd7,0x40,0x25,0x3a]
vslideup.vx v1, v2, a0
# ENC: vmv1r.v v1, v2{{.*}}# encoding: [0xd7,0x30,0x20,0x9e]
vmv1r.v v1, v2
# ENC: vmv.x.s a0, v1{{.*}}# encoding: [0x57,0x25,0x10,0x42]
vmv.x.s a0, v1
# ENC: vmv.s.x v1, a0{{.*}}# encoding: [0xd7,0x60,0x05,0x42]
vmv.s.x v1, a0
# ENC: vfirst.m a0, v1{{.*}}# encoding: [0x57,0xa5,0x18,0x42]
vfirst.m a0, v1
# ENC: vle64.v v1, (a0){{.*}}# encoding: [0x87,0x70,0x05,0x02]
vle64.v v1, (a0)
# ENC: vse64.v v1, (a0){{.*}}# encoding: [0xa7,0x70,0x05,0x02]
vse64.v v1, (a0)

# DIS: vsetvli t0, zero, e32, m1, ta, ma
# DIS: vsetivli t0, 4, e32, m1, ta, ma
# DIS: vle32.v v1, (a0)
# DIS: vse32.v v1, (a0)
# DIS: vadd.vv v1, v2, v3
# DIS: vredsum.vs v1, v2, v3
# DIS: vle64.v v1, (a0)
