; RUN: llc -mtriple=rvtrim64 -mattr=+zve64x,+xtinyv -O2 < %s | FileCheck %s

; The instruction record comes from auto-td; the intrinsic and its selection
; are the hand-written semantic layer.  The record has no mask operand, so vm
; is encoded as 1.
;
; The record uses VR (LMUL=1).  A value whose ABI class is wider (for example
; nxv4i32, which the ABI passes in v8m2) would need an LMUL-converting copy;
; RVTrim does not lower that yet, so this test stays at LMUL=1.

declare <vscale x 2 x i32> @llvm.rvtrim.vfexp.nxv2i32(<vscale x 2 x i32>)

define <vscale x 2 x i32> @vfexp(<vscale x 2 x i32> %x) {
  %r = call <vscale x 2 x i32> @llvm.rvtrim.vfexp.nxv2i32(<vscale x 2 x i32> %x)
  ret <vscale x 2 x i32> %r
}

; CHECK-LABEL: vfexp:
; CHECK: yushuxin.vfexp
