; RUN: llc < %s -mtriple=minirv64 -O2 | FileCheck %s

; A dense switch becomes a table of block addresses: the index is scaled by
; the entry size, the table is addressed with the same auipc+addi pair as any
; other symbol, and the branch is an indirect "jalr x0, rs, 0".
;
; CHECK-LABEL: sw:
; CHECK:       slli [[IDX:x[0-9]+]], x10, 3
; CHECK:       .Lpcrel_hi{{[0-9]+}}:
; CHECK-NEXT:  auipc [[TAB:x[0-9]+]], .LJTI0_0
; CHECK-NEXT:  addi [[TAB]], [[TAB]], .Lpcrel_hi
; CHECK-NEXT:  add [[IDX]], [[TAB]], [[IDX]]
; CHECK-NEXT:  ld [[DST:x[0-9]+]], 0([[IDX]])
; CHECK-NEXT:  jalr x0, 0([[DST]])
;
; Every block the table refers to has to carry a label, even the one that is
; the fallthrough of the indirect branch.  The labels follow the layout order,
; which is not the switch order, so capture them from the blocks themselves.
; CHECK-DAG:   [[C0:.LBB[0-9]+_[0-9]+]]:{{.*}}# %c0
; CHECK-DAG:   [[C1:.LBB[0-9]+_[0-9]+]]:{{.*}}# %c1
; CHECK-DAG:   [[C2:.LBB[0-9]+_[0-9]+]]:{{.*}}# %c2
; CHECK-DAG:   [[C3:.LBB[0-9]+_[0-9]+]]:{{.*}}# %c3
; CHECK-DAG:   [[C4:.LBB[0-9]+_[0-9]+]]:{{.*}}# %c4
; CHECK-DAG:   [[C5:.LBB[0-9]+_[0-9]+]]:{{.*}}# %c5
; CHECK-DAG:   [[C6:.LBB[0-9]+_[0-9]+]]:{{.*}}# %c6
; CHECK-DAG:   [[C7:.LBB[0-9]+_[0-9]+]]:{{.*}}# %c7
;
; The entries are pointers to those blocks, in switch order.
; CHECK:       .LJTI0_0:
; CHECK-NEXT:  .quad [[C0]]
; CHECK-NEXT:  .quad [[C1]]
; CHECK-NEXT:  .quad [[C2]]
; CHECK-NEXT:  .quad [[C3]]
; CHECK-NEXT:  .quad [[C4]]
; CHECK-NEXT:  .quad [[C5]]
; CHECK-NEXT:  .quad [[C6]]
; CHECK-NEXT:  .quad [[C7]]
define i64 @sw(i64 %x) {
entry:
  switch i64 %x, label %def [
    i64 0, label %c0
    i64 1, label %c1
    i64 2, label %c2
    i64 3, label %c3
    i64 4, label %c4
    i64 5, label %c5
    i64 6, label %c6
    i64 7, label %c7
  ]
c0: ret i64 10
c1: ret i64 11
c2: ret i64 12
c3: ret i64 13
c4: ret i64 14
c5: ret i64 15
c6: ret i64 16
c7: ret i64 17
def: ret i64 0
}

; A sparse switch is lowered to a tree of comparisons and needs no table.
; CHECK-LABEL: sparse:
; CHECK-NOT:   .LJTI
; CHECK:       jalr x0, 0(x1)
define i64 @sparse(i64 %x) {
entry:
  switch i64 %x, label %def [
    i64 1, label %c0
    i64 1000, label %c1
  ]
c0: ret i64 1
c1: ret i64 2
def: ret i64 0
}
