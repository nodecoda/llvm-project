; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O0 -mattr=+v \
; RUN:   -verify-machineinstrs < %s | FileCheck %s --check-prefix=O0
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 -mattr=+v \
; RUN:   -verify-machineinstrs < %s | FileCheck %s --check-prefix=O2
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 -mattr=+v < %s \
; RUN:   -o - | llvm-mc -triple=riscv32 -mattr=+v -filetype=obj -o /dev/null
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 -mattr=+v,+f \
; RUN:   -target-abi=ilp32f -verify-machineinstrs < %s \
; RUN:   | FileCheck %s --check-prefix=FPV
;
; Chapter 55: end-to-end LMUL=1 vector ABI coverage.
;
;   call17       : sixteen vectors in v8-v23 and the seventeenth on the stack
;   callee_saved : twenty live vectors across a call force the v24-v31
;                  callee-saved window to be spilled and restored
;   m2_csr       : an LMUL=2 value spans V24/V25; PEI saves both halves as
;                  separate m1 stores, so CSR_RVMin_V needs no m2 alias
;   fp_v_csr     : with +v,+f the same prologue saves both fs0 and v24-v29

target triple = "riscv32-unknown-elf"

declare <4 x i32> @callee17(<4 x i32>, <4 x i32>, <4 x i32>, <4 x i32>,
                            <4 x i32>, <4 x i32>, <4 x i32>, <4 x i32>,
                            <4 x i32>, <4 x i32>, <4 x i32>, <4 x i32>,
                            <4 x i32>, <4 x i32>, <4 x i32>, <4 x i32>,
                            <4 x i32>)
declare void @bar()

define <4 x i32> @call17(<4 x i32> %a0, <4 x i32> %a1, <4 x i32> %a2,
                         <4 x i32> %a3, <4 x i32> %a4, <4 x i32> %a5,
                         <4 x i32> %a6, <4 x i32> %a7, <4 x i32> %a8,
                         <4 x i32> %a9, <4 x i32> %a10, <4 x i32> %a11,
                         <4 x i32> %a12, <4 x i32> %a13, <4 x i32> %a14,
                         <4 x i32> %a15, <4 x i32> %a16) {
  %r = call <4 x i32> @callee17(<4 x i32> %a0, <4 x i32> %a1,
                                <4 x i32> %a2, <4 x i32> %a3,
                                <4 x i32> %a4, <4 x i32> %a5,
                                <4 x i32> %a6, <4 x i32> %a7,
                                <4 x i32> %a8, <4 x i32> %a9,
                                <4 x i32> %a10, <4 x i32> %a11,
                                <4 x i32> %a12, <4 x i32> %a13,
                                <4 x i32> %a14, <4 x i32> %a15,
                                <4 x i32> %a16)
  ret <4 x i32> %r
}

define void @callee_saved(ptr %p, ptr %out) {
  %v0 = load volatile <4 x i32>, ptr %p, align 16
  %v1 = load volatile <4 x i32>, ptr %p, align 16
  %v2 = load volatile <4 x i32>, ptr %p, align 16
  %v3 = load volatile <4 x i32>, ptr %p, align 16
  %v4 = load volatile <4 x i32>, ptr %p, align 16
  %v5 = load volatile <4 x i32>, ptr %p, align 16
  %v6 = load volatile <4 x i32>, ptr %p, align 16
  %v7 = load volatile <4 x i32>, ptr %p, align 16
  %v8 = load volatile <4 x i32>, ptr %p, align 16
  %v9 = load volatile <4 x i32>, ptr %p, align 16
  %v10 = load volatile <4 x i32>, ptr %p, align 16
  %v11 = load volatile <4 x i32>, ptr %p, align 16
  %v12 = load volatile <4 x i32>, ptr %p, align 16
  %v13 = load volatile <4 x i32>, ptr %p, align 16
  %v14 = load volatile <4 x i32>, ptr %p, align 16
  %v15 = load volatile <4 x i32>, ptr %p, align 16
  %v16 = load volatile <4 x i32>, ptr %p, align 16
  %v17 = load volatile <4 x i32>, ptr %p, align 16
  %v18 = load volatile <4 x i32>, ptr %p, align 16
  %v19 = load volatile <4 x i32>, ptr %p, align 16
  call void @bar()
  %s1 = add <4 x i32> %v0, %v1
  %s2 = add <4 x i32> %s1, %v2
  %s3 = add <4 x i32> %s2, %v3
  %s4 = add <4 x i32> %s3, %v4
  %s5 = add <4 x i32> %s4, %v5
  %s6 = add <4 x i32> %s5, %v6
  %s7 = add <4 x i32> %s6, %v7
  %s8 = add <4 x i32> %s7, %v8
  %s9 = add <4 x i32> %s8, %v9
  %s10 = add <4 x i32> %s9, %v10
  %s11 = add <4 x i32> %s10, %v11
  %s12 = add <4 x i32> %s11, %v12
  %s13 = add <4 x i32> %s12, %v13
  %s14 = add <4 x i32> %s13, %v14
  %s15 = add <4 x i32> %s14, %v15
  %s16 = add <4 x i32> %s15, %v16
  %s17 = add <4 x i32> %s16, %v17
  %s18 = add <4 x i32> %s17, %v18
  %s19 = add <4 x i32> %s18, %v19
  store <4 x i32> %s19, ptr %out, align 16
  ret void
}

