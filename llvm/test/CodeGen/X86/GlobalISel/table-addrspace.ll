; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -global-isel -O0 \
; RUN:     -global-isel-abort=0 -stop-after=legalizer \
; RUN:     -x86-gisel-table-stats -o - 2> %t.stats | FileCheck %s
; RUN: FileCheck %s --check-prefix=STATS < %t.stats
;
; The class list is built from the LLT constants the rules name, and every one
; of them is p0.  A pointer in another address space must therefore reach the
; chain, not a p0 cell: the chain refuses it.  Answering it from the p0 cell
; would turn a legalization failure into silently accepted MIR, and these three
; rules sets -- G_PTR_ADD, G_PTRTOINT, G_INTTOPTR -- are one-question tables,
; so no replay guard stands between a fold and the answer.
;
; The counts are the control.  With the address-space check removed from
; classifyType the table answers all three (hit 3, addrspace 0) and the chain
; never sees them; every function here also stops being `failedISel`, so both
; checks go red together.  The shapes are MIR-level: no IR in the corpus uses
; an address space the rule set does not name, which is why this needs its own
; test rather than a corpus run.
;
; STATS: x86-gisel-table: hit 0 outside 0 ptrvec 0 addrspace 3 untabulated 0
; CHECK: %{{[0-9]+}}:_(p272) = G_PTR_ADD
; CHECK: %{{[0-9]+}}:_(s64) = G_PTRTOINT %{{[0-9]+}}(p272)
; CHECK: %{{[0-9]+}}:_(p272) = G_INTTOPTR %{{[0-9]+}}(s64)

define ptr addrspace(272) @gep(ptr addrspace(272) %p, i64 %i) {
  %q = getelementptr i8, ptr addrspace(272) %p, i64 %i
  ret ptr addrspace(272) %q
}

define i64 @ptrtoint(ptr addrspace(272) %p) {
  %q = ptrtoint ptr addrspace(272) %p to i64
  ret i64 %q
}

define ptr addrspace(272) @inttoptr(i64 %v) {
  %q = inttoptr i64 %v to ptr addrspace(272)
  ret ptr addrspace(272) %q
}
