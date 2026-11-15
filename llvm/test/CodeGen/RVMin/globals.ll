; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O1 < %s | FileCheck %s
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O1 < %s -filetype=obj -o %t.o
; RUN: llvm-readelf -r %t.o | FileCheck %s --check-prefix=RELOC
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O1 < %s -o - | llvm-mc -triple=riscv32 -filetype=obj -o /dev/null

; Chapter 7: global addresses, static data and relocations.

@x = global i32 42, align 4
@arr = global [4 x i32] [i32 1, i32 2, i32 3, i32 4], align 4
@p = global ptr @x, align 4
@msg = private unnamed_addr constant [6 x i8] c"hello\00", align 1

define i32 @get_x() nounwind {
  %v = load i32, ptr @x, align 4
  ret i32 %v
}
; CHECK-LABEL: get_x:
; CHECK:         lui a0, %hi(x)
; CHECK:         addi a0, a0, %lo(x)
; CHECK:         lw a0, 0(a0)

define void @set_x(i32 %v) nounwind {
  store i32 %v, ptr @x, align 4
  ret void
}
; CHECK-LABEL: set_x:
; CHECK:         lui a1, %hi(x)
; CHECK:         addi a1, a1, %lo(x)
; CHECK:         sw a0, 0(a1)

; A variable index needs the address in a register; the %hi/%lo pair is
; materialized once and then added to the scaled index.
define i32 @sum_arr(i32 %i) nounwind {
  %p = getelementptr [4 x i32], ptr @arr, i32 0, i32 %i
  %v = load i32, ptr %p, align 4
  ret i32 %v
}
; CHECK-LABEL: sum_arr:
; CHECK:         slli a0, a0, 2
; CHECK:         lui a1, %hi(arr)
; CHECK:         addi a1, a1, %lo(arr)
; CHECK:         add a0, a1, a0
; CHECK:         lw a0, 0(a0)

; Static data is emitted by the target-independent asm printer once the MC
; layer can print directives.
; CHECK:         x:
; CHECK:         .word 42
; CHECK:         arr:
; CHECK:         .word 1
; CHECK:         .word 2
; CHECK:         .word 3
; CHECK:         .word 4
; CHECK:         .asciz "hello"

; Code relocations: the global address halves.
; RELOC: R_RISCV_HI20{{.*}}x
; RELOC: R_RISCV_LO12_I{{.*}}x

; A pointer initializer in .data needs an absolute 32-bit relocation.
; RELOC: R_RISCV_32 {{.*}}x
