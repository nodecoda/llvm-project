; RUN: llc -march=gpumin -O1 -verify-machineinstrs < %s | FileCheck %s

; Chapter 17: the block barrier.  It is effectful and convergent, so the DAG
; may neither merge two barriers into one nor move one across the stores it
; is supposed to order.

declare void @llvm.gpumin.barrier()

define void @one() {
; CHECK-LABEL: one:
; CHECK:       bar.sync
; CHECK-NOT:   bar.sync
  call void @llvm.gpumin.barrier()
  ret void
}

define void @two() {
; CHECK-LABEL: two:
; CHECK:       bar.sync
; CHECK:       bar.sync
  call void @llvm.gpumin.barrier()
  call void @llvm.gpumin.barrier()
  ret void
}

; The barrier sits between the two shared accesses.  If it were removed or
; moved, the store and the load could be reordered and the test would see
; `ld.shared` before `st.shared`.
define i32 @between(ptr addrspace(2) %p, i32 %v) {
; CHECK-LABEL: between:
; CHECK:       st.shared.b32
; CHECK:       bar.sync
; CHECK:       ld.shared.b32
  store i32 %v, ptr addrspace(2) %p
  call void @llvm.gpumin.barrier()
  %x = load i32, ptr addrspace(2) %p
  ret i32 %x
}

; A barrier inside one arm of a branch stays inside it.  (The condition comes
; from a compare because GPUMin's argument lowering has no i1 register class.)
define void @in_branch(i32 %x) {
; CHECK-LABEL: in_branch:
; CHECK:       bra p0,
; CHECK:       bar.sync
; CHECK:       ret
  %c = icmp eq i32 %x, 0
  br i1 %c, label %then, label %end
then:
  call void @llvm.gpumin.barrier()
  br label %end
end:
  ret void
}
