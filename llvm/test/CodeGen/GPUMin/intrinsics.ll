; RUN: llc -march=gpumin -verify-machineinstrs < %s | FileCheck %s

; Chapter 09: the seven special-register intrinsics each select one SYS
; instruction.  Nothing here is a call: the IR node becomes `sr.<name>`.

target triple = "gpumin-unknown-elf"

declare i32 @llvm.gpumin.laneid()
declare i32 @llvm.gpumin.warpid()
declare i32 @llvm.gpumin.blockid()
declare i32 @llvm.gpumin.tid()
declare i32 @llvm.gpumin.ntid()
declare i32 @llvm.gpumin.nwarps()
declare i32 @llvm.gpumin.activemask()
declare i32 @llvm.gpumin.args()

define i32 @laneid() {
; CHECK-LABEL: laneid:
; CHECK:       sr.laneid r4
; CHECK:       ret
  %v = call i32 @llvm.gpumin.laneid()
  ret i32 %v
}

define i32 @warpid() {
; CHECK-LABEL: warpid:
; CHECK:       sr.warpid r4
; CHECK:       ret
  %v = call i32 @llvm.gpumin.warpid()
  ret i32 %v
}

define i32 @blockid() {
; CHECK-LABEL: blockid:
; CHECK:       sr.blockid r4
; CHECK:       ret
  %v = call i32 @llvm.gpumin.blockid()
  ret i32 %v
}

define i32 @tid() {
; CHECK-LABEL: tid:
; CHECK:       sr.tid r4
; CHECK:       ret
  %v = call i32 @llvm.gpumin.tid()
  ret i32 %v
}

define i32 @ntid() {
; CHECK-LABEL: ntid:
; CHECK:       sr.ntid r4
; CHECK:       ret
  %v = call i32 @llvm.gpumin.ntid()
  ret i32 %v
}

define i32 @nwarps() {
; CHECK-LABEL: nwarps:
; CHECK:       sr.nwarps r4
; CHECK:       ret
  %v = call i32 @llvm.gpumin.nwarps()
  ret i32 %v
}

define i32 @activemask() {
; CHECK-LABEL: activemask:
; CHECK:       sr.activemask r4
; CHECK:       ret
  %v = call i32 @llvm.gpumin.activemask()
  ret i32 %v
}

; A kernel reads the same state and ends in `exit`, not `ret`.
define void @kernel_ids() #0 {
; CHECK-LABEL: kernel_ids:
; The scheduler may interleave the seven reads with the adds, so match the
; set, not an order.
; CHECK-DAG:   sr.laneid
; CHECK-DAG:   sr.warpid
; CHECK-DAG:   sr.blockid
; CHECK-DAG:   sr.tid
; CHECK-DAG:   sr.ntid
; CHECK-DAG:   sr.nwarps
; CHECK-DAG:   sr.activemask
; CHECK:       exit
  %lane = call i32 @llvm.gpumin.laneid()
  %warp = call i32 @llvm.gpumin.warpid()
  %block = call i32 @llvm.gpumin.blockid()
  %tid = call i32 @llvm.gpumin.tid()
  %ntid = call i32 @llvm.gpumin.ntid()
  %nwarps = call i32 @llvm.gpumin.nwarps()
  %mask = call i32 @llvm.gpumin.activemask()
  %a = add i32 %lane, %warp
  %b = add i32 %block, %tid
  %c = add i32 %ntid, %nwarps
  %d = add i32 %a, %b
  %e = add i32 %c, %mask
  %f = add i32 %d, %e
  store i32 %f, ptr addrspace(1) null
  ret void
}

attributes #0 = { "gpumin_kernel" }

define i32 @args() {
; CHECK-LABEL: args:
; CHECK:       sr.args r4
; CHECK:       ret
  %v = call i32 @llvm.gpumin.args()
  ret i32 %v
}
