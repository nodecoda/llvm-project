; RUN: not --crash llc -mtriple=minirv64 < %s -o /dev/null 2>&1 | FileCheck %s

; A stack map is a pseudo instruction with no encoding.  Without a
; target-specific lowering that records the map and pads the code, reject it
; instead of letting the instruction printer assert.

; CHECK: LLVM ERROR: MiniRV does not support stack maps
declare void @llvm.experimental.stackmap(i64, i32, ...)

define void @f(i64 %x) {
  call void (i64, i32, ...) @llvm.experimental.stackmap(i64 1, i32 0, i64 %x)
  ret void
}
