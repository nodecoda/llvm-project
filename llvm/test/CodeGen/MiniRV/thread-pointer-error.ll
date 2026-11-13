; RUN: not --crash llc -mtriple=minirv64 < %s -o /dev/null 2>&1 | FileCheck %s

; llvm.thread.pointer reads the thread pointer register.  MiniRV defines no
; TLS model and has no such register, so the intrinsic is rejected with the
; same diagnostic as a thread-local global instead of the selector's
; "Cannot select: intrinsic %llvm.thread.pointer".

; CHECK: LLVM ERROR: MiniRV does not support thread-local storage
define ptr @thread_pointer() {
  %p = call ptr @llvm.thread.pointer()
  ret ptr %p
}

declare ptr @llvm.thread.pointer()
