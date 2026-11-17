; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 -mattr=+v \
; RUN:   -verify-machineinstrs < %s | FileCheck %s --check-prefix=ABI
;
; Chapter 55 implements the LMUL=1 vector calling convention:
;
;   vector arguments : v8-v23, then a 16-byte stack slot
;   vector returns   : v8
;
; The first function proves that the vector parameter lands in v8.  The second
; passes sixteen vectors in v8-v23 and puts the seventeenth on the stack.

define void @vec_arg(ptr %out, <4 x i32> %x) {
  store <4 x i32> %x, ptr %out, align 16
  ret void
}

define <4 x i32> @arg17(<4 x i32> %a0, <4 x i32> %a1, <4 x i32> %a2,
                        <4 x i32> %a3, <4 x i32> %a4, <4 x i32> %a5,
                        <4 x i32> %a6, <4 x i32> %a7, <4 x i32> %a8,
                        <4 x i32> %a9, <4 x i32> %a10, <4 x i32> %a11,
                        <4 x i32> %a12, <4 x i32> %a13, <4 x i32> %a14,
                        <4 x i32> %a15, <4 x i32> %a16) {
  ret <4 x i32> %a16
}

; ABI-LABEL: vec_arg:
; ABI:         vsetivli zero, 4, e32, m1, ta, ma
; ABI-NEXT:    vse32.v v8, (a0)

; ABI-LABEL: arg17:
; ABI:         addi a0, sp, 0
; ABI:         vle32.v v8, (a0)
