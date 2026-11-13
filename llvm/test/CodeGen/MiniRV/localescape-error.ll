; RUN: not --crash llc -mtriple=minirv64 < %s -o /dev/null 2>&1 | FileCheck %s

; llvm.localescape is accepted (the frame index becomes the offset from the
; frame address), but there is no selection for llvm.localrecover.  Report the
; pair instead of an assertion or "Cannot select".

; CHECK: LLVM ERROR: MiniRV does not support llvm.localescape/llvm.localrecover
declare void @llvm.localescape(...)
declare ptr @llvm.localrecover(ptr, ptr, i32)

define void @f() {
entry:
  %a = alloca i32
  call void (...) @llvm.localescape(ptr %a)
  ret void
}

define ptr @g(ptr %parent) {
entry:
  %p = call ptr @llvm.localrecover(ptr @f, ptr %parent, i32 0)
  ret ptr %p
}
