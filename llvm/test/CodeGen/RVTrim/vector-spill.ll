; RUN: llc -mtriple=rvtrim64 -mattr=+zve64x -rvtrim-v-vector-bits-min=128 -O2 \
; RUN:   -verify-machineinstrs < %s | FileCheck %s

; A vector live range across a call is spilled with whole-register
; vs1r.v/vl1r.v and the stack is adjusted by vlenb at runtime, so the frame
; works for any VLEN.

declare void @foo()

define void @spill(ptr %a, ptr %c) {
; CHECK-LABEL: spill:
; CHECK: csrr [[VLEN:a[0-9]+]], vlenb
; CHECK: sub sp, sp, [[VLEN]]
; CHECK: vs1r.v v8, ({{.*}}){{.*}}vscale x 8-byte Folded Spill
; CHECK: call foo
; CHECK: vl1r.v v8, ({{.*}}){{.*}}vscale x 8-byte Folded Reload
; CHECK: vadd.vv v8, v8, v8
; CHECK: vse32.v v8, ({{.*}})
; CHECK: csrr [[VLEN2:a[0-9]+]], vlenb
; CHECK: add sp, sp, [[VLEN2]]
; CHECK: ret
entry:
  %x = load <4 x i32>, ptr %a
  call void @foo()
  %r = add <4 x i32> %x, %x
  store <4 x i32> %r, ptr %c
  ret void
}