; O0-LABEL: call17:
; O0:         vse32.v {{v[0-9]+}}, (sp)
; O0:         jalr ra, 0(t0)
;
; O2-LABEL: call17:
; O2:         vse32.v {{v[0-9]+}}, (sp)
; O2:         jalr ra, 0(t0)
;
; O2-LABEL: callee_saved:
; O2:         sw ra,
; O2:         vse32.v v24,
; O2:         jalr ra, 0(t0)
; O2:         vle32.v v24,
; O2:         jalr zero, 0(ra)

define void @m2_csr(ptr %p, ptr %out) {
  %v0 = load volatile <8 x i32>, ptr %p, align 32
  %v1 = load volatile <8 x i32>, ptr %p, align 32
  %v2 = load volatile <8 x i32>, ptr %p, align 32
  %v3 = load volatile <8 x i32>, ptr %p, align 32
  %v4 = load volatile <8 x i32>, ptr %p, align 32
  %v5 = load volatile <8 x i32>, ptr %p, align 32
  %v6 = load volatile <8 x i32>, ptr %p, align 32
  %v7 = load volatile <8 x i32>, ptr %p, align 32
  %v8 = load volatile <8 x i32>, ptr %p, align 32
  %v9 = load volatile <8 x i32>, ptr %p, align 32
  %v10 = load volatile <8 x i32>, ptr %p, align 32
  %v11 = load volatile <8 x i32>, ptr %p, align 32
  %v12 = load volatile <8 x i32>, ptr %p, align 32
  %v13 = load volatile <8 x i32>, ptr %p, align 32
  %v14 = load volatile <8 x i32>, ptr %p, align 32
  %v15 = load volatile <8 x i32>, ptr %p, align 32
  %v16 = load volatile <8 x i32>, ptr %p, align 32
  %v17 = load volatile <8 x i32>, ptr %p, align 32
  %v18 = load volatile <8 x i32>, ptr %p, align 32
  %v19 = load volatile <8 x i32>, ptr %p, align 32
  call void @bar()
  %s1 = add <8 x i32> %v0, %v1
  %s2 = add <8 x i32> %s1, %v2
  %s3 = add <8 x i32> %s2, %v3
  %s4 = add <8 x i32> %s3, %v4
  %s5 = add <8 x i32> %s4, %v5
  %s6 = add <8 x i32> %s5, %v6
  %s7 = add <8 x i32> %s6, %v7
  %s8 = add <8 x i32> %s7, %v8
  %s9 = add <8 x i32> %s8, %v9
  %s10 = add <8 x i32> %s9, %v10
  %s11 = add <8 x i32> %s10, %v11
  %s12 = add <8 x i32> %s11, %v12
  %s13 = add <8 x i32> %s12, %v13
  %s14 = add <8 x i32> %s13, %v14
  %s15 = add <8 x i32> %s14, %v15
  %s16 = add <8 x i32> %s15, %v16
  %s17 = add <8 x i32> %s16, %v17
  %s18 = add <8 x i32> %s17, %v18
  %s19 = add <8 x i32> %s18, %v19
  store <8 x i32> %s19, ptr %out, align 32
  ret void
}

