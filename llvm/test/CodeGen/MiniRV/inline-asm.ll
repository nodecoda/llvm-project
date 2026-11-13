; RUN: llc < %s -mtriple=minirv64 -O2 | FileCheck %s

; Inline asm operands are machine operands, but the generic printer only knows
; the operand modifiers.  The target maps "r" to the general register class and
; prints plain operands with the same register names as ordinary instructions.

; CHECK-LABEL: add_asm:
; CHECK:       #APP
; CHECK-NEXT:  add [[R0:x[0-9]+]], [[R1:x[0-9]+]], [[R2:x[0-9]+]]
; CHECK-NEXT:  #NO_APP
define i64 @add_asm(i64 %a, i64 %b) {
  %r = call i64 asm "add $0, $1, $2", "=r,r,r"(i64 %a, i64 %b)
  ret i64 %r
}

; A tied operand names the same register as the output it is tied to.
; CHECK-LABEL: tied:
; CHECK:       #APP
; CHECK-NEXT:  addi [[R:x[0-9]+]], [[R]], 1
; CHECK-NEXT:  #NO_APP
define i64 @tied(i64 %a) {
  %r = call i64 asm "addi $0, $0, 1", "=r,0"(i64 %a)
  ret i64 %r
}

; An "i" operand is printed as an immediate.
; CHECK-LABEL: imm_asm:
; CHECK:       addi [[C:x[0-9]+]], x0, 7
; CHECK:       #APP
; CHECK-NEXT:  addi {{x[0-9]+}}, [[C]], 3
; CHECK-NEXT:  #NO_APP
define i64 @imm_asm() {
  %r = call i64 asm "addi $0, $1, $2", "=r,r,i"(i64 7, i64 3)
  ret i64 %r
}

; A memory operand prints as "disp(base)".  A constant folded into the address
; becomes the displacement.
; CHECK-LABEL: mem_store_disp:
; CHECK:       #APP
; CHECK-NEXT:  sd [[V:x[0-9]+]], 24([[P:x[0-9]+]])
; CHECK-NEXT:  #NO_APP
define void @mem_store_disp(ptr %p, i64 %v) {
  %q = getelementptr i64, ptr %p, i64 3
  call void asm sideeffect "sd $1, $0", "*m,r"(ptr elementtype(i64) %q, i64 %v)
  ret void
}

; CHECK-LABEL: mem_load:
; CHECK:       #APP
; CHECK-NEXT:  ld [[D:x[0-9]+]], 0([[P:x[0-9]+]])
; CHECK-NEXT:  #NO_APP
define i64 @mem_load(i64 %x, ptr %p) {
  %r = call i64 asm "ld $0, $1", "=r,*m"(ptr elementtype(i64) %p)
  ret i64 %r
}
