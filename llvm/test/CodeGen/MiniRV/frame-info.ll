; RUN: llc < %s -mtriple=minirv64 -O2 | FileCheck %s

; The frame address is the incoming stack pointer.  It cannot be recomputed
; from the stack pointer once the frame has been allocated, so asking for it is
; what makes the function keep X8 as a frame pointer.  X8 is written after the
; incoming value has been spilled, and the intrinsic returns X8 itself.

declare ptr @llvm.frameaddress.p0(i32)
declare ptr @llvm.returnaddress(i32)
declare ptr @llvm.stacksave.p0()
declare void @llvm.stackrestore.p0(ptr)
declare ptr @opaque(ptr)

; CHECK-LABEL: frame:
; CHECK:       addi x2, x2, -8
; CHECK-NEXT:  sd x8, 0(x2)
; CHECK-NEXT:  addi x8, x2, 8
; CHECK-NEXT:  addi x10, x8, 0
; CHECK:       addi x2, x8, -8
; CHECK-NEXT:  ld x8, 0(x2)
; CHECK-NEXT:  addi x2, x2, 8
; CHECK-NEXT:  jalr x0, 0(x1)
define ptr @frame() {
  %p = call ptr @llvm.frameaddress.p0(i32 0)
  ret ptr %p
}

; Local objects are addressed through the frame pointer once it exists, so a
; frame address can be combined with an ordinary fixed-size alloca.
; CHECK-LABEL: frame_and_alloca:
; CHECK:       addi x2, x2, -24
; CHECK-NEXT:  sd x8, 16(x2)
; CHECK-NEXT:  addi x8, x2, 24
; CHECK-NEXT:  addi [[A:x[0-9]+]], x8, -24
; CHECK-NEXT:  sd x10, 0([[A]])
; CHECK-NEXT:  add x10, x10, x8
define i64 @frame_and_alloca(i64 %n) {
  %a = alloca [2 x i64], align 8
  store i64 %n, ptr %a
  %p = call ptr @llvm.frameaddress.p0(i32 0)
  %v = load i64, ptr %a
  %fp = ptrtoint ptr %p to i64
  %r = add i64 %v, %fp
  ret i64 %r
}

; A leaf function still has the link register in place, so the return address
; is just X1 and no frame is needed at all.
; CHECK-LABEL: retaddr:
; CHECK-NOT:   addi x2, x2, -
; CHECK:       addi x10, x1, 0
; CHECK-NEXT:  jalr x0, 0(x1)
define ptr @retaddr() {
  %p = call ptr @llvm.returnaddress(i32 0)
  ret ptr %p
}

; After a call X1 no longer holds the incoming return address, so the value is
; copied to a callee-saved register before the call.  The prologue spill of X1
; must not claim to kill it: the copy is a later use of the incoming value.
; CHECK-LABEL: retaddr_after_call:
; CHECK:       addi x2, x2, -16
; CHECK-NEXT:  sd x1, 8(x2)
; CHECK-NEXT:  sd x8, 0(x2)
; CHECK-NEXT:  addi x8, x1, 0
; CHECK-NEXT:  jal x1, opaque
; CHECK-NEXT:  addi x10, x8, 0
; CHECK:       ld x8, 0(x2)
; CHECK-NEXT:  ld x1, 8(x2)
define ptr @retaddr_after_call(ptr %x) {
  %r = call ptr @opaque(ptr %x)
  %p = call ptr @llvm.returnaddress(i32 0)
  ret ptr %p
}

; stacksave hands out the stack pointer and stackrestore puts it back.  The
; dynamic alloca in between moves the stack pointer, and the frame pointer
; keeps the fixed part of the frame reachable.
; CHECK-LABEL: save_restore:
; CHECK:       addi x2, x2, -8
; CHECK-NEXT:  sd x8, 0(x2)
; CHECK-NEXT:  addi x8, x2, 8
; CHECK-NEXT:  addi [[SAVED:x[0-9]+]], x2, 0
; CHECK:       sub [[NEWSP:x[0-9]+]], x2,
; CHECK-NEXT:  addi x2, [[NEWSP]], 0
; CHECK:       sd
; CHECK:       addi x2, [[SAVED]], 0
define i64 @save_restore(i64 %n) {
  %sp = call ptr @llvm.stacksave.p0()
  %a = alloca i64, i64 %n, align 8
  store i64 5, ptr %a
  call void @llvm.stackrestore.p0(ptr %sp)
  ret i64 5
}
