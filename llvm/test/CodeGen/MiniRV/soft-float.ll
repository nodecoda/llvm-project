; RUN: llc -mtriple=minirv64 < %s | FileCheck %s
; RUN: llc -mtriple=minirv64 -filetype=obj < %s -o %t.o
; RUN: llvm-objdump -dr --triple=minirv64 %t.o | FileCheck %s --check-prefix=OBJ

; There is no floating-point unit, so every floating-point operation becomes a
; call to the soft-float helpers.  This is the same set of symbols compiler-rt
; and libgcc provide for a soft-float RISC-V target.

; CHECK-LABEL: fadd64:
; CHECK:         jal     x1, __adddf3
define double @fadd64(double %a, double %b) {
  %r = fadd double %a, %b
  ret double %r
}

; CHECK-LABEL: fsub64:
; CHECK:         jal     x1, __subdf3
define double @fsub64(double %a, double %b) {
  %r = fsub double %a, %b
  ret double %r
}

; CHECK-LABEL: fmul64:
; CHECK:         jal     x1, __muldf3
define double @fmul64(double %a, double %b) {
  %r = fmul double %a, %b
  ret double %r
}

; CHECK-LABEL: fdiv64:
; CHECK:         jal     x1, __divdf3
define double @fdiv64(double %a, double %b) {
  %r = fdiv double %a, %b
  ret double %r
}

; CHECK-LABEL: fadd32:
; CHECK:         jal     x1, __addsf3
define float @fadd32(float %a, float %b) {
  %r = fadd float %a, %b
  ret float %r
}

; CHECK-LABEL: fdiv32:
; CHECK:         jal     x1, __divsf3
define float @fdiv32(float %a, float %b) {
  %r = fdiv float %a, %b
  ret float %r
}

; The register pair is converted to an integer by __fixdfdi, and an integer is
; converted back by __floatdidf.
; CHECK-LABEL: fptosi64:
; CHECK:         jal     x1, __fixdfdi
define i64 @fptosi64(double %a) {
  %r = fptosi double %a to i64
  ret i64 %r
}

; CHECK-LABEL: sitofp64:
; CHECK:         jal     x1, __floatdidf
define double @sitofp64(i64 %a) {
  %r = sitofp i64 %a to double
  ret double %r
}

; An i32 result is promoted to i64 before the conversion, so the 64-bit
; helper is the one that is called.
; CHECK-LABEL: fptoui32:
; CHECK:         jal     x1, __fixunsdfdi
define i32 @fptoui32(double %a) {
  %r = fptoui double %a to i32
  ret i32 %r
}

; CHECK-LABEL: uitofp64:
; CHECK:         jal     x1, __floatundidf
define double @uitofp64(i64 %a) {
  %r = uitofp i64 %a to double
  ret double %r
}

; A width change is a helper call as well.
; CHECK-LABEL: fpext:
; CHECK:         jal     x1, __extendsfdf2
define double @fpext(float %a) {
  %r = fpext float %a to double
  ret double %r
}

; CHECK-LABEL: fptrunc:
; CHECK:         jal     x1, __truncdfsf2
define float @fptrunc(double %a) {
  %r = fptrunc double %a to float
  ret float %r
}

; An ordered compare calls the three-way helper and tests the result against
; zero.  The unordered predicate uses __unorddf2 instead.
; CHECK-LABEL: fcmplt:
; CHECK:         jal     x1, __ltdf2
; CHECK:         slt
define i32 @fcmplt(double %a, double %b) {
  %c = fcmp olt double %a, %b
  %r = zext i1 %c to i32
  ret i32 %r
}

; CHECK-LABEL: fcmpuno:
; CHECK:         jal     x1, __unorddf2
define i32 @fcmpuno(double %a, double %b) {
  %c = fcmp uno double %a, %b
  %r = zext i1 %c to i32
  ret i32 %r
}

; Half precision has no arithmetic helpers: the operands are widened to f32,
; added with __addsf3, and narrowed again.
; CHECK-LABEL: hadd:
; CHECK:         jal     x1, __extendhfsf2
; CHECK:         jal     x1, __addsf3
; CHECK:         jal     x1, __truncsfhf2
define half @hadd(half %a, half %b) {
  %r = fadd half %a, %b
  ret half %r
}

; bfloat16 is widened the same way; the arguments are already promoted to f32
; by the calling convention, so only the result is narrowed.
; CHECK-LABEL: bfadd:
; CHECK:         jal     x1, __addsf3
; CHECK:         jal     x1, __truncsfbf2
define bfloat @bfadd(bfloat %a, bfloat %b) {
  %r = fadd bfloat %a, %b
  ret bfloat %r
}

; A 128-bit integer conversion uses the quad helpers.
; CHECK-LABEL: fptosi128:
; CHECK:         jal     x1, __fixdfti
define i128 @fptosi128(double %a) {
  %r = fptosi double %a to i128
  ret i128 %r
}

; CHECK-LABEL: sitofp128:
; CHECK:         jal     x1, __floattidf
define double @sitofp128(i128 %a) {
  %r = sitofp i128 %a to double
  ret double %r
}

; long double is IEEE quad here, so it uses the tf helpers.
; CHECK-LABEL: fadd128:
; CHECK:         jal     x1, __addtf3
define fp128 @fadd128(fp128 %a, fp128 %b) {
  %r = fadd fp128 %a, %b
  ret fp128 %r
}

; CHECK-LABEL: fptosi128f:
; CHECK:         jal     x1, __fixtfdi
define i64 @fptosi128f(fp128 %a) {
  %r = fptosi fp128 %a to i64
  ret i64 %r
}

; CHECK-LABEL: sitofp128f:
; CHECK:         jal     x1, __floatditf
define fp128 @sitofp128f(i64 %a) {
  %r = sitofp i64 %a to fp128
  ret fp128 %r
}

; CHECK-LABEL: trunc128:
; CHECK:         jal     x1, __trunctfdf2
define double @trunc128(fp128 %a) {
  %r = fptrunc fp128 %a to double
  ret double %r
}

; CHECK-LABEL: ext128:
; CHECK:         jal     x1, __extenddftf2
define fp128 @ext128(double %a) {
  %r = fpext double %a to fp128
  ret fp128 %r
}

; CHECK-LABEL: cmp128:
; CHECK:         jal     x1, __lttf2
define i32 @cmp128(fp128 %a, fp128 %b) {
  %c = fcmp olt fp128 %a, %b
  %r = zext i1 %c to i32
  ret i32 %r
}

; The helpers must be relocations against real symbols.
; OBJ-LABEL: <fadd64>:
; OBJ:         R_RISCV_JAL  __adddf3
; OBJ-LABEL: <fptosi64>:
; OBJ:         R_RISCV_JAL  __fixdfdi
; OBJ-LABEL: <fcmplt>:
; OBJ:         R_RISCV_JAL  __ltdf2
