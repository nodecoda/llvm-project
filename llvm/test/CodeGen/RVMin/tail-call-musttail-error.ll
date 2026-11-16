; RUN: not llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 < %s 2>&1 \
; RUN:   | FileCheck %s

; Chapter 17: `musttail` means the frontend relies on the frame being gone
; after the call.  RVMin can only do that when every argument fits in a
; register, so a musttail call with stack arguments has to be diagnosed
; instead of silently turning into a call + return.

declare i32 @g9(i32, i32, i32, i32, i32, i32, i32, i32, i32)

define i32 @needs_stack(i32 %a, i32 %b, i32 %c, i32 %d, i32 %e, i32 %f,
                        i32 %g, i32 %h, i32 %i) {
entry:
  %r = musttail call i32 @g9(i32 %a, i32 %b, i32 %c, i32 %d, i32 %e,
                             i32 %f, i32 %g, i32 %h, i32 %i)
  ret i32 %r
}

; CHECK: RVMin: musttail requires a fixed frame and arguments that fit in registers
