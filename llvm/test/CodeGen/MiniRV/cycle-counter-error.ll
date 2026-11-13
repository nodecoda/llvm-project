; RUN: not --crash llc -mtriple=minirv64 < %s -o /dev/null 2>&1 | FileCheck %s

; The base ISA has no cycle or time CSR.  The generic expansion would fold the
; intrinsic to zero, which is worse than refusing to compile it.

; CHECK: LLVM ERROR: MiniRV has no cycle counter
declare i64 @llvm.readcyclecounter()

define i64 @rdcycle() {
  %v = call i64 @llvm.readcyclecounter()
  ret i64 %v
}
