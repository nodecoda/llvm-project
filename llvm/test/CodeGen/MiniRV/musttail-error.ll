; RUN: not --crash llc -mtriple=minirv64 < %s -o /dev/null 2>&1 | FileCheck %s

; A musttail call cannot be demoted to an ordinary call, and MiniRV only
; forwards register arguments: the ninth argument would have to be written to
; the caller's incoming stack argument area, which the epilogue is about to
; release.  Reject the call with a diagnostic instead of silently dropping the
; tail-call requirement.

; CHECK: LLVM ERROR: MiniRV cannot tail call a function with stack arguments
declare i64 @nine(i64, i64, i64, i64, i64, i64, i64, i64, i64)

define i64 @musttail_stack(i64 %a, i64 %b, i64 %c, i64 %d, i64 %e, i64 %f,
                           i64 %g, i64 %h, i64 %i) {
  %r = musttail call i64 @nine(i64 %a, i64 %b, i64 %c, i64 %d, i64 %e, i64 %f,
                               i64 %g, i64 %h, i64 %i)
  ret i64 %r
}
