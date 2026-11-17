; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 -mattr=+v \
; RUN:   -verify-machineinstrs < %s | FileCheck %s --check-prefix=ABI
;
; Chapter 55: a <4 x i32> return travels in v8.

define <4 x i32> @vec_ret(ptr %p) {
  %v = load <4 x i32>, ptr %p, align 16
  ret <4 x i32> %v
}

; ABI-LABEL: vec_ret:
; ABI:         vsetivli zero, 4, e32, m1, ta, ma
; ABI-NEXT:    vle32.v v8, (a0)
; ABI:         jalr zero, 0(ra)
