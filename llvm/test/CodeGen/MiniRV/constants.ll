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

; A 64-bit value is built from its two words.  The low word is materialised
; zero-extended, the high word is materialised and shifted into place, and the
; two are combined with an or.
; 0x0123456789ABCDEF: high word 0x01234567, low word 0x89ABCDEF.
; CHECK-LABEL: wide64:
; CHECK:         lui     [[HI:x[0-9]+]], 4660
; CHECK-NEXT:    addi    [[HI]], [[HI]], 1383
; CHECK-NEXT:    slli    [[HI]], [[HI]], 32
; CHECK-NEXT:    lui     [[LO:x[0-9]+]], -484675
; CHECK-NEXT:    addi    [[LO]], [[LO]], -529
; CHECK-NEXT:    slli    [[LO]], [[LO]], 32
; CHECK-NEXT:    srli    [[LO]], [[LO]], 32
; CHECK-NEXT:    or      x10, [[HI]], [[LO]]
define i64 @wide64() {
  ret i64 81985529216486895
}

; 0xFFFFFFFF7FFFFFFF: the low word 0x7FFFFFFF fits in the signed 32-bit range;
; the high word 0xFFFFFFFF needs the upper-word clearing pair.
; CHECK-LABEL: negative64:
; CHECK:         lui     [[LO:x[0-9]+]], 524288
; CHECK-NEXT:    addi    [[LO]], [[LO]], -1
; CHECK-NEXT:    lui     [[HI:x[0-9]+]], 0
; CHECK-NEXT:    addi    [[HI]], [[HI]], -1
; CHECK-NEXT:    slli    [[HI]], [[HI]], 32
; CHECK-NEXT:    srli    [[HI]], [[HI]], 32
; CHECK-NEXT:    slli    [[HI]], [[HI]], 32
; CHECK-NEXT:    or      x10, [[HI]], [[LO]]
define i64 @negative64() {
  ret i64 -2147483649
}
