; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -mcpu=x86-64 \
; RUN:     -x86-gisel-table-check -o /dev/null 2>&1 | FileCheck %s
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -mcpu=x86-64-v2 \
; RUN:     -x86-gisel-table-check -o /dev/null 2>&1 | FileCheck %s
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -mcpu=x86-64-v3 \
; RUN:     -x86-gisel-table-check -o /dev/null 2>&1 | FileCheck %s
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -mcpu=x86-64-v4 \
; RUN:     -x86-gisel-table-check -o /dev/null 2>&1 | FileCheck %s

; The table is filled from the rule chains and then asked the same questions
; the domain enumerates.  A mismatch means the key merges two shapes the rules
; treat differently, which is the only way this table can be wrong: it cannot
; invent an answer the chain would not give.
;
; CHECK: x86-gisel-table-check: points [[P:[0-9]+]] compared [[P]] mismatches 0 skipped 0

; Every point the domain enumerates must be compared, not skipped.  On LP64
; every class representative and every memory-type fiber member is a shape the
; key can classify, so `skipped` has exactly one meaning here: a generator
; defect that quietly turned a point into a non-question.  It was 590 before
; the fiber stopped emitting three-lane vectors of pointers (total size 3x the
; cell they stood for); those points were then answered from a different cell
; under this cell's name.  Pinning `compared == points` keeps that from coming
; back as a number nobody reads.

define void @f() {
  ret void
}
