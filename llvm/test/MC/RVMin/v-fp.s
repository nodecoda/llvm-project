# RUN: llvm-mc -triple=rvmin -mattr=+v,+f -show-encoding %s | FileCheck %s
# RUN: llvm-mc -triple=rvmin -mattr=+v,+f -filetype=obj %s -o %t.o
# RUN: llvm-objdump -d --triple=rvmin --mattr=+v,+f %t.o | FileCheck %s --check-prefix=DIS
# RUN: not llvm-mc -triple=rvmin -mattr=+v -filetype=obj %s -o /dev/null 2>&1 | FileCheck %s --check-prefix=NOF
#
# Chapter 54: Zve32f.  These five instructions are the vector-float surface:
# three arithmetic forms in OPFVV and the two scalar/vector moves.

vsetivli zero, 4, e32, m1, ta, ma
# CHECK: vsetivli zero, 4, e32, m1, ta, ma # encoding: [0x57,0x70,0x02,0xcd]

vfadd.vv v8, v9, v10
# CHECK: vfadd.vv v8, v9, v10 # encoding: [0x57,0x14,0x95,0x02]

vfsub.vv v8, v9, v10
# CHECK: vfsub.vv v8, v9, v10 # encoding: [0x57,0x14,0x95,0x0a]

vfmul.vv v8, v9, v10
# CHECK: vfmul.vv v8, v9, v10 # encoding: [0x57,0x14,0x95,0x92]

vfmv.v.f v8, fa0
# CHECK: vfmv.v.f v8, fa0 # encoding: [0x57,0x54,0x05,0x5e]

vfmv.f.s fa0, v8
# CHECK: vfmv.f.s fa0, v8 # encoding: [0x57,0x15,0x80,0x42]

# DIS: vsetivli{{[[:space:]]+}}zero, 4, e32, m1, ta, ma
# DIS: vfadd.vv{{[[:space:]]+}}v8, v9, v10
# DIS: vfsub.vv{{[[:space:]]+}}v8, v9, v10
# DIS: vfmul.vv{{[[:space:]]+}}v8, v9, v10
# DIS: vfmv.v.f{{[[:space:]]+}}v8, fa0
# DIS: vfmv.f.s{{[[:space:]]+}}fa0, v8

# NOF: error: instruction requires the following: 'F' (Single-Precision Floating-Point)
