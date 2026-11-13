; RUN: llc -mtriple=minirv64 < %s | FileCheck %s

; There is no conditional move, so a select becomes mask arithmetic:
; a comparison produces zero or one, negating it gives an all-ones mask, and
; the result is false ^ ((true ^ false) & mask).  A condition that is already
; a sign-extended i1 is its own mask.

; CHECK-LABEL: sel64:
; CHECK:         xor     x3, x11, x12
; CHECK:         slli    x4, x10, 63
; CHECK:         srai    x4, x4, 63
; CHECK:         and     x3, x3, x4
; CHECK:         xor     x10, x12, x3
define i64 @sel64(i1 %c, i64 %a, i64 %b) {
  %r = select i1 %c, i64 %a, i64 %b
  ret i64 %r
}

; CHECK-LABEL: sel_i1:
; CHECK-NOT:     slli
; CHECK:         xor     x3, x11, x12
; CHECK-NEXT:    and     x3, x3, x10
; CHECK-NEXT:    xor     x10, x12, x3
define signext i32 @sel_i1(i1 signext %c, i32 signext %a, i32 signext %b) {
  %r = select i1 %c, i32 %a, i32 %b
  ret i32 %r
}

; CHECK-LABEL: selcmp:
; CHECK:         slt     x3, x10, x11
; CHECK:         sub     x3, x4, x3
; CHECK:         xor     x4, x10, x11
; CHECK:         and     x3, x4, x3
; CHECK:         xor     x10, x11, x3
define signext i32 @selcmp(i32 signext %a, i32 signext %b) {
  %c = icmp slt i32 %a, %b
  %r = select i1 %c, i32 %a, i32 %b
  ret i32 %r
}
