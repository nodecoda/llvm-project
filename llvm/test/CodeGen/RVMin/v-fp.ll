; RUN: llc < %s -march=rvmin -mtriple=riscv32-unknown-elf -mattr=+v,+f -verify-machineinstrs -O0 | FileCheck %s --check-prefix=CHECK
; RUN: llc < %s -march=rvmin -mtriple=riscv32-unknown-elf -mattr=+v,+f -verify-machineinstrs -O2 | FileCheck %s --check-prefix=CHECK
; RUN: llc < %s -march=rvmin -mtriple=riscv32-unknown-elf -mattr=+v,+f -verify-machineinstrs -O2 \
; RUN:   | llvm-mc -triple=riscv32 -mattr=+v,+f -filetype=obj -o /dev/null
; RUN: llc < %s -march=rvmin -mtriple=riscv32-unknown-elf -mattr=+v -verify-machineinstrs -O2 \
; RUN:   | FileCheck %s --check-prefix=NOF
;
; Chapter 54: <4 x float> uses the same 128-bit VR file as <4 x i32>.
; With +v,+f the arithmetic selects vfadd/vfsub/vfmul; the scalar boundary
; uses vfmv.v.f / vfmv.f.s.  Without +f the same IR softens to scalar calls
; and must not select any vector-float instruction.

target triple = "riscv32-unknown-elf"

define void @fadd4(ptr %a, ptr %b, ptr %out) {
  %va = load <4 x float>, ptr %a, align 16
  %vb = load <4 x float>, ptr %b, align 16
  %r = fadd <4 x float> %va, %vb
  store <4 x float> %r, ptr %out, align 16
  ret void
}

define void @fsub4(ptr %a, ptr %b, ptr %out) {
  %va = load <4 x float>, ptr %a, align 16
  %vb = load <4 x float>, ptr %b, align 16
  %r = fsub <4 x float> %va, %vb
  store <4 x float> %r, ptr %out, align 16
  ret void
}

define void @fmul4(ptr %a, ptr %b, ptr %out) {
  %va = load <4 x float>, ptr %a, align 16
  %vb = load <4 x float>, ptr %b, align 16
  %r = fmul <4 x float> %va, %vb
  store <4 x float> %r, ptr %out, align 16
  ret void
}

define void @splat_store(float %x, ptr %out) {
  %v0 = insertelement <4 x float> poison, float %x, i32 0
  %v = shufflevector <4 x float> %v0, <4 x float> poison, <4 x i32> zeroinitializer
  store <4 x float> %v, ptr %out, align 16
  ret void
}

define float @extract0(ptr %p) {
  %v = load volatile <4 x float>, ptr %p, align 16
  %r = extractelement <4 x float> %v, i32 0
  ret float %r
}

define float @extract2(ptr %p) {
  %v = load volatile <4 x float>, ptr %p, align 16
  %r = extractelement <4 x float> %v, i32 2
  ret float %r
}

; CHECK-LABEL: fadd4:
; CHECK: vsetivli zero, 4, e32, m1, ta, ma
; CHECK: vle32.v
; CHECK: vfadd.vv
; CHECK: vse32.v
;
; CHECK-LABEL: fsub4:
; CHECK: vfsub.vv
;
; CHECK-LABEL: fmul4:
; CHECK: vfmul.vv
;
; CHECK-LABEL: splat_store:
; CHECK: vfmv.v.f
; CHECK: vse32.v
;
; CHECK-LABEL: extract0:
; CHECK: vfmv.f.s
;
; CHECK-LABEL: extract2:
; CHECK: vse32.v
; CHECK: flw

; NOF-NOT: vfadd.vv
; NOF-NOT: vfsub.vv
; NOF-NOT: vfmul.vv
; NOF-NOT: vfmv.v.f
; NOF-NOT: vfmv.f.s
; NOF: __addsf3
