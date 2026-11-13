; RUN: llc < %s -mtriple=minirv64 -O2 | FileCheck %s
; RUN: llc < %s -mtriple=minirv64 -O2 -filetype=obj -o %t.o
; RUN: llvm-readobj -r %t.o | FileCheck %s --check-prefix=RELOC

; An indirect call keeps the callee in a register and jumps through it.
; CHECK-LABEL: call_reg:
; CHECK:       jalr x1, 0(
; CHECK:       jalr x0, 0(x1)
define i64 @call_reg(ptr %f, i64 %x) {
entry:
  %r = call i64 %f(i64 %x)
  ret i64 %r
}

; The address of a symbol is auipc (high 20 bits) plus addi (low 12).  The
; addi names the label of the auipc so that the assembler and the linker can
; find the high part that the low part belongs to.
; CHECK-LABEL: take_address:
; CHECK:       [[HI:\.Lpcrel_hi[0-9]+]]:
; CHECK-NEXT:  auipc x10, callee
; CHECK-NEXT:  addi x10, x10, [[HI]]
; CHECK:       jal x1, call_reg
define i64 @take_address() {
entry:
  %r = call i64 @call_reg(ptr @callee, i64 7)
  ret i64 %r
}

; A function pointer in a global initialiser is a data relocation.
; CHECK: fp:
; CHECK-NEXT: .quad callee
@fp = global ptr @callee

; RELOC: R_RISCV_PCREL_HI20 callee
; RELOC: R_RISCV_PCREL_LO12_I
; RELOC: R_RISCV_64 callee
define i64 @callee(i64 %x) {
entry:
  %r = mul i64 %x, 5
  ret i64 %r
}
