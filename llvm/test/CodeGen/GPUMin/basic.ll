; RUN: llc -march=gpumin -O1 -verify-machineinstrs < %s | FileCheck %s
; RUN: llc -march=gpumin -O1 -filetype=obj < %s -o %t.o
; RUN: llvm-objdump -d %t.o | FileCheck %s --check-prefix=OBJ

; Chapter 03: the smallest function, the return contract and the kernel exit.

target triple = "gpumin-unknown-elf"

define i32 @pick_second(i32 %a, i32 %b) {
; CHECK-LABEL: pick_second:
; CHECK:       mov r4, r5
; CHECK:       ret
  ret i32 %b
}

define i32 @forty_two() {
; CHECK-LABEL: forty_two:
; CHECK:       movi r4, 42
; CHECK:       ret
  ret i32 42
}

define void @k() #0 {
; CHECK-LABEL: k:
; CHECK:       exit
  ret void
}

attributes #0 = { "gpumin_kernel" }

; OBJ-LABEL: <pick_second>:
; OBJ:         mov r4, r5
; OBJ:         ret
; OBJ-LABEL: <forty_two>:
; OBJ:         movi r4, 42
; OBJ:         ret
; OBJ-LABEL: <k>:
; OBJ:         exit
