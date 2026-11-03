; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -global-isel \
; RUN:     -x86-gisel-table-stats -x86-gisel-table-residuals \
; RUN:     -o /dev/null 2>&1 | FileCheck %s
; RUN: llc < %s -mtriple=i386-unknown-linux-gnu -global-isel \
; RUN:     -x86-gisel-table-stats -x86-gisel-table-residuals \
; RUN:     -o /dev/null 2>&1 | FileCheck %s

; G_BR carries no types, so the key has no slot-0 type to classify.  The rules
; cannot read a slot the query does not have, so the answer cannot depend on
; that slot: the query belongs in a cell like any other.  It used to be
; treated as an unknown class and sent to the evaluator, which on real code
; meant every branch was a "residual" query -- 11% of all queries on sqlite3.
;
; A non-zero residual or no-type count here means the zero-type path regressed;
; an untabulated count means G_BR stopped being tabulated at all.

; CHECK: x86-gisel-table: signature {{0x[0-9a-f]+}} hit {{[1-9][0-9]*}} residual 0 escape 0 outside 0 ptrvec 0 addrspace 0 untabulated 0
; CHECK: x86-gisel-table residuals: no-type 0

define i32 @diamond(i32 %a, i32 %b) {
entry:
  %c = icmp slt i32 %a, %b
  br i1 %c, label %then, label %else
then:
  %x = add i32 %a, 1
  br label %join
else:
  %y = add i32 %b, 1
  br label %join
join:
  %p = phi i32 [ %x, %then ], [ %y, %else ]
  ret i32 %p
}

define i32 @loop(i32 %n) {
entry:
  br label %body
body:
  %i = phi i32 [ 0, %entry ], [ %i2, %body ]
  %i2 = add i32 %i, 1
  %c = icmp slt i32 %i2, %n
  br i1 %c, label %body, label %exit
exit:
  ret i32 %i2
}
