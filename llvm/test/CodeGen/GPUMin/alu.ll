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

; Chapter 28: the extended ALU funct space.  Chapter 29 adds integer mul.
define i32 @a_mul(i32 %a, i32 %b) { %c = mul i32 %a, %b ret i32 %c }
; CHECK-LABEL: a_mul:
; CHECK:       mul r4, r4, r5
define i32 @a_sdiv(i32 %a, i32 %b) { %c = sdiv i32 %a, %b ret i32 %c }
; CHECK-LABEL: a_sdiv:
; CHECK:       sdiv r4, r4, r5
define i32 @a_udiv(i32 %a, i32 %b) { %c = udiv i32 %a, %b ret i32 %c }
; CHECK-LABEL: a_udiv:
; CHECK:       udiv r4, r4, r5
define i32 @a_srem(i32 %a, i32 %b) { %c = srem i32 %a, %b ret i32 %c }
; CHECK-LABEL: a_srem:
; CHECK:       srem r4, r4, r5
define i32 @a_urem(i32 %a, i32 %b) { %c = urem i32 %a, %b ret i32 %c }
; CHECK-LABEL: a_urem:
; CHECK:       urem r4, r4, r5

; A non-power-of-two constant multiplier is a real mul, not a libcall.
define i32 @mul10(i32 %x) {
; CHECK-LABEL: mul10:
; CHECK:       movi {{r[0-9]+}}, 10
; CHECK:       mul r4, r4, {{r[0-9]+}}
  %r = mul i32 %x, 10
  ret i32 %r
}

; isIntDivCheap: a constant divisor stays a real sdiv, never a magic multiply.
define i32 @div3(i32 %x) {
; CHECK-LABEL: div3:
; CHECK:       movi {{r[0-9]+}}, 3
; CHECK:       sdiv r4, r4, {{r[0-9]+}}
; CHECK-NOT:   mulhi
  %r = sdiv i32 %x, 3
  ret i32 %r
}

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

; A constant that needs all 32 bits: high half, shift, low half with its sign
; extension shifted back out, then or.  `movi` sign-extends, so the low half
; cannot simply be `or`-ed in.
define i32 @wide_constant() {
; CHECK-LABEL: wide_constant:
; CHECK:       movi {{r[0-9]+}}, 16
; CHECK:       movi {{r[0-9]+}}, 4660
; CHECK:       shl {{r[0-9]+}}, {{r[0-9]+}}, {{r[0-9]+}}
; CHECK:       movi {{r[0-9]+}}, 22136
; CHECK:       shl {{r[0-9]+}}, {{r[0-9]+}}, {{r[0-9]+}}
; CHECK:       shr {{r[0-9]+}}, {{r[0-9]+}}, {{r[0-9]+}}
; CHECK:       or {{r[0-9]+}}, {{r[0-9]+}}, {{r[0-9]+}}
  ret i32 305419896
}

; The same path with a negative high half: 0x88776655.
define i32 @wide_negative() {
; CHECK-LABEL: wide_negative:
; CHECK:       movi {{r[0-9]+}}, -30601
; CHECK:       or {{r[0-9]+}}, {{r[0-9]+}}, {{r[0-9]+}}
  ret i32 -2005440939
}
