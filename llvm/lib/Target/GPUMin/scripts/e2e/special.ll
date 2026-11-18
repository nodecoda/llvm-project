; Chapter 12 runtime sample: all seven special-register reads.
; Lane t of block b stores an 8-word record at out[gid*8], where
; gid = blockid * 32 + tid (the chapter 12 launch uses 32 threads):
;   [laneid, warpid, blockid, tid, ntid, nwarps, activemask, 0]
target triple = "gpumin-unknown-elf"

declare i32 @llvm.gpumin.laneid()
declare i32 @llvm.gpumin.warpid()
declare i32 @llvm.gpumin.blockid()
declare i32 @llvm.gpumin.tid()
declare i32 @llvm.gpumin.ntid()
declare i32 @llvm.gpumin.nwarps()
declare i32 @llvm.gpumin.activemask()

define void @special_kernel(ptr addrspace(1) %out) #0 {
entry:
  %t = call i32 @llvm.gpumin.tid()
  %b = call i32 @llvm.gpumin.blockid()
  %b32 = shl i32 %b, 5
  %gid = add i32 %b32, %t
  %base = shl i32 %gid, 3
  %p0 = getelementptr i32, ptr addrspace(1) %out, i32 %base
  %lane = call i32 @llvm.gpumin.laneid()
  store i32 %lane, ptr addrspace(1) %p0
  %p1 = getelementptr i32, ptr addrspace(1) %p0, i32 1
  %warp = call i32 @llvm.gpumin.warpid()
  store i32 %warp, ptr addrspace(1) %p1
  %p2 = getelementptr i32, ptr addrspace(1) %p0, i32 2
  store i32 %b, ptr addrspace(1) %p2
  %p3 = getelementptr i32, ptr addrspace(1) %p0, i32 3
  store i32 %t, ptr addrspace(1) %p3
  %p4 = getelementptr i32, ptr addrspace(1) %p0, i32 4
  %ntid = call i32 @llvm.gpumin.ntid()
  store i32 %ntid, ptr addrspace(1) %p4
  %p5 = getelementptr i32, ptr addrspace(1) %p0, i32 5
  %nwarps = call i32 @llvm.gpumin.nwarps()
  store i32 %nwarps, ptr addrspace(1) %p5
  %p6 = getelementptr i32, ptr addrspace(1) %p0, i32 6
  %mask = call i32 @llvm.gpumin.activemask()
  store i32 %mask, ptr addrspace(1) %p6
  ret void
}

attributes #0 = { "gpumin_kernel" }
