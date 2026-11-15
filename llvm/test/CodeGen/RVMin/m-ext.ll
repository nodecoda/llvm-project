; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O1 -mattr=+m < %s | FileCheck %s --check-prefix=M
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O1 -mcpu=generic-rv32-m < %s | FileCheck %s --check-prefix=M
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O1 < %s | FileCheck %s --check-prefix=RV32I
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O0 -mattr=+m < %s -o /dev/null
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O1 -mattr=+m < %s -o - \
; RUN:   | llvm-mc -triple=riscv32 -mattr=+m -filetype=obj -o /dev/null
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O1 < %s -o - \
; RUN:   | llvm-mc -triple=riscv32 -filetype=obj -o /dev/null
;
; Chapter 13: the optional M extension.  With `+m` every multiply, divide and
; remainder is one instruction; without it the same IR still calls the
; compiler runtime, exactly as in chapter 10.
;
; Both spellings of "enable M" must work: `-mattr=+m` and the processor
; `generic-rv32-m` defined in RVMin.td.

define i32 @mul(i32 %a, i32 %b) nounwind {
  %r = mul i32 %a, %b
  ret i32 %r
}
; M-LABEL: mul:
; M:         mul a0, a0, a1
; M-NOT:     __mulsi3
; RV32I-LABEL: mul:
; RV32I:         lui t0, %hi(__mulsi3)

define i32 @sdiv(i32 %a, i32 %b) {
  %r = sdiv i32 %a, %b
  ret i32 %r
}
; M-LABEL: sdiv:
; M:         div a0, a0, a1
; M-NOT:     __divsi3

define i32 @udiv(i32 %a, i32 %b) {
  %r = udiv i32 %a, %b
  ret i32 %r
}
; M-LABEL: udiv:
; M:         divu a0, a0, a1
; M-NOT:     __udivsi3

define i32 @srem(i32 %a, i32 %b) {
  %r = srem i32 %a, %b
  ret i32 %r
}
; M-LABEL: srem:
; M:         rem a0, a0, a1
; M-NOT:     __modsi3

define i32 @urem(i32 %a, i32 %b) {
  %r = urem i32 %a, %b
  ret i32 %r
}
; M-LABEL: urem:
; M:         remu a0, a0, a1
; M-NOT:     __umodsi3

; `mulhs`: the high 32 bits of a signed 64-bit product, recognised by the DAG
; combiner from the sext/mul/ashr idiom.
define i32 @mulhs(i32 %a, i32 %b) {
  %ae = sext i32 %a to i64
  %be = sext i32 %b to i64
  %m = mul i64 %ae, %be
  %s = ashr i64 %m, 32
  %r = trunc i64 %s to i32
  ret i32 %r
}
; M-LABEL: mulhs:
; M:         mulh a0, a0, a1

; `mulhu`: the high 32 bits of an unsigned 64-bit product.
define i32 @mulhu(i32 %a, i32 %b) {
  %az = zext i32 %a to i64
  %bz = zext i32 %b to i64
  %m = mul i64 %az, %bz
  %s = lshr i64 %m, 32
  %r = trunc i64 %s to i32
  ret i32 %r
}
; M-LABEL: mulhu:
; M:         mulhu a0, a0, a1
