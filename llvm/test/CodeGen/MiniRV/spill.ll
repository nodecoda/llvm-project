; RUN: llc < %s -mtriple=minirv64 -O0 | FileCheck %s

; Forty live values cannot fit in the 32 GPRs, so the register allocator has to
; use the stack.  Every GPR is 64 bits wide, so one sd/ld pair covers all
; spills; the frame is adjusted with the only stack instruction in the corpus.

; CHECK-LABEL: spill:
; CHECK: addi x2, x2, -96
; CHECK: sd x{{[0-9]+}}, {{-?[0-9]+\(x2\)}}
; CHECK: ld x{{[0-9]+}}, {{-?[0-9]+\(x2\)}}
; CHECK: addi x2, x2, 96
; CHECK: jalr x0, 0(x1)
define i64 @spill(ptr %p) {
entry:
  %g0 = getelementptr i64, ptr %p, i64 0
  %v0 = load i64, ptr %g0, align 8
  %g1 = getelementptr i64, ptr %p, i64 1
  %v1 = load i64, ptr %g1, align 8
  %g2 = getelementptr i64, ptr %p, i64 2
  %v2 = load i64, ptr %g2, align 8
  %g3 = getelementptr i64, ptr %p, i64 3
  %v3 = load i64, ptr %g3, align 8
  %g4 = getelementptr i64, ptr %p, i64 4
  %v4 = load i64, ptr %g4, align 8
  %g5 = getelementptr i64, ptr %p, i64 5
  %v5 = load i64, ptr %g5, align 8
  %g6 = getelementptr i64, ptr %p, i64 6
  %v6 = load i64, ptr %g6, align 8
  %g7 = getelementptr i64, ptr %p, i64 7
  %v7 = load i64, ptr %g7, align 8
  %g8 = getelementptr i64, ptr %p, i64 8
  %v8 = load i64, ptr %g8, align 8
  %g9 = getelementptr i64, ptr %p, i64 9
  %v9 = load i64, ptr %g9, align 8
  %g10 = getelementptr i64, ptr %p, i64 10
  %v10 = load i64, ptr %g10, align 8
  %g11 = getelementptr i64, ptr %p, i64 11
  %v11 = load i64, ptr %g11, align 8
  %g12 = getelementptr i64, ptr %p, i64 12
  %v12 = load i64, ptr %g12, align 8
  %g13 = getelementptr i64, ptr %p, i64 13
  %v13 = load i64, ptr %g13, align 8
  %g14 = getelementptr i64, ptr %p, i64 14
  %v14 = load i64, ptr %g14, align 8
  %g15 = getelementptr i64, ptr %p, i64 15
  %v15 = load i64, ptr %g15, align 8
  %g16 = getelementptr i64, ptr %p, i64 16
  %v16 = load i64, ptr %g16, align 8
  %g17 = getelementptr i64, ptr %p, i64 17
  %v17 = load i64, ptr %g17, align 8
  %g18 = getelementptr i64, ptr %p, i64 18
  %v18 = load i64, ptr %g18, align 8
  %g19 = getelementptr i64, ptr %p, i64 19
  %v19 = load i64, ptr %g19, align 8
  %g20 = getelementptr i64, ptr %p, i64 20
  %v20 = load i64, ptr %g20, align 8
  %g21 = getelementptr i64, ptr %p, i64 21
  %v21 = load i64, ptr %g21, align 8
  %g22 = getelementptr i64, ptr %p, i64 22
  %v22 = load i64, ptr %g22, align 8
  %g23 = getelementptr i64, ptr %p, i64 23
  %v23 = load i64, ptr %g23, align 8
  %g24 = getelementptr i64, ptr %p, i64 24
  %v24 = load i64, ptr %g24, align 8
  %g25 = getelementptr i64, ptr %p, i64 25
  %v25 = load i64, ptr %g25, align 8
  %g26 = getelementptr i64, ptr %p, i64 26
  %v26 = load i64, ptr %g26, align 8
  %g27 = getelementptr i64, ptr %p, i64 27
  %v27 = load i64, ptr %g27, align 8
  %g28 = getelementptr i64, ptr %p, i64 28
  %v28 = load i64, ptr %g28, align 8
  %g29 = getelementptr i64, ptr %p, i64 29
  %v29 = load i64, ptr %g29, align 8
  %g30 = getelementptr i64, ptr %p, i64 30
  %v30 = load i64, ptr %g30, align 8
  %g31 = getelementptr i64, ptr %p, i64 31
  %v31 = load i64, ptr %g31, align 8
  %g32 = getelementptr i64, ptr %p, i64 32
  %v32 = load i64, ptr %g32, align 8
  %g33 = getelementptr i64, ptr %p, i64 33
  %v33 = load i64, ptr %g33, align 8
  %g34 = getelementptr i64, ptr %p, i64 34
  %v34 = load i64, ptr %g34, align 8
  %g35 = getelementptr i64, ptr %p, i64 35
  %v35 = load i64, ptr %g35, align 8
  %g36 = getelementptr i64, ptr %p, i64 36
  %v36 = load i64, ptr %g36, align 8
  %g37 = getelementptr i64, ptr %p, i64 37
  %v37 = load i64, ptr %g37, align 8
  %g38 = getelementptr i64, ptr %p, i64 38
  %v38 = load i64, ptr %g38, align 8
  %g39 = getelementptr i64, ptr %p, i64 39
  %v39 = load i64, ptr %g39, align 8
  %s1 = add i64 %v0, %v1
  %s2 = add i64 %s1, %v2
  %s3 = add i64 %s2, %v3
  %s4 = add i64 %s3, %v4
  %s5 = add i64 %s4, %v5
  %s6 = add i64 %s5, %v6
  %s7 = add i64 %s6, %v7
  %s8 = add i64 %s7, %v8
  %s9 = add i64 %s8, %v9
  %s10 = add i64 %s9, %v10
  %s11 = add i64 %s10, %v11
  %s12 = add i64 %s11, %v12
  %s13 = add i64 %s12, %v13
  %s14 = add i64 %s13, %v14
  %s15 = add i64 %s14, %v15
  %s16 = add i64 %s15, %v16
  %s17 = add i64 %s16, %v17
  %s18 = add i64 %s17, %v18
  %s19 = add i64 %s18, %v19
  %s20 = add i64 %s19, %v20
  %s21 = add i64 %s20, %v21
  %s22 = add i64 %s21, %v22
  %s23 = add i64 %s22, %v23
  %s24 = add i64 %s23, %v24
  %s25 = add i64 %s24, %v25
  %s26 = add i64 %s25, %v26
  %s27 = add i64 %s26, %v27
  %s28 = add i64 %s27, %v28
  %s29 = add i64 %s28, %v29
  %s30 = add i64 %s29, %v30
  %s31 = add i64 %s30, %v31
  %s32 = add i64 %s31, %v32
  %s33 = add i64 %s32, %v33
  %s34 = add i64 %s33, %v34
  %s35 = add i64 %s34, %v35
  %s36 = add i64 %s35, %v36
  %s37 = add i64 %s36, %v37
  %s38 = add i64 %s37, %v38
  %s39 = add i64 %s38, %v39
  ret i64 %s39
}
