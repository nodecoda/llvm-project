; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O1 < %s | FileCheck %s
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O0 < %s -o /dev/null
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O1 < %s -filetype=obj -o %t.o
; RUN: llvm-readelf -r %t.o | FileCheck %s --check-prefix=RELOC

; Chapter 10: everything RV32I cannot do in one instruction becomes a call.
; RV32I has no M extension (mul/div/rem) and no F/D extension (any floating
; point), so the legalizer calls the compiler runtime instead.

define i32 @mul(i32 %a, i32 %b) nounwind {
  %r = mul i32 %a, %b
  ret i32 %r
}
; CHECK-LABEL: mul:
; CHECK:         lui t0, %hi(__mulsi3)
; CHECK:         addi t0, t0, %lo(__mulsi3)
; CHECK:         jalr ra, 0(t0)
; CHECK:         lw ra, 12(sp)

define i32 @sdiv(i32 %a, i32 %b) nounwind {
  %r = sdiv i32 %a, %b
  ret i32 %r
}
; CHECK-LABEL: sdiv:
; CHECK:         lui t0, %hi(__divsi3)

define i32 @udiv(i32 %a, i32 %b) nounwind {
  %r = udiv i32 %a, %b
  ret i32 %r
}
; CHECK-LABEL: udiv:
; CHECK:         lui t0, %hi(__udivsi3)

define i32 @srem(i32 %a, i32 %b) nounwind {
  %r = srem i32 %a, %b
  ret i32 %r
}
; CHECK-LABEL: srem:
; CHECK:         lui t0, %hi(__modsi3)

define i32 @urem(i32 %a, i32 %b) nounwind {
  %r = urem i32 %a, %b
  ret i32 %r
}
; CHECK-LABEL: urem:
; CHECK:         lui t0, %hi(__umodsi3)

; 64-bit multiply/divide use the i64 libcalls: the arguments are already split
; into two registers by the ILP32 calling convention.
define i64 @mul64(i64 %a, i64 %b) nounwind {
  %r = mul i64 %a, %b
  ret i64 %r
}
; CHECK-LABEL: mul64:
; CHECK:         lui t0, %hi(__muldi3)

define i64 @div64(i64 %a, i64 %b) nounwind {
  %r = sdiv i64 %a, %b
  ret i64 %r
}
; CHECK-LABEL: div64:
; CHECK:         lui t0, %hi(__divdi3)

; Multiplication by a power of two and division by a power of two are folded
; into shifts (divided by a signed constant needs the sign fixup).
define i32 @mul_pow2(i32 %a) nounwind {
  %r = mul i32 %a, 4
  ret i32 %r
}
; CHECK-LABEL: mul_pow2:
; CHECK:         slli a0, a0, 2
; CHECK-NOT:     __mulsi3

define i32 @div_pow2(i32 %a) nounwind {
  %r = sdiv i32 %a, 8
  ret i32 %r
}
; CHECK-LABEL: div_pow2:
; CHECK:         srai a1, a0, 31
; CHECK:         srli a1, a1, 29
; CHECK:         add a0, a0, a1
; CHECK:         srai a0, a0, 3

; Floating point: there is no f32/f64 register class, so the type legalizer
; softens the values into i32 and every operation becomes a libcall.
define float @fadd(float %a, float %b) nounwind {
  %r = fadd float %a, %b
  ret float %r
}
; CHECK-LABEL: fadd:
; CHECK:         lui t0, %hi(__addsf3)

define double @dmul(double %a, double %b) nounwind {
  %r = fmul double %a, %b
  ret double %r
}
; CHECK-LABEL: dmul:
; CHECK:         lui t0, %hi(__muldf3)

; A float comparison is a three-way comparison returning <0/0/>0, so the
; result still needs one `slti` to turn it into a bool.
define i32 @fcmp(float %a, float %b) nounwind {
  %c = fcmp olt float %a, %b
  %r = zext i1 %c to i32
  ret i32 %r
}
; CHECK-LABEL: fcmp:
; CHECK:         lui t0, %hi(__ltsf2)
; CHECK:         jalr ra, 0(t0)
; CHECK:         slti a0, a0, 0

define i32 @f2i(float %f) nounwind {
  %r = fptosi float %f to i32
  ret i32 %r
}
; CHECK-LABEL: f2i:
; CHECK:         lui t0, %hi(__fixsfsi)

define float @i2f(i32 %i) nounwind {
  %r = sitofp i32 %i to float
  ret float %r
}
; CHECK-LABEL: i2f:
; CHECK:         lui t0, %hi(__floatsisf)

; The byte swap is *not* a libcall: the DAG combiner rewrites it into rotates,
; and a rotate is a shift pair on RV32I.
declare i32 @llvm.bswap.i32(i32)
define i32 @bswap(i32 %x) nounwind {
  %r = call i32 @llvm.bswap.i32(i32 %x)
  ret i32 %r
}
; CHECK-LABEL: bswap:
; CHECK-NOT:     lui t0
; CHECK:         srli a2, a0, 8
; CHECK:         slli a0, a0, 24

; RELOC:         R_RISCV_HI20 {{.*}}__mulsi3
; RELOC:         R_RISCV_LO12_I {{.*}}__mulsi3
; RELOC:         R_RISCV_HI20 {{.*}}__addsf3
; RELOC:         R_RISCV_LO12_I {{.*}}__addsf3
