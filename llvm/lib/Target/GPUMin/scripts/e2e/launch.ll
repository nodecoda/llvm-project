; Chapter 13 runtime sample: the kernel receives no register arguments.
; The launcher writes the argument block address into ur0; `sr.args` reads it,
; and the kernel loads three pointers (a, b, out) from that block.
;
; The launch is 2 blocks x 8 threads, so gid = (blockid << 3) + tid.
; A generic kernel would use blockDim, but the MVP ISA has no multiply yet.
target triple = "gpumin-unknown-elf"

declare i32 @llvm.gpumin.args()
declare i32 @llvm.gpumin.tid()
declare i32 @llvm.gpumin.blockid()

define void @vecadd_launch() #0 {
entry:
  %args = call i32 @llvm.gpumin.args()
  %ap = inttoptr i32 %args to ptr addrspace(1)
  %a = load i32, ptr addrspace(1) %ap
  %bp = getelementptr i32, ptr addrspace(1) %ap, i32 1
  %b = load i32, ptr addrspace(1) %bp
  %op = getelementptr i32, ptr addrspace(1) %ap, i32 2
  %out = load i32, ptr addrspace(1) %op

  %t = call i32 @llvm.gpumin.tid()
  %bid = call i32 @llvm.gpumin.blockid()
  %b8 = shl i32 %bid, 3
  %gid = add i32 %b8, %t

  %pa0 = inttoptr i32 %a to ptr addrspace(1)
  %pb0 = inttoptr i32 %b to ptr addrspace(1)
  %po0 = inttoptr i32 %out to ptr addrspace(1)
  %pa = getelementptr i32, ptr addrspace(1) %pa0, i32 %gid
  %pb = getelementptr i32, ptr addrspace(1) %pb0, i32 %gid
  %po = getelementptr i32, ptr addrspace(1) %po0, i32 %gid
  %x = load i32, ptr addrspace(1) %pa
  %y = load i32, ptr addrspace(1) %pb
  %s = add i32 %x, %y
  store i32 %s, ptr addrspace(1) %po
  ret void
}

attributes #0 = { "gpumin_kernel" }
