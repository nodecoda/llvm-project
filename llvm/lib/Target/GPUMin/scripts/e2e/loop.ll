; Chapter 12 runtime sample: a device function with a loop, called from a
; kernel.  Every lane computes 0 + 1 + ... + (8 + warpid - 1) and stores the
; result.  The trip count is uniform inside a warp on purpose: the chapter 12
; machine has no divergence stack, so a per-lane trip count would (correctly)
; be rejected until chapter 14.
target triple = "gpumin-unknown-elf"

declare i32 @llvm.gpumin.tid()
declare i32 @llvm.gpumin.warpid()

define i32 @sum_to(i32 %n) {
entry:
  br label %head
head:
  %i = phi i32 [ 0, %entry ], [ %next, %body ]
  %sum = phi i32 [ 0, %entry ], [ %add, %body ]
  %c = icmp slt i32 %i, %n
  br i1 %c, label %body, label %done
body:
  %add = add i32 %sum, %i
  %next = add i32 %i, 1
  br label %head
done:
  ret i32 %sum
}

define void @sum_kernel(ptr addrspace(1) %out) #0 {
entry:
  %t = call i32 @llvm.gpumin.tid()
  %w = call i32 @llvm.gpumin.warpid()
  %n = add i32 %w, 8
  %v = call i32 @sum_to(i32 %n)
  %p = getelementptr i32, ptr addrspace(1) %out, i32 %t
  store i32 %v, ptr addrspace(1) %p
  ret void
}

attributes #0 = { "gpumin_kernel" }
