# Chapter 47: V-extension encoding corpus (Zve32x teaching subset).
#
# Nineteen instructions: three vset forms, unit-stride 32-bit load/store, four
# moves, three vadd forms, two vsub forms, two vmul forms and three bitwise
# operations.  check-rvmin-encodings.sh compares every encoding with
# `llvm-mc -triple=riscv32 -mattr=+v` and requires byte equality.
vsetivli zero, 4, e32, m1, ta, ma
vsetvli a0, a1, e32, m1, ta, ma
vsetvl a0, a1, a2
vle32.v v8, (a0)
vse32.v v8, (a0)
vmv.v.i v8, 5
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
vor.vv v8, v9, v10
vxor.vv v8, v9, v10
