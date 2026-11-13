; RUN: llc < %s -mtriple=minirv64 -O2 | FileCheck %s

; Every branch condition is normalized to one of the six records the corpus
; provides; the rest are formed by swapping the operands.

; CHECK-LABEL: sgt:
; CHECK: bge x11, x10, .LBB0_2
define i64 @sgt(i64 %a, i64 %b) {
entry:
  %c = icmp sgt i64 %a, %b
  br i1 %c, label %t, label %f
t:
  ret i64 %a
f:
  ret i64 %b
}

; CHECK-LABEL: slt:
; CHECK: bge x10, x11, .LBB1_2
define i64 @slt(i64 %a, i64 %b) {
entry:
  %c = icmp slt i64 %a, %b
  br i1 %c, label %t, label %f
t:
  ret i64 %a
f:
  ret i64 %b
}

; CHECK-LABEL: ule:
; CHECK: bltu x11, x10, .LBB2_2
define i64 @ule(i64 %a, i64 %b) {
entry:
  %c = icmp ule i64 %a, %b
  br i1 %c, label %t, label %f
t:
  ret i64 %a
f:
  ret i64 %b
}

; CHECK-LABEL: ugt:
; CHECK: bgeu x11, x10, .LBB3_2
define i64 @ugt(i64 %a, i64 %b) {
entry:
  %c = icmp ugt i64 %a, %b
  br i1 %c, label %t, label %f
t:
  ret i64 %a
f:
  ret i64 %b
}

; CHECK-LABEL: eq:
; CHECK: bne x10, x11, .LBB4_2
define i64 @eq(i64 %a, i64 %b) {
entry:
  %c = icmp eq i64 %a, %b
  br i1 %c, label %t, label %f
t:
  ret i64 %a
f:
  ret i64 %b
}

; CHECK-LABEL: ne:
; CHECK: beq x10, x11, .LBB5_2
define i64 @ne(i64 %a, i64 %b) {
entry:
  %c = icmp ne i64 %a, %b
  br i1 %c, label %t, label %f
t:
  ret i64 %a
f:
  ret i64 %b
}

; A loop exercises the same records plus the unconditional jump and the
; pseudo return, which expands to jalr x0, 0(x1).

; CHECK-LABEL: sum_to:
; CHECK: add x{{[0-9]+}}, x{{[0-9]+}}, x{{[0-9]+}}
; CHECK: bne x{{[0-9]+}}, x{{[0-9]+}}, .LBB6_1
; CHECK: jal x0, .LBB6_2
; CHECK: jalr x0, 0(x1)
define i64 @sum_to(i64 %n) {
entry:
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %i1, %loop ]
  %s = phi i64 [ 0, %entry ], [ %s1, %loop ]
  %i1 = add i64 %i, 1
  %s1 = add i64 %s, %i
  %done = icmp eq i64 %i1, %n
  br i1 %done, label %exit, label %loop
exit:
  ret i64 %s1
}
