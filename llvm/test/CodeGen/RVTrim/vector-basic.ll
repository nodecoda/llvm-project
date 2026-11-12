; RUN: llc -mtriple=rvtrim64 -mattr=+zve64x -rvtrim-v-vector-bits-min=128 -O2 \
; RUN:   -verify-machineinstrs < %s | FileCheck %s

; Fixed-length IR must legalize onto RVV and every vector operation must be
; preceded by a vsetvli/vsetivli carrying the right SEW and LMUL.

define void @vadd(ptr %a, ptr %b, ptr %c) {
; CHECK-LABEL: vadd:
; CHECK: vsetivli zero, 4, e32, m1, ta, ma
; CHECK: vle32.v v8, (a0)
; CHECK: vle32.v v9, (a1)
; CHECK: vadd.vv v8, v8, v9
; CHECK: vse32.v v8, (a2)
; CHECK: ret
entry:
  %x = load <4 x i32>, ptr %a
  %y = load <4 x i32>, ptr %b
  %r = add <4 x i32> %x, %y
  store <4 x i32> %r, ptr %c
  ret void
}

define i32 @vred(ptr %a) {
; CHECK-LABEL: vred:
; CHECK: vsetivli zero, 4, e32, m1, ta, ma
; CHECK: vmv.s.x v8, zero
; CHECK: vle32.v v9, (a0)
; CHECK: vredsum.vs v8, v9, v8
; CHECK: vmv.x.s a0, v8
; CHECK: ret
entry:
  %x = load <4 x i32>, ptr %a
  %r = call i32 @llvm.vector.reduce.add.v4i32(<4 x i32> %x)
  ret i32 %r
}

declare i32 @llvm.vector.reduce.add.v4i32(<4 x i32>)
