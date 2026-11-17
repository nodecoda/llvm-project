# Chapter 47: V-extension encoding corpus (Zve32x teaching subset).
#
# Thirty-six instructions: four vset forms (including e32/m2), unit-stride
# 32-bit load/store, the vmv moves plus vmv2r.v, three vadd forms, two vsub
# forms, two vmul forms, three bitwise operations with their scalar
# immediate/register forms, vredsum.vs, and the chapter-52 mask forms
# (vmerge.vvm plus six masked ALU .vv instructions).
# check-rvmin-encodings.sh compares every encoding with
# `llvm-mc -triple=riscv32 -mattr=+v` and requires byte equality.
vsetivli zero, 4, e32, m1, ta, ma
vsetvli a0, a1, e32, m1, ta, ma
vsetvl a0, a1, a2
vle32.v v8, (a0)
vse32.v v8, (a0)
vmv.v.i v8, 5
vmv.v.x v8, a0
vmv.v.v v8, v9
vmv.x.s a0, v8
vmv.s.x v8, a0
vadd.vv v8, v9, v10
vadd.vx v8, v9, a0
vadd.vi v8, v9, 5
vsub.vv v8, v9, v10
vsub.vx v8, v9, a0
vmul.vv v8, v9, v10
vmul.vx v8, v9, a0
vand.vv v8, v9, v10
vand.vx v8, v9, a0
vand.vi v8, v9, 5
vor.vv v8, v9, v10
vor.vx v8, v9, a0
vor.vi v8, v9, 5
vxor.vv v8, v9, v10
vxor.vx v8, v9, a0
vxor.vi v8, v9, 5
vredsum.vs v8, v9, v10
vadd.vv v8, v9, v10, v0.t
vsub.vv v8, v9, v10, v0.t
vmul.vv v8, v9, v10, v0.t
vand.vv v8, v9, v10, v0.t
vor.vv v8, v9, v10, v0.t
vxor.vv v8, v9, v10, v0.t
vmerge.vvm v8, v9, v10, v0
vsetivli zero, 8, e32, m2, ta, ma
vmv2r.v v8, v10
