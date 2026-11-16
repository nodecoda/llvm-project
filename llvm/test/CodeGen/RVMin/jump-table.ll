; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 -verify-machineinstrs < %s | FileCheck %s
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 < %s \
; RUN:   | llvm-mc -triple=rvmin -filetype=obj -o /dev/null
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 < %s -filetype=obj -o %t.o
; RUN: llvm-readelf -r %t.o | FileCheck %s --check-prefix=RELOC
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O0 < %s | FileCheck %s --check-prefix=O0

; Chapter 18: a dense switch becomes a jump table.  The selector is bounded,
; scaled by four, added to the table address, loaded, and jumped through:
;
;   slli idx, idx, 2
;   lui  t, %hi(.LJTI0_0)
;   addi t, t, %lo(.LJTI0_0)
;   add  t, t, idx
;   lw   t, 0(t)
;   jalr zero, 0(t)
;
; The table itself is emitted by the generic AsmPrinter as eight .word block
; addresses; R_RISCV_32 (already implemented in chapter 7) resolves them.
; A sparse switch keeps the comparison chain -- see `sparse` below.

define i32 @dense(i32 %x) {
entry:
  switch i32 %x, label %def [
    i32 0, label %c0
    i32 1, label %c1
    i32 2, label %c2
    i32 3, label %c3
    i32 4, label %c4
    i32 5, label %c5
    i32 6, label %c6
    i32 7, label %c7
  ]
c0: ret i32 100
c1: ret i32 101
c2: ret i32 102
c3: ret i32 103
c4: ret i32 104
c5: ret i32 105
c6: ret i32 106
c7: ret i32 107
def: ret i32 -1
}
; CHECK-LABEL: dense:
; CHECK:         addi a1, zero, 7
; CHECK-NEXT:    bltu a1, a0, .LBB0_10
; CHECK:         slli a0, a0, 2
; CHECK-NEXT:    lui a1, %hi(.LJTI0_0)
; CHECK-NEXT:    addi a1, a1, %lo(.LJTI0_0)
; CHECK-NEXT:    add a0, a1, a0
; CHECK-NEXT:    lw a0, 0(a0)
; CHECK-NEXT:    jalr zero, 0(a0)
; The first table target is the layout successor of the indirect jump; its
; label must still be emitted, which is why PseudoBRIND sets isIndirectBranch.
; CHECK-NEXT:  .LBB0_2:
; CHECK:         .section .rodata
; CHECK:         .LJTI0_0:
; CHECK-NEXT:    .word .LBB0_2
; CHECK-NEXT:    .word .LBB0_3
; CHECK-NEXT:    .word .LBB0_4
; CHECK-NEXT:    .word .LBB0_5
; CHECK-NEXT:    .word .LBB0_6
; CHECK-NEXT:    .word .LBB0_7
; CHECK-NEXT:    .word .LBB0_8
; CHECK-NEXT:    .word .LBB0_9
; CHECK-NEXT:    .size .LJTI0_0, 32

; Switch lowering runs in SelectionDAG at every optimization level, so -O0
; builds the same table (the IR-level LowerSwitch pass is not involved).
; O0-LABEL: dense:
; O0:         lui a0, %hi(.LJTI0_0)
; O0-NEXT:    addi a0, a0, %lo(.LJTI0_0)
; O0:         jalr zero, 0(a0)

define i32 @sparse(i32 %x) {
entry:
  switch i32 %x, label %def [
    i32 -1000, label %a
    i32 7, label %b
    i32 42, label %c
    i32 999, label %d
  ]
a: ret i32 1
b: ret i32 2
c: ret i32 3
d: ret i32 4
def: ret i32 0
}
; CHECK-LABEL: sparse:
; CHECK-NOT:     .LJTI
; CHECK:         beq a0, a1
; CHECK:         .Lfunc_end1:

; RELOC:      R_RISCV_HI20
; RELOC:      R_RISCV_LO12_I
; RELOC-COUNT-8: R_RISCV_32
