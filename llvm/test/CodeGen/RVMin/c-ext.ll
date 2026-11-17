; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 -mattr=+c < %s \
; RUN:   | FileCheck %s --check-prefix=C
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 < %s -o %t.noc.s
; RUN: not grep -qE '^[[:space:]]+c\.' %t.noc.s
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 -mattr=+c \
; RUN:   -verify-machineinstrs < %s -o /dev/null
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 -mattr=+c < %s -o - \
; RUN:   | llvm-mc -triple=rvmin -mattr=+c -filetype=obj -o /dev/null
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O2 -mattr=+c < %s -o - \
; RUN:   | llvm-mc -triple=riscv32 -mattr=+c -filetype=obj -o /dev/null
;
; Chapter 44: -mattr=+c compresses the instructions whose operands fit the
; 16-bit encodings.  The compressor runs in the asm printer, so this file
; checks the assembly text; the e2e gate re-checks that the object bytes are
; still identical to what llvm-mc produces from that text.
;
; Without +c every line must stay in its expanded form: compression is a pure
; projection, not a codegen change.

define i32 @inc(i32 %x) {
  %r = add i32 %x, 5
  ret i32 %r
}
; C-LABEL: inc:
; C:         c.addi a0, 5
; C:         c.jr ra

define i32 @sum(i32 %x, i32 %y) {
  %r = add i32 %x, %y
  ret i32 %r
}
; C-LABEL: sum:
; C:         c.add a0, a1

define i32 @load(ptr %p) {
  %v = load i32, ptr %p
  ret i32 %v
}
; C-LABEL: load:
; C:         c.lw a0, 0(a0)

define void @store(ptr %p, i32 %v) {
  store i32 %v, ptr %p
  ret void
}
; C-LABEL: store:
; C:         c.sw a1, 0(a0)

define i32 @konst() {
  ret i32 7
}
; C-LABEL: konst:
; C:         c.li a0, 7

define i32 @big() {
  ret i32 4096
}
; C-LABEL: big:
; C:         c.lui a0, 1
; The DAG carries the 20-bit field sign-extended; the printer normalizes it
; back to the wide form that the parser accepts.
; C:         c.mv a0, a0

define i32 @br(i32 %x) {
entry:
  %c = icmp eq i32 %x, 0
  br i1 %c, label %zero, label %done
zero:
  br label %done
done:
  %r = phi i32 [ 1, %zero ], [ 0, %entry ]
  ret i32 %r
}
; C-LABEL: br:
; C:         c.li a0, 0
; C:         c.bnez a1, {{.}}
; C:         c.li a0, 1
; C:         c.jr ra
