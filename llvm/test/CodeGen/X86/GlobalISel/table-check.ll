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
; CHECK: x86-gisel-table-check: points {{[0-9]+}} compared {{[1-9][0-9][0-9]+}} mismatches 0

define void @f() {
  ret void
}
