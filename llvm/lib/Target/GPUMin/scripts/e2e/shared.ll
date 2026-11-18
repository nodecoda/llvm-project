; Chapter 16 sample: a static shared tile.
;
; `@tile` lives in address space 2 with no object-file symbol: its address is
; `sr.shared` plus a static offset that the backend assigns.  Each lane stores
; its element, then reads the element one warp-width away, so the value that
; comes back out only exists in shared memory.
;
; One warp per block on purpose: with no barrier instruction yet (chapter 17),
; lanes of the same warp are still in lockstep, which is exactly what makes
; the store/load pair safe here.
target triple = "gpumin-unknown-elf"

@tile = internal addrspace(2) global [8 x i32] zeroinitializer, align 4

declare i32 @llvm.gpumin.tid()

define void @shared_copy(ptr addrspace(1) %in, ptr addrspace(1) %out) #0 {
entry:
  %t = call i32 @llvm.gpumin.tid()
  %pin = getelementptr i32, ptr addrspace(1) %in, i32 %t
  %v = load i32, ptr addrspace(1) %pin
  %pt = getelementptr [8 x i32], ptr addrspace(2) @tile, i32 0, i32 %t
  store i32 %v, ptr addrspace(2) %pt

  %back = sub i32 7, %t
  %pt2 = getelementptr [8 x i32], ptr addrspace(2) @tile, i32 0, i32 %back
  %w = load i32, ptr addrspace(2) %pt2

  %pout = getelementptr i32, ptr addrspace(1) %out, i32 %t
  store i32 %w, ptr addrspace(1) %pout
  ret void
}

attributes #0 = { "gpumin_kernel" }
