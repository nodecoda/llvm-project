; RUN: llc -mtriple=minirv64 -filetype=obj < %s -o %t.o
; RUN: llvm-readobj --sections %t.o | FileCheck %s --check-prefix=SECTIONS
; RUN: llvm-readobj --relocations %t.o | FileCheck %s --check-prefix=RELOC
; RUN: llvm-objdump -dr --triple=minirv64 %t.o | FileCheck %s --check-prefix=OBJ

; The instrumented program counter is a plain counter that the function bumps,
; and the descriptor stores the counter relative to itself.  A symbol
; difference cannot be a single relocation: the linker gets an ADD for the
; counter and a SUB for the descriptor.

; SECTIONS: Name: __llvm_prf_cnts
; SECTIONS: Name: __llvm_prf_data
; SECTIONS: Name: .rela__llvm_prf_data

; RELOC: R_RISCV_ADD64
; RELOC: R_RISCV_SUB64

; OBJ: auipc
; OBJ: R_RISCV_PCREL_HI20 {{.*}}__profc_foo
; OBJ: ld
; OBJ: addi
; OBJ: sd

$__profc_foo = comdat nodeduplicate

@__profc_foo = private global [1 x i64] zeroinitializer, section "__llvm_prf_cnts", comdat, align 8
@__profd_foo = private global { i64, i64, i64, ptr, ptr, i32 } { i64 123, i64 40, i64 sub (i64 ptrtoint (ptr @__profc_foo to i64), i64 ptrtoint (ptr @__profd_foo to i64)), ptr null, ptr null, i32 1 }, section "__llvm_prf_data", comdat($__profc_foo), align 8

define void @foo() #0 {
  %v = load i64, ptr @__profc_foo, align 8
  %i = add i64 %v, 1
  store i64 %i, ptr @__profc_foo, align 8
  ret void
}

attributes #0 = { nounwind }
