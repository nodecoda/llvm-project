; RUN: llc -march=gpumin -O1 -verify-machineinstrs < %s | FileCheck %s

; Chapter 07: call/ret, the private frame, lr and the ret/exit split.

target triple = "gpumin-unknown-elf"

define i32 @add1(i32 %x) {
; CHECK-LABEL: add1:
; CHECK:       addi r4, r4, 1
; CHECK:       ret
  %r = add i32 %x, 1
  ret i32 %r
}

define i32 @fib(i32 %n) {
; CHECK-LABEL: fib:
; CHECK:       addi r30, r30, -{{[0-9]+}}
; CHECK:       st.local.b32 r31, {{[0-9]+}}(r30)
; CHECK:       call fib
; CHECK:       call fib
; CHECK:       ld.local.b32 r31, {{[0-9]+}}(r30)
; CHECK:       ret
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

define i32 @kernel(ptr addrspace(1) %p) #0 {
; CHECK-LABEL: kernel:
; CHECK:       call fib
; CHECK:       exit
  %x = load volatile i32, ptr addrspace(1) %p
  %y = call i32 @fib(i32 %x)
  ret i32 %y
}

attributes #0 = { "gpumin_kernel" }
