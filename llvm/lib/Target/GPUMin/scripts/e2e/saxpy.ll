; Chapter 15 acceptance sample: out[gid] = 3*x[gid] + y[gid].
;
; GMVP has no multiplier, so the constant factor is a shift-add:
;   3*x = (x << 1) + x
; That is not a workaround for the simulator, it is what the ISA offers; the
; chapter uses it to show why "real" GPU ISAs spend area on IMAD.
;
; The launch is 2 blocks x 8 threads with n = 16, so every lane is in range.
target triple = "gpumin-unknown-elf"

declare i32 @llvm.gpumin.args()
declare i32 @llvm.gpumin.tid()
declare i32 @llvm.gpumin.blockid()

define void @saxpy_kernel() #0 {
entry:
  %args = call i32 @llvm.gpumin.args()
  %ap = inttoptr i32 %args to ptr addrspace(1)
  %xp = getelementptr i32, ptr addrspace(1) %ap, i32 0
  %yp = getelementptr i32, ptr addrspace(1) %ap, i32 1
  %op = getelementptr i32, ptr addrspace(1) %ap, i32 2
  %np = getelementptr i32, ptr addrspace(1) %ap, i32 3
  %x0 = load i32, ptr addrspace(1) %xp
  %y0 = load i32, ptr addrspace(1) %yp
  %o0 = load i32, ptr addrspace(1) %op
  %n = load i32, ptr addrspace(1) %np

  %t = call i32 @llvm.gpumin.tid()
  %bid = call i32 @llvm.gpumin.blockid()
  %b8 = shl i32 %bid, 3
  %gid = add i32 %b8, %t
  %in = icmp ult i32 %gid, %n
  br i1 %in, label %body, label %done

body:
  %xbase = inttoptr i32 %x0 to ptr addrspace(1)
  %ybase = inttoptr i32 %y0 to ptr addrspace(1)
  %obase = inttoptr i32 %o0 to ptr addrspace(1)
  %px = getelementptr i32, ptr addrspace(1) %xbase, i32 %gid
  %py = getelementptr i32, ptr addrspace(1) %ybase, i32 %gid
  %po = getelementptr i32, ptr addrspace(1) %obase, i32 %gid
  %x = load i32, ptr addrspace(1) %px
  %y = load i32, ptr addrspace(1) %py
  %x2 = shl i32 %x, 1
  %x3 = add i32 %x2, %x
  %s = add i32 %x3, %y
  store i32 %s, ptr addrspace(1) %po
  br label %done

done:
  ret void
}

attributes #0 = { "gpumin_kernel" }
