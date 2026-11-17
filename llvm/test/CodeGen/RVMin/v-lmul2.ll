; RUN: llc < %s -march=rvmin -mtriple=riscv32-unknown-elf -mattr=+v -verify-machineinstrs -O0 | FileCheck %s --check-prefix=CHECK
; RUN: llc < %s -march=rvmin -mtriple=riscv32-unknown-elf -mattr=+v -verify-machineinstrs -O2 | FileCheck %s --check-prefix=CHECK
; RUN: llc < %s -march=rvmin -mtriple=riscv32-unknown-elf -mattr=+v -verify-machineinstrs -O2 -pre-RA-sched=source | FileCheck %s --check-prefix=SPILL
; RUN: llc < %s -march=rvmin -mtriple=riscv32-unknown-elf -mattr=+v -verify-machineinstrs -O2 \
; RUN:   | llvm-mc -triple=riscv32 -mattr=+v -filetype=obj -o /dev/null
;
; Chapter 53: LMUL=2 register pairs.  A <8 x i32> value lives in a
; 256-bit pair (v8/v9, v10/v11, ...); the m2 configuration comes from
; the vtype, not from a different instruction encoding.
;
; The loads and stores use align 32 so that the value is a single
; unit-stride access.  align 16 still works but inserts scalar copies
; to realign the value, so the tests keep the clean shape.

target triple = "riscv32-unknown-elf"

define void @add8(ptr %a, ptr %b, ptr %out) {
  %va = load <8 x i32>, ptr %a, align 32
  %vb = load <8 x i32>, ptr %b, align 32
  %r = add <8 x i32> %va, %vb
  store <8 x i32> %r, ptr %out, align 32
  ret void
}

; Twenty volatile loads keep all twenty pairs live at once.  The
; register file has sixteen pairs, so the allocator must spill and
; reload through 32-byte stack slots.
define void @spill20(ptr %p, ptr %out) {
  %v0 = load volatile <8 x i32>, ptr %p, align 32
  %v1 = load volatile <8 x i32>, ptr %p, align 32
  %v2 = load volatile <8 x i32>, ptr %p, align 32
  %v3 = load volatile <8 x i32>, ptr %p, align 32
  %v4 = load volatile <8 x i32>, ptr %p, align 32
  %v5 = load volatile <8 x i32>, ptr %p, align 32
  %v6 = load volatile <8 x i32>, ptr %p, align 32
  %v7 = load volatile <8 x i32>, ptr %p, align 32
  %v8 = load volatile <8 x i32>, ptr %p, align 32
  %v9 = load volatile <8 x i32>, ptr %p, align 32
  %v10 = load volatile <8 x i32>, ptr %p, align 32
  %v11 = load volatile <8 x i32>, ptr %p, align 32
  %v12 = load volatile <8 x i32>, ptr %p, align 32
  %v13 = load volatile <8 x i32>, ptr %p, align 32
  %v14 = load volatile <8 x i32>, ptr %p, align 32
  %v15 = load volatile <8 x i32>, ptr %p, align 32
  %v16 = load volatile <8 x i32>, ptr %p, align 32
  %v17 = load volatile <8 x i32>, ptr %p, align 32
  %v18 = load volatile <8 x i32>, ptr %p, align 32
  %v19 = load volatile <8 x i32>, ptr %p, align 32
  %s1 = add <8 x i32> %v0, %v1
  %s2 = add <8 x i32> %s1, %v2
  %s3 = add <8 x i32> %s2, %v3
  %s4 = add <8 x i32> %s3, %v4
  %s5 = add <8 x i32> %s4, %v5
  %s6 = add <8 x i32> %s5, %v6
  %s7 = add <8 x i32> %s6, %v7
  %s8 = add <8 x i32> %s7, %v8
  %s9 = add <8 x i32> %s8, %v9
  %s10 = add <8 x i32> %s9, %v10
  %s11 = add <8 x i32> %s10, %v11
  %s12 = add <8 x i32> %s11, %v12
  %s13 = add <8 x i32> %s12, %v13
  %s14 = add <8 x i32> %s13, %v14
  %s15 = add <8 x i32> %s14, %v15
  %s16 = add <8 x i32> %s15, %v16
  %s17 = add <8 x i32> %s16, %v17
  %s18 = add <8 x i32> %s17, %v18
  %s19 = add <8 x i32> %s18, %v19
  store <8 x i32> %s19, ptr %out, align 32
  ret void
}

; One block mixes the two configurations.  The asm printer caches VL
; and VTYPE, so it must notice that the second group is m2 and emit a
; second vsetivli instead of reusing the m1 one.
define void @mix_m1_m2(ptr %a, ptr %b, ptr %out) {
  %a1 = load <4 x i32>, ptr %a, align 16
  %b1 = load <4 x i32>, ptr %b, align 16
  %r1 = add <4 x i32> %a1, %b1
  store <4 x i32> %r1, ptr %out, align 16
  %a2 = load <8 x i32>, ptr %a, align 32
  %b2 = load <8 x i32>, ptr %b, align 32
  %r2 = add <8 x i32> %a2, %b2
  store <8 x i32> %r2, ptr %out, align 32
  ret void
}

; The loop body changes the configuration and jumps back to the header.
; Every block entry forgets the cached VL/VTYPE, so both blocks carry
; their own vsetivli on every iteration.
define void @cf_mix(i32 %n, ptr %a, ptr %b) {
entry:
  br label %header
header:
  %i = phi i32 [ 0, %entry ], [ %i1, %body ]
  %u = load <4 x i32>, ptr %a, align 16
  %v = add <4 x i32> %u, %u
  store <4 x i32> %v, ptr %b, align 16
  %c = icmp slt i32 %i, %n
  br i1 %c, label %body, label %exit
body:
  %x = load <8 x i32>, ptr %a, align 32
  %y = add <8 x i32> %x, %x
  store <8 x i32> %y, ptr %b, align 32
  %i1 = add i32 %i, 1
  br label %header
exit:
  ret void
}

; CHECK-LABEL: add8:
; CHECK: vsetivli zero, 8, e32, m2, ta, ma
; CHECK: vle32.v
; CHECK: vadd.vv
; CHECK: vse32.v
;
; CHECK-LABEL: mix_m1_m2:
; CHECK: vsetivli zero, 4, e32, m1, ta, ma
; CHECK: vle32.v
; CHECK: vadd.vv
; CHECK: vse32.v
; CHECK: vsetivli zero, 8, e32, m2, ta, ma
; CHECK: vle32.v
; CHECK: vadd.vv
; CHECK: vse32.v
;
; CHECK-LABEL: cf_mix:
; CHECK: vsetivli zero, 4, e32, m1, ta, ma
; CHECK: vsetivli zero, 8, e32, m2, ta, ma
;
; SPILL-LABEL: spill20:
; SPILL: addi sp, sp, -
; SPILL: vse32.v {{.*}}(sp)
; SPILL: vle32.v {{.*}}(sp)
