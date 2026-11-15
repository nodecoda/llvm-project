; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O1 < %s | FileCheck %s
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O0 < %s -o /dev/null
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O1 < %s -filetype=obj -o %t.o
; RUN: llvm-readelf -r %t.o | FileCheck %s --check-prefix=RELOC
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O1 < %s -o - | llvm-mc -triple=riscv32 -filetype=obj -o /dev/null

; Chapter 9: the integer types RV32I has no registers for.
;
; The type legalizer promotes i1/i8/i16 to i32, so a narrow value always
; lives in a full GPR and only *memory* keeps its width.  i64 is split into
; two i32 halves, which is why a 64-bit shift becomes a libcall.

; A signed byte load sign-extends, an unsigned one zero-extends.
define i32 @load_i8(ptr %p) nounwind {
  %v = load i8, ptr %p, align 1
  %r = sext i8 %v to i32
  ret i32 %r
}
; CHECK-LABEL: load_i8:
; CHECK:         lb a0, 0(a0)
; CHECK:         jalr zero, 0(ra)

define i32 @load_u8(ptr %p) nounwind {
  %v = load i8, ptr %p, align 1
  %r = zext i8 %v to i32
  ret i32 %r
}
; CHECK-LABEL: load_u8:
; CHECK:         lbu a0, 0(a0)

define i32 @load_i16(ptr %p) nounwind {
  %v = load i16, ptr %p, align 2
  %r = sext i16 %v to i32
  ret i32 %r
}
; CHECK-LABEL: load_i16:
; CHECK:         lh a0, 0(a0)

define i32 @load_u16(ptr %p) nounwind {
  %v = load i16, ptr %p, align 2
  %r = zext i16 %v to i32
  ret i32 %r
}
; CHECK-LABEL: load_u16:
; CHECK:         lhu a0, 0(a0)

; Stores truncate the register to the low byte/halfword; the fold with the
; GEP also exercises the `address + offset` pattern.
define void @store_narrow(ptr %p, i32 %v) nounwind {
  %c = trunc i32 %v to i8
  store i8 %c, ptr %p, align 1
  %s = trunc i32 %v to i16
  %q = getelementptr i16, ptr %p, i32 1
  store i16 %s, ptr %q, align 2
  ret void
}
; CHECK-LABEL: store_narrow:
; CHECK-DAG:     sb a1, 0(a0)
; CHECK-DAG:     sh a1, 2(a0)

; `sext i16 (trunc i32 %x)` becomes sign_extend_inreg, which RV32I performs
; with a shift pair.  The legalizer is asked about the *inner* type here.
define i32 @sext_inreg(i32 %x) nounwind {
  %t = trunc i32 %x to i16
  %r = sext i16 %t to i32
  ret i32 %r
}
; CHECK-LABEL: sext_inreg:
; CHECK:         slli a0, a0, 16
; CHECK:         srai a0, a0, 16

; 64-bit addition is two 32-bit adds plus a carry produced by `sltu`.
define i64 @add_i64(i64 %a, i64 %b) nounwind {
  %r = add i64 %a, %b
  ret i64 %r
}
; CHECK-LABEL: add_i64:
; CHECK-DAG:     add
; CHECK-DAG:     sltu
; CHECK:         addi a0, {{a[0-9]+}}, 0

; A 64-bit comparison is not two comparisons and an `and`: the low halves
; only matter when the high halves are equal.
define i32 @lt_i64(i64 %a, i64 %b) nounwind {
  %c = icmp slt i64 %a, %b
  %r = zext i1 %c to i32
  ret i32 %r
}
; CHECK-LABEL: lt_i64:
; CHECK:         slt
; CHECK:         sltu
; CHECK:         sltiu

; A variable 64-bit shift has no single instruction, so the legalizer expands
; it into a call to the compiler runtime, which chapter 8 already supports.
define i64 @shl_i64(i64 %a, i32 %n) nounwind {
  %s = zext i32 %n to i64
  %r = shl i64 %a, %s
  ret i64 %r
}
; CHECK-LABEL: shl_i64:
; CHECK:         lui t0, %hi(__ashldi3)
; CHECK:         addi t0, t0, %lo(__ashldi3)
; CHECK:         jalr ra, 0(t0)
; CHECK:         lw ra, 12(sp)

; RELOC:         R_RISCV_HI20 {{.*}}__ashldi3
; RELOC:         R_RISCV_LO12_I {{.*}}__ashldi3

; A 64-bit load is two word loads at the same base address.
define i64 @load_i64(ptr %p) nounwind {
  %v = load i64, ptr %p, align 8
  ret i64 %v
}
; CHECK-LABEL: load_i64:
; CHECK:         lw a2, 0(a0)
; CHECK:         lw a1, 4(a0)

; `_Bool` is a byte in memory: the memory type is promoted to i8 so the
; normal narrow load/store patterns apply.  Storing one masks to the low bit.
define i32 @load_bool(ptr %p) nounwind {
  %v = load i1, ptr %p, align 1
  %r = zext i1 %v to i32
  ret i32 %r
}
; CHECK-LABEL: load_bool:
; CHECK:         lbu a0, 0(a0)

define void @store_bool(ptr %p, i1 %v) nounwind {
  store i1 %v, ptr %p, align 1
  ret void
}
; CHECK-LABEL: store_bool:
; CHECK:         andi a1, a1, 1
; CHECK:         sb a1, 0(a0)
