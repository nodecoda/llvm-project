; RUN: llc -march=gpumin -O1 -verify-machineinstrs < %s | FileCheck %s

; Chapter 04: eight ALU nodes, the predicate file and selp.

target triple = "gpumin-unknown-elf"

define i32 @a_add(i32 %a, i32 %b) { %c = add  i32 %a, %b ret i32 %c }
; CHECK-LABEL: a_add:
; CHECK:       add r4, r4, r5
define i32 @a_sub(i32 %a, i32 %b) { %c = sub  i32 %a, %b ret i32 %c }
; CHECK-LABEL: a_sub:
; CHECK:       sub r4, r4, r5
define i32 @a_and(i32 %a, i32 %b) { %c = and  i32 %a, %b ret i32 %c }
; CHECK-LABEL: a_and:
; CHECK:       and r4, r4, r5
define i32 @a_or (i32 %a, i32 %b) { %c = or   i32 %a, %b ret i32 %c }
; CHECK-LABEL: a_or:
; CHECK:       or r4, r4, r5
define i32 @a_xor(i32 %a, i32 %b) { %c = xor  i32 %a, %b ret i32 %c }
; CHECK-LABEL: a_xor:
; CHECK:       xor r4, r4, r5
define i32 @a_shl(i32 %a, i32 %b) { %c = shl  i32 %a, %b ret i32 %c }
; CHECK-LABEL: a_shl:
; CHECK:       shl r4, r4, r5
define i32 @a_shr(i32 %a, i32 %b) { %c = lshr i32 %a, %b ret i32 %c }
; CHECK-LABEL: a_shr:
; CHECK:       shr r4, r4, r5
define i32 @a_sar(i32 %a, i32 %b) { %c = ashr i32 %a, %b ret i32 %c }
; CHECK-LABEL: a_sar:
; CHECK:       sar r4, r4, r5

define i32 @c_slt(i32 %a, i32 %b) { %c = icmp slt i32 %a, %b %d = select i1 %c, i32 %a, i32 %b ret i32 %d }
; CHECK-LABEL: c_slt:
; CHECK:       setp.lt p0, r4, r5
; CHECK:       selp r4, p0, r4, r5
; CHECK-NOT:   movi

define i32 @c_ugt(i32 %a, i32 %b) { %c = icmp ugt i32 %a, %b %d = select i1 %c, i32 %a, i32 %b ret i32 %d }
; CHECK-LABEL: c_ugt:
; CHECK:       setp.ugt p0, r4, r5

define i32 @c_eq(i32 %a, i32 %b) { %c = icmp eq i32 %a, %b %d = select i1 %c, i32 1, i32 0 ret i32 %d }
; CHECK-LABEL: c_eq:
; CHECK:       setp.eq p0, r4, r5
; CHECK:       selp r4, p0, {{r[0-9]+}}, {{r[0-9]+}}

; A constant folded into ADDI instead of a materialised register.
define i32 @bump(i32 %x) {
; CHECK-LABEL: bump:
; CHECK:       addi r4, r4, 100
; CHECK-NOT:   movi
  %r = add i32 %x, 100
  ret i32 %r
}
