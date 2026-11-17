; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O1 -mattr=+f < %s \
; RUN:   | FileCheck %s --check-prefix=F
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O1 < %s \
; RUN:   | FileCheck %s --check-prefix=SOFT
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O0 -mattr=+f < %s -o /dev/null
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O1 -mattr=+f < %s -o - \
; RUN:   | llvm-mc -triple=riscv32 -mattr=+f -filetype=obj -o /dev/null
; RUN: not llc -march=rvmin -mtriple=riscv32-unknown-elf -O1 \
; RUN:   -target-abi=ilp32f < %s -o /dev/null 2>&1 \
; RUN:   | FileCheck %s --check-prefix=NOF
;
; Chapter 40: with F, f32 is a legal type and every operation that has a
; single instruction is selected as one.  Without F the same IR still goes
; through the soft-float libcalls of chapter 10, byte for byte.
;
; ABI (decision 1, B-lite): the ABI is derived from the ISA when nothing else
; says otherwise, so `-mattr=+f` alone means ilp32f.  Chapter 51 implements
; the remaining combination, a soft-float ABI with hardware FP; see
; abi-soft-float.ll for the boundary conversion.

declare float @llvm.fabs.f32(float)
declare float @llvm.minnum.f32(float, float)
declare float @llvm.maxnum.f32(float, float)
declare float @llvm.fma.f32(float, float, float)
declare float @llvm.sqrt.f32(float)

define float @fadd(float %a, float %b) nounwind {
  %r = fadd float %a, %b
  ret float %r
}
; F-LABEL: fadd:
; F:         fadd.s fa0, fa0, fa1
; SOFT-LABEL: fadd:
; SOFT:         lui t0, %hi(__addsf3)

define float @fmul(float %a, float %b) nounwind {
  %r = fmul float %a, %b
  ret float %r
}
; F-LABEL: fmul:
; F:         fmul.s fa0, fa0, fa1

define float @fsqrt(float %x) nounwind {
  %r = call float @llvm.sqrt.f32(float %x)
  ret float %r
}
; F-LABEL: fsqrt:
; F:         fsqrt.s fa0, fa0

define float @fneg(float %x) nounwind {
  %r = fneg float %x
  ret float %r
}
; F-LABEL: fneg:
; F:         fsgnjn.s fa0, fa0, fa0

define float @fabs(float %x) nounwind {
  %r = call float @llvm.fabs.f32(float %x)
  ret float %r
}
; F-LABEL: fabs:
; F:         fsgnjx.s fa0, fa0, fa0

define float @fminmax(float %a, float %b) nounwind {
  %lo = call float @llvm.minnum.f32(float %a, float %b)
  %hi = call float @llvm.maxnum.f32(float %a, float %b)
  %r = fadd float %lo, %hi
  ret float %r
}
; F-LABEL: fminmax:
; F-DAG:     fmin.s
; F-DAG:     fmax.s

define float @ffma(float %a, float %b, float %c) nounwind {
  %r = call float @llvm.fma.f32(float %a, float %b, float %c)
  ret float %r
}
; F-LABEL: ffma:
; F:         fmadd.s fa0, fa0, fa1, fa2

define i32 @flt(float %a, float %b) nounwind {
  %c = fcmp olt float %a, %b
  %r = zext i1 %c to i32
  ret i32 %r
}
; F-LABEL: flt:
; F:         flt.s a0, fa0, fa1

define i32 @feq(float %a, float %b) nounwind {
  %c = fcmp oeq float %a, %b
  %r = zext i1 %c to i32
  ret i32 %r
}
; F-LABEL: feq:
; F:         feq.s a0, fa0, fa1

define i32 @fne(float %a, float %b) nounwind {
  %c = fcmp une float %a, %b
  %r = zext i1 %c to i32
  ret i32 %r
}
; F-LABEL: fne:
; F:         feq.s a0, fa0, fa1
; F-NEXT:    xori a0, a0, 1

define i32 @ftoi(float %x) nounwind {
  %r = fptosi float %x to i32
  ret i32 %r
}
; F-LABEL: ftoi:
; F:         fcvt.w.s a0, fa0, rtz

define i32 @ftou(float %x) nounwind {
  %r = fptoui float %x to i32
  ret i32 %r
}
; F-LABEL: ftou:
; F:         fcvt.wu.s a0, fa0, rtz

define float @itof(i32 %x) nounwind {
  %r = sitofp i32 %x to float
  ret float %r
}
; F-LABEL: itof:
; F:         fcvt.s.w fa0, a0

define float @utof(i32 %x) nounwind {
  %r = uitofp i32 %x to float
  ret float %r
}
; F-LABEL: utof:
; F:         fcvt.s.wu fa0, a0

define i32 @f2i(float %x) nounwind {
  %r = bitcast float %x to i32
  ret i32 %r
}
; F-LABEL: f2i:
; F:         fmv.x.w a0, fa0

define float @i2f(i32 %x) nounwind {
  %r = bitcast i32 %x to float
  ret float %r
}
; F-LABEL: i2f:
; F:         fmv.w.x fa0, a0

define float @fconst() nounwind {
  ret float 1.5
}
; F-LABEL: fconst:
; F:         fmv.w.x fa0, a0

define float @fld(float* %p) nounwind {
  %v = load float, float* %p, align 4
  ret float %v
}
; F-LABEL: fld:
; F:         flw fa0, 0(a0)

define void @fst(float* %p, float %v) nounwind {
  store float %v, float* %p, align 4
  ret void
}
; F-LABEL: fst:
; F:         fsw fa0, 0(a0)

declare float @callee(float, float)
define float @fcall(float %x) nounwind {
  %r = call float @callee(float %x, float %x)
  ret float %r
}
; F-LABEL: fcall:
; F:         fsgnj.s fa1, fa0, fa0
; F:         jalr ra, 0(t0)
; F:         jalr zero, 0(ra)

define float @ninea(float %a, float %b, float %c, float %d, float %e,
                    float %f, float %g, float %h, float %i) nounwind {
  %s1 = fadd float %a, %b
  %s2 = fadd float %s1, %c
  %s3 = fadd float %s2, %d
  %s4 = fadd float %s3, %e
  %s5 = fadd float %s4, %f
  %s6 = fadd float %s5, %g
  %s7 = fadd float %s6, %h
  %s8 = fadd float %s7, %i
  ret float %s8
}
; F-LABEL: ninea:
; F:         flw fa1, 0(sp)

; NOF: the ilp32f ABI requires the F extension
