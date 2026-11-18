; RUN: llc -march=gpumin -O1 -verify-machineinstrs < %s | FileCheck %s

; Chapter 16: address spaces.  The memory opcode's space field has to follow
; the IR pointer's address space, and a shared global has no object-file
; symbol: its address is `sr.shared` plus a static window offset.

@tile = internal addrspace(2) global [8 x i32] zeroinitializer, align 4

define void @st_shared(ptr addrspace(2) %p, i32 %v) {
; CHECK-LABEL: st_shared:
; CHECK:       st.shared.b32
  store i32 %v, ptr addrspace(2) %p
  ret void
}

define i32 @ld_shared(ptr addrspace(2) %p) {
; CHECK-LABEL: ld_shared:
; CHECK:       ld.shared.b32
  %v = load i32, ptr addrspace(2) %p
  ret i32 %v
}

define i32 @ld_shared_u8(ptr addrspace(2) %p) {
; CHECK-LABEL: ld_shared_u8:
; CHECK:       ld.shared.u8
  %v = load i8, ptr addrspace(2) %p
  %r = zext i8 %v to i32
  ret i32 %r
}

define void @st_shared_b8(ptr addrspace(2) %p, i32 %v) {
; CHECK-LABEL: st_shared_b8:
; CHECK:       st.shared.b8
  %t = trunc i32 %v to i8
  store i8 %t, ptr addrspace(2) %p
  ret void
}

; The static tile lowers to `sr.shared` (offset 0 here) and the element index
; is folded into the displacement where it fits.
define i32 @tile_read(i32 %i) {
; CHECK-LABEL: tile_read:
; CHECK:       sr.shared
; CHECK:       ld.shared.b32
  %p = getelementptr [8 x i32], ptr addrspace(2) @tile, i32 0, i32 %i
  %v = load i32, ptr addrspace(2) %p
  ret i32 %v
}

; A fixed element is a pure offset from the window base.
define i32 @tile_last() {
; CHECK-LABEL: tile_last:
; CHECK:       sr.shared
; CHECK:       addi {{r[0-9]+}}, {{r[0-9]+}}, 28
; CHECK:       ld.shared.b32
  %p = getelementptr [8 x i32], ptr addrspace(2) @tile, i32 0, i32 7
  %v = load i32, ptr addrspace(2) %p
  ret i32 %v
}

; Global stays on the tablegen patterns: the selector only takes over the
; other address spaces.
define i32 @ld_global(ptr addrspace(1) %p) {
; CHECK-LABEL: ld_global:
; CHECK:       ld.global.b32
  %v = load i32, ptr addrspace(1) %p
  ret i32 %v
}

; The static allocation walks the module in declaration order.  `@tile` ends at
; byte 32, `@flag` needs one byte and no padding, and `@tile2` is 16-byte
; aligned, so it starts at 48 rather than 33.
@flag = internal addrspace(2) global i8 zeroinitializer, align 1
@tile2 = internal addrspace(2) global [4 x i32] zeroinitializer, align 16

define i32 @tile2_first() {
; CHECK-LABEL: tile2_first:
; CHECK:       sr.shared
; CHECK:       addi {{r[0-9]+}}, {{r[0-9]+}}, 48
; CHECK:       ld.shared.b32
  %p = getelementptr [4 x i32], ptr addrspace(2) @tile2, i32 0, i32 0
  %v = load i32, ptr addrspace(2) %p
  ret i32 %v
}

define i32 @flag_byte() {
; CHECK-LABEL: flag_byte:
; CHECK:       sr.shared
; CHECK:       addi {{r[0-9]+}}, {{r[0-9]+}}, 32
; CHECK:       ld.shared.u8
  %v = load i8, ptr addrspace(2) @flag
  %r = zext i8 %v to i32
  ret i32 %r
}
