; RUN: llc < %s -mtriple=minirv64 -O2 | FileCheck %s

; The convention puts the first eight integer arguments in x10..x17 and the
; rest in the outgoing argument area.  That area is reserved in the prologue
; (hasReservedCallFrame), so the call site only stores into it.

; CHECK-LABEL: call_many:
; CHECK: addi x2, x2, -16
; CHECK: sd x1, 8(x2)
; CHECK: sd x{{[0-9]+}}, 0(x2)
; CHECK: jal x1, sink
; CHECK: ld x1, 8(x2)
; CHECK: addi x2, x2, 16
define i64 @call_many() {
entry:
  %r = call i64 @sink(i64 1, i64 2, i64 3, i64 4, i64 5, i64 6, i64 7, i64 8,
                      i64 9)
  ret i64 %r
}

declare i64 @sink(i64, i64, i64, i64, i64, i64, i64, i64, i64)

; The callee reads the ninth argument from 0(x2): it is the first word above
; the incoming stack pointer.
; CHECK-LABEL: read_nine:
; CHECK: ld x10, 0(x2)
; CHECK: jalr x0, 0(x1)
define i64 @read_nine(i64 %a, i64 %b, i64 %c, i64 %d, i64 %e, i64 %f, i64 %g,
                      i64 %h, i64 %i) {
entry:
  ret i64 %i
}

; An i32 argument on the stack occupies a full XLEN slot, but only its low
; word is defined, so the callee reads it with LW (or LWU for zeroext).  The
; first eight i32 arguments are already in x10..x17, so the wrapper only has
; to forward the ninth.
; CHECK-LABEL: forward_nine:
; CHECK: addi x2, x2, -16
; CHECK: sd x1, 8(x2)
; CHECK: lw x3, 16(x2)
; CHECK: sd x3, 0(x2)
; CHECK: jal x1, sink32
define void @forward_nine(i32 signext %a, i32 signext %b, i32 signext %c,
                          i32 signext %d, i32 signext %e, i32 signext %f,
                          i32 signext %g, i32 signext %h, i32 signext %i) {
entry:
  call void @sink32(i32 %a, i32 %b, i32 %c, i32 %d, i32 %e, i32 %f, i32 %g,
                    i32 %h, i32 %i)
  ret void
}

declare void @sink32(i32, i32, i32, i32, i32, i32, i32, i32, i32)

; CHECK-LABEL: read_nine32:
; CHECK: lw x10, 0(x2)
; CHECK: jalr x0, 0(x1)
define signext i32 @read_nine32(i32 signext %a, i32 signext %b, i32 signext %c,
                                i32 signext %d, i32 signext %e, i32 signext %f,
                                i32 signext %g, i32 signext %h, i32 signext %i) {
entry:
  ret i32 %i
}
