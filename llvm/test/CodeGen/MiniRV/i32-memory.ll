; RUN: llc -mtriple=minirv64 < %s | FileCheck %s

; Narrow accesses are legal: the type legalizer promotes the value to i64 and
; the extend/truncate flavour of the node picks the byte, halfword or word
; instruction.  A word access that only feeds a 32-bit value is zero-extended
; (LWU); it becomes LW when the caller asked for a sign-extended i32.

; CHECK-LABEL: load_i8:
; CHECK:         lb      x10, 0(x10)
define signext i32 @load_i8(ptr %p) {
  %v = load i8, ptr %p
  %e = sext i8 %v to i32
  ret i32 %e
}

; CHECK-LABEL: load_u8:
; CHECK:         lbu     x10, 0(x10)
define i32 @load_u8(ptr %p) {
  %v = load i8, ptr %p
  %e = zext i8 %v to i32
  ret i32 %e
}

; CHECK-LABEL: load_i16:
; CHECK:         lh      x10, 0(x10)
define signext i32 @load_i16(ptr %p) {
  %v = load i16, ptr %p
  %e = sext i16 %v to i32
  ret i32 %e
}

; CHECK-LABEL: load_u16:
; CHECK:         lhu     x10, 0(x10)
define i32 @load_u16(ptr %p) {
  %v = load i16, ptr %p
  %e = zext i16 %v to i32
  ret i32 %e
}

; CHECK-LABEL: load_i32:
; CHECK:         lw      x10, 0(x10)
define signext i32 @load_i32(ptr %p) {
  %v = load i32, ptr %p
  ret i32 %v
}

; CHECK-LABEL: load_u32:
; CHECK:         lwu     x10, 0(x10)
define i64 @load_u32(ptr %p) {
  %v = load i32, ptr %p
  %e = zext i32 %v to i64
  ret i64 %e
}

; CHECK-LABEL: store_i8:
; CHECK:         sb      x11, 0(x10)
define void @store_i8(ptr %p, i8 %v) {
  store i8 %v, ptr %p
  ret void
}

; CHECK-LABEL: store_i16:
; CHECK:         sh      x11, 0(x10)
define void @store_i16(ptr %p, i16 %v) {
  store i16 %v, ptr %p
  ret void
}

; CHECK-LABEL: store_i32:
; CHECK:         sw      x11, 0(x10)
define void @store_i32(ptr %p, i32 %v) {
  store i32 %v, ptr %p
  ret void
}
