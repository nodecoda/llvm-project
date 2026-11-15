; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O1 < %s | FileCheck %s
; RUN: llc -march=rvmin -mtriple=riscv32-unknown-elf -O1 < %s -o - | llvm-mc -triple=riscv32 -filetype=obj -o /dev/null

; Chapter 5: conditional branches, unconditional branches and block layout.
; BranchFolder/MachineBlockPlacement use RVMinInstrInfo::analyzeBranch to
; delete jumps to the layout successor.

define i32 @max(i32 %a, i32 %b) nounwind {
entry:
  %c = icmp sgt i32 %a, %b
  br i1 %c, label %then, label %else
then:
  ret i32 %a
else:
  ret i32 %b
}
; CHECK-LABEL: max:
; CHECK:         bge a1, a0, {{\.LBB[0-9_]+}}
; CHECK-NOT:     jal zero
; CHECK:         jalr zero, 0(ra)
; CHECK-NOT:     jal zero
; CHECK:         jalr zero, 0(ra)

define i32 @count_down(i32 %n) nounwind {
entry:
  br label %loop
loop:
  %i = phi i32 [ %n, %entry ], [ %next, %body ]
  %acc = phi i32 [ 0, %entry ], [ %sum, %body ]
  %cmp = icmp sgt i32 %i, 0
  br i1 %cmp, label %body, label %exit
body:
  %sum = add i32 %acc, %i
  %next = sub i32 %i, 1
  br label %loop
exit:
  ret i32 %acc
}
; CHECK-LABEL: count_down:
; CHECK:         blt a0, a2, {{\.LBB[0-9_]+}}
; CHECK:         add {{[a-z0-9]+}}, {{[a-z0-9]+}}, a0
; CHECK:         addi a0, a0, -1
; CHECK:         jal zero, {{\.LBB[0-9_]+}}

define i32 @nested(i32 %a, i32 %b) nounwind {
entry:
  %c1 = icmp slt i32 %a, 0
  br i1 %c1, label %neg, label %pos
neg:
  ret i32 1
pos:
  %c2 = icmp ult i32 %b, 10
  br i1 %c2, label %small, label %big
small:
  ret i32 2
big:
  ret i32 3
}
; CHECK-LABEL: nested:
; CHECK:         blt {{[a-z0-9]+}}, a0, {{\.LBB[0-9_]+}}
; CHECK:         bltu {{[a-z0-9]+}}, a1, {{\.LBB[0-9_]+}}
