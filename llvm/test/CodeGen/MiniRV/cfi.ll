; RUN: llc -mtriple=minirv64 < %s | FileCheck %s
; RUN: llc -mtriple=minirv64 -filetype=obj < %s -o %t.o
; RUN: llvm-readobj --relocations %t.o | FileCheck %s --check-prefix=RELOC

; A function with an unwind table describes the frame moves: the CFA follows
; the stack pointer adjustment, the saved registers are listed at their
; CFA-relative slots, and the epilogue undoes all of it.

declare void @callee()

; CHECK-LABEL: caller:
; CHECK:       addi    x2, x2, -16
; CHECK-NEXT:  .cfi_def_cfa_offset 16
; CHECK:       sd      x1, 8(x2)
; CHECK:       sd      x8, 0(x2)
; CHECK:       .cfi_offset x1, -8
; CHECK:       .cfi_offset x8, -16
; CHECK:       jal     x1, callee
; CHECK:       ld      x8, 0(x2)
; CHECK-NEXT:  .cfi_restore x8
; CHECK:       ld      x1, 8(x2)
; CHECK-NEXT:  .cfi_restore x1
; CHECK:       addi    x2, x2, 16
; CHECK-NEXT:  .cfi_def_cfa_offset 0
; CHECK:       jalr    x0, 0(x1)
define i32 @caller(i32 %a) #0 {
  %b = add i32 %a, 1
  call void @callee()
  ret i32 %b
}

; A dynamic alloca forces a frame pointer.  The CFA switches to X8 once it has
; been set up, and back to X2 when the epilogue recomputes the stack pointer.
; CHECK-LABEL: framepointer:
; CHECK:       addi    x2, x2, -32
; CHECK-NEXT:  .cfi_def_cfa_offset 32
; CHECK:       sd      x1, 24(x2)
; CHECK:       .cfi_offset x1, -8
; CHECK:       addi    x8, x2, 32
; CHECK-NEXT:  .cfi_def_cfa_register x8
; CHECK:       addi    x2, x8, -32
; CHECK-NEXT:  .cfi_def_cfa_register x2
; CHECK:       addi    x2, x2, 32
; CHECK-NEXT:  .cfi_def_cfa_offset 0
define i32 @framepointer(i64 %n) #0 {
  %a = alloca i8, i64 %n
  store volatile i8 1, ptr %a
  call void @callee()
  ret i32 1
}

; The PC-relative field of the FDE must not become an absolute relocation.
; RELOC: R_RISCV_32_PCREL

attributes #0 = { uwtable }
