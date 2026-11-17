; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 -mattr=+f,+d \
; RUN:   -target-abi=ilp32 < %s | FileCheck %s --check-prefix=SOFT
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 -mattr=+f \
; RUN:   -target-abi=ilp32 < %s | FileCheck %s --check-prefix=SOFTF
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 -mattr=+f,+d \
; RUN:   -target-abi=ilp32 < %s -o - \
; RUN:   | llvm-mc -triple=riscv32 -mattr=+f,+d -filetype=obj -o /dev/null
; RUN: not llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 -mattr=+f,+d \
; RUN:   -target-abi=ilp32f < %s -o /dev/null 2>&1 \
; RUN:   | FileCheck %s --check-prefix=BADF
;
; Chapter 51: `ilp32` with hardware F/D.  Values cross calls as integer bit
; patterns even though arithmetic uses the FP instructions:
;
;   f32: one GPR (a0-a7), then a 4-byte stack slot
;   f64: an even-aligned GPR pair (a0/a1, a2/a3, ...), then an 8-byte slot
;
; RV32D has no fmv.x.d/fmv.d.x, so f64 is split/joined through a private
; 8-byte stack object.  The hard-float ABIs are unchanged.

define float @f32_add(float %a, float %b) {
  %r = fadd float %a, %b
  ret float %r
}
; SOFT-LABEL: f32_add:
; SOFT:         fmv.w.x {{fa[0-9]+}}, a{{[0-9]+}}
; SOFT:         fadd.s
; SOFT:         fmv.x.w a0, {{fa[0-9]+}}
; SOFTF-LABEL: f32_add:
; SOFTF:         fmv.w.x {{fa[0-9]+}}, a{{[0-9]+}}
; SOFTF:         fadd.s
; SOFTF:         fmv.x.w a0, {{fa[0-9]+}}

define float @f32_call(float %a, float %b) {
  %r = call float @f32_add(float %a, float %b)
  ret float %r
}
; The soft-float caller and callee agree on a0/a1, so the values can flow
; through without a register-class copy.
; SOFT-LABEL: f32_call:
; SOFT:         jalr
; SOFT:         jalr zero, 0(ra)

define double @f64_add(double %a, double %b) {
  %r = fadd double %a, %b
  ret double %r
}
; SOFT-LABEL: f64_add:
; SOFT:         fld
; SOFT:         fld
; SOFT:         fadd.d
; SOFT:         fsd
; SOFT:         lw a0,
; SOFT:         lw a1,

define double @f64_call(double %a, double %b) {
  %r = call double @f64_add(double %a, double %b)
  ret double %r
}
; The fifth argument register pair is a2/a3; the result comes back in a0/a1.
; SOFT-LABEL: f64_call:
; SOFT:         lw a2,
; SOFT:         lw a3,
; SOFT:         jalr
; SOFT:         lw a0,
; SOFT:         lw a1,

declare void @sink(double, double, double, double, double)
define void @f64_many(double %a, double %b, double %c, double %d, double %e) {
  call void @sink(double %a, double %b, double %c, double %d, double %e)
  ret void
}
; Four doubles take a0-a7; the fifth arrives and departs on the stack.
; SOFT-LABEL: f64_many:
; SOFT:         fld {{fa[0-9]+}}, {{[0-9]+}}(sp)
; SOFT:         fsd {{fa[0-9]+}}, 0(sp)

; The mixed ABI remains rejected: ilp32f + D would put f64 in fa0-fa7, which
; is the ilp32d convention.
; BADF: the ilp32f ABI with the D extension is not implemented
