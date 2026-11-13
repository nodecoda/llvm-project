; RUN: llc -mtriple=minirv64 -O2 < %s | FileCheck %s

; The ABI only guarantees the stack alignment declared by the frame lowering.
; An object that asks for more is reached by rounding the stack pointer down in
; the prologue; the frame pointer keeps the incoming stack pointer so that the
; epilogue can undo the rounding before it reloads the callee-saved registers.

; CHECK-LABEL: aligned32:
; CHECK:       andi    x2, x2, -32
; CHECK-NEXT:  addi    x10, x2, 0
; CHECK:       addi    x2, x8,
define ptr @aligned32() {
  %p = alloca i8, i64 64, align 32
  ret ptr %p
}

; CHECK-LABEL: aligned64:
; CHECK:       andi    x2, x2, -64
; CHECK-NEXT:  addi    x10, x2, 0
; CHECK:       addi    x2, x8,
define ptr @aligned64() {
  %p = alloca i8, i64 64, align 64
  ret ptr %p
}

; The object is addressed from the rounded stack pointer, so the loads and
; stores below it stay inside the realigned area.
; CHECK-LABEL: access_aligned32:
; CHECK:       andi    x2, x2, -32
; CHECK:       addi    x3, x2, 0
; CHECK:       sd      x7, 0(x4)
; CHECK:       ld      x3, 0(x3)
define i64 @access_aligned32() {
  %p = alloca [4 x i64], align 32
  %q = getelementptr [4 x i64], ptr %p, i64 0, i64 3
  store i64 42, ptr %q, align 8
  %v = load i64, ptr %p, align 8
  %w = load i64, ptr %q, align 8
  %r = add i64 %v, %w
  ret i64 %r
}
