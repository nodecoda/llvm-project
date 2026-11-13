; RUN: llc -mtriple=minirv64 < %s | FileCheck %s

; The corpus has SLT/SLTU but no "set if equal", so equality is XOR followed
; by a compare against zero.  Sign-extended i32 operands compare correctly
; with the 64-bit instructions: sign extension is monotonic for signed
; compares, and it maps the two halves of the unsigned range in order, so SLTU
; also agrees with a 32-bit unsigned compare.

; CHECK-LABEL: eq32:
; CHECK:         xor     x3, x10, x11
; CHECK-NEXT:    sltiu   x10, x3, 1
define signext i32 @eq32(i32 signext %a, i32 signext %b) {
  %c = icmp eq i32 %a, %b
  %r = zext i1 %c to i32
  ret i32 %r
}

; CHECK-LABEL: ne32:
; CHECK:         xor     x3, x10, x11
; CHECK-NEXT:    sltu    x10, x0, x3
define signext i32 @ne32(i32 signext %a, i32 signext %b) {
  %c = icmp ne i32 %a, %b
  %r = zext i1 %c to i32
  ret i32 %r
}

; CHECK-LABEL: slt32:
; CHECK:         slt     x10, x10, x11
define signext i32 @slt32(i32 signext %a, i32 signext %b) {
  %c = icmp slt i32 %a, %b
  %r = zext i1 %c to i32
  ret i32 %r
}

; CHECK-LABEL: ult32:
; CHECK:         sltu    x10, x10, x11
define signext i32 @ult32(i32 signext %a, i32 signext %b) {
  %c = icmp ult i32 %a, %b
  %r = zext i1 %c to i32
  ret i32 %r
}

; CHECK-LABEL: sge32:
; CHECK:         slt     x3, x10, x11
; CHECK-NEXT:    xori    x10, x3, 1
define signext i32 @sge32(i32 signext %a, i32 signext %b) {
  %c = icmp sge i32 %a, %b
  %r = zext i1 %c to i32
  ret i32 %r
}

; CHECK-LABEL: ule32:
; CHECK:         sltu    x3, x11, x10
; CHECK-NEXT:    xori    x10, x3, 1
define signext i32 @ule32(i32 signext %a, i32 signext %b) {
  %c = icmp ule i32 %a, %b
  %r = zext i1 %c to i32
  ret i32 %r
}

; An i32 compare that feeds a branch is fused into a single conditional
; branch; the branch is inverted and the true block falls through.

; CHECK-LABEL: branch32:
; CHECK:         bge     x10, x11
define signext i32 @branch32(i32 signext %a, i32 signext %b) {
entry:
  %c = icmp slt i32 %a, %b
  br i1 %c, label %t, label %f
t:
  ret i32 1
f:
  ret i32 2
}
