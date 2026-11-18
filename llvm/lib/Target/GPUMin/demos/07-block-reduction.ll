; Demo 7: shared memory, a barrier, and two blocks that cannot see each other.
;
; Phase 1: every lane stores its input into the block's shared window.
; Barrier: no warp may read the window until every warp has written it.
; Phase 2: lane 0 of warp 0 sums the 32 words and writes one result.
;
; Launch: 2 blocks x 32 threads.  The two sums come from two separate windows,
; which is what makes the shared window block-scoped rather than grid-scoped.
;
; out[blockid] = sum(in[blockid * 32 .. blockid * 32 + 31])
target triple = "gpumin-unknown-elf"

@window = internal addrspace(2) global [32 x i32] zeroinitializer, align 4

declare i32 @llvm.gpumin.tid()
declare i32 @llvm.gpumin.blockid()
declare void @llvm.gpumin.barrier()

define void @block_sum(ptr addrspace(1) %in, ptr addrspace(1) %out) #0 {
entry:
  %t = call i32 @llvm.gpumin.tid()
  %b = call i32 @llvm.gpumin.blockid()
  %pin = getelementptr i32, ptr addrspace(1) %in, i32 %t
  %v = load i32, ptr addrspace(1) %pin
  %pw = getelementptr [32 x i32], ptr addrspace(2) @window, i32 0, i32 %t
  store i32 %v, ptr addrspace(2) %pw

  call void @llvm.gpumin.barrier()

  %first = icmp eq i32 %t, 0
  br i1 %first, label %sum, label %end

sum:
  br label %sloop

sloop:
  %i = phi i32 [ 0, %sum ], [ %inext, %sloop ]
  %acc = phi i32 [ 0, %sum ], [ %acc2, %sloop ]
  %pi = getelementptr [32 x i32], ptr addrspace(2) @window, i32 0, i32 %i
  %w = load i32, ptr addrspace(2) %pi
  %acc2 = add i32 %acc, %w
  %inext = add i32 %i, 1
  %last = icmp eq i32 %inext, 32
  br i1 %last, label %done, label %sloop

done:
  %pout = getelementptr i32, ptr addrspace(1) %out, i32 %b
  store i32 %acc2, ptr addrspace(1) %pout
  br label %end

end:
  ret void
}

attributes #0 = { "gpumin_kernel" }
