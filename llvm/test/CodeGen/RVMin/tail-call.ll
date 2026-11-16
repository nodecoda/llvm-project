; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 < %s | FileCheck %s
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 < %s \
; RUN:   | llvm-mc -triple=rvmin -filetype=obj -o /dev/null
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 < %s -filetype=obj -o %t.o
; RUN: llvm-readelf --unwind %t.o | FileCheck %s --check-prefix=UNWIND

; Chapter 17: tail calls.  A call in tail position does not need a return
; address of its own: the epilogue restores sp and the callee-saved registers,
; then `jalr zero, 0(t0)` jumps to the callee, which returns straight to our
; caller.  `tail-call-musttail-error.ll` covers the diagnostic for a musttail
; call that cannot reuse the frame.

declare i32 @g(i32)
declare i32 @h(i32)

; Direct tail call: the same %hi/%lo pair as PseudoCALL, but rd = x0.
define i32 @direct(i32 %x) {
entry:
  %r = tail call i32 @g(i32 %x)
  ret i32 %r
}
; CHECK-LABEL: direct:
; CHECK:         lui t0, %hi(g)
; CHECK-NEXT:    addi t0, t0, %lo(g)
; CHECK-NEXT:    jalr zero, 0(t0)
; CHECK-NOT:     jalr ra
; CHECK:         .Lfunc_end0:

; Indirect tail call: one instruction, the address is already in a register.
define i32 @indirect(ptr %p, i32 %x) {
entry:
  %r = tail call i32 %p(i32 %x)
  ret i32 %r
}
; CHECK-LABEL: indirect:
; CHECK:         jalr zero, 0({{[a-z][a-z0-9]*}})
; CHECK-NOT:     jalr ra

; A tail call cannot reuse a frame in place: sp and the callee-saved registers
; are restored first, and only then does control leave.  A function with a
; frame must therefore pop it before the jump.
define i32 @frame_tail(i32 %x) {
entry:
  %p = alloca i32
  store volatile i32 %x, ptr %p
  %v = load volatile i32, ptr %p
  %r = tail call i32 @g(i32 %v)
  ret i32 %r
}
; CHECK-LABEL: frame_tail:
; CHECK:         addi sp, sp, -16
; CHECK:         lw {{[a-z][a-z0-9]*}}, {{[0-9]+}}(sp)
; CHECK:         addi sp, sp, 16
; CHECK-NEXT:    .cfi_def_cfa_offset 0
; CHECK-NEXT:    lui t0, %hi(g)
; CHECK-NEXT:    addi t0, t0, %lo(g)
; CHECK-NEXT:    jalr zero, 0(t0)

; A non-tail call still clobbers `ra`, so it is saved and restored; the tail
; call that follows must not overwrite it again.
define i32 @mixed(i32 %x) {
entry:
  %a = call i32 @h(i32 %x)
  %b = tail call i32 @g(i32 %a)
  ret i32 %b
}
; CHECK-LABEL: mixed:
; CHECK:         sw ra, {{[0-9]+}}(sp)
; CHECK:         jalr ra, 0(t0)
; CHECK:         lw ra, {{[0-9]+}}(sp)
; CHECK:         .cfi_restore ra
; CHECK-NEXT:    .cfi_def_cfa_offset 0
; CHECK-NEXT:    lui t0, %hi(g)
; CHECK-NEXT:    addi t0, t0, %lo(g)
; CHECK-NEXT:    jalr zero, 0(t0)

; Arguments that need the outgoing area cannot be passed by a frame-reusing
; jump: the fallback is an ordinary call that returns, and the `tail` marker
; is dropped.
define i32 @stack_args(i32 %x) {
entry:
  %r = tail call i32 @g9(i32 %x, i32 %x, i32 %x, i32 %x, i32 %x, i32 %x,
                         i32 %x, i32 %x, i32 %x)
  ret i32 %r
}
declare i32 @g9(i32, i32, i32, i32, i32, i32, i32, i32, i32)
; CHECK-LABEL: stack_args:
; CHECK:         sw {{[a-z][a-z0-9]*}}, 0(sp)
; CHECK:         jalr ra, 0(t0)
; CHECK:         addi sp, sp, 16
; CHECK-NEXT:    .cfi_restore ra
; CHECK-NEXT:    .cfi_def_cfa_offset 0
; CHECK-NEXT:    jalr zero, 0(ra)

; The unwinder has to see the caller's frame again before the jump: the FDE of
; a tail-calling function ends with CFA = sp and no saved registers.
; UNWIND:       .eh_frame section
; UNWIND:       DW_CFA_def_cfa: reg2 +0
; UNWIND:       DW_CFA_def_cfa_offset: +16
; UNWIND:       DW_CFA_def_cfa_offset: +0
; UNWIND:       DW_CFA_offset: reg1 -4
; UNWIND:       DW_CFA_restore: reg1
; UNWIND:       DW_CFA_def_cfa_offset: +0
