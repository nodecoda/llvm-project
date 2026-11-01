; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -global-isel -O0 \
; RUN:     -stop-after=legalizer -x86-gisel-extended-llt \
; RUN:     -x86-gisel-table-check -x86-gisel-table-stats -o /dev/null 2>&1 \
; RUN:     | FileCheck %s --check-prefix=VERIFY
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -global-isel -O0 \
; RUN:     -stop-after=legalizer -x86-gisel-extended-llt -o - 2>/dev/null \
; RUN:     | FileCheck %s --check-prefix=KIND
; RUN: not --crash llc < %s -mtriple=x86_64-unknown-linux-gnu -global-isel -O0 \
; RUN:     -stop-after=legalizer -x86-gisel-extended-llt -x86-gisel-table-check \
; RUN:     -x86-gisel-table-cache-extended -o /dev/null 2>&1 \
; RUN:     | FileCheck %s --check-prefix=CACHE-IS-WRONG

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
; The domain check's *representative* points cannot see any of this -- that is
; one point per cell, and the stored answer is that point's own answer.  What
; can see it is the fiber expansion: under -x86-gisel-extended-llt every class
; whose keyed slot can hold an integer is asked twice, s128 and i128, so a cell
; that answers both with s64 disagrees with the chain on the second.  `fiber`
; counts those extra points and `fiber_max 2` is the size of the largest
; expansion; both are matched below so a run that stopped expanding cannot look
; like a run that found nothing.
;
; The third RUN line is the negative control, made permanent:
; -x86-gisel-table-cache-extended ignores the !LLT::getUseExtended() gate, and
; the check must then report CACHED-VS-REPLAY.  Without that line, "mismatches
; 0" above would also be produced by a check that cannot fail this way.

; VERIFY: x86-gisel-table-check: points [[P:[0-9]+]] compared [[P]] mismatches 0 skipped 0 {{.*}} fiber [[F:[1-9][0-9]*]] fiber_refused 0 fiber_max 2
; VERIFY: x86-gisel-table: hit {{[1-9][0-9]*}} {{.*}} cached 0 checked {{[1-9][0-9]*}}

; CACHE-IS-WRONG: CACHED-VS-REPLAY G_ADD(i128): replay {{.*}}i64 cached {{.*}}s64

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
