; RUN: llc < %s -mtriple=minirv64 -O2 | FileCheck %s

declare i64 @callee(i64)
declare i64 @nine(i64, i64, i64, i64, i64, i64, i64, i64, i64)

; A call in tail position becomes a jump: the link register is not written, so
; the callee returns straight to this function's caller.
; CHECK-LABEL: direct:
; CHECK:       jal x0, callee
; CHECK-NOT:   jalr x0, 0(x1)
define i64 @direct(i64 %x) {
entry:
  %r = tail call i64 @callee(i64 %x)
  ret i64 %r
}

; The same with the callee in a register.
; CHECK-LABEL: indirect:
; CHECK:       jalr x0, 0(
define i64 @indirect(ptr %f, i64 %x) {
entry:
  %r = tail call i64 %f(i64 %x)
  ret i64 %r
}

; A frame is released before the jump, because the callee reuses the stack
; from the point this function was entered at.
; CHECK-LABEL: with_frame:
; CHECK:       addi x2, x2, -32
; CHECK:       sd x10, 0(
; CHECK:       addi x2, x2, 32
; CHECK:       jal x0, callee
define i64 @with_frame(i64 %x) {
entry:
  %a = alloca [4 x i64], align 8
  store i64 %x, ptr %a
  %v = load i64, ptr %a
  %r = tail call i64 @callee(i64 %v)
  ret i64 %r
}

; A stack argument cannot be forwarded: the argument would have to be written
; to the area this function received its own stack arguments in, which the
; callee would then read instead.  Such a call stays an ordinary call.
; CHECK-LABEL: with_stack_arg:
; CHECK:       jal x1, nine
; CHECK:       jalr x0, 0(x1)
define i64 @with_stack_arg(i64 %x) {
entry:
  %r = tail call i64 @nine(i64 1, i64 2, i64 3, i64 4, i64 5, i64 6, i64 7,
                           i64 8, i64 %x)
  ret i64 %r
}

; A self-recursive tail call is a loop.
; CHECK-LABEL: loop:
; CHECK:       jal x0, loop
define i64 @loop(i64 %n, i64 %acc) {
entry:
  %c = icmp eq i64 %n, 0
  br i1 %c, label %done, label %step
step:
  %acc1 = add i64 %acc, 1
  %n1 = sub i64 %n, 1
  %r = tail call i64 @loop(i64 %n1, i64 %acc1)
  ret i64 %r
done:
  ret i64 %acc
}
