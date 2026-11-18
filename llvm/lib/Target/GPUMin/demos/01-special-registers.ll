; Demo 1: the SIMT identity a lane can read but not write.
;
; Every lane writes eight words -- tid, laneid, warpid, blockid, ntid, nwarps,
; the active mask and the shared-window base -- so one run shows how the grid
; is carved into blocks, warps and lanes, and proves that all eight reads are
; distinct instructions rather than one generic "read special register".
;
; Launch: 2 blocks x 16 threads (2 warps of 8 lanes).
; Output: 32 records of 8 words, indexed by (blockid * 16 + tid).
target triple = "gpumin-unknown-elf"

declare i32 @llvm.gpumin.tid()
declare i32 @llvm.gpumin.laneid()
declare i32 @llvm.gpumin.warpid()
declare i32 @llvm.gpumin.blockid()
declare i32 @llvm.gpumin.ntid()
declare i32 @llvm.gpumin.nwarps()
declare i32 @llvm.gpumin.activemask()
declare i32 @llvm.gpumin.shared()

define void @ids(ptr addrspace(1) %out) #0 {
entry:
  %t = call i32 @llvm.gpumin.tid()
  %b = call i32 @llvm.gpumin.blockid()
  ; record = (blockid * ntid + tid) * 8
  %b16 = shl i32 %b, 4
  %g = add i32 %b16, %t
  %g8 = shl i32 %g, 3
  %p = getelementptr i32, ptr addrspace(1) %out, i32 %g8

  store i32 %t, ptr addrspace(1) %p
  %p1 = getelementptr i32, ptr addrspace(1) %p, i32 1
  %lane = call i32 @llvm.gpumin.laneid()
  store i32 %lane, ptr addrspace(1) %p1
  %p2 = getelementptr i32, ptr addrspace(1) %p, i32 2
  %warp = call i32 @llvm.gpumin.warpid()
  store i32 %warp, ptr addrspace(1) %p2
  %p3 = getelementptr i32, ptr addrspace(1) %p, i32 3
  store i32 %b, ptr addrspace(1) %p3
  %p4 = getelementptr i32, ptr addrspace(1) %p, i32 4
  %ntid = call i32 @llvm.gpumin.ntid()
  store i32 %ntid, ptr addrspace(1) %p4
  %p5 = getelementptr i32, ptr addrspace(1) %p, i32 5
  %nwarps = call i32 @llvm.gpumin.nwarps()
  store i32 %nwarps, ptr addrspace(1) %p5
  %p6 = getelementptr i32, ptr addrspace(1) %p, i32 6
  %mask = call i32 @llvm.gpumin.activemask()
  store i32 %mask, ptr addrspace(1) %p6
  %p7 = getelementptr i32, ptr addrspace(1) %p, i32 7
  %win = call i32 @llvm.gpumin.shared()
  store i32 %win, ptr addrspace(1) %p7
  ret void
}

attributes #0 = { "gpumin_kernel" }
