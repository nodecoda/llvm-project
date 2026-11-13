; RUN: not --crash llc < %s -mtriple=minirv64 -O2 2>&1 | FileCheck %s

; The convention puts the first eight integer arguments in x10..x17 and the
; rest on the stack.  Nothing reserves the outgoing argument area yet, so a
; ninth argument is rejected rather than silently dropped.

; CHECK: LLVM ERROR: MiniRV cannot pass arguments on the stack
define i64 @call_many() {
entry:
  %r = call i64 @sink(i64 1, i64 2, i64 3, i64 4, i64 5, i64 6, i64 7, i64 8,
                      i64 9)
  ret i64 %r
}

declare i64 @sink(i64, i64, i64, i64, i64, i64, i64, i64, i64)
