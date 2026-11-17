; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O1 -mattr=+f,+d < %s \
; RUN:   | FileCheck %s --check-prefix=D
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O1 < %s \
; RUN:   | FileCheck %s --check-prefix=SOFT
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O0 -mattr=+f,+d < %s -o /dev/null
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O1 -mattr=+f,+d < %s -o - \
; RUN:   | llvm-mc -triple=riscv32 -mattr=+f,+d -filetype=obj -o /dev/null
; RUN: not llc -march=rvmin -mtriple=riscv32-unknown-elf -O1 -mattr=+f,+d \
; RUN:   -target-abi=ilp32f < %s -o /dev/null 2>&1 \
; RUN:   | FileCheck %s --check-prefix=BADABI
; RUN: not llc -march=rvmin -mtriple=riscv32-unknown-elf -O1 -mattr=+f \
; RUN:   -target-abi=ilp32d < %s -o /dev/null 2>&1 \
; RUN:   | FileCheck %s --check-prefix=NOD
;
; Chapter 41: with D, f64 is a legal type and lives in the 64-bit view of the
; same register file.  Without D the same IR still goes through the soft-float
; libcalls of chapter 10, byte for byte.
;
; ABI (decision 1, B-lite): the ABI is derived from the ISA when nothing else
; says otherwise, so `-mattr=+f,+d` alone means ilp32d.  ilp32f + D is
; rejected: f64 would cross calls in the ilp32d convention.

declare double @llvm.fabs.f64(double)
declare double @llvm.minnum.f64(double, double)
declare double @llvm.maxnum.f64(double, double)
declare double @llvm.fma.f64(double, double, double)
declare double @llvm.sqrt.f64(double)

define double @dadd(double %a, double %b) nounwind {
  %r = fadd double %a, %b
  ret double %r
}
; D-LABEL: dadd:
; D:         fadd.d fa0, fa0, fa1
; SOFT-LABEL: dadd:
; SOFT:         lui t0, %hi(__adddf3)

define double @dsub(double %a, double %b) nounwind {
  %r = fsub double %a, %b
  ret double %r
}
; D-LABEL: dsub:
; D:         fsub.d fa0, fa0, fa1

define double @dmul(double %a, double %b) nounwind {
  %r = fmul double %a, %b
  ret double %r
}
; D-LABEL: dmul:
; D:         fmul.d fa0, fa0, fa1

define double @ddiv(double %a, double %b) nounwind {
  %r = fdiv double %a, %b
  ret double %r
}
; D-LABEL: ddiv:
; D:         fdiv.d fa0, fa0, fa1

define double @dsqrt(double %x) nounwind {
  %r = call double @llvm.sqrt.f64(double %x)
  ret double %r
}
; D-LABEL: dsqrt:
; D:         fsqrt.d fa0, fa0

define double @dneg(double %x) nounwind {
  %r = fneg double %x
  ret double %r
}
; D-LABEL: dneg:
; D:         fsgnjn.d fa0, fa0, fa0

define double @dabs(double %x) nounwind {
  %r = call double @llvm.fabs.f64(double %x)
  ret double %r
}
; D-LABEL: dabs:
; D:         fsgnjx.d fa0, fa0, fa0

define double @dminmax(double %a, double %b) nounwind {
  %lo = call double @llvm.minnum.f64(double %a, double %b)
  %hi = call double @llvm.maxnum.f64(double %a, double %b)
  %r = fadd double %lo, %hi
  ret double %r
}
; D-LABEL: dminmax:
; D-DAG:     fmin.d
; D-DAG:     fmax.d

define double @dfma(double %a, double %b, double %c) nounwind {
  %r = call double @llvm.fma.f64(double %a, double %b, double %c)
  ret double %r
}
; D-LABEL: dfma:
; D:         fmadd.d fa0, fa0, fa1, fa2

define i32 @dlt(double %a, double %b) nounwind {
  %c = fcmp olt double %a, %b
  %r = zext i1 %c to i32
  ret i32 %r
}
; D-LABEL: dlt:
; D:         flt.d a0, fa0, fa1

define i32 @deq(double %a, double %b) nounwind {
  %c = fcmp oeq double %a, %b
  %r = zext i1 %c to i32
  ret i32 %r
}
; D-LABEL: deq:
; D:         feq.d a0, fa0, fa1

