; RUN: llc < %s -mtriple=minirv64 -O2 | FileCheck %s

; The triple is not in the runtime libcall table, so the subtarget names the C
; library entry points the memory intrinsics fall back to.  Without a name the
; lowering used to build a symbol from a null string and crash.

declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)
declare void @llvm.memmove.p0.p0.i64(ptr, ptr, i64, i1)
declare void @llvm.memset.p0.i64(ptr, i8, i64, i1)

; A fixed-size copy that does not fit the inline limits calls memcpy.
; CHECK-LABEL: fixed_copy:
; CHECK:         addi     x12, x0, 24
; CHECK-NEXT:    jal      x1, memcpy
define void @fixed_copy(ptr %d, ptr %s) {
  call void @llvm.memcpy.p0.p0.i64(ptr %d, ptr %s, i64 24, i1 false)
  ret void
}

; CHECK-LABEL: fixed_set:
; CHECK:         addi     x11, x0, 0
; CHECK-NEXT:    addi     x12, x0, 16
; CHECK-NEXT:    jal      x1, memset
define void @fixed_set(ptr %d) {
  call void @llvm.memset.p0.i64(ptr %d, i8 0, i64 16, i1 false)
  ret void
}

; A variable-size copy reaches the same call: the pre-isel pass only expands
; the intrinsic to a loop when no libcall is available.
; CHECK-LABEL: variable_copy:
; CHECK:         jal      x1, memcpy
define void @variable_copy(ptr %d, ptr %s, i64 %n) {
  call void @llvm.memcpy.p0.p0.i64(ptr %d, ptr %s, i64 %n, i1 false)
  ret void
}

; CHECK-LABEL: variable_move:
; CHECK:         jal      x1, memmove
define void @variable_move(ptr %d, ptr %s, i64 %n) {
  call void @llvm.memmove.p0.p0.i64(ptr %d, ptr %s, i64 %n, i1 false)
  ret void
}
