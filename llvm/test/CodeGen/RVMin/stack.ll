; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O1 < %s | FileCheck %s
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O0 < %s | FileCheck %s --check-prefix=CHECK-O0
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O1 < %s -o - | llvm-mc -triple=riscv32 -filetype=obj -o /dev/null

; Chapter 6: stack objects, frame-index elimination, spills and callee-saved
; registers.  The volatile accesses keep the allocas alive at -O1 so that the
; stack code is actually exercised.

define i32 @roundtrip(i32 %a) nounwind {
entry:
  %slot = alloca i32, align 4
  store volatile i32 %a, ptr %slot, align 4
  %v = load volatile i32, ptr %slot, align 4
  ret i32 %v
}
; CHECK-LABEL: roundtrip:
; CHECK:         addi sp, sp, -16
; CHECK:         sw a0, 12(sp)
; CHECK:         lw a0, 12(sp)
; CHECK:         addi sp, sp, 16
; CHECK:         jalr zero, 0(ra)
; CHECK-O0-LABEL: roundtrip:
; CHECK-O0:       addi sp, sp, -16
; CHECK-O0:       sw a0, {{[0-9]+}}(sp)
; CHECK-O0:       lw a0, {{[0-9]+}}(sp)
; CHECK-O0:       addi sp, sp, 16

define i32 @two(i32 %a, i32 %b) nounwind {
entry:
  %x = alloca i32, align 4
  %y = alloca i32, align 4
  store volatile i32 %a, ptr %x, align 4
  store volatile i32 %b, ptr %y, align 4
  %p = load volatile i32, ptr %x, align 4
  %q = load volatile i32, ptr %y, align 4
  %r = add i32 %p, %q
  ret i32 %r
}
; CHECK-LABEL: two:
; CHECK:         sw a0, 12(sp)
; CHECK:         sw a1, 8(sp)
; CHECK:         lw a0, 12(sp)
; CHECK:         lw a1, 8(sp)
; CHECK:         add a0, a0, a1

; A constant index is folded into the load/store immediate: index 2 of a
; 4-element array is 8 bytes from the base of the object.
define i32 @array(i32 %a) nounwind {
entry:
  %arr = alloca [4 x i32], align 4
  %p = getelementptr [4 x i32], ptr %arr, i32 0, i32 2
  store volatile i32 %a, ptr %p, align 4
  %v = load volatile i32, ptr %p, align 4
  ret i32 %v
}
; CHECK-LABEL: array:
; CHECK:         addi sp, sp, -16
; CHECK:         sw a0, 8(sp)
; CHECK:         lw a0, 8(sp)

; A variable index cannot be folded into the immediate: the address is
; materialized with `addi rd, sp, offset`, then scaled and added.
define i32 @dynamic(i32 %i, i32 %a) nounwind {
entry:
  %arr = alloca [8 x i32], align 4
  %p = getelementptr [8 x i32], ptr %arr, i32 0, i32 %i
  store volatile i32 %a, ptr %p, align 4
  %v = load volatile i32, ptr %p, align 4
  ret i32 %v
}
; CHECK-LABEL: dynamic:
; CHECK:         slli a0, a0, 2
; CHECK:         addi a2, sp, 0
; CHECK:         add a0, a2, a0
; CHECK:         sw a1, 0(a0)
; CHECK:         lw a0, 0(a0)

; Frames larger than ADDI's 12-bit range need LUI+ADDI in the epilogue.
define i32 @big(i32 %a) nounwind {
entry:
  %p = alloca [512 x i32], align 4
  %q = getelementptr [512 x i32], ptr %p, i32 0, i32 0
  store volatile i32 %a, ptr %q, align 4
  %v = load volatile i32, ptr %q, align 4
  ret i32 %v
}
; CHECK-LABEL: big:
; CHECK:         addi sp, sp, -2048
; CHECK:         sw a0, 0(sp)
; CHECK:         lui t0, 1
; CHECK:         addi t0, t0, -2048
; CHECK:         add sp, sp, t0

