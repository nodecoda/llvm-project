; RUN: not --crash llc -mtriple=minirv64 < %s -o /dev/null 2>&1 | FileCheck %s

; Realigning the stack needs the frame pointer to restore the unrounded stack
; pointer, and a variable-length allocation moves the stack pointer again.
; There is no second base register to reach the locals, so the combination is
; rejected instead of being miscompiled.

; CHECK: LLVM ERROR: MiniRV cannot realign the stack with variable-sized allocations
define ptr @vla_and_align(i64 %n) {
  %p = alloca i8, i64 %n
  %q = alloca i8, i64 64, align 32
  ret ptr %q
}
