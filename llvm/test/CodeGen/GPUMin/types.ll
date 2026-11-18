; RUN: llc -march=gpumin -O1 -verify-machineinstrs < %s | FileCheck %s

; Chapter 08: narrow loads/stores, i1 as a byte and i64 as two i32 halves.

target triple = "gpumin-unknown-elf"

define i32 @ld8_z(ptr addrspace(1) %p) {
; CHECK-LABEL: ld8_z:
; CHECK:       ld.global.u8 r4, 0(r4)
  %v = load i8, ptr addrspace(1) %p
  %r = zext i8 %v to i32
  ret i32 %r
}

define i32 @ld8_s(ptr addrspace(1) %p) {
; CHECK-LABEL: ld8_s:
; CHECK:       ld.global.s8 r4, 0(r4)
  %v = load i8, ptr addrspace(1) %p
  %r = sext i8 %v to i32
  ret i32 %r
}

define i32 @ld16_z(ptr addrspace(1) %p) {
; CHECK-LABEL: ld16_z:
; CHECK:       ld.global.u16 r4, 0(r4)
  %v = load i16, ptr addrspace(1) %p
  %r = zext i16 %v to i32
  ret i32 %r
}

define void @st8(ptr addrspace(1) %p, i32 %v) {
; CHECK-LABEL: st8:
; CHECK:       st.global.b8 r5, 0(r4)
  %t = trunc i32 %v to i8
  store i8 %t, ptr addrspace(1) %p
  ret void
}

define void @st16(ptr addrspace(1) %p, i32 %v) {
; CHECK-LABEL: st16:
; CHECK:       st.global.b16 r5, 0(r4)
  %t = trunc i32 %v to i16
  store i16 %t, ptr addrspace(1) %p
  ret void
}

define i64 @ld64(ptr addrspace(1) %p) {
; CHECK-LABEL: ld64:
; CHECK:       ld.global.b32 r1, 0(r4)
; CHECK:       ld.global.b32 r5, 4(r4)
; CHECK:       mov r4, r1
  %v = load i64, ptr addrspace(1) %p
  ret i64 %v
}

define i64 @add64(i64 %a, i64 %b) {
; CHECK-LABEL: add64:
; CHECK:       setp.ult
; CHECK:       add
  %r = add i64 %a, %b
  ret i64 %r
}

define i1 @cmp64(i64 %a, i64 %b) {
; CHECK-LABEL: cmp64:
; CHECK:       setp.eq
; CHECK:       setp.ult
; CHECK:       selp
  %r = icmp ult i64 %a, %b
  ret i1 %r
}

; i1 is legal as a *value*, but the chapter-07 ABI only assigns i32 to
; r4-r11, so an i1 formal argument is out of scope.  Derive the predicate
; from a compare instead: that is the shape real kernels use before a store.
define void @st_i1(ptr addrspace(1) %p, i32 %v) {
; CHECK-LABEL: st_i1:
; CHECK:       selp
; CHECK:       st.global.b8
  %c = icmp ne i32 %v, 0
  store i1 %c, ptr addrspace(1) %p
  ret void
}

define i32 @ld_i1(ptr addrspace(1) %p) {
; CHECK-LABEL: ld_i1:
; CHECK:       ld.global.u8
; CHECK:       and
  %v = load i1, ptr addrspace(1) %p
  %r = zext i1 %v to i32
  ret i32 %r
}

; Chapter 14: an i1 value that is not itself a compare result.  `trunc i32 to
; i1` means "take bit 0", and the DAG combiner canonicalises `(x & 1) != 0`
; (the way C spells a boolean test) into exactly that truncate, so this node
; has to reach instruction selection with a defined expansion.
define i32 @trunc_i1(i32 %v) {
; CHECK-LABEL: trunc_i1:
; CHECK:       and {{r[0-9]+}}, r4, {{r[0-9]+}}
; CHECK:       setp.ne
; CHECK:       selp
  %b = trunc i32 %v to i1
  %r = select i1 %b, i32 7, i32 9
  ret i32 %r
}

; Branching on a truncated value: the DAG combiner rewrites `br i1 (trunc x)`
; into a compare against true, which the legalizer then turns into a negated
; predicate.  Predicates have no NOT, so the branch is "compare 0/1 against
; zero" -- the four instructions below are the whole expansion.
define i32 @br_trunc_i1(i32 %v) {
; CHECK-LABEL: br_trunc_i1:
; CHECK:       and
; CHECK:       setp.ne
; CHECK:       selp
; CHECK:       setp.eq
; CHECK:       bra p{{[0-9]+}},
  %b = trunc i32 %v to i1
  br i1 %b, label %t, label %f
t:
  ret i32 1
f:
  ret i32 2
}
