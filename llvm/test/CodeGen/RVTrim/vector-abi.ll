; RUN: llc -mtriple=rvtrim64 -mattr=+zve64x -rvtrim-v-vector-bits-min=128 -O2 \
; RUN:   -verify-machineinstrs < %s | FileCheck %s

; The copied RISC-V calling convention must still hold for the trimmed target:
; fixed-length vectors are passed in v8/v9 per the standard RVV vector ABI and
; the callee returns in v8.

define <4 x i32> @ret_vec(<4 x i32> %x) {
; CHECK-LABEL: ret_vec:
; CHECK: vsetivli zero, 4, e32, m1, ta, ma
; CHECK: vadd.vv v8, v8, v8
; CHECK: ret
  %r = add <4 x i32> %x, %x
  ret <4 x i32> %r
}

define void @call_vec(ptr %a, ptr %c) {
; CHECK-LABEL: call_vec:
; CHECK: vle32.v v8, (a0)
; CHECK: call ret_vec
; CHECK: vse32.v v8, ({{.*}})
; CHECK: ret
  %x = load <4 x i32>, ptr %a
  %r = call <4 x i32> @ret_vec(<4 x i32> %x)
  store <4 x i32> %r, ptr %c
  ret void
}
