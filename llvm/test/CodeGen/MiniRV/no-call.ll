; RUN: not --crash llc -mtriple=minirv64 < %s -o /dev/null 2>&1 | FileCheck %s

; The corpus has no call, load or store instruction, so a function that needs
; one is rejected instead of being miscompiled.  Keeping the diagnostic in a
; test means that adding memory instructions later is a deliberate change.

; CHECK: LLVM ERROR: MiniRV has no call instruction
define i64 @call(i64 %a) {
  %r = call i64 @add(i64 %a, i64 %a)
  ret i64 %r
}

define i64 @add(i64 %a, i64 %b) {
  %r = add i64 %a, %b
  ret i64 %r
}
