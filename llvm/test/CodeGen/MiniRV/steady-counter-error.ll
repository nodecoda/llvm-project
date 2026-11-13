; RUN: not --crash llc -mtriple=minirv64 < %s -o /dev/null 2>&1 | FileCheck %s

; llvm.readsteadycounter shares the cycle-counter expansion, which would fold
; to zero without a CSR.  Give it its own diagnostic so the message names the
; intrinsic that was actually used.

; CHECK: LLVM ERROR: MiniRV has no steady counter
declare i64 @llvm.readsteadycounter()

define i64 @rdsteady() {
  %v = call i64 @llvm.readsteadycounter()
  ret i64 %v
}
