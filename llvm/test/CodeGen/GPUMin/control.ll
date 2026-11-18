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
; CHECK:       bra [[POS:.LBB[0-9_]+]]
; CHECK: [[POS]]:
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
; CHECK: [[HEAD:.LBB[0-9_]+]]:
; CHECK:       setp.ge p0, r2, r1
; CHECK:       bra p0, [[DONE:.LBB[0-9_]+]]
; CHECK:       bra [[BODY:.LBB[0-9_]+]]
; CHECK: [[BODY]]:
; CHECK:       add r4, r4, r2
; CHECK:       addi r2, r2, 1
; CHECK:       bra [[HEAD]]
; CHECK: [[DONE]]:
; CHECK:       ret
}
