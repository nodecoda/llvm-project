; RUN: llc -mtriple=minirv64 < %s | FileCheck %s

; Complex multiplication and division are lowered by clang to the compiler-rt
; helpers; the backend only has to pass the pieces correctly.  float complex
; comes back packed in one GPR, double complex in a pair, and long double
; complex through an sret pointer with its arguments passed by reference, as
; the psABI requires for 128-bit scalars.

declare i64 @__mulsc3(float, float, float, float)
declare i64 @__divsc3(float, float, float, float)
declare [2 x i64] @__muldc3(double, double, double, double)
declare [2 x i64] @__divdc3(double, double, double, double)
declare void @__multc3(ptr, fp128, fp128, fp128, fp128)
declare void @__divtc3(ptr, fp128, fp128, fp128, fp128)

; CHECK-LABEL: cmul:
; CHECK:       jal     x1, __mulsc3
; CHECK:       jalr    x0, 0(x1)
define i64 @cmul(float %ar, float %ai, float %br, float %bi) {
  %r = call i64 @__mulsc3(float %ar, float %ai, float %br, float %bi)
  ret i64 %r
}

; CHECK-LABEL: cdiv:
; CHECK:       jal     x1, __divsc3
define i64 @cdiv(float %ar, float %ai, float %br, float %bi) {
  %r = call i64 @__divsc3(float %ar, float %ai, float %br, float %bi)
  ret i64 %r
}

; The two halves of the result come back in x10 and x11.
; CHECK-LABEL: dmul:
; CHECK:       jal     x1, __muldc3
; CHECK:       jalr    x0, 0(x1)
define [2 x i64] @dmul(double %ar, double %ai, double %br, double %bi) {
  %r = call [2 x i64] @__muldc3(double %ar, double %ai, double %br, double %bi)
  ret [2 x i64] %r
}

; CHECK-LABEL: ddiv:
; CHECK:       jal     x1, __divdc3
define [2 x i64] @ddiv(double %ar, double %ai, double %br, double %bi) {
  %r = call [2 x i64] @__divdc3(double %ar, double %ai, double %br, double %bi)
  ret [2 x i64] %r
}

; The fifth argument does not fit in the argument registers because the sret
; pointer takes one and the four fp128 values are passed by reference; the
; callee hands its incoming stack slot to the helper.
; CHECK-LABEL: lmul:
; CHECK:       ld      [[SLOT:x[0-9]+]], 16(x2)
; CHECK-NEXT:  sd      [[SLOT]], 0(x2)
; CHECK-NEXT:  jal     x1, __multc3
define void @lmul(ptr sret({ fp128, fp128 }) align 16 %s, fp128 %ar, fp128 %ai, fp128 %br, fp128 %bi) {
  call void @__multc3(ptr align 16 %s, fp128 %ar, fp128 %ai, fp128 %br, fp128 %bi)
  ret void
}

; CHECK-LABEL: ldiv:
; CHECK:       jal     x1, __divtc3
define void @ldiv(ptr sret({ fp128, fp128 }) align 16 %s, fp128 %ar, fp128 %ai, fp128 %br, fp128 %bi) {
  call void @__divtc3(ptr align 16 %s, fp128 %ar, fp128 %ai, fp128 %br, fp128 %bi)
  ret void
}
