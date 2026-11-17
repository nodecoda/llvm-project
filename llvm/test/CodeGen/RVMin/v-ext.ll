; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 -mattr=+v \
; RUN:   -verify-machineinstrs < %s | FileCheck %s --check-prefix=V
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 -mattr=+v \
; RUN:   -pre-RA-sched=source -verify-machineinstrs < %s \
; RUN:   | FileCheck %s --check-prefix=SPILL
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 -mattr=+v < %s -o - \
; RUN:   | llvm-mc -triple=rvmin -mattr=+v -filetype=obj -o /dev/null
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 -mattr=+v < %s -o - \
; RUN:   | llvm-mc -triple=riscv32 -mattr=+v -filetype=obj -o /dev/null
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 < %s -o /dev/null
;
; Chapter 48: <4 x i32> is legal with +v.  The chapter selects loads, stores,
; integer arithmetic, constant splats, element extraction and 16-byte spills.
; Every data-processing vector instruction is conservatively preceded by the
; same vsetivli (e32, m1, ta, ma); chapter 49 removes redundant ones.
;
; Without +v the same IR must still compile through the generic scalarizer.

define void @add4(ptr %out, ptr %a, ptr %b) {
  %va = load <4 x i32>, ptr %a, align 16
  %vb = load <4 x i32>, ptr %b, align 16
  %v = add <4 x i32> %va, %vb
  store <4 x i32> %v, ptr %out, align 16
  ret void
}
; V-LABEL: add4:
; V:         vsetivli zero, 4, e32, m1, ta, ma
; V-NEXT:    vle32.v
; V:         vsetivli zero, 4, e32, m1, ta, ma
; V-NEXT:    vle32.v
; V:         vsetivli zero, 4, e32, m1, ta, ma
; V-NEXT:    vadd.vv
; V:         vsetivli zero, 4, e32, m1, ta, ma
; V-NEXT:    vse32.v

define void @ops(ptr %out, ptr %a, ptr %b) {
  %va = load <4 x i32>, ptr %a, align 16
  %vb = load <4 x i32>, ptr %b, align 16
  %s = sub <4 x i32> %va, %vb
  %m = mul <4 x i32> %s, %vb
  %an = and <4 x i32> %m, %va
  %or = or <4 x i32> %an, %vb
  %x = xor <4 x i32> %or, %va
  store <4 x i32> %x, ptr %out, align 16
  ret void
}
; V-LABEL: ops:
; V:         vsub.vv
; V:         vmul.vv
; V:         vand.vv
; V:         vor.vv
; V:         vxor.vv

define void @splat5(ptr %out, ptr %a) {
  %va = load <4 x i32>, ptr %a, align 16
  %v = add <4 x i32> %va, <i32 5, i32 5, i32 5, i32 5>
  store <4 x i32> %v, ptr %out, align 16
  ret void
}
; V-LABEL: splat5:
; V:         vmv.v.i {{v[0-9]+}}, 5
; V:         vadd.vv

; The 5-bit signed immediate cannot encode 100, so the constant goes through a
; private 16-byte stack object and is read back with vle32.v.
define void @splat100(ptr %out, ptr %a) {
  %va = load <4 x i32>, ptr %a, align 16
  %v = add <4 x i32> %va, <i32 100, i32 100, i32 100, i32 100>
  store <4 x i32> %v, ptr %out, align 16
  ret void
}
; V-LABEL: splat100:
; V:         addi a2, zero, 100
; V:         sw a2
; V:         vle32.v

define i32 @extract0(ptr %a) {
  %va = load <4 x i32>, ptr %a, align 16
  %e = extractelement <4 x i32> %va, i32 0
  ret i32 %e
}
; V-LABEL: extract0:
; V:         vmv.x.s a0, {{v[0-9]+}}

