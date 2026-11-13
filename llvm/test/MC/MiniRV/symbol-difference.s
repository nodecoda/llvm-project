# RUN: llvm-mc -triple=minirv64 -filetype=obj %s -o %t.o
# RUN: llvm-readobj --relocations %t.o | FileCheck %s

# A difference of two symbols in different sections cannot be resolved by the
# assembler, and a single relocation cannot describe it either: the linker
# needs an ADD for the first symbol and a SUB for the second.  Profile data
# describes a counter relative to its descriptor exactly this way.

.data
.globl table
table:
  .quad end - start

.section .data.a,"aw"
start:
  .word 1

.section .data.b,"aw"
end:
  .word 2

# CHECK: R_RISCV_ADD64
# CHECK: R_RISCV_SUB64
