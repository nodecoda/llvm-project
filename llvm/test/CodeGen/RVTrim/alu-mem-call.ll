; RUN: llc -mtriple=rvtrim64 -mattr=+m,+a -O2 -verify-machineinstrs < %s \
; RUN:   | FileCheck %s

; The generated records must be usable by instruction selection, not just by
; the assembler: ALU, memory, atomics, calls and CFI all come out of the same
; records that auto-td emits.

define i64 @alu(i64 %a, i64 %b) {
; CHECK-LABEL: alu:
; CHECK: mul a2, a0, a1
; CHECK: divu a1, a2, a1
; CHECK: remu a0, a1, a0
; CHECK: add a0, a0, a2
; CHECK: ret
entry:
  %m = mul i64 %a, %b
  %d = udiv i64 %m, %b
  %r = urem i64 %d, %a
  %s = add i64 %r, %m
  ret i64 %s
}

define i64 @mem(i64 %p, i64 %v) {
; CHECK-LABEL: mem:
; CHECK: sd a1, 0(a2)
; CHECK: ret
entry:
  %q = inttoptr i64 %p to ptr
  store i64 %v, ptr %q, align 8
  %l = load i64, ptr %q, align 8
  ret i64 %l
}

define i64 @amo(ptr %p, i64 %v) {
; CHECK-LABEL: amo:
; CHECK: amoadd.d.aqrl a0, a1, (a0)
; CHECK: ret
entry:
  %old = atomicrmw add ptr %p, i64 %v seq_cst
  ret i64 %old
}

declare i64 @callee(i64)

define i64 @call(i64 %x) {
; CHECK-LABEL: call:
; CHECK: addi sp, sp, -16
; CHECK: sd ra, 8(sp)
; CHECK: call callee
; CHECK: ld ra, 8(sp)
; CHECK: addi sp, sp, 16
; CHECK: ret
entry:
  %r = call i64 @callee(i64 %x)
  ret i64 %r
}

define i64 @br(i64 %x, i64 %y) {
; CHECK-LABEL: br:
; CHECK: bge a0, a1, .LBB
entry:
  %c = icmp slt i64 %x, %y
  br i1 %c, label %t, label %f
t:
  ret i64 1
f:
  ret i64 0
}