define i32 @stress(i32 %a) nounwind {
entry:
  %p = alloca [20 x i32], align 4
  %s0 = getelementptr [20 x i32], ptr %p, i32 0, i32 0
  store volatile i32 %a, ptr %s0, align 4
  %s1 = getelementptr [20 x i32], ptr %p, i32 0, i32 1
  store volatile i32 %a, ptr %s1, align 4
  %s2 = getelementptr [20 x i32], ptr %p, i32 0, i32 2
  store volatile i32 %a, ptr %s2, align 4
  %s3 = getelementptr [20 x i32], ptr %p, i32 0, i32 3
  store volatile i32 %a, ptr %s3, align 4
  %s4 = getelementptr [20 x i32], ptr %p, i32 0, i32 4
  store volatile i32 %a, ptr %s4, align 4
  %s5 = getelementptr [20 x i32], ptr %p, i32 0, i32 5
  store volatile i32 %a, ptr %s5, align 4
  %s6 = getelementptr [20 x i32], ptr %p, i32 0, i32 6
  store volatile i32 %a, ptr %s6, align 4
  %s7 = getelementptr [20 x i32], ptr %p, i32 0, i32 7
  store volatile i32 %a, ptr %s7, align 4
  %s8 = getelementptr [20 x i32], ptr %p, i32 0, i32 8
  store volatile i32 %a, ptr %s8, align 4
  %s9 = getelementptr [20 x i32], ptr %p, i32 0, i32 9
  store volatile i32 %a, ptr %s9, align 4
  %s10 = getelementptr [20 x i32], ptr %p, i32 0, i32 10
  store volatile i32 %a, ptr %s10, align 4
  %s11 = getelementptr [20 x i32], ptr %p, i32 0, i32 11
  store volatile i32 %a, ptr %s11, align 4
  %s12 = getelementptr [20 x i32], ptr %p, i32 0, i32 12
  store volatile i32 %a, ptr %s12, align 4
  %s13 = getelementptr [20 x i32], ptr %p, i32 0, i32 13
  store volatile i32 %a, ptr %s13, align 4
  %s14 = getelementptr [20 x i32], ptr %p, i32 0, i32 14
  store volatile i32 %a, ptr %s14, align 4
  %s15 = getelementptr [20 x i32], ptr %p, i32 0, i32 15
  store volatile i32 %a, ptr %s15, align 4
  %s16 = getelementptr [20 x i32], ptr %p, i32 0, i32 16
  store volatile i32 %a, ptr %s16, align 4
  %s17 = getelementptr [20 x i32], ptr %p, i32 0, i32 17
  store volatile i32 %a, ptr %s17, align 4
  %s18 = getelementptr [20 x i32], ptr %p, i32 0, i32 18
  store volatile i32 %a, ptr %s18, align 4
  %s19 = getelementptr [20 x i32], ptr %p, i32 0, i32 19
  store volatile i32 %a, ptr %s19, align 4
  %s0b = getelementptr [20 x i32], ptr %p, i32 0, i32 0
  %v0 = load volatile i32, ptr %s0b, align 4
  %s1b = getelementptr [20 x i32], ptr %p, i32 0, i32 1
  %v1 = load volatile i32, ptr %s1b, align 4
  %s2b = getelementptr [20 x i32], ptr %p, i32 0, i32 2
  %v2 = load volatile i32, ptr %s2b, align 4
  %s3b = getelementptr [20 x i32], ptr %p, i32 0, i32 3
  %v3 = load volatile i32, ptr %s3b, align 4
  %s4b = getelementptr [20 x i32], ptr %p, i32 0, i32 4
  %v4 = load volatile i32, ptr %s4b, align 4
  %s5b = getelementptr [20 x i32], ptr %p, i32 0, i32 5
  %v5 = load volatile i32, ptr %s5b, align 4
  %s6b = getelementptr [20 x i32], ptr %p, i32 0, i32 6
  %v6 = load volatile i32, ptr %s6b, align 4
  %s7b = getelementptr [20 x i32], ptr %p, i32 0, i32 7
  %v7 = load volatile i32, ptr %s7b, align 4
  %s8b = getelementptr [20 x i32], ptr %p, i32 0, i32 8
  %v8 = load volatile i32, ptr %s8b, align 4
  %s9b = getelementptr [20 x i32], ptr %p, i32 0, i32 9
  %v9 = load volatile i32, ptr %s9b, align 4
  %s10b = getelementptr [20 x i32], ptr %p, i32 0, i32 10
  %v10 = load volatile i32, ptr %s10b, align 4
  %s11b = getelementptr [20 x i32], ptr %p, i32 0, i32 11
  %v11 = load volatile i32, ptr %s11b, align 4
  %s12b = getelementptr [20 x i32], ptr %p, i32 0, i32 12
  %v12 = load volatile i32, ptr %s12b, align 4
  %s13b = getelementptr [20 x i32], ptr %p, i32 0, i32 13
  %v13 = load volatile i32, ptr %s13b, align 4
  %s14b = getelementptr [20 x i32], ptr %p, i32 0, i32 14
  %v14 = load volatile i32, ptr %s14b, align 4
  %s15b = getelementptr [20 x i32], ptr %p, i32 0, i32 15
  %v15 = load volatile i32, ptr %s15b, align 4
  %s16b = getelementptr [20 x i32], ptr %p, i32 0, i32 16
  %v16 = load volatile i32, ptr %s16b, align 4
  %s17b = getelementptr [20 x i32], ptr %p, i32 0, i32 17
  %v17 = load volatile i32, ptr %s17b, align 4
  %s18b = getelementptr [20 x i32], ptr %p, i32 0, i32 18
  %v18 = load volatile i32, ptr %s18b, align 4
  %s19b = getelementptr [20 x i32], ptr %p, i32 0, i32 19
  %v19 = load volatile i32, ptr %s19b, align 4
  %t1 = add i32 %v0, %v1
  %t2 = add i32 %t1, %v2
  %t3 = add i32 %t2, %v3
  %t4 = add i32 %t3, %v4
  %t5 = add i32 %t4, %v5
  %t6 = add i32 %t5, %v6
  %t7 = add i32 %t6, %v7
  %t8 = add i32 %t7, %v8
  %t9 = add i32 %t8, %v9
  %t10 = add i32 %t9, %v10
  %t11 = add i32 %t10, %v11
  %t12 = add i32 %t11, %v12
  %t13 = add i32 %t12, %v13
  %t14 = add i32 %t13, %v14
  %t15 = add i32 %t14, %v15
  %t16 = add i32 %t15, %v16
  %t17 = add i32 %t16, %v17
  %t18 = add i32 %t17, %v18
  %t19 = add i32 %t18, %v19
  ret i32 %t19
}
; With more live values than a/t registers, the allocator uses s0/s1 and the
; prologue spills them.  -O0 additionally spills temporaries, exercising
; storeRegToStackSlot/loadRegFromStackSlot.
; CHECK-O0-LABEL: stress:
; CHECK-O0:       addi sp, sp, -{{[0-9]+}}
; CHECK-O0:       sw s0, {{[0-9]+}}(sp)
; CHECK-O0:       sw s1, {{[0-9]+}}(sp)
; CHECK-O0:       lw s1, {{[0-9]+}}(sp)
; CHECK-O0:       lw s0, {{[0-9]+}}(sp)
; CHECK-O0:       addi sp, sp, {{[0-9]+}}
