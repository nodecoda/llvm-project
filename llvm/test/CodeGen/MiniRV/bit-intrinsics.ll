; RUN: llc < %s -mtriple=minirv64 -O2 | FileCheck %s

; The bit-manipulation intrinsics expand to shifts, masks and arithmetic.  The
; masks are full 64-bit constants, which the selector materialises from their
; two words.

declare i64 @llvm.ctpop.i64(i64)
declare i64 @llvm.ctlz.i64(i64, i1)
declare i64 @llvm.cttz.i64(i64, i1)
declare i64 @llvm.bswap.i64(i64)
declare i64 @llvm.bitreverse.i64(i64)

; The last step of the population count multiplies by 0x0101010101010101 and
; shifts the top byte down.
; CHECK-LABEL: ctpop:
; CHECK:         mul      [[S:x[0-9]+]], [[S]], [[M:x[0-9]+]]
; CHECK-NEXT:    srli     x10, [[S]], 56
define i64 @ctpop(i64 %x) {
  %r = call i64 @llvm.ctpop.i64(i64 %x)
  ret i64 %r
}

; A zero input takes the early exit and yields the word width.
; CHECK-LABEL: ctlz:
; CHECK:         beq      x10, x0,
; CHECK:         srli     [[T:x[0-9]+]], x10, 1
; CHECK-NEXT:    or       [[T]], x10, [[T]]
define i64 @ctlz(i64 %x) {
  %r = call i64 @llvm.ctlz.i64(i64 %x, i1 false)
  ret i64 %r
}

; CHECK-LABEL: cttz:
; CHECK:         beq      x10, x0,
; CHECK:         addi     [[T:x[0-9]+]], x10, -1
; CHECK-NEXT:    xori     [[U:x[0-9]+]], x10, -1
; CHECK-NEXT:    and      [[T]], [[U]], [[T]]
define i64 @cttz(i64 %x) {
  %r = call i64 @llvm.cttz.i64(i64 %x, i1 false)
  ret i64 %r
}

; CHECK-LABEL: bswap:
; CHECK:         srli     [[T:x[0-9]+]], x10, 24
; CHECK:         slli     {{x[0-9]+}}, x10, 56
define i64 @bswap(i64 %x) {
  %r = call i64 @llvm.bswap.i64(i64 %x)
  ret i64 %r
}

; CHECK-LABEL: bitreverse:
; CHECK:         srli     [[T:x[0-9]+]], x10, 24
; CHECK:         slli     {{x[0-9]+}}, x10, 56
define i64 @bitreverse(i64 %x) {
  %r = call i64 @llvm.bitreverse.i64(i64 %x)
  ret i64 %r
}
