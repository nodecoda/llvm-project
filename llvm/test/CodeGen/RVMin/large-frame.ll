; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O0 < %s | FileCheck %s
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O1 < %s | FileCheck %s
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 < %s | FileCheck %s
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O3 < %s | FileCheck %s
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 < %s \
; RUN:   | llvm-mc -triple=rvmin -filetype=obj -o /dev/null

; Chapter 16: a stack frame larger than 2 KiB.  The 12-bit immediate field of
; LW/SW/SB/LB only reaches +/-2 KiB, so eliminateFrameIndex has to materialize
; the address of the object in a scratch register.  That register is created
; after register allocation, which means PEI's register scavenger picks it.
;
; The frame is 3072 + padding bytes, so the prologue's own `sp` adjustment and
; every access near the top of the frame (callee-saved spills included) take
; the same LUI+ADDI path.

define i32 @big_frame(i32 %x) {
entry:
  %buf = alloca [3072 x i8], align 16
  %p = getelementptr [3072 x i8], ptr %buf, i32 0, i32 3000
  store volatile i8 7, ptr %p, align 1
  %v = load volatile i8, ptr %p, align 1
  %r = zext i8 %v to i32
  ret i32 %r
}

; CHECK-LABEL: big_frame:
; The prologue: sp -= 3088, materialized with LUI+ADDI in t0.
; CHECK:         lui t0, 1
; CHECK-NEXT:    addi t0, t0, -{{[0-9]+}}
; CHECK-NEXT:    sub sp, sp, t0
; CHECK:         .cfi_def_cfa_offset 3088
; The far store: lui+addi+add build sp+3000 in a scavenged register, then the
; original instruction uses offset 0.
; CHECK:         lui [[ADDR:[a-z][a-z0-9]*]], 1
; CHECK-NEXT:    addi [[ADDR]], [[ADDR]], -
; CHECK-NEXT:    add [[ADDR]], [[ADDR]], sp
; CHECK-NEXT:    sb {{[a-z][a-z0-9]*}}, 0([[ADDR]])
; CHECK:         lui [[ADDR2:[a-z][a-z0-9]*]], 1
; CHECK-NEXT:    addi [[ADDR2]], [[ADDR2]], -
; CHECK-NEXT:    add [[ADDR2]], [[ADDR2]], sp
; CHECK-NEXT:    lbu {{[a-z][a-z0-9]*}}, 0([[ADDR2]])
; CHECK:         lui t0, 1
; CHECK-NEXT:    addi t0, t0, -{{[0-9]+}}
; CHECK-NEXT:    add sp, sp, t0

; A non-leaf function adds a callee-saved slot at the *top* of the frame, which
; is even further from sp than the object above.  The same materialization has
; to run for storeRegToStackSlot's `sw` and its matching `lw`.
define void @big_frame_call(i32 %x) {
entry:
  %buf = alloca [3072 x i8], align 16
  %p = getelementptr [3072 x i8], ptr %buf, i32 0, i32 3000
  store volatile i8 7, ptr %p, align 1
  call void @callee()
  %v = load volatile i8, ptr %p, align 1
  ret void
}

declare void @callee()

; CHECK-LABEL: big_frame_call:
; CHECK:         lui t0, 1
; CHECK-NEXT:    addi t0, t0, -{{[0-9]+}}
; CHECK-NEXT:    sub sp, sp, t0
; The ra spill slot is at the far end of the frame.
; CHECK:         lui [[SLOT:[a-z][a-z0-9]*]], 1
; CHECK-NEXT:    addi [[SLOT]], [[SLOT]], -
; CHECK-NEXT:    add [[SLOT]], [[SLOT]], sp
; CHECK-NEXT:    sw ra, 0([[SLOT]])
; CHECK:         .cfi_offset ra, -{{[0-9]+}}
; The volatile store in the body.
; CHECK:         lui [[BODY:[a-z][a-z0-9]*]], 1
; CHECK-NEXT:    addi [[BODY]], [[BODY]], -
; CHECK-NEXT:    add [[BODY]], [[BODY]], sp
; CHECK-NEXT:    sb {{[a-z][a-z0-9]*}}, 0([[BODY]])
; The reload of ra uses the same far slot.
; CHECK:         lb {{[a-z][a-z0-9]*}}, 0(
; CHECK:         lui [[SLOT2:[a-z][a-z0-9]*]], 1
; CHECK-NEXT:    addi [[SLOT2]], [[SLOT2]], -
; CHECK-NEXT:    add [[SLOT2]], [[SLOT2]], sp
; CHECK-NEXT:    lw ra, 0([[SLOT2]])
; CHECK:         add sp, sp, t0
