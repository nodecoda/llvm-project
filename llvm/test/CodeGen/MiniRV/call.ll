; RUN: llc < %s -mtriple=minirv64 -O2 | FileCheck %s

; A direct call is "jal x1, target": the link register is written and the
; return address is spilled by any function that makes a call, because x1 is
; in the callee-saved set.  The corpus has no move, so the argument copies are
; addi rd, rs, 0 when they survive register allocation at all.

; CHECK-LABEL: callee:
; CHECK: add x10, x10, x11
; CHECK: jalr x0, 0(x1)
define i64 @callee(i64 %a, i64 %b) {
entry:
  %s = add i64 %a, %b
  ret i64 %s
}

; CHECK-LABEL: caller:
; CHECK: addi x2, x2, -16
; CHECK: sd x1, 8(x2)
; CHECK: jal x1, callee
; CHECK: ld x1, 8(x2)
; CHECK: addi x2, x2, 16
; CHECK: jalr x0, 0(x1)
define i64 @caller(i64 %a, i64 %b) {
entry:
  %r = call i64 @callee(i64 %a, i64 %b)
  ret i64 %r
}

; Two calls: the return address is saved once, not twice.
; CHECK-LABEL: nested:
; CHECK: sd x1, {{-?[0-9]+\(x2\)}}
; CHECK: jal x1, callee
; CHECK: jal x1, callee
; CHECK: ld x1, {{-?[0-9]+\(x2\)}}
define i64 @nested(i64 %a, i64 %b) {
entry:
  %r = call i64 @callee(i64 %a, i64 %b)
  %s = call i64 @callee(i64 %r, i64 %b)
  ret i64 %s
}

; A recursive call needs the same save and reload around both calls.
; CHECK-LABEL: fib:
; CHECK: sd x1, {{-?[0-9]+\(x2\)}}
; CHECK: jal x1, fib
; CHECK: jal x1, fib
; CHECK: ld x1, {{-?[0-9]+\(x2\)}}
define i64 @fib(i64 %n) {
entry:
  %c = icmp slt i64 %n, 2
  br i1 %c, label %base, label %rec
base:
  ret i64 %n
rec:
  %n1 = sub i64 %n, 1
  %n2 = sub i64 %n, 2
  %a = call i64 @fib(i64 %n1)
  %b = call i64 @fib(i64 %n2)
  %s = add i64 %a, %b
  ret i64 %s
}
