; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -global-isel -O0 \
; RUN:     -stop-after=legalizer -x86-gisel-extended-llt \
; RUN:     -x86-gisel-table-check -x86-gisel-table-stats -o /dev/null 2>&1 \
; RUN:     | FileCheck %s --check-prefix=VERIFY
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -global-isel -O0 \
; RUN:     -stop-after=legalizer -x86-gisel-extended-llt -o - 2>/dev/null \
; RUN:     | FileCheck %s --check-prefix=KIND

; The table stores, per cell, the rule that wins -- not the answer it gave.
; The answer is recomputed by replaying that rule on the real query, so a
; query whose LLT kinds the key folded into one class still gets its own
; spelling back (a076fbbf9e72).  This test pins that down on the one shape
; where the difference is visible: classifyType() folds INTEGER, FLOAT and
; ANY_SCALAR into one scalar class, so i128 lands in the s128 cell and the
; *rule* has to widen i128 to i64, not to s64.
;
; X86 does not use extended LLTs, so -x86-gisel-extended-llt is a test hook
; that makes the situation reachable; x86 instruction selection is written for
; ANY_SCALAR, hence -stop-after=legalizer.
;
; The domain check's *representative* points cannot see this -- one point per
; cell, and each representative answers with its own spelling.  What can see
; it is the fiber expansion: under -x86-gisel-extended-llt every class whose
; keyed slot can hold an integer is asked s128 and i128, so a table that
; replayed the representative's answer would disagree with the chain on the
; second.  `fiber` counts those extra points and `fiber_max` is the size of
; the largest expansion -- 4 where the slot also admits the float spellings,
; which the generator decides per slot from the widths the rules force
; (G_FADD does, G_FCMP cannot: its clamp narrows to s8).  `fiber_skipped`
; counts the float spellings the fiber declined for that reason, so a slot
; with no float coverage is a number here rather than an absence: 288 of them
; across the tables.  All four numbers are matched below, so a run that
; stopped expanding, a run that started asking floats of a slot the chain
; aborts on, and a run that quietly dropped a spelling cannot look like a run
; that found nothing.

; VERIFY: x86-gisel-table-check: points [[P:[0-9]+]] compared [[P]] mismatches 0 skipped 0 {{.*}} fiber [[F:[1-9][0-9]*]] fiber_refused 0 fiber_skipped [[SK:[1-9][0-9]*]] fiber_max 4 outside_checked [[OC:[1-9][0-9]*]]
; VERIFY: x86-gisel-table: hit {{[1-9][0-9]*}} {{.*}} checked {{[1-9][0-9]*}}

; KIND: %{{[0-9]+}}:_(i32) = G_ADD
; KIND: %{{[0-9]+}}:_(i64) = G_MUL
; KIND: %{{[0-9]+}}:_(f64) = G_FADD
; KIND: %{{[0-9]+}}:_(i64), %{{[0-9]+}}:_(i8) = G_UADDO

define i32 @scalar(i32 %a, i32 %b) {
  %r = add i32 %a, %b
  ret i32 %r
}

define i64 @mul64(i64 %a, i64 %b) {
  %r = mul i64 %a, %b
  ret i64 %r
}

define double @fp(double %a, double %b) {
  %r = fadd double %a, %b
  ret double %r
}

; The mutation case: i128 is not legal, the rule widens it to 64 bits, and the
; widened type's spelling comes from the query, not from the cell.
define i128 @big(i128 %a, i128 %b) {
  %c = add i128 %a, %b
  ret i128 %c
}

define i32 @mem(ptr %p) {
  %v = load i32, ptr %p
  store i32 %v, ptr %p
  ret i32 %v
}