define void @fp_v_csr(ptr %p, ptr %out) {
  %f0 = load volatile float, ptr %p, align 4
  %f1 = load volatile float, ptr %p, align 4
  %f2 = load volatile float, ptr %p, align 4
  %f3 = load volatile float, ptr %p, align 4
  %f4 = load volatile float, ptr %p, align 4
  %f5 = load volatile float, ptr %p, align 4
  %f6 = load volatile float, ptr %p, align 4
  %f7 = load volatile float, ptr %p, align 4
  %f8 = load volatile float, ptr %p, align 4
  %f9 = load volatile float, ptr %p, align 4
  %f10 = load volatile float, ptr %p, align 4
  %f11 = load volatile float, ptr %p, align 4
  %v0 = load volatile <4 x i32>, ptr %p, align 16
  %v1 = load volatile <4 x i32>, ptr %p, align 16
  %v2 = load volatile <4 x i32>, ptr %p, align 16
  %v3 = load volatile <4 x i32>, ptr %p, align 16
  %v4 = load volatile <4 x i32>, ptr %p, align 16
  %v5 = load volatile <4 x i32>, ptr %p, align 16
  %v6 = load volatile <4 x i32>, ptr %p, align 16
  %v7 = load volatile <4 x i32>, ptr %p, align 16
  %v8 = load volatile <4 x i32>, ptr %p, align 16
  %v9 = load volatile <4 x i32>, ptr %p, align 16
  %v10 = load volatile <4 x i32>, ptr %p, align 16
  %v11 = load volatile <4 x i32>, ptr %p, align 16
  %v12 = load volatile <4 x i32>, ptr %p, align 16
  %v13 = load volatile <4 x i32>, ptr %p, align 16
  %v14 = load volatile <4 x i32>, ptr %p, align 16
  %v15 = load volatile <4 x i32>, ptr %p, align 16
  %v16 = load volatile <4 x i32>, ptr %p, align 16
  %v17 = load volatile <4 x i32>, ptr %p, align 16
  %v18 = load volatile <4 x i32>, ptr %p, align 16
  %v19 = load volatile <4 x i32>, ptr %p, align 16
  call void @bar()
  %a0 = fadd float %f0, %f1
  %a1 = fadd float %a0, %f2
  %a2 = fadd float %a1, %f3
  %a3 = fadd float %a2, %f4
  %a4 = fadd float %a3, %f5
  %a5 = fadd float %a4, %f6
  %a6 = fadd float %a5, %f7
  %a7 = fadd float %a6, %f8
  %a8 = fadd float %a7, %f9
  %a9 = fadd float %a8, %f10
  %a10 = fadd float %a9, %f11
  store volatile float %a10, ptr %out, align 4
  %s1 = add <4 x i32> %v0, %v1
  %s2 = add <4 x i32> %s1, %v2
  %s3 = add <4 x i32> %s2, %v3
  %s4 = add <4 x i32> %s3, %v4
  %s5 = add <4 x i32> %s4, %v5
  %s6 = add <4 x i32> %s5, %v6
  %s7 = add <4 x i32> %s6, %v7
  %s8 = add <4 x i32> %s7, %v8
  %s9 = add <4 x i32> %s8, %v9
  %s10 = add <4 x i32> %s9, %v10
  %s11 = add <4 x i32> %s10, %v11
  %s12 = add <4 x i32> %s11, %v12
  %s13 = add <4 x i32> %s12, %v13
  %s14 = add <4 x i32> %s13, %v14
  %s15 = add <4 x i32> %s14, %v15
  %s16 = add <4 x i32> %s15, %v16
  %s17 = add <4 x i32> %s16, %v17
  %s18 = add <4 x i32> %s17, %v18
  %s19 = add <4 x i32> %s18, %v19
  store <4 x i32> %s19, ptr %out, align 16
  ret void
}

; O2-LABEL: m2_csr:
; O2:         vse32.v v24,
; O2:         vse32.v v25,
; O2:         jalr ra, 0(
; O2:         vle32.v v24,
; O2:         vle32.v v25,
;
; FPV-LABEL: fp_v_csr:
; FPV:         fsw fs0,
; FPV:         vse32.v v24,
; FPV:         jalr ra, 0(
; FPV:         flw fs0,
; FPV:         vle32.v v24,
