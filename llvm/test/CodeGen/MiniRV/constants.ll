; RUN: llc -mtriple=minirv64 < %s | FileCheck %s

; The corpus has no "load immediate" instruction, so the selector builds
; constants out of the two records it does have: LUI for the high 20 bits and
; ADDI for the signed low 12.  These tests pin the split, including the
; rounding that the LUI immediate needs when bit 11 of the low part is set.

; A value that fits in a signed 12-bit immediate needs only ADDI from x0.
; CHECK-LABEL: small:
; CHECK:         addi    x10, x0, 42
define i64 @small() {
  ret i64 42
}

; CHECK-LABEL: negative:
; CHECK:         addi    x10, x0, -2048
define i64 @negative() {
  ret i64 -2048
}

; 0x12345678: high = 0x12345, low = 0x678.
; CHECK-LABEL: wide:
; CHECK:         lui     x3, 74565
; CHECK-NEXT:    addi    x10, x3, 1656
define i64 @wide() {
  ret i64 305419896
}

; 0x1000800: the low part is 0x800, which does not fit in a signed 12-bit
; immediate, so the high part is rounded up and the low part becomes -2048.
; CHECK-LABEL: rounded:
; CHECK:         lui     x3, 4097
; CHECK-NEXT:    addi    x10, x3, -2048
define i64 @rounded() {
  ret i64 16779264
}
