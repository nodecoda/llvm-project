; Chapter 15 acceptance sample: per-lane strided sum.
;
; out[tid] = in[tid] + in[tid+stride] + ... while the index is below n.
; The block reduction that needs lanes to talk to each other is chapter 16/17
; (shared memory + barrier); this program is a single lane's share of it, and
; it is the first sample where lanes run different numbers of iterations:
; with n = 60 and stride = 8, lanes 0-3 loop eight times and lanes 4-7 seven.
target triple = "gpumin-unknown-elf"

declare i32 @llvm.gpumin.args()
declare i32 @llvm.gpumin.tid()

define void @reduce_kernel() #0 {
entry:
  %args = call i32 @llvm.gpumin.args()
  %ap = inttoptr i32 %args to ptr addrspace(1)
  %ip = getelementptr i32, ptr addrspace(1) %ap, i32 0
  %op = getelementptr i32, ptr addrspace(1) %ap, i32 1
  %np = getelementptr i32, ptr addrspace(1) %ap, i32 2
  %sp = getelementptr i32, ptr addrspace(1) %ap, i32 3
  %in = load i32, ptr addrspace(1) %ip
  %out = load i32, ptr addrspace(1) %op
  %n = load i32, ptr addrspace(1) %np
  %stride = load i32, ptr addrspace(1) %sp

  %t = call i32 @llvm.gpumin.tid()
  %ibase = inttoptr i32 %in to ptr addrspace(1)
  %obase = inttoptr i32 %out to ptr addrspace(1)
  br label %head

head:
  %i = phi i32 [ %t, %entry ], [ %next, %body ]
  %acc = phi i32 [ 0, %entry ], [ %sum, %body ]
  %c = icmp ult i32 %i, %n
  br i1 %c, label %body, label %done

body:
  %pi = getelementptr i32, ptr addrspace(1) %ibase, i32 %i
  %v = load i32, ptr addrspace(1) %pi
  %sum = add i32 %acc, %v
  %next = add i32 %i, %stride
  br label %head

done:
  %po = getelementptr i32, ptr addrspace(1) %obase, i32 %t
  store i32 %acc, ptr addrspace(1) %po
  ret void
}

attributes #0 = { "gpumin_kernel" }