define i32 @dne(double %a, double %b) nounwind {
  %c = fcmp une double %a, %b
  %r = zext i1 %c to i32
  ret i32 %r
}
; D-LABEL: dne:
; D:         feq.d a0, fa0, fa1
; D-NEXT:    xori a0, a0, 1

define i32 @dtoi(double %x) nounwind {
  %r = fptosi double %x to i32
  ret i32 %r
}
; D-LABEL: dtoi:
; D:         fcvt.w.d a0, fa0, rtz

define i32 @dtou(double %x) nounwind {
  %r = fptoui double %x to i32
  ret i32 %r
}
; D-LABEL: dtou:
; D:         fcvt.wu.d a0, fa0, rtz

define double @itod(i32 %x) nounwind {
  %r = sitofp i32 %x to double
  ret double %r
}
; D-LABEL: itod:
; D:         fcvt.d.w fa0, a0

define double @utod(i32 %x) nounwind {
  %r = uitofp i32 %x to double
  ret double %r
}
; D-LABEL: utod:
; D:         fcvt.d.wu fa0, a0

define double @fpext(float %x) nounwind {
  %r = fpext float %x to double
  ret double %r
}
; D-LABEL: fpext:
; D:         fcvt.d.s fa0, fa0

define float @fptrunc(double %x) nounwind {
  %r = fptrunc double %x to float
  ret float %r
}
; D-LABEL: fptrunc:
; D:         fcvt.s.d fa0, fa0

; DAGCombine fuses the fptrunc into the store, producing a truncating store
; that RVMin has no pattern for; the legalizer expands it back into
; FP_ROUND + FSW.  The extending load is the mirror image.
define void @dtruncstore(float* %p, double %x) nounwind {
  %t = fptrunc double %x to float
  store float %t, float* %p, align 4
  ret void
}
; D-LABEL: dtruncstore:
; D:         fcvt.s.d
; D:         fsw

define double @dextload(float* %p) nounwind {
  %v = load float, float* %p, align 4
  %r = fpext float %v to double
  ret double %r
}
; D-LABEL: dextload:
; D:         flw
; D:         fcvt.d.s fa0, fa0

define double @dconst() nounwind {
  ret double 3.25
}
; D-LABEL: dconst:
; D:         lui
; D:         sw
; D:         sw
; D:         fld fa0, {{[0-9]+}}(sp)

define double @dload(double* %p) nounwind {
  %v = load double, double* %p, align 8
  ret double %v
}
; D-LABEL: dload:
; D:         fld fa0, 0(a0)

define void @dstore(double* %p, double %v) nounwind {
  store double %v, double* %p, align 8
  ret void
}
; D-LABEL: dstore:
; D:         fsd fa0, 0(a0)

declare double @callee(double, double)
define double @dcall(double %x) nounwind {
  %r = call double @callee(double %x, double %x)
  ret double %r
}
; D-LABEL: dcall:
; D:         fsgnj.d fa1, fa0, fa0
; D:         jalr ra, 0(t0)
; D:         jalr zero, 0(ra)

define double @ninea(double %a, double %b, double %c, double %d, double %e,
                     double %f, double %g, double %h, double %i) nounwind {
  %s1 = fadd double %a, %b
  %s2 = fadd double %s1, %c
  %s3 = fadd double %s2, %d
  %s4 = fadd double %s3, %e
  %s5 = fadd double %s4, %f
  %s6 = fadd double %s5, %g
  %s7 = fadd double %s6, %h
  %s8 = fadd double %s7, %i
  ret double %s8
}
; D-LABEL: ninea:
; D:         fld fa1, 0(sp)

define double @dsel(i1 %c, double %a, double %b) nounwind {
  %r = select i1 %c, double %a, double %b
  ret double %r
}
; D-LABEL: dsel:
; D-DAG:     fsd
; D-DAG:     fsd
; D:         fld fa0, 0(

define double @dspill(double %a, double %b, double %c, double %d, double %e,
                      double %f, double %g, double %h) nounwind {
  %t = call double @callee(double %a, double %b)
  %r1 = fadd double %c, %d
  %r2 = fadd double %e, %f
  %r3 = fadd double %g, %h
  %r4 = fadd double %r1, %r2
  %r5 = fadd double %r3, %t
  %r6 = fadd double %r4, %r5
  ret double %r6
}
; D-LABEL: dspill:
; D:         fsd fs{{[0-9]+}},
; D:         fld fs{{[0-9]+}},

; BADABI: the ilp32f ABI with the D extension is not implemented
; NOD: the ilp32d ABI requires the D extension
