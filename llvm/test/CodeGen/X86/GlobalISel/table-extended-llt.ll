; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -global-isel -O0 \
; RUN:     -stop-after=legalizer -x86-gisel-extended-llt \
; RUN:     -x86-gisel-table-check -x86-gisel-table-stats -o /dev/null 2>&1 \
; RUN:     | FileCheck %s --check-prefix=VERIFY
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -global-isel -O0 \
; RUN:     -stop-after=legalizer -x86-gisel-extended-llt -o - 2>/dev/null \
; RUN:     | FileCheck %s --check-prefix=KIND

; A cell of a one-question table stores the chain's answer for the cell's
; *representative*.  That is only the answer for every query in the cell if the
; class key is injective over the LLTs the target can produce, and it is not
; when LLT kinds are distinct: classifyType() folds INTEGER, FLOAT and
; ANY_SCALAR into one scalar class, so i128 lands in the s128 cell and the
; stored answer spells s64 where the chain spells i64 (a076fbbf9e72).
;
; X86 does not use extended LLTs, so -x86-gisel-extended-llt is a test hook
; that makes the situation reachable; x86 instruction selection is written for
; ANY_SCALAR, hence -stop-after=legalizer.  On this target the answer cache
; must be off: the rule number is replayed on the real query, which recomputes
; the mutation and keeps the kind.
;
; Two independent red signals:
;  - `cached 0`: an answer served from a cell.  Removing the
;    !LLT::getUseExtended() gate in queryImpl() makes this non-zero.
;  - `i64`/`f64` in the MIR below: with the gate removed the same queries are
;    handed s64/s8, i.e. the representative's spelling.
; The domain check alone cannot see any of this: it asks the key's
; representatives, whose answer is the one that was stored.

; VERIFY: x86-gisel-table-check: points [[P:[0-9]+]] compared [[P]] mismatches 0 skipped 0
; VERIFY: x86-gisel-table: hit {{[1-9][0-9]*}} {{.*}} cached 0 checked {{[1-9][0-9]*}}

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
