; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -mcpu=x86-64 \
; RUN:     -x86-gisel-table-check -o /dev/null 2>&1 | FileCheck %s
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -mcpu=x86-64-v2 \
; RUN:     -x86-gisel-table-check -o /dev/null 2>&1 | FileCheck %s
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -mcpu=x86-64-v3 \
; RUN:     -x86-gisel-table-check -o /dev/null 2>&1 | FileCheck %s
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -mcpu=x86-64-v4 \
; RUN:     -x86-gisel-table-check -o /dev/null 2>&1 | FileCheck %s
; RUN: llc < %s -mtriple=i386-unknown-linux-gnu \
; RUN:     -x86-gisel-table-check -o /dev/null 2>&1 | FileCheck %s

; The table is answered by the evaluator the table generator emits for each
; rule set -- the chain's guards and mutations written out, not the chain
; itself -- and the same questions the domain enumerates are put to both.  A
; mismatch means the key merges two shapes the rules treat differently, which
; is the only way this table can be wrong: the evaluator cannot invent an
; answer the chain would not give.
;
; Every tabulated rule set has an evaluator, so no enumerated point is
; skipped: points == compared.  The domain also includes types the class list
; does not name (s12/s27/s35/s42/s48, v4s1, v3s32), which take the residual
; path: the evaluator answers the concrete query instead of a cell.  A skipped
; point would mean a rule set the generator cannot translate, which is a gap to
; close rather than to allow.
;
; CHECK: x86-gisel-table-check: points 1408 compared 1408 mismatches 0 skipped 0 residual 811 outside 0 ptrvec 0 addrspace {{[0-9]+}} untabulated 0

define void @f() {
  ret void
}
