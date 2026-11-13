; RUN: not --crash llc -mtriple=minirv64 < %s -o /dev/null 2>&1 | FileCheck %s

; A constant that needs more than 32 bits would have to be assembled with
; shifts, which the corpus does not have, so the selector refuses it instead of
; emitting a wrong value.

; CHECK: LLVM ERROR: MiniRV cannot materialise a 64-bit constant
define i64 @too_wide() {
  ret i64 4294967296
}
