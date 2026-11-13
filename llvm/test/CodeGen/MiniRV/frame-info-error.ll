; RUN: not --crash llc -mtriple=minirv64 < %s -o /dev/null 2>&1 | FileCheck %s

; The frame pointer is the incoming stack pointer and the saved one lives at a
; fixed offset below it, but MiniRV does not define a frame chain, so a nonzero
; depth is rejected instead of returning a wrong address.

declare ptr @llvm.frameaddress.p0(i32)

; CHECK: LLVM ERROR: MiniRV cannot walk the frame chain
define ptr @frame_depth() {
  %p = call ptr @llvm.frameaddress.p0(i32 1)
  ret ptr %p
}
