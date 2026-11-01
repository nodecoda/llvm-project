; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -mcpu=x86-64 \
; RUN:     -x86-gisel-table-check -o /dev/null 2>&1 | FileCheck %s
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -mcpu=x86-64-v2 \
; RUN:     -x86-gisel-table-check -o /dev/null 2>&1 | FileCheck %s
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -mcpu=x86-64-v3 \
; RUN:     -x86-gisel-table-check -o /dev/null 2>&1 | FileCheck %s
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -mcpu=x86-64-v4 \
; RUN:     -x86-gisel-table-check -o /dev/null 2>&1 | FileCheck %s
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -mcpu=x86-64 -global-isel -O0 \
; RUN:     -x86-gisel-table-check -x86-gisel-table-stats -o /dev/null 2>&1 \
; RUN:     | FileCheck %s --check-prefix=VERIFY
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -mcpu=x86-64-v3 \
; RUN:     -global-isel -O0 -x86-gisel-table-check -x86-gisel-table-stats \
; RUN:     -o /dev/null 2>&1 | FileCheck %s --check-prefix=VERIFY

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

; The VERIFY runs put real instructions through the GlobalISel pipeline with
; the table installed.  The check above asks the domain the key was built
; from; this one asks the questions a compilation actually asks, and the
; cached answers -- one per cell, for the tables whose cell is one question --
; are only ever exercised here.  `checked` is the number of real queries the
; table compared against the chain; a run that verified nothing would satisfy
; the mismatch line trivially, so it is matched as non-zero.
;
; VERIFY: x86-gisel-table-check: points [[P:[0-9]+]] compared [[P]] mismatches 0 skipped 0
; VERIFY: x86-gisel-table: hit {{[1-9][0-9]*}} {{.*}} checked {{[1-9][0-9]*}}

define void @f() {
  ret void
}

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

define i32 @mem(ptr %p) {
  %v = load i32, ptr %p
  %w = add i32 %v, 1
  store i32 %w, ptr %p
  ret i32 %w
}

define <4 x i32> @vec(<4 x i32> %a, <4 x i32> %b) {
  %r = add <4 x i32> %a, %b
  ret <4 x i32> %r
}

define i32 @conv(double %x) {
  %a = fptosi double %x to i32
  ret i32 %a
}

; A shape the rules answer with a mutation rather than "legal".  Every check
; above still passes if the table hands back a stale "Legal" here -- for a
; question both paths answer Legal, `mismatches 0` says nothing -- so the gate
; needs at least one question whose answer is not the trivial one.  Measured:
; with the cached answer forced to Legal this fails the check; with only the
; shapes above, it did not.
define i128 @add128(i128 %a, i128 %b) {
  %r = add i128 %a, %b
  ret i128 %r
}


; A scalar wider than any class the table names must be Outside, not answered.
; The key packs (kind, element bits, lanes) into one integer, and the element
; field is the width LLT gives it: 32 bits.  When it had 16, i65537's element
; size 0x10001 carried into the kind field, classified as i1, and the table
; answered "narrow to s8" for a type the chain widens to s131072.  Wide enough
; to exercise the field, small enough that the chain legalizes it (a vector of
; i65537 does not, which is why this is a scalar).
define i65537 @huge_scalar(i65537 %a, i65537 %b) {
  %r = add i65537 %a, %b
  ret i65537 %r
}
