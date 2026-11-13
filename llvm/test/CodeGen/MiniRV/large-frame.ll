; RUN: llc < %s -mtriple=minirv64 -O2 | FileCheck %s

; A frame larger than the 12-bit signed immediate range cannot be allocated
; with a single addi.  Values that fit in two addis are split, keeping the
; intermediate stack pointer aligned.

; 2400 bytes: addi -2048 followed by addi -352.  The object sits 2400 bytes
; above the stack pointer, so its address is built the same way.
; CHECK-LABEL: big_array:
; CHECK:       addi x2, x2, -2048
; CHECK-NEXT:  addi x2, x2, -352
; CHECK:       addi x3, x2, 2047
; CHECK-NEXT:  addi x3, x3, 345
; CHECK-NEXT:  sd x10, 0(x3)
; CHECK:       addi x2, x2, 2032
; CHECK-NEXT:  addi x2, x2, 368
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
; register allocation.  The 4800-byte frame still needs lui+addi+add, but the
; object address is two addis.
; CHECK-LABEL: two_arrays:
; CHECK:       lui x6, -1
; CHECK-NEXT:  addi x6, x6, -704
; CHECK-NEXT:  add x2, x6, x2
; CHECK-NEXT:  addi x3, x2, 2032
; CHECK-NEXT:  addi x3, x3, 368
; CHECK-NEXT:  sd x10, 0(x3)
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
; CHECK:       addi x2, x2, -2048
; CHECK-NEXT:  addi x2, x2, -368
; CHECK:       addi x5, x2, 2032
; CHECK-NEXT:  addi x5, x5, 376
; CHECK-NEXT:  sd x1, 0(x5)
; CHECK:       jal x1, callee
; CHECK:       addi x5, x2, 2032
; CHECK-NEXT:  addi x5, x5, 368
; CHECK-NEXT:  ld x8, 0(x5)
; CHECK:       addi x5, x2, 2032
; CHECK-NEXT:  addi x5, x5, 376
; CHECK-NEXT:  ld x1, 0(x5)
; CHECK:       addi x2, x2, 2032
; CHECK-NEXT:  addi x2, x2, 384
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
