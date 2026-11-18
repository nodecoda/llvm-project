; Chapter 12 runtime sample: recursion, the private stack and lr.
; fib(6 + warpid) is small enough to run without a step-limit surprise, and it
; is uniform inside a warp: recursion needs the private stack, not divergence
; (which arrives in chapter 14).
target triple = "gpumin-unknown-elf"

declare i32 @llvm.gpumin.tid()
declare i32 @llvm.gpumin.warpid()

define i32 @fib(i32 %n) {
  %c = icmp slt i32 %n, 2
  br i1 %c, label %base, label %rec
base:
  ret i32 %n
rec:
  %n1 = sub i32 %n, 1
  %n2 = sub i32 %n, 2
  %a = call i32 @fib(i32 %n1)
  %b = call i32 @fib(i32 %n2)
  %r = add i32 %a, %b
  ret i32 %r
}

define void @fib_kernel(ptr addrspace(1) %out) #0 {
entry:
  %t = call i32 @llvm.gpumin.tid()
  %w = call i32 @llvm.gpumin.warpid()
  %n = add i32 %w, 6
  %v = call i32 @fib(i32 %n)
  %p = getelementptr i32, ptr addrspace(1) %out, i32 %t
  store i32 %v, ptr addrspace(1) %p
  ret void
}

attributes #0 = { "gpumin_kernel" }
