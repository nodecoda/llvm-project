; RUN: llc < %s -mtriple=minirv64 -O2 | FileCheck %s

; The DAG builder canonicalises comparisons against zero into comparisons
; against one ("x > 0" becomes "x >= 1").  Those forms must be rewritten back
; to a zero comparison so that the x0 branch encodings match without first
; materialising the constant.

; CHECK-LABEL: sgt_zero:
; CHECK: bge x0, x10, .LBB0_2
; CHECK-NOT: addi x{{[0-9]+}}, x0, 1
define i64 @sgt_zero(i64 %x) {
entry:
  %c = icmp sgt i64 %x, 0
  br i1 %c, label %t, label %f
t:
  ret i64 %x
f:
  ret i64 0
}

; CHECK-LABEL: sge_one:
; CHECK: bge x0, x10, .LBB1_2
; CHECK-NOT: addi x{{[0-9]+}}, x0, 1
define i64 @sge_one(i64 %x) {
entry:
  %c = icmp sge i64 %x, 1
  br i1 %c, label %t, label %f
t:
  ret i64 %x
f:
  ret i64 0
}

; CHECK-LABEL: slt_one:
; CHECK: bge x0, x10, .LBB2_2
; CHECK-NOT: addi x{{[0-9]+}}, x0, 1
define i64 @slt_one(i64 %x) {
entry:
  %c = icmp slt i64 %x, 1
  br i1 %c, label %t, label %f
t:
  ret i64 %x
f:
  ret i64 0
}

; CHECK-LABEL: sle_zero:
; CHECK: blt x0, x10, .LBB3_2
; CHECK-NOT: addi x{{[0-9]+}}, x0, 1
define i64 @sle_zero(i64 %x) {
entry:
  %c = icmp sle i64 %x, 0
  br i1 %c, label %t, label %f
t:
  ret i64 %x
f:
  ret i64 0
}

; CHECK-LABEL: ugt_zero:
; CHECK: beq x10, x0, .LBB4_2
; CHECK-NOT: addi x{{[0-9]+}}, x0, 1
define i64 @ugt_zero(i64 %x) {
entry:
  %c = icmp ugt i64 %x, 0
  br i1 %c, label %t, label %f
t:
  ret i64 %x
f:
  ret i64 0
}

; CHECK-LABEL: ult_one:
; CHECK: bne x10, x0, .LBB5_2
; CHECK-NOT: addi x{{[0-9]+}}, x0, 1
define i64 @ult_one(i64 %x) {
entry:
  %c = icmp ult i64 %x, 1
  br i1 %c, label %t, label %f
t:
  ret i64 %x
f:
  ret i64 0
}

; Equality is unaffected: the zero is already an operand of BEQ/BNE.

; CHECK-LABEL: eq_zero:
; CHECK: beq x10, x0, .LBB6_2
define i64 @eq_zero(i64 %x) {
entry:
  %c = icmp eq i64 %x, 0
  br i1 %c, label %t, label %f
t:
  ret i64 %x
f:
  ret i64 0
}

; CHECK-LABEL: ne_zero:
; CHECK: beq x10, x0, .LBB7_2
define i64 @ne_zero(i64 %x) {
entry:
  %c = icmp ne i64 %x, 0
  br i1 %c, label %t, label %f
t:
  ret i64 %x
f:
  ret i64 0
}

; Sign-extended i32 operands take the same 64-bit path.

; CHECK-LABEL: sgt_zero32:
; CHECK: bge x0, x10, .LBB8_2
; CHECK-NOT: addi x{{[0-9]+}}, x0, 1
define i32 @sgt_zero32(i32 signext %x) {
entry:
  %c = icmp sgt i32 %x, 0
  br i1 %c, label %t, label %f
t:
  ret i32 %x
f:
  ret i32 0
}

; CHECK-LABEL: ult_one32:
; CHECK: bne x10, x0, .LBB9_2
; CHECK-NOT: addi x{{[0-9]+}}, x0, 1
define i32 @ult_one32(i32 signext %x) {
entry:
  %c = icmp ult i32 %x, 1
  br i1 %c, label %t, label %f
t:
  ret i32 %x
f:
  ret i32 0
}
