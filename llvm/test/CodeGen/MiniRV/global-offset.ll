; RUN: llc -mtriple=minirv64 < %s | FileCheck %s
; RUN: llc -mtriple=minirv64 -filetype=obj < %s -o %t.o
; RUN: llvm-objdump -dr --triple=minirv64 %t.o | FileCheck %s --check-prefix=OBJ

; A global address can carry a constant offset: expanding a 16-byte memcpy
; loads the two halves of the constant separately.  The offset has to become
; part of the relocation (sym+8), not be dropped or mistaken for an assembler
; specifier.

; CHECK-LABEL: f:
; CHECK:         auipc   {{x[0-9]+}}, .Lc+8
; CHECK:         addi    {{x[0-9]+}}, {{x[0-9]+}}, .Lpcrel_hi
@c = private constant [16 x i8] c"\01\02\03\04\05\06\07\08\09\0A\0B\0C\0D\0E\0F\10"

declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)

define void @f(ptr %dst) {
  call void @llvm.memcpy.p0.p0.i64(ptr align 8 %dst, ptr align 8 @c, i64 16, i1 false)
  ret void
}

; OBJ-LABEL: <f>:
; OBJ:         R_RISCV_PCREL_HI20  .Lc+0x8
