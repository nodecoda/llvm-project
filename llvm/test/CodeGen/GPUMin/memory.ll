; RUN: llc -march=gpumin -O1 -verify-machineinstrs < %s | FileCheck %s

; Chapter 06: global memory and the 11-bit byte displacement.

target triple = "gpumin-unknown-elf"

define i32 @roundtrip(ptr addrspace(1) %p, i32 %v) {
entry:
  store volatile i32 %v, ptr addrspace(1) %p
  %x = load volatile i32, ptr addrspace(1) %p
  ret i32 %x
; CHECK-LABEL: roundtrip:
; CHECK:       st.global.b32 r5, 0(r4)
; CHECK:       ld.global.b32 r4, 0(r4)
}

define i32 @folded(ptr addrspace(1) %p) {
entry:
  %q = getelementptr i32, ptr addrspace(1) %p, i32 2
  %x = load volatile i32, ptr addrspace(1) %q
  ret i32 %x
; CHECK-LABEL: folded:
; CHECK:       ld.global.b32 r4, 8(r4)
}

define i32 @indexed(ptr addrspace(1) %p, i32 %i) {
entry:
  %q = getelementptr i32, ptr addrspace(1) %p, i32 %i
  %x = load volatile i32, ptr addrspace(1) %q
  ret i32 %x
; CHECK-LABEL: indexed:
; CHECK:       shl r{{[0-9]+}}, r5, r{{[0-9]+}}
; CHECK:       ld.global.b32 r4, 0(r{{[0-9]+}})
}
