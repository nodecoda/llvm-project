; RUN: llc -mtriple=minirv64 < %s | FileCheck %s
; RUN: llc -mtriple=minirv64 -filetype=obj < %s -o %t.o
; RUN: llvm-objdump -dr --triple=minirv64 %t.o | FileCheck %s --check-prefix=OBJ

; The ISA has no 128-bit divide, so the type legalizer calls the quad-precision
; helpers.  The operands are already in x10..x13 and the result comes back in
; x10/x11, so the call needs no register shuffling; only the return address is
; spilled.

; CHECK-LABEL: sdiv:
; CHECK:         jal     x1, __divti3
define i128 @sdiv(i128 %a, i128 %b) {
  %r = sdiv i128 %a, %b
  ret i128 %r
}

; CHECK-LABEL: udiv:
; CHECK:         jal     x1, __udivti3
define i128 @udiv(i128 %a, i128 %b) {
  %r = udiv i128 %a, %b
  ret i128 %r
}

; CHECK-LABEL: srem:
; CHECK:         jal     x1, __modti3
define i128 @srem(i128 %a, i128 %b) {
  %r = srem i128 %a, %b
  ret i128 %r
}

; CHECK-LABEL: urem:
; CHECK:         jal     x1, __umodti3
define i128 @urem(i128 %a, i128 %b) {
  %r = urem i128 %a, %b
  ret i128 %r
}

; The calls must be relocations against the runtime symbols, not calls to
; address zero.
; OBJ-LABEL: <sdiv>:
; OBJ:         R_RISCV_JAL  __divti3
; OBJ-LABEL: <udiv>:
; OBJ:         R_RISCV_JAL  __udivti3
; OBJ-LABEL: <srem>:
; OBJ:         R_RISCV_JAL  __modti3
; OBJ-LABEL: <urem>:
; OBJ:         R_RISCV_JAL  __umodti3
