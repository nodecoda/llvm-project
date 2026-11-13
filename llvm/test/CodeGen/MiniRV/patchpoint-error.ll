; RUN: not --crash llc -mtriple=minirv64 < %s -o /dev/null 2>&1 | FileCheck %s

; A patchpoint is lowered the same way as a stack map, so it is rejected with
; the same diagnostic.  The call is surrounded by a call sequence, which the
; patchpoint lowering requires.

; CHECK: LLVM ERROR: MiniRV does not support stack maps
declare void @llvm.experimental.patchpoint.void(i64, i32, ptr, i32, ...)
declare void @target()

define void @f() {
  call void (i64, i32, ptr, i32, ...) @llvm.experimental.patchpoint.void(i64 1, i32 4, ptr @target, i32 0)
  ret void
}
