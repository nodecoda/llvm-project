; Chapter 17 sample: rotate values across warps through shared memory.
;
; Every lane stores its input into its own shared slot, waits at a block
; barrier, then reads the slot written by lane (tid + 8) -- the lane one warp
; over.  The value that comes back out of the second phase was written by a
; *different warp*, so it only exists if the barrier really orders the two
; phases.  Without it the answer depends on how the block schedules its
; warps, which is what --schedule exists to demonstrate.
target triple = "gpumin-unknown-elf"

@tile = internal addrspace(2) global [32 x i32] zeroinitializer, align 4

declare i32 @llvm.gpumin.tid()
declare void @llvm.gpumin.barrier()

define void @rotate(ptr addrspace(1) %in, ptr addrspace(1) %out) #0 {
entry:
  %t = call i32 @llvm.gpumin.tid()
  %pin = getelementptr i32, ptr addrspace(1) %in, i32 %t
  %v = load i32, ptr addrspace(1) %pin
  %pt = getelementptr [32 x i32], ptr addrspace(2) @tile, i32 0, i32 %t
  store i32 %v, ptr addrspace(2) %pt

  call void @llvm.gpumin.barrier()

  %t8 = add i32 %t, 8
  %src = and i32 %t8, 31
  %ps = getelementptr [32 x i32], ptr addrspace(2) @tile, i32 0, i32 %src
  %w = load i32, ptr addrspace(2) %ps
  %pout = getelementptr i32, ptr addrspace(1) %out, i32 %t
  store i32 %w, ptr addrspace(1) %pout
  ret void
}

attributes #0 = { "gpumin_kernel" }
