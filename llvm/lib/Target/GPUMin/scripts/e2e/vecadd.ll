; Static end-to-end input for the chapter-10 gate.  The simulator (chapter 12)
; will run the same shape with real launch state; here the goal is that a
; kernel with memory, a special read, a branch and a store survives the whole
; IR -> object path and reassembles identically.
target triple = "gpumin-unknown-elf"

declare i32 @llvm.gpumin.tid()

define void @vecadd(ptr addrspace(1) %a, ptr addrspace(1) %b,
                    ptr addrspace(1) %out, i32 %n) #0 {
entry:
  %t = call i32 @llvm.gpumin.tid()
  %in = icmp ult i32 %t, %n
  br i1 %in, label %body, label %done
body:
  %pa = getelementptr i32, ptr addrspace(1) %a, i32 %t
  %pb = getelementptr i32, ptr addrspace(1) %b, i32 %t
  %po = getelementptr i32, ptr addrspace(1) %out, i32 %t
  %x = load i32, ptr addrspace(1) %pa
  %y = load i32, ptr addrspace(1) %pb
  %s = add i32 %x, %y
  store i32 %s, ptr addrspace(1) %po
  br label %done
done:
  ret void
}

attributes #0 = { "gpumin_kernel" }
