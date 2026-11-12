; RUN: llc -mtriple=rvtrim64 -mattr=+zve64x -rvtrim-v-vector-bits-min=256 -O2 \
; RUN:   -verify-machineinstrs < %s | FileCheck %s

; With a 256-bit minimum VLEN, <8 x i32> is a single vector register group.
; Spilling two of them must reserve two vlenb slots and keep the frame 16-byte
; aligned.

declare void @foo()

define void @v256(ptr %a, ptr %b, ptr %c) {
; CHECK-LABEL: v256:
; CHECK: addi sp, sp, -32
; CHECK: csrr [[VLEN:a[0-9]+]], vlenb
; CHECK: slli [[VLEN2:a[0-9]+]], [[VLEN]], 1
; CHECK: sub sp, sp, [[VLEN2]]
; CHECK: vsetivli zero, 8, e32, m1, ta, ma
; CHECK: vle32.v v8, (a0)
; CHECK: vle32.v v9, (a1)
; CHECK: vadd.vv v8, v8, v9
; CHECK: call foo
; CHECK: vadd.vv v8, v8, v9
; CHECK: vse32.v v8, ({{.*}})
; CHECK: ret
entry:
  %x = load <8 x i32>, ptr %a
  %y = load <8 x i32>, ptr %b
  %r = add <8 x i32> %x, %y
  call void @foo()
  %r2 = add <8 x i32> %r, %y
  store <8 x i32> %r2, ptr %c
  ret void
}
