; RUN: llc < %s -mtriple=minirv64 -O2 | FileCheck %s

; A frame larger than the 12-bit signed immediate range cannot be allocated
; with a single addi.  The adjustment is materialised with lui+addi and then
; added to the stack pointer.

; 2400 bytes: lui x6, -1 (0xfffff000) + 1696 = -2400, added to the stack
; pointer.  The constant is built in x6 because the stack pointer is both
; source and destination, and lui would destroy it.
; CHECK-LABEL: big_array:
; CHECK:       lui x6, -1
; CHECK-NEXT:  addi x6, x6, 1696
; CHECK-NEXT:  add x2, x6, x2
; CHECK:       lui x6, 1
; CHECK-NEXT:  addi x6, x6, -1696
; CHECK-NEXT:  add x2, x6, x2
; CHECK-NEXT:  jalr x0, 0(x1)
define i64 @big_array(i64 %n) #0 {
entry:
  %a = alloca [300 x i64], align 8
  %p = getelementptr inbounds [300 x i64], ptr %a, i64 0, i64 299
  store i64 %n, ptr %p
  %v = load i64, ptr %p
  ret i64 %v
}

; An object more than 2047 bytes away from the stack pointer is reached
; through a base register that LocalStackSlotAllocation materialises before
; register allocation.
; CHECK-LABEL: two_arrays:
; CHECK:       lui [[BASE:x[0-9]+]], 1
; CHECK-NEXT:  addi [[BASE]], [[BASE]], -1696
; CHECK-NEXT:  add [[BASE]], [[BASE]], x2
; CHECK-NEXT:  sd x10, 0([[BASE]])
define i64 @two_arrays(i64 %n) #0 {
entry:
  %a = alloca [300 x i64], align 8
  %b = alloca [300 x i64], align 8
  %pa = getelementptr inbounds [300 x i64], ptr %a, i64 0, i64 0
  %pb = getelementptr inbounds [300 x i64], ptr %b, i64 0, i64 0
  store i64 %n, ptr %pa
  store i64 %n, ptr %pb
  %va = load i64, ptr %pa
  %vb = load i64, ptr %pb
  %r = add i64 %va, %vb
  ret i64 %r
}

; The return address and the frame's other callee-saved registers live at the
; top of the frame, out of reach of a 12-bit displacement.  They are spilled
; after the stack pointer has been lowered and restored before it is raised
; again.
; CHECK-LABEL: caller:
; CHECK:       lui x6, -1
; CHECK-NEXT:  addi x6, x6, 1680
; CHECK-NEXT:  add x2, x6, x2
; CHECK:       lui [[TMP:x[0-9]+]], 1
; CHECK-NEXT:  addi [[TMP]], [[TMP]], -1688
; CHECK-NEXT:  add [[TMP]], [[TMP]], x2
; CHECK-NEXT:  sd x1, 0([[TMP]])
; CHECK:       jal x1, callee
; CHECK:       ld x1, 0(
; CHECK:       lui x6, 1
; CHECK-NEXT:  addi x6, x6, -1680
; CHECK-NEXT:  add x2, x6, x2
; CHECK-NEXT:  jalr x0, 0(x1)
define i64 @caller(i64 %n) #0 {
entry:
  %a = alloca [300 x i64], align 8
  %p = getelementptr inbounds [300 x i64], ptr %a, i64 0, i64 0
  store i64 %n, ptr %p
  %r = call i64 @callee(i64 %n)
  %v = load i64, ptr %p
  %s = add i64 %r, %v
  ret i64 %s
}

declare i64 @callee(i64)

; These tests describe the frame layout itself; opt out of the unwind table so
; that CFI directives do not clutter the instruction sequences under test.
attributes #0 = { nounwind }
