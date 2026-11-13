; RUN: not --crash llc -mtriple=minirv64 < %s -o /dev/null 2>&1 | FileCheck %s

; llvm.experimental.deoptimize needs a patchpoint at the call site and the
; __llvm_deoptimize runtime entry.  The libcall is named so the lowering
; reaches the patchpoint, where the missing stack-map support is diagnosed
; instead of the call being dropped and an empty function being emitted.

; CHECK: LLVM ERROR: MiniRV does not support stack maps
declare i64 @llvm.experimental.deoptimize.i64(...)

define i64 @caller(i64 %x) {
entry:
  %v = call i64(...) @llvm.experimental.deoptimize.i64(i64 %x) [ "deopt"(i64 %x) ]
  ret i64 %v
}
