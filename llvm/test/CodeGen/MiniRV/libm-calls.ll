; RUN: llc -mtriple=minirv64 < %s | FileCheck %s
; RUN: llc -mtriple=minirv64 -filetype=obj < %s -o %t.o
; RUN: llvm-objdump -dr --triple=minirv64 %t.o | FileCheck %s --check-prefix=OBJ

; Without an FPU the rounding and transcendental intrinsics cannot be selected,
; so they become calls to libm.  llvm.trap is an abort.

; CHECK-LABEL: sqrt64:
; CHECK:         jal     x1, sqrt
define double @sqrt64(double %a) {
  %r = call double @llvm.sqrt.f64(double %a)
  ret double %r
}

; CHECK-LABEL: sqrt32:
; CHECK:         jal     x1, sqrtf
define float @sqrt32(float %a) {
  %r = call float @llvm.sqrt.f32(float %a)
  ret float %r
}

; CHECK-LABEL: fma64:
; CHECK:         jal     x1, fma
define double @fma64(double %a, double %b, double %c) {
  %r = call double @llvm.fma.f64(double %a, double %b, double %c)
  ret double %r
}

; CHECK-LABEL: floor64:
; CHECK:         jal     x1, floor
define double @floor64(double %a) {
  %r = call double @llvm.floor.f64(double %a)
  ret double %r
}

; CHECK-LABEL: ceil64:
; CHECK:         jal     x1, ceil
define double @ceil64(double %a) {
  %r = call double @llvm.ceil.f64(double %a)
  ret double %r
}

; CHECK-LABEL: trunc64:
; CHECK:         jal     x1, trunc
define double @trunc64(double %a) {
  %r = call double @llvm.trunc.f64(double %a)
  ret double %r
}

; CHECK-LABEL: round64:
; CHECK:         jal     x1, round
define double @round64(double %a) {
  %r = call double @llvm.round.f64(double %a)
  ret double %r
}

; CHECK-LABEL: rint64:
; CHECK:         jal     x1, rint
define double @rint64(double %a) {
  %r = call double @llvm.rint.f64(double %a)
  ret double %r
}

; CHECK-LABEL: nearbyint64:
; CHECK:         jal     x1, nearbyint
define double @nearbyint64(double %a) {
  %r = call double @llvm.nearbyint.f64(double %a)
  ret double %r
}

; minnum/maxnum follow the IEEE 754-2008 semantics and map to fmin/fmax.
; CHECK-LABEL: minnum64:
; CHECK:         jal     x1, fmin
define double @minnum64(double %a, double %b) {
  %r = call double @llvm.minnum.f64(double %a, double %b)
  ret double %r
}

; CHECK-LABEL: maxnum64:
; CHECK:         jal     x1, fmax
define double @maxnum64(double %a, double %b) {
  %r = call double @llvm.maxnum.f64(double %a, double %b)
  ret double %r
}

; A floating-point remainder is fmod.
; CHECK-LABEL: frem64:
; CHECK:         jal     x1, fmod
define double @frem64(double %a, double %b) {
  %r = frem double %a, %b
  ret double %r
}

; llvm.trap must not fall through.
; CHECK-LABEL: trap:
; CHECK:         jal     x1, abort
define void @trap() {
  call void @llvm.trap()
  unreachable
}

; OBJ-LABEL: <sqrt64>:
; OBJ:         R_RISCV_JAL  sqrt
; OBJ-LABEL: <frem64>:
; OBJ:         R_RISCV_JAL  fmod

declare double @llvm.sqrt.f64(double)
declare float @llvm.sqrt.f32(float)
declare double @llvm.fma.f64(double, double, double)
declare double @llvm.floor.f64(double)
declare double @llvm.ceil.f64(double)
declare double @llvm.trunc.f64(double)
declare double @llvm.round.f64(double)
declare double @llvm.rint.f64(double)
declare double @llvm.nearbyint.f64(double)
declare double @llvm.minnum.f64(double, double)
declare double @llvm.maxnum.f64(double, double)
declare void @llvm.trap()
