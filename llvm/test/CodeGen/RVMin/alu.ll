; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O1 < %s | FileCheck %s
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O1 < %s -o - | llvm-mc -triple=riscv32 -filetype=obj -o /dev/null

; Chapter 4: integer arithmetic, logic, shifts, comparisons and select.
; Big constants are materialized with LUI+ADDI by RVMinDAGToDAGISel::Select.

define i32 @add_rr(i32 %a, i32 %b) nounwind {
entry:
  %r = add i32 %a, %b
  ret i32 %r
}
; CHECK-LABEL: add_rr:
; CHECK:         add a0, a0, a1

define i32 @sub_rr(i32 %a, i32 %b) nounwind {
entry:
  %r = sub i32 %a, %b
  ret i32 %r
}
; CHECK-LABEL: sub_rr:
; CHECK:         sub a0, a0, a1

define i32 @and_imm(i32 %a) nounwind {
entry:
  %r = and i32 %a, 255
  ret i32 %r
}
; CHECK-LABEL: and_imm:
; CHECK:         andi a0, a0, 255

define i32 @or_imm(i32 %a) nounwind {
entry:
  %r = or i32 %a, 15
  ret i32 %r
}
; CHECK-LABEL: or_imm:
; CHECK:         ori a0, a0, 15

define i32 @xor_rr(i32 %a, i32 %b) nounwind {
entry:
  %r = xor i32 %a, %b
  ret i32 %r
}
; CHECK-LABEL: xor_rr:
; CHECK:         xor a0, a0, a1

define i32 @shl_rr(i32 %a, i32 %b) nounwind {
entry:
  %r = shl i32 %a, %b
  ret i32 %r
}
; CHECK-LABEL: shl_rr:
; CHECK:         sll a0, a0, a1

define i32 @srl_imm(i32 %a) nounwind {
entry:
  %r = lshr i32 %a, 3
  ret i32 %r
}
; CHECK-LABEL: srl_imm:
; CHECK:         srli a0, a0, 3

define i32 @sra_rr(i32 %a, i32 %b) nounwind {
entry:
  %r = ashr i32 %a, %b
  ret i32 %r
}
; CHECK-LABEL: sra_rr:
; CHECK:         sra a0, a0, a1

define i32 @slt_rr(i32 %a, i32 %b) nounwind {
entry:
  %c = icmp slt i32 %a, %b
  %r = zext i1 %c to i32
  ret i32 %r
}
; CHECK-LABEL: slt_rr:
; CHECK:         slt a0, a0, a1

define i32 @sltu_rr(i32 %a, i32 %b) nounwind {
entry:
  %c = icmp ult i32 %a, %b
  %r = zext i1 %c to i32
  ret i32 %r
}
; CHECK-LABEL: sltu_rr:
; CHECK:         sltu a0, a0, a1

define i32 @eq_rr(i32 %a, i32 %b) nounwind {
entry:
  %c = icmp eq i32 %a, %b
  %r = zext i1 %c to i32
  ret i32 %r
}
; CHECK-LABEL: eq_rr:
; CHECK:         xor [[X:[a-z0-9]+]], a0, a1
; CHECK-NEXT:    sltiu {{[a-z0-9]+}}, [[X]], 1

define i32 @ne_rr(i32 %a, i32 %b) nounwind {
entry:
  %c = icmp ne i32 %a, %b
  %r = zext i1 %c to i32
  ret i32 %r
}
; CHECK-LABEL: ne_rr:
; CHECK:         xor [[X:[a-z0-9]+]], a0, a1
; CHECK-NEXT:    sltu {{[a-z0-9]+}}, zero, [[X]]

define i32 @le_rr(i32 %a, i32 %b) nounwind {
entry:
  %c = icmp sle i32 %a, %b
  %r = zext i1 %c to i32
  ret i32 %r
}
; CHECK-LABEL: le_rr:
; CHECK:         slt [[T:[a-z0-9]+]], a1, a0
; CHECK-NEXT:    xori {{[a-z0-9]+}}, [[T]], 1

define i32 @big_const() nounwind {
entry:
  ret i32 305419896
}
; CHECK-LABEL: big_const:
; CHECK:         lui [[R:[a-z0-9]+]], 74565
; CHECK-NEXT:    addi [[R]], [[R]], 1656

define i32 @neg_const() nounwind {
entry:
  ret i32 -305419896
}
; CHECK-LABEL: neg_const:
; The LUI field is 20 bits and unsigned: -74565 prints as 974011 (the same
; bits), exactly like the reference RISC-V assembler does.
; CHECK:         lui [[R:[a-z0-9]+]], 974011
; CHECK-NEXT:    addi [[R]], [[R]], -1656

define i32 @sel(i32 %a, i32 %b) nounwind {
entry:
  %c = icmp slt i32 %a, %b
  %s = select i1 %c, i32 %a, i32 %b
  ret i32 %s
}
; CHECK-LABEL: sel:
; CHECK:         slt [[C:[a-z0-9]+]], a0, a1
; CHECK:         sub [[M:[a-z0-9]+]], zero, [[C]]
; CHECK:         and [[T:[a-z0-9]+]], {{[a-z0-9]+}}, [[M]]
; CHECK:         xor {{[a-z0-9]+}}, [[T]], a1
; CHECK:         jalr zero, 0(ra)
