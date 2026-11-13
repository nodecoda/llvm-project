; RUN: not --crash llc -mtriple=minirv64 < %s -o /dev/null 2>&1 | FileCheck %s

; A scalable vector has no fixed byte size, so the argument lowering cannot
; assign it to a register or a stack slot.  Reject it with a diagnostic
; instead of failing inside TypeSize.

; CHECK: LLVM ERROR: MiniRV does not support scalable vectors
define <vscale x 4 x i64> @add(<vscale x 4 x i64> %a, <vscale x 4 x i64> %b) {
  %r = add <vscale x 4 x i64> %a, %b
  ret <vscale x 4 x i64> %r
}
