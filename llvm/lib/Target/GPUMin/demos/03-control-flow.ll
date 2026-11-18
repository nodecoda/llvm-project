; Demo 3: a loop and an if/else whose condition is *uniform*.
;
; The accumulator is the same in every lane (the loop bounds are constants),
; so the branch is taken by the whole warp at once: `--stats` reports
; splits=0, frames_max=1.  Compare with demo 6, where the condition comes
; from per-lane data and the warp really splits.
;
; out[t] = in[t] + (1 + 2 + 3 + 4 + 5), or the bare sum in the other arm.
target triple = "gpumin-unknown-elf"

declare i32 @llvm.gpumin.tid()

define void @control(ptr addrspace(1) %in, ptr addrspace(1) %out) #0 {
entry:
  br label %loop

loop:
  %i = phi i32 [ 1, %entry ], [ %inext, %loop ]
  %acc = phi i32 [ 0, %entry ], [ %acc2, %loop ]
  %acc2 = add i32 %acc, %i
  %inext = add i32 %i, 1
  %done = icmp ugt i32 %inext, 5
  br i1 %done, label %check, label %loop

check:
  %t = call i32 @llvm.gpumin.tid()
  %pin = getelementptr i32, ptr addrspace(1) %in, i32 %t
  %x = load i32, ptr addrspace(1) %pin
  %v = add i32 %acc2, %x
  %big = icmp ugt i32 %acc2, 10        ; 15 > 10, same for every lane
  br i1 %big, label %pos, label %neg

pos:
  %pout = getelementptr i32, ptr addrspace(1) %out, i32 %t
  store i32 %v, ptr addrspace(1) %pout
  br label %end

neg:
  %pout2 = getelementptr i32, ptr addrspace(1) %out, i32 %t
  store i32 %acc2, ptr addrspace(1) %pout2
  br label %end

end:
  ret void
}

attributes #0 = { "gpumin_kernel" }
