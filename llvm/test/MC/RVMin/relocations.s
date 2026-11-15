# RUN: llvm-mc -triple=rvmin -filetype=obj %s -o %t.o
# RUN: llvm-readelf -r %t.o | FileCheck %s
#
# Chapter 7: a symbolic address is built from two instructions and two
# relocations -- the same pair the AsmPrinter emits for a global.

        lui     a0, %hi(sym)
# CHECK: R_RISCV_HI20
        addi    a0, a0, %lo(sym)
# CHECK: R_RISCV_LO12_I
        sw      a1, %lo(sym)(a0)
# CHECK: R_RISCV_LO12_S
# CHECK: sym

        .data
        .globl  sym
sym:
        .word   0