; Element 1 has no direct vector-to-GPR move; spill the vector and reload the
; requested word.
define i32 @extract1(ptr %a) {
  %va = load <4 x i32>, ptr %a, align 16
  %e = extractelement <4 x i32> %va, i32 1
  ret i32 %e
}
; V-LABEL: extract1:
; V:         addi a0, sp, 0
; V:         vse32.v {{v[0-9]+}}, (a0)
; V:         lw a0, 4(sp)

; Register pressure: 40 simultaneously live vectors exceed the 32 architectural
; vector registers, so the source scheduler forces vse32.v/vle32.v reloads.
define void @spill(ptr %out, ptr %p) {
  %p0 = getelementptr i8, ptr %p, i32 0
  %v0 = load <4 x i32>, ptr %p0, align 16
  %p1 = getelementptr i8, ptr %p, i32 16
  %v1 = load <4 x i32>, ptr %p1, align 16
  %p2 = getelementptr i8, ptr %p, i32 32
  %v2 = load <4 x i32>, ptr %p2, align 16
  %p3 = getelementptr i8, ptr %p, i32 48
  %v3 = load <4 x i32>, ptr %p3, align 16
  %p4 = getelementptr i8, ptr %p, i32 64
  %v4 = load <4 x i32>, ptr %p4, align 16
  %p5 = getelementptr i8, ptr %p, i32 80
  %v5 = load <4 x i32>, ptr %p5, align 16
  %p6 = getelementptr i8, ptr %p, i32 96
  %v6 = load <4 x i32>, ptr %p6, align 16
  %p7 = getelementptr i8, ptr %p, i32 112
  %v7 = load <4 x i32>, ptr %p7, align 16
  %p8 = getelementptr i8, ptr %p, i32 128
  %v8 = load <4 x i32>, ptr %p8, align 16
  %p9 = getelementptr i8, ptr %p, i32 144
  %v9 = load <4 x i32>, ptr %p9, align 16
  %p10 = getelementptr i8, ptr %p, i32 160
  %v10 = load <4 x i32>, ptr %p10, align 16
  %p11 = getelementptr i8, ptr %p, i32 176
  %v11 = load <4 x i32>, ptr %p11, align 16
  %p12 = getelementptr i8, ptr %p, i32 192
  %v12 = load <4 x i32>, ptr %p12, align 16
  %p13 = getelementptr i8, ptr %p, i32 208
  %v13 = load <4 x i32>, ptr %p13, align 16
  %p14 = getelementptr i8, ptr %p, i32 224
  %v14 = load <4 x i32>, ptr %p14, align 16
  %p15 = getelementptr i8, ptr %p, i32 240
  %v15 = load <4 x i32>, ptr %p15, align 16
  %p16 = getelementptr i8, ptr %p, i32 256
  %v16 = load <4 x i32>, ptr %p16, align 16
  %p17 = getelementptr i8, ptr %p, i32 272
  %v17 = load <4 x i32>, ptr %p17, align 16
  %p18 = getelementptr i8, ptr %p, i32 288
  %v18 = load <4 x i32>, ptr %p18, align 16
  %p19 = getelementptr i8, ptr %p, i32 304
  %v19 = load <4 x i32>, ptr %p19, align 16
  %p20 = getelementptr i8, ptr %p, i32 320
  %v20 = load <4 x i32>, ptr %p20, align 16
  %p21 = getelementptr i8, ptr %p, i32 336
  %v21 = load <4 x i32>, ptr %p21, align 16
  %p22 = getelementptr i8, ptr %p, i32 352
  %v22 = load <4 x i32>, ptr %p22, align 16
  %p23 = getelementptr i8, ptr %p, i32 368
  %v23 = load <4 x i32>, ptr %p23, align 16
  %p24 = getelementptr i8, ptr %p, i32 384
  %v24 = load <4 x i32>, ptr %p24, align 16
  %p25 = getelementptr i8, ptr %p, i32 400
  %v25 = load <4 x i32>, ptr %p25, align 16
  %p26 = getelementptr i8, ptr %p, i32 416
  %v26 = load <4 x i32>, ptr %p26, align 16
  %p27 = getelementptr i8, ptr %p, i32 432
  %v27 = load <4 x i32>, ptr %p27, align 16
  %p28 = getelementptr i8, ptr %p, i32 448
  %v28 = load <4 x i32>, ptr %p28, align 16
  %p29 = getelementptr i8, ptr %p, i32 464
  %v29 = load <4 x i32>, ptr %p29, align 16
  %p30 = getelementptr i8, ptr %p, i32 480
  %v30 = load <4 x i32>, ptr %p30, align 16
  %p31 = getelementptr i8, ptr %p, i32 496
  %v31 = load <4 x i32>, ptr %p31, align 16
  %p32 = getelementptr i8, ptr %p, i32 512
  %v32 = load <4 x i32>, ptr %p32, align 16
  %p33 = getelementptr i8, ptr %p, i32 528
  %v33 = load <4 x i32>, ptr %p33, align 16
  %p34 = getelementptr i8, ptr %p, i32 544
  %v34 = load <4 x i32>, ptr %p34, align 16
  %p35 = getelementptr i8, ptr %p, i32 560
  %v35 = load <4 x i32>, ptr %p35, align 16
  %p36 = getelementptr i8, ptr %p, i32 576
  %v36 = load <4 x i32>, ptr %p36, align 16
  %p37 = getelementptr i8, ptr %p, i32 592
  %v37 = load <4 x i32>, ptr %p37, align 16
  %p38 = getelementptr i8, ptr %p, i32 608
  %v38 = load <4 x i32>, ptr %p38, align 16
  %p39 = getelementptr i8, ptr %p, i32 624
  %v39 = load <4 x i32>, ptr %p39, align 16
  %s0 = add <4 x i32> %v0, %v1
  %s1 = add <4 x i32> %s0, %v2
  %s2 = add <4 x i32> %s1, %v3
  %s3 = add <4 x i32> %s2, %v4
  %s4 = add <4 x i32> %s3, %v5
  %s5 = add <4 x i32> %s4, %v6
  %s6 = add <4 x i32> %s5, %v7
  %s7 = add <4 x i32> %s6, %v8
  %s8 = add <4 x i32> %s7, %v9
  %s9 = add <4 x i32> %s8, %v10
  %s10 = add <4 x i32> %s9, %v11
  %s11 = add <4 x i32> %s10, %v12
  %s12 = add <4 x i32> %s11, %v13
  %s13 = add <4 x i32> %s12, %v14
  %s14 = add <4 x i32> %s13, %v15
  %s15 = add <4 x i32> %s14, %v16
  %s16 = add <4 x i32> %s15, %v17
  %s17 = add <4 x i32> %s16, %v18
  %s18 = add <4 x i32> %s17, %v19
  %s19 = add <4 x i32> %s18, %v20
  %s20 = add <4 x i32> %s19, %v21
  %s21 = add <4 x i32> %s20, %v22
  %s22 = add <4 x i32> %s21, %v23
  %s23 = add <4 x i32> %s22, %v24
  %s24 = add <4 x i32> %s23, %v25
  %s25 = add <4 x i32> %s24, %v26
  %s26 = add <4 x i32> %s25, %v27
  %s27 = add <4 x i32> %s26, %v28
  %s28 = add <4 x i32> %s27, %v29
  %s29 = add <4 x i32> %s28, %v30
  %s30 = add <4 x i32> %s29, %v31
  %s31 = add <4 x i32> %s30, %v32
  %s32 = add <4 x i32> %s31, %v33
  %s33 = add <4 x i32> %s32, %v34
  %s34 = add <4 x i32> %s33, %v35
  %s35 = add <4 x i32> %s34, %v36
  %s36 = add <4 x i32> %s35, %v37
  %s37 = add <4 x i32> %s36, %v38
  %s38 = add <4 x i32> %s37, %v39
  store <4 x i32> %s38, ptr %out, align 16
  ret void
}
; SPILL-LABEL: spill:
; SPILL:       vse32.v {{v[0-9]+}}, (sp)
; SPILL:       vle32.v {{v[0-9]+}}, (sp)
