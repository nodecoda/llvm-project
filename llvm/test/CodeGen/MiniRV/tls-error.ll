; RUN: not --crash llc -mtriple=minirv64 < %s -o /dev/null 2>&1 | FileCheck %s

; MiniRV defines no TLS model.  A thread-local global is rejected with a
; targeted diagnostic instead of the selector's "Cannot select:
; GlobalTLSAddress".

; CHECK: LLVM ERROR: MiniRV does not support thread-local storage
@t = thread_local global i64 7

define i64 @get_tls() {
  %v = load i64, ptr @t
  ret i64 %v
}
