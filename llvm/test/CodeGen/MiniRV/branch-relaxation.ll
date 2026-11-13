; RUN: llc -mtriple=minirv64 < %s -o - | FileCheck %s

; The conditional branches reach +-4 KiB and jal reaches +-1 MiB.  A target
; beyond those limits used to be encoded with a wrapped displacement, which
; silently jumped somewhere else.  BranchRelaxation now rewrites the branch.

; A far conditional target is reached by inverting the condition so that the
; fallthrough block gets a short branch, then jumping over it.
;
; CHECK-LABEL: cond_far:
; CHECK:         bne     {{x[0-9]+}}, x0, .LBB
; CHECK-NEXT:    jal     x0, .LBB
; CHECK-NOT:     beq     {{x[0-9]+}}, x0,
define i64 @cond_far(i64 %x) {
entry:
  %c = icmp eq i64 %x, 0
  br i1 %c, label %far, label %near
near:
  ; 4200 bytes pushes %far past the 13-bit branch displacement.
  call void asm sideeffect ".space 4200", ""()
  br label %exit
far:
  br label %exit
exit:
  ret i64 %x
}

; An unconditional target beyond +-1 MiB needs a register: la x5, block
; followed by jr x5.  X5 is reserved, so nothing live is clobbered and no
; restore block is required.
;
; CHECK-LABEL: uncond_far:
; CHECK:         bne     {{x[0-9]+}}, x0, .LBB
; CHECK:       .Lpcrel_hi0:
; CHECK-NEXT:    auipc   x5,
; CHECK-NEXT:    addi    x5, x5,
; CHECK-NEXT:    jalr    x0, 0(x5)
define i64 @uncond_far(i64 %x) {
entry:
  %c = icmp eq i64 %x, 0
  br i1 %c, label %far, label %near
near:
  call void asm sideeffect ".space 1100000", ""()
  ret i64 1
far:
  ret i64 0
}
