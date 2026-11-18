; Chapter 20 sample: 4x4 matrix multiply, naive and tiled.
;
; The ISA has no multiply instruction (chapter 15's saxpy uses shift-add), so
; both kernels call the same @mul device function: 32 shift-add steps.  That
; keeps the comparison honest -- the only difference between the two kernels
; is where the operands come from.
;
;   matmul_naive: every lane loads its row of A and its column of B from
;                 global memory, so the block reads each matrix element four
;                 times.
;   matmul_tiled: each lane loads one element of A and one of B into the
;                 block's shared window, barriers, then every lane reads the
;                 tile from shared memory.  Global traffic drops fourfold and
;                 shared traffic replaces it.
target triple = "gpumin-unknown-elf"

@tile = internal addrspace(2) global [32 x i32] zeroinitializer, align 4

declare i32 @llvm.gpumin.tid()
declare void @llvm.gpumin.barrier()

; acc = a * b, shift-add.  The loop stops as soon as the multiplier is
; exhausted, so the cost tracks the operand values instead of a fixed 32
; steps -- that matters for the comparison in this chapter, because a
; 32-step multiply would dwarf every memory effect.
define i32 @mul(i32 %a, i32 %b) {
entry:
  br label %loop

loop:
  %x = phi i32 [ %a, %entry ], [ %x2, %body ]
  %y = phi i32 [ %b, %entry ], [ %y2, %body ]
  %acc = phi i32 [ 0, %entry ], [ %acc2, %body ]
  %done = icmp eq i32 %y, 0
  br i1 %done, label %exit, label %body

body:
  %bit = and i32 %y, 1
  %nz = icmp ne i32 %bit, 0
  %term = select i1 %nz, i32 %x, i32 0
  %acc2 = add i32 %acc, %term
  %x2 = shl i32 %x, 1
  %y2 = lshr i32 %y, 1
  br label %loop

exit:
  ret i32 %acc
}

define void @matmul_naive(ptr addrspace(1) %a, ptr addrspace(1) %b,
                          ptr addrspace(1) %c) #0 {
entry:
  %t = call i32 @llvm.gpumin.tid()
  %r = lshr i32 %t, 2
  %r4 = shl i32 %r, 2
  %col = and i32 %t, 3

  %pa = getelementptr i32, ptr addrspace(1) %a, i32 %r4
  %pa1 = getelementptr i32, ptr addrspace(1) %pa, i32 1
  %pa2 = getelementptr i32, ptr addrspace(1) %pa, i32 2
  %pa3 = getelementptr i32, ptr addrspace(1) %pa, i32 3
  %va0 = load i32, ptr addrspace(1) %pa
  %va1 = load i32, ptr addrspace(1) %pa1
  %va2 = load i32, ptr addrspace(1) %pa2
  %va3 = load i32, ptr addrspace(1) %pa3

  %pb = getelementptr i32, ptr addrspace(1) %b, i32 %col
  %pb1 = getelementptr i32, ptr addrspace(1) %pb, i32 4
  %pb2 = getelementptr i32, ptr addrspace(1) %pb, i32 8
  %pb3 = getelementptr i32, ptr addrspace(1) %pb, i32 12
  %vb0 = load i32, ptr addrspace(1) %pb
  %vb1 = load i32, ptr addrspace(1) %pb1
  %vb2 = load i32, ptr addrspace(1) %pb2
  %vb3 = load i32, ptr addrspace(1) %pb3

  %m0 = call i32 @mul(i32 %va0, i32 %vb0)
  %m1 = call i32 @mul(i32 %va1, i32 %vb1)
  %m2 = call i32 @mul(i32 %va2, i32 %vb2)
  %m3 = call i32 @mul(i32 %va3, i32 %vb3)
  %s0 = add i32 %m0, %m1
  %s1 = add i32 %s0, %m2
  %s2 = add i32 %s1, %m3

  %pc = getelementptr i32, ptr addrspace(1) %c, i32 %t
  store i32 %s2, ptr addrspace(1) %pc
  ret void
}

define void @matmul_tiled(ptr addrspace(1) %a, ptr addrspace(1) %b,
                          ptr addrspace(1) %c) #0 {
entry:
  %t = call i32 @llvm.gpumin.tid()

  ; Phase 1: one global load per lane per matrix, into the block window.
  %pa = getelementptr i32, ptr addrspace(1) %a, i32 %t
  %pb = getelementptr i32, ptr addrspace(1) %b, i32 %t
  %va = load i32, ptr addrspace(1) %pa
  %vb = load i32, ptr addrspace(1) %pb
  %sta = getelementptr [32 x i32], ptr addrspace(2) @tile, i32 0, i32 %t
  store i32 %va, ptr addrspace(2) %sta
  %tb = add i32 %t, 16
  %stb = getelementptr [32 x i32], ptr addrspace(2) @tile, i32 0, i32 %tb
  store i32 %vb, ptr addrspace(2) %stb

  call void @llvm.gpumin.barrier()

  ; Phase 2: the row of A and the column of B come from shared memory.
  %r = lshr i32 %t, 2
  %r4 = shl i32 %r, 2
  %col = and i32 %t, 3

  %pa4 = getelementptr [32 x i32], ptr addrspace(2) @tile, i32 0, i32 %r4
  %qa0 = load i32, ptr addrspace(2) %pa4
  %pa4_1 = getelementptr i32, ptr addrspace(2) %pa4, i32 1
  %qa1 = load i32, ptr addrspace(2) %pa4_1
  %pa4_2 = getelementptr i32, ptr addrspace(2) %pa4, i32 2
  %qa2 = load i32, ptr addrspace(2) %pa4_2
  %pa4_3 = getelementptr i32, ptr addrspace(2) %pa4, i32 3
  %qa3 = load i32, ptr addrspace(2) %pa4_3

  %bcol = add i32 %col, 16
  %pb4 = getelementptr [32 x i32], ptr addrspace(2) @tile, i32 0, i32 %bcol
  %qb0 = load i32, ptr addrspace(2) %pb4
  %pb4_1 = getelementptr i32, ptr addrspace(2) %pb4, i32 4
  %qb1 = load i32, ptr addrspace(2) %pb4_1
  %pb4_2 = getelementptr i32, ptr addrspace(2) %pb4, i32 8
  %qb2 = load i32, ptr addrspace(2) %pb4_2
  %pb4_3 = getelementptr i32, ptr addrspace(2) %pb4, i32 12
  %qb3 = load i32, ptr addrspace(2) %pb4_3

  %m0 = call i32 @mul(i32 %qa0, i32 %qb0)
  %m1 = call i32 @mul(i32 %qa1, i32 %qb1)
  %m2 = call i32 @mul(i32 %qa2, i32 %qb2)
  %m3 = call i32 @mul(i32 %qa3, i32 %qb3)
  %s0 = add i32 %m0, %m1
  %s1 = add i32 %s0, %m2
  %s2 = add i32 %s1, %m3

  %pc = getelementptr i32, ptr addrspace(1) %c, i32 %t
  store i32 %s2, ptr addrspace(1) %pc
  ret void
}

attributes #0 = { "gpumin_kernel" }
