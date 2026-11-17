; RUN: split-file %s %t
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O0 -mattr=+v \
; RUN:   -verify-machineinstrs < %t/mask.ll | FileCheck %s --check-prefix=ASM
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O0 -mattr=+v \
; RUN:   -stop-after=finalize-isel < %t/mask.ll | FileCheck %s --check-prefix=MIR
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 -mattr=+v \
; RUN:   -verify-machineinstrs < %t/mask.ll | FileCheck %s --check-prefix=ASM
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 -mattr=+v < %t/mask.ll \
; RUN:   -o - | llvm-mc -triple=rvmin -mattr=+v -filetype=obj -o /dev/null
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 -mattr=+v < %t/mask.ll \
; RUN:   -o - | llvm-mc -triple=riscv32 -mattr=+v -filetype=obj -o /dev/null
; RUN: not llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 -mattr=+v \
; RUN:   < %t/nonconst.ll -o /dev/null 2>&1 | FileCheck %s --check-prefix=ERR
;
; Chapter 52: <4 x i1> is legal with +v and lives in the vector register file.
; A constant mask is materialized as a 4-bit integer in a GPR, broadcast into
; v0 with vmv.v.x, and consumed by vmerge.vvm.
;
; The MIR check pins the operand order: vmerge.vvm computes
;   vd = mask ? vs1 : vs2
; while vselect is (cond, true, false), so the true value must be vs1.  The
; loads make the two values distinguishable: %a comes from %pa, %b from %pb.
;
; Non-constant masks need a vector compare (vmseq.vv and friends) and are
; rejected with a clean diagnostic rather than miscompiled.

;--- mask.ll
define void @selc(ptr %pa, ptr %pb, ptr %po) {
  %a = load <4 x i32>, ptr %pa, align 16
  %b = load <4 x i32>, ptr %pb, align 16
  %r = select <4 x i1> <i1 true, i1 false, i1 true, i1 false>,
                     <4 x i32> %a, <4 x i32> %b
  store <4 x i32> %r, ptr %po, align 16
  ret void
}

;--- nonconst.ll
define void @selcmp(ptr %pa, ptr %pb, ptr %po) {
  %a = load <4 x i32>, ptr %pa, align 16
  %b = load <4 x i32>, ptr %pb, align 16
  %m = icmp slt <4 x i32> %a, %b
  %r = select <4 x i1> %m, <4 x i32> %a, <4 x i32> %b
  store <4 x i32> %r, ptr %po, align 16
  ret void
}

; ASM-LABEL: selc:
; ASM:         vsetivli zero, 4, e32, m1, ta, ma
; ASM:         vmv.v.x v0, a0
; ASM:         vmerge.vvm {{v[0-9]+}}, {{v[0-9]+}}, {{v[0-9]+}}, v0
; ASM:         vse32.v

; MIR-LABEL: name:            selc
; MIR:         %[[A:[0-9]+]]:vr = VLE32_V %{{[0-9]+}} :: (load (s128) from %ir.pa)
; MIR:         %[[B:[0-9]+]]:vr = VLE32_V %{{[0-9]+}} :: (load (s128) from %ir.pb)
; MIR:         %[[K:[0-9]+]]:gpr = ADDI $x0, 5
; MIR:         %[[V:[0-9]+]]:vr = VMV_V_X %[[K]]
; MIR:         %[[M:[0-9]+]]:vmv0 = COPY %[[V]]
; MIR:         VMERGE_VVM %[[B]], %[[A]], %[[M]]
; MIR:         VSE32_V

; ERR: LLVM ERROR: RVMin: non-constant vector masks are not supported yet
