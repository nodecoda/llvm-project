; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O1 < %s | FileCheck %s
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O1 < %s -filetype=obj -o %t.o
; RUN: llvm-readelf --unwind %t.o | FileCheck %s --check-prefix=UNWIND

; Chapter 11: DWARF unwind information.
;
; Functions are only given CFI when it is requested: `llc` emits `.eh_frame`
; for IR without `nounwind` (`Function::needsUnwindTableEntry()`), and clang
; adds it for C only with `-funwind-tables`.  The directives must describe the
; frame *after* the stores have run.

declare void @sink(i32)

define i32 @framed(i32 %a, i32 %b) {
  %m = mul i32 %a, %b
  call void @sink(i32 %m)
  ret i32 %m
}
; CHECK-LABEL: framed:
; CHECK:         addi sp, sp, -16
; CHECK:         sw ra, 12(sp)
; CHECK:         sw s0, 8(sp)
; The CFA is sp + 16 once the frame is allocated, and the saved registers are
; described relative to it.
; CHECK:         .cfi_def_cfa_offset 16
; CHECK:         .cfi_offset ra, -4
; CHECK:         .cfi_offset s0, -8
; CHECK:         jalr ra, 0(t0)
; After the reloads and `sp += 16` the frame is gone again.
; CHECK:         .cfi_restore ra
; CHECK:         .cfi_restore s0
; CHECK:         .cfi_def_cfa_offset 0
; CHECK:         jalr zero, 0(ra)

; A leaf function has no frame, so its .eh_frame entry is empty.
define i32 @leaf(i32 %a) {
  %r = add i32 %a, 1
  ret i32 %r
}
; CHECK-LABEL: leaf:
; CHECK-NOT:     .cfi_def_cfa_offset
; CHECK:         jalr zero, 0(ra)

; UNWIND:       .eh_frame section
; UNWIND:       DW_CFA_def_cfa: reg2 +0
; UNWIND:       DW_CFA_def_cfa_offset: +16
; UNWIND:       DW_CFA_offset: reg1 -4
; UNWIND:       DW_CFA_offset: reg8 -8
; UNWIND:       DW_CFA_restore: reg1
; UNWIND:       DW_CFA_restore: reg8
; UNWIND:       DW_CFA_def_cfa_offset: +0
