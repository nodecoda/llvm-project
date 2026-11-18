; RUN: llc -march=gpumin -O1 -verify-machineinstrs < %s | FileCheck %s

; Chapter 05: labels, predicated branches and the word-scaled fixup.

target triple = "gpumin-unknown-elf"

define i32 @choose(i32 %n) {
entry:
  %c = icmp sgt i32 %n, 0
  br i1 %c, label %pos, label %nonpos
pos:
  ret i32 1
nonpos:
  ret i32 0
; CHECK-LABEL: choose:
; CHECK:       movi r1, 1
; CHECK:       setp.lt p0, r4, r1
; CHECK:       bra p0, [[NONPOS:.LBB[0-9_]+]]
; CHECK:       movi r4, 1
; CHECK:       ret
; CHECK: [[NONPOS]]:
; CHECK:       movi r4, 0
; CHECK:       ret
}

define i32 @loop(i32 %n) {
entry:
  br label %head
head:
  %i = phi i32 [ 0, %entry ], [ %next, %body ]
  %sum = phi i32 [ 0, %entry ], [ %add, %body ]
  %c = icmp slt i32 %i, %n
  br i1 %c, label %body, label %done
body:
  %add = add i32 %sum, %i
  %next = add i32 %i, 1
  br label %head
done:
  ret i32 %sum
; CHECK-LABEL: loop:
; CHECK:       mov r1, r4
; CHECK:       movi r4, 0
; CHECK:       setp.ge p0, r2, r1
; CHECK:       bra p0, [[DONE:.LBB[0-9_]+]]
; CHECK: [[BODY:.LBB[0-9_]+]]:
; CHECK:       add r4, r4, r2
; CHECK-NEXT:  addi r2, r2, 1
; CHECK:       setp.ge p0, r2, r1
; CHECK:       bra p0, [[DONE]]
; CHECK:       bra [[BODY]]
; CHECK: [[DONE]]:
; CHECK:       ret
}

; Regression for chapter 12: analyzeBranch() must report the BRA_P target as
; TBB and the following BRA target as FBB.  With those swapped, block
; placement keeps the predicate but exchanges the two targets, which inverts
; the branch without changing the assembly's shape.
define i32 @recur(i32 %n) {
entry:
  %c = icmp slt i32 %n, 2
  br i1 %c, label %base, label %rec
base:
  ret i32 %n
rec:
  %n1 = sub i32 %n, 1
  %r = call i32 @recur(i32 %n1)
  %s = add i32 %r, 1
  ret i32 %s
; CHECK-LABEL: recur:
; CHECK:       setp.gt p0, r4, r1
; CHECK:       bra p0, [[REC:.LBB[0-9_]+]]
; CHECK:       bra [[BASE:.LBB[0-9_]+]]
; CHECK: [[REC]]:
; CHECK:       call recur
; CHECK: [[BASE]]:
; CHECK:       ret
}
