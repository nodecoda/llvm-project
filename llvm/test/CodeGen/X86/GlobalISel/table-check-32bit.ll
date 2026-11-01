; RUN: llc < %s -mtriple=i386-unknown-linux-gnu -global-isel -O0 \
; RUN:     -x86-gisel-table-check -o /dev/null 2>&1 | FileCheck %s
;
; The table's key pins p0 to 64 bits, so it only installs on a 64-bit
; subtarget.  The check builds its own table, so it has to follow the same
; condition: on a 32-bit subtarget the rule chain is written in terms of p0:32
; and every class representative the check would ask about is a p0:64 question
; the chain is not written for -- asking one asserts inside a rule
; (changeElementSize on a pointer).  That is the check asking the wrong
; question, not the table disagreeing, so the check says why it is not running
; rather than crashing or reporting a bogus count.
;
; CHECK: x86-gisel-table-check: the table is not installed on a 32-bit subtarget
; CHECK-NOT: x86-gisel-table-check: points

define void @f() {
  ret void
}
