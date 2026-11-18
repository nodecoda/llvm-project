; Demo 6: real divergence -- the trip count comes from per-lane data.
;
; Lane t runs its loop `in[t]` times, so the lanes leave the loop at different
; iterations.  The simulator's frame stack keeps the lagging lanes and merges
; them back at the exit block; `--stats` shows splits > 0 and frames_max > 1,
; which is exactly what demo 3 does not show.
;
; out[t] = 0 + 1 + ... + (in[t] - 1)
target triple = "gpumin-unknown-elf"

declare i32 @llvm.gpumin.tid()

define void @divergent(ptr addrspace(1) %in, ptr addrspace(1) %out) #0 {
entry:
  %t = call i32 @llvm.gpumin.tid()
  %pin = getelementptr i32, ptr addrspace(1) %in, i32 %t
  %n = load i32, ptr addrspace(1) %pin
  br label %loop

loop:
  %i = phi i32 [ 0, %entry ], [ %inext, %body ]
  %acc = phi i32 [ 0, %entry ], [ %acc2, %body ]
  %more = icmp ult i32 %i, %n
  br i1 %more, label %body, label %done

body:
  %acc2 = add i32 %acc, %i
  %inext = add i32 %i, 1
  br label %loop

done:
  %pout = getelementptr i32, ptr addrspace(1) %out, i32 %t
  store i32 %acc, ptr addrspace(1) %pout
  ret void
}

attributes #0 = { "gpumin_kernel" }
