; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf < %s | FileCheck %s
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O1 < %s -o - | llvm-mc -triple=riscv32 -filetype=obj -o /dev/null

; Chapter 3: just enough instruction selection to compile a constant return and
; an integer add.  Returns use `jalr zero, 0(ra)`, which PseudoRET expands to
; from the generated pseudo-lowering table.

define i32 @f() nounwind {
entry:
  ret i32 42
}

; CHECK-LABEL: f:
; CHECK:         addi a0, zero, 42
; CHECK-NEXT:    jalr zero, 0(ra)

define i32 @g(i32 %a, i32 %b) nounwind {
entry:
  %r = add i32 %a, %b
  ret i32 %r
}

; CHECK-LABEL: g:
; CHECK:         add a0, a0, a1
; CHECK-NEXT:    jalr zero, 0(ra)
