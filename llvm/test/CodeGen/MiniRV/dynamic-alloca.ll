; RUN: llc < %s -mtriple=minirv64 -O2 | FileCheck %s

; A variable-length alloca moves the stack pointer while the function runs, so
; the fixed part of the frame can no longer be addressed from the stack
; pointer.  X8 becomes a frame pointer holding the incoming stack pointer; it
; is written after the old value has been spilled, and the stack pointer is
; recomputed from it in the epilogue.
;
; The size is rounded up to the stack alignment before it reaches the
; backend, so the allocation is a shift, a subtraction and a move.
; CHECK-LABEL: vla:
; CHECK:       addi x2, x2, -16
; CHECK-NEXT:  sd x8, 8(x2)
; CHECK-NEXT:  addi x8, x2, 16
; CHECK:       slli [[SZ:x[0-9]+]], x10, 3
; CHECK:       addi [[SZ]], [[SZ]], 15
; CHECK-NEXT:  andi [[SZ]], [[SZ]], -16
; CHECK-NEXT:  sub [[NEWSP:x[0-9]+]], x2, [[SZ]]
; CHECK-NEXT:  addi x2, [[NEWSP]], 0
; CHECK:       sd x10, 8([[NEWSP]])
; CHECK:       addi x2, x8, -16
; CHECK-NEXT:  ld x8, 8(x2)
; CHECK-NEXT:  addi x2, x2, 16
; CHECK-NEXT:  jalr x0, 0(x1)
define i64 @vla(i64 %n) #0 {
entry:
  %a = alloca i64, i64 %n, align 8
  %p = getelementptr i64, ptr %a, i64 1
  store i64 7, ptr %p
  %v = load i64, ptr %p
  ret i64 %v
}

declare i64 @nine(i64, i64, i64, i64, i64, i64, i64, i64, i64)

; The outgoing argument area is part of the prologue only when the stack
; pointer stays put.  Here the call sequence has to claim it at the call site.
; CHECK-LABEL: vla_call:
; CHECK:       sd x10, 8([[DYN:x[0-9]+]])
; CHECK:       addi x2, x2, -16
; CHECK-NEXT:  sd x10, 0(x2)
; CHECK:       jal x1, nine
; CHECK-NEXT:  addi x2, x2, 16
define i64 @vla_call(i64 %n) #0 {
entry:
  %a = alloca i64, i64 %n, align 8
  %p = getelementptr i64, ptr %a, i64 1
  store i64 5, ptr %p
  %v = load i64, ptr %p
  %r = call i64 @nine(i64 %v, i64 1, i64 2, i64 3, i64 4, i64 5, i64 6, i64 7,
                      i64 %v)
  ret i64 %r
}

; With a large fixed frame the objects are out of reach of a 12-bit
; displacement even from the frame pointer, so their address is materialised
; in the address scratch register.  The frame pointer itself is set up after
; the spill of the incoming one, which is what makes the spill store the
; caller's value.
; CHECK-LABEL: vla_big:
; CHECK:       addi x2, x2, -2032
; CHECK-NEXT:  sd x8, 2024(x2)
; CHECK-NEXT:  addi x2, x2, -384
; CHECK:       addi x8, x2, 2032
; CHECK-NEXT:  addi x8, x8, 384
; CHECK:       addi [[A:x[0-9]+]], x8, -2048
; CHECK-NEXT:  addi [[A]], [[A]], -360
; CHECK:       sd {{x[0-9]+}}, 0([[A]])
; CHECK:       addi x2, x8, -2048
; CHECK-NEXT:  addi x2, x2, -368
; CHECK-NEXT:  addi x2, x2, 384
; CHECK:       ld x8, 2024(x2)
; CHECK-NEXT:  addi x2, x2, 2032
define i64 @vla_big(i64 %n) #0 {
entry:
  %fixed = alloca [300 x i64], align 8
  %dyn = alloca i64, i64 %n, align 8
  store i64 9, ptr %fixed
  %p = getelementptr i64, ptr %dyn, i64 1
  store i64 3, ptr %p
  %v = load i64, ptr %p
  %w = load i64, ptr %fixed
  %r = add i64 %v, %w
  ret i64 %r
}

; A function without a variable-length allocation keeps its frame
; stack-pointer relative and never touches the frame pointer.
; CHECK-LABEL: no_vla:
; CHECK-NOT:   x8
; CHECK:       jalr x0, 0(x1)
define i64 @no_vla(i64 %n) #0 {
entry:
  %a = alloca [2 x i64], align 8
  %p = getelementptr [2 x i64], ptr %a, i64 0, i64 1
  store i64 %n, ptr %p
  %v = load i64, ptr %p
  ret i64 %v
}

; These tests describe the frame layout itself; opt out of the unwind table so
; that CFI directives do not clutter the instruction sequences under test.
attributes #0 = { nounwind }
