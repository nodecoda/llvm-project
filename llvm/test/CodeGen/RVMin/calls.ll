; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O1 < %s | FileCheck %s
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O0 < %s -o /dev/null
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O1 < %s -filetype=obj -o %t.o
; RUN: llvm-readelf -r %t.o | FileCheck %s --check-prefix=RELOC
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O1 < %s -o - | llvm-mc -triple=riscv32 -filetype=obj -o /dev/null

; Chapter 8: calls, recursion, the return address and callee-saved registers.

define i32 @callee(i32 %a) nounwind {
  %r = add i32 %a, 1
  ret i32 %r
}
; A leaf function keeps its incoming ra: nothing is saved, nothing is reloaded.
; CHECK-LABEL: callee:
; CHECK-NOT:     sw ra
; CHECK:         addi a0, a0, 1
; CHECK:         jalr zero, 0(ra)

define i32 @caller(i32 %a) nounwind {
  %r = call i32 @callee(i32 %a)
  ret i32 %r
}
; A non-leaf function saves ra in the prologue (the call defines it) and
; reloads it before returning.  The direct call is
; `lui t0,%hi; addi t0,t0,%lo; jalr ra,0(t0)`.
; CHECK-LABEL: caller:
; CHECK:         addi sp, sp, -16
; CHECK:         sw ra, 12(sp)
; CHECK:         lui t0, %hi(callee)
; CHECK:         addi t0, t0, %lo(callee)
; CHECK:         jalr ra, 0(t0)
; CHECK:         lw ra, 12(sp)
; CHECK:         addi sp, sp, 16
; CHECK:         jalr zero, 0(ra)

define i32 @fact(i32 %n) nounwind {
entry:
  %c = icmp eq i32 %n, 0
  br i1 %c, label %base, label %rec
base:
  ret i32 1
rec:
  %m = add i32 %n, -1
  %r = call i32 @fact(i32 %m)
  %v = add i32 %r, 1
  ret i32 %v
}
; Recursion: the callee is the function itself, and ra must survive the call.
; CHECK-LABEL: fact:
; CHECK:         sw ra, {{[0-9]+}}(sp)
; CHECK:         lui t0, %hi(fact)
; CHECK:         addi t0, t0, %lo(fact)
; CHECK:         jalr ra, 0(t0)

declare i32 @ext(i32)

define i32 @call_ext(i32 %a) nounwind {
  %r = call i32 @ext(i32 %a)
  ret i32 %r
}
; An external callee has no definition in this object: the %hi/%lo pair turns
; into relocations the linker resolves.
; CHECK-LABEL: call_ext:
; CHECK:         lui t0, %hi(ext)
; CHECK:         addi t0, t0, %lo(ext)
; CHECK:         jalr ra, 0(t0)
; RELOC:         R_RISCV_HI20 {{.*}}ext
; RELOC:         R_RISCV_LO12_I {{.*}}ext

define i32 @call_indirect(ptr %fp, i32 %x) nounwind {
  %r = call i32 %fp(i32 %x)
  ret i32 %r
}
; Function pointer call: the callee address is already in a register, so the
; pseudo expands to a single `jalr ra, rs1, 0`.
; CHECK-LABEL: call_indirect:
; CHECK:         jalr ra, 0({{a[0-9]}})
; CHECK:         lw ra,

define i32 @many(i32 %a, i32 %b, i32 %c, i32 %d, i32 %e, i32 %f, i32 %g,
                 i32 %h, i32 %i, i32 %j) nounwind {
  %s1 = add i32 %a, %b
  %s2 = add i32 %s1, %c
  %s3 = add i32 %s2, %d
  %s4 = add i32 %s3, %e
  %s5 = add i32 %s4, %f
  %s6 = add i32 %s5, %g
  %s7 = add i32 %s6, %h
  %s8 = add i32 %s7, %i
  %s9 = add i32 %s8, %j
  ret i32 %s9
}
; Arguments 9 and 10 arrive on the stack, at 0(sp) and 4(sp).
; CHECK-LABEL: many:
; CHECK:         lw {{a[0-9]}}, 0(sp)
; CHECK:         lw {{a[0-9]}}, 4(sp)

define i32 @call_many() nounwind {
  %r = call i32 @many(i32 1, i32 2, i32 3, i32 4, i32 5, i32 6, i32 7, i32 8,
                      i32 9, i32 10)
  ret i32 %r
}
; The caller stores them into its outgoing argument area; the frame is rounded
; up to the 16-byte ABI alignment.
; CHECK-LABEL: call_many:
; CHECK:         addi sp, sp, -16
; CHECK:         sw {{a[0-9]}}, 4(sp)
; CHECK:         sw {{a[0-9]}}, 0(sp)
; CHECK:         lui t0, %hi(many)
; CHECK:         addi t0, t0, %lo(many)
; CHECK:         jalr ra, 0(t0)

; Values that stay live across a call end up in s0-s11, so PEI has to spill
; them in the prologue and reload them in the epilogue.
define i32 @pressure(i32 %n) nounwind {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %i.next, %loop ]
  %a0 = phi i32 [ 0, %entry ], [ %a0.n, %loop ]
  %a1 = phi i32 [ 1, %entry ], [ %a1.n, %loop ]
  %a2 = phi i32 [ 2, %entry ], [ %a2.n, %loop ]
  %r = call i32 @callee(i32 %i)
  %a0.n = add i32 %a0, %r
  %a1.n = add i32 %a1, %r
  %a2.n = add i32 %a2, %r
  %i.next = add i32 %i, 1
  %c = icmp slt i32 %i.next, %n
  br i1 %c, label %loop, label %exit
exit:
  ret i32 %a0.n
}
; CHECK-LABEL: pressure:
; CHECK:         sw ra,
; CHECK:         sw s0,
; CHECK:         sw s1,
; CHECK:         sw s2,
; CHECK:         lui t0, %hi(callee)
; CHECK:         jalr ra, 0(t0)
; CHECK:         lw ra,
; CHECK:         lw s0,
; CHECK:         lw s1,
; CHECK:         lw s2,
