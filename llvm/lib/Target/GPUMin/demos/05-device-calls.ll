; Demo 5: device functions, the link register and the private stack.
;
; @quad calls @twice twice, so the kernel exercises call/ret, the stack frame
; that -O1 inserts for the spill around the calls, and the r30/r31 pair
; (stack pointer and link register).
;
; out[t] = in[t] * 4 + 1
target triple = "gpumin-unknown-elf"

declare i32 @llvm.gpumin.tid()

define i32 @twice(i32 %x) {
entry:
  %r = add i32 %x, %x
  ret i32 %r
}

define i32 @quad(i32 %x) {
entry:
  %a = call i32 @twice(i32 %x)
  %b = call i32 @twice(i32 %a)
  ret i32 %b
}

define void @calls(ptr addrspace(1) %in, ptr addrspace(1) %out) #0 {
entry:
  %t = call i32 @llvm.gpumin.tid()
  %pin = getelementptr i32, ptr addrspace(1) %in, i32 %t
  %x = load i32, ptr addrspace(1) %pin
  %q = call i32 @quad(i32 %x)
  %r = add i32 %q, 1
  %pout = getelementptr i32, ptr addrspace(1) %out, i32 %t
  store i32 %r, ptr addrspace(1) %pout
  ret void
}

attributes #0 = { "gpumin_kernel" }
