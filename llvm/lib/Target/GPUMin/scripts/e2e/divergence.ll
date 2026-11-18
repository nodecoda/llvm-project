; Chapter 14 runtime sample: lanes of one warp take different paths.
;
; `diverge` splits a warp in half at an if/else, reads `activemask` inside each
; arm and once more at the join, and stores a four-word record per lane at
; out[tid*4]:
;   [tid, mask inside the arm, path value, mask after the join]
;
; `triangle` makes every lane run a different number of iterations, so the
; lanes reach the loop exit at different times.  The result, out[tid] =
; 0 + 1 + ... + (tid-1), only comes out right if the exit block is entered
; independently by each lane and merged on arrival.
target triple = "gpumin-unknown-elf"

declare i32 @llvm.gpumin.tid()
declare i32 @llvm.gpumin.activemask()

define void @diverge(ptr addrspace(1) %out) #0 {
entry:
  %t = call i32 @llvm.gpumin.tid()
  %low = icmp slt i32 %t, 4
  br i1 %low, label %lowarm, label %higharm

lowarm:
  %vlow = shl i32 %t, 1
  %mlow = call i32 @llvm.gpumin.activemask()
  br label %join

higharm:
  %mhigh = call i32 @llvm.gpumin.activemask()
  %vhigh = add i32 %t, 100
  br label %join

join:
  %m = phi i32 [ %mlow, %lowarm ], [ %mhigh, %higharm ]
  %v = phi i32 [ %vlow, %lowarm ], [ %vhigh, %higharm ]
  %mjoin = call i32 @llvm.gpumin.activemask()
  %base = shl i32 %t, 2
  %p0 = getelementptr i32, ptr addrspace(1) %out, i32 %base
  store i32 %t, ptr addrspace(1) %p0
  %p1 = getelementptr i32, ptr addrspace(1) %p0, i32 1
  store i32 %m, ptr addrspace(1) %p1
  %p2 = getelementptr i32, ptr addrspace(1) %p0, i32 2
  store i32 %v, ptr addrspace(1) %p2
  %p3 = getelementptr i32, ptr addrspace(1) %p0, i32 3
  store i32 %mjoin, ptr addrspace(1) %p3
  ret void
}

define internal i32 @sum_to(i32 %n) {
entry:
  br label %head
head:
  %i = phi i32 [ 0, %entry ], [ %next, %body ]
  %sum = phi i32 [ 0, %entry ], [ %add, %body ]
  %c = icmp ult i32 %i, %n
  br i1 %c, label %body, label %done
body:
  %add = add i32 %sum, %i
  %next = add i32 %i, 1
  br label %head
done:
  ret i32 %sum
}

define void @triangle(ptr addrspace(1) %out) #0 {
entry:
  %t = call i32 @llvm.gpumin.tid()
  %v = call i32 @sum_to(i32 %t)
  %p = getelementptr i32, ptr addrspace(1) %out, i32 %t
  store i32 %v, ptr addrspace(1) %p
  ret void
}

attributes #0 = { "gpumin_kernel" }
