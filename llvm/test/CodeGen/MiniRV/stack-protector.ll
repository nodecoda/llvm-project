; RUN: llc -mtriple=minirv64 < %s | FileCheck %s

; A function with a stack buffer and sspstrong loads the guard from the
; __stack_chk_guard global into the frame, compares it again before the return
; and calls __stack_chk_fail when the two differ.

; CHECK-LABEL: guarded:
; CHECK:         auipc   {{x[0-9]+}}, __stack_chk_guard
; CHECK:         ld
; CHECK:         sd      {{x[0-9]+}}, 0(
; CHECK:         bne
; CHECK:         jal     x1, __stack_chk_fail
define void @guarded(ptr %p) sspstrong {
  %buf = alloca [16 x i8]
  store i8 0, ptr %buf
  call void @use(ptr %buf)
  ret void
}

declare void @use(ptr)
