; RUN: llc -mtriple=minirv64 < %s | FileCheck %s

; i32 is not a legal type: the type legalizer promotes it to i64.  Operations
; that have an *W form are custom-lowered through a target node so that the
; 32-bit wraparound is preserved; the W instruction already writes a
; sign-extended result, which is the promoted representation of an i32 value.

; CHECK-LABEL: add32:
; CHECK:         addw    x10, x10, x11
define signext i32 @add32(i32 signext %a, i32 signext %b) {
  %r = add i32 %a, %b
  ret i32 %r
}

; CHECK-LABEL: sub32:
; CHECK:         subw    x10, x10, x11
define signext i32 @sub32(i32 signext %a, i32 signext %b) {
  %r = sub i32 %a, %b
  ret i32 %r
}

; CHECK-LABEL: addi32:
; CHECK:         addiw   x10, x10, 12
define signext i32 @addi32(i32 signext %a) {
  %r = add i32 %a, 12
  ret i32 %r
}

; CHECK-LABEL: mul32:
; CHECK:         mulw    x10, x10, x11
define signext i32 @mul32(i32 signext %a, i32 signext %b) {
  %r = mul i32 %a, %b
  ret i32 %r
}

; CHECK-LABEL: sdiv32:
; CHECK:         divw    x10, x10, x11
define signext i32 @sdiv32(i32 signext %a, i32 signext %b) {
  %r = sdiv i32 %a, %b
  ret i32 %r
}

; CHECK-LABEL: udiv32:
; CHECK:         divuw   x10, x10, x11
define signext i32 @udiv32(i32 signext %a, i32 signext %b) {
  %r = udiv i32 %a, %b
  ret i32 %r
}

; CHECK-LABEL: srem32:
; CHECK:         remw    x10, x10, x11
define signext i32 @srem32(i32 signext %a, i32 signext %b) {
  %r = srem i32 %a, %b
  ret i32 %r
}

; CHECK-LABEL: urem32:
; CHECK:         remuw   x10, x10, x11
define signext i32 @urem32(i32 signext %a, i32 signext %b) {
  %r = urem i32 %a, %b
  ret i32 %r
}

; A register shift amount is a 32-bit value, so it is zero-extended before the
; W shift reads its low five bits.

; CHECK-LABEL: shl32:
; CHECK:         slli    x3, x11, 32
; CHECK-NEXT:    srli    x3, x3, 32
; CHECK-NEXT:    sllw    x10, x10, x3
define signext i32 @shl32(i32 signext %a, i32 signext %b) {
  %r = shl i32 %a, %b
  ret i32 %r
}

; CHECK-LABEL: srl32:
; CHECK:         srlw    x10, x10, x3
define signext i32 @srl32(i32 signext %a, i32 signext %b) {
  %r = lshr i32 %a, %b
  ret i32 %r
}

; CHECK-LABEL: sra32:
; CHECK:         sraw    x10, x10, x3
define signext i32 @sra32(i32 signext %a, i32 signext %b) {
  %r = ashr i32 %a, %b
  ret i32 %r
}

; CHECK-LABEL: shli32:
; CHECK:         slliw   x10, x10, 3
define signext i32 @shli32(i32 signext %a) {
  %r = shl i32 %a, 3
  ret i32 %r
}

; A logical right shift zero-fills the upper word, but an i32 result is
; sign-extended, so the low word is widened with a shift pair.

; CHECK-LABEL: srli32:
; CHECK:         srliw   x3, x10, 3
; CHECK-NEXT:    slli    x3, x3, 32
; CHECK-NEXT:    srli    x10, x3, 32
define signext i32 @srli32(i32 signext %a) {
  %r = lshr i32 %a, 3
  ret i32 %r
}

; CHECK-LABEL: srai32:
; CHECK:         sraiw   x10, x10, 3
define signext i32 @srai32(i32 signext %a) {
  %r = ashr i32 %a, 3
  ret i32 %r
}

; i8 and i16 are promoted through i32, so byte and halfword arithmetic is
; plain 64-bit arithmetic plus the mask or shift pair that models the narrow
; type.

; CHECK-LABEL: add8:
; CHECK:         add     x3, x10, x11
; CHECK-NEXT:    andi    x10, x3, 255
define i32 @add8(i8 signext %a, i8 signext %b) {
  %s = add i8 %a, %b
  %z = zext i8 %s to i32
  ret i32 %z
}

; CHECK-LABEL: mul16:
; CHECK:         mul     x3, x10, x11
; CHECK-NEXT:    slli    x3, x3, 48
; CHECK-NEXT:    srai    x10, x3, 48
define i32 @mul16(i16 signext %a, i16 signext %b) {
  %s = mul i16 %a, %b
  %z = sext i16 %s to i32
  ret i32 %z
}
