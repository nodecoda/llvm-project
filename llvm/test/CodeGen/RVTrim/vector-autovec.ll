; RUN: opt -mtriple=rvtrim64 -mattr=+m,+a,+zve64x -passes='default<O2>' -S < %s \
; RUN:   | llc -mtriple=rvtrim64 -mattr=+m,+a,+zve64x -rvtrim-v-vector-bits-min=128 -O2 \
; RUN:   | FileCheck %s --check-prefix=RVV
; RUN: opt -mtriple=rvtrim64 -mattr=+m,+a -passes='default<O2>' -S < %s \
; RUN:   | llc -mtriple=rvtrim64 -mattr=+m,+a -O2 | FileCheck %s --check-prefix=SCALAR

; The loop vectorizer must see the vector registers and the RVV cost model
; under the RV64IMA_Zve64x profile, and must not try to use them under the
; default RV64IMA profile.

define void @add_loop(ptr %a, ptr %b, ptr %c, i64 %n) {
; RVV-LABEL: add_loop:
; RVV: vsetvli {{.*}}e32, m2, ta, ma
; RVV: vle32.v v8,
; RVV: vle32.v v10,
; RVV: vadd.vv v8, v10, v8
; RVV: vse32.v v8,
; RVV: ret
entry:
  %cmp = icmp sgt i64 %n, 0
  br i1 %cmp, label %loop, label %exit
loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %pa = getelementptr i32, ptr %a, i64 %i
  %x = load i32, ptr %pa, align 4
  %pb = getelementptr i32, ptr %b, i64 %i
  %y = load i32, ptr %pb, align 4
  %z = add i32 %x, %y
  %pc = getelementptr i32, ptr %c, i64 %i
  store i32 %z, ptr %pc, align 4
  %i.next = add i64 %i, 1
  %cond = icmp slt i64 %i.next, %n
  br i1 %cond, label %loop, label %exit
exit:
  ret void
}

define void @mul_loop(ptr %a, ptr %b, ptr %c, i64 %n) {
; RVV-LABEL: mul_loop:
; RVV: vsetvli {{.*}}e32, m2, ta, ma
; RVV: vle32.v v8,
; RVV: vle32.v v10,
; RVV: vmul.vv v8, v10, v8
; RVV: vse32.v v8,
; RVV: ret
entry:
  %cmp = icmp sgt i64 %n, 0
  br i1 %cmp, label %loop, label %exit
loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %pa = getelementptr i32, ptr %a, i64 %i
  %x = load i32, ptr %pa, align 4
  %pb = getelementptr i32, ptr %b, i64 %i
  %y = load i32, ptr %pb, align 4
  %z = mul i32 %x, %y
  %pc = getelementptr i32, ptr %c, i64 %i
  store i32 %z, ptr %pc, align 4
  %i.next = add i64 %i, 1
  %cond = icmp slt i64 %i.next, %n
  br i1 %cond, label %loop, label %exit
exit:
  ret void
}

define i32 @sum(ptr %a, i64 %n) {
; RVV-LABEL: sum:
; RVV: vsetvli {{.*}}e32, m2, ta, ma
; RVV: vredsum.vs
; RVV: ret
entry:
  %cmp = icmp sgt i64 %n, 0
  br i1 %cmp, label %loop, label %exit
loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %acc = phi i32 [ 0, %entry ], [ %acc.next, %loop ]
  %p = getelementptr i32, ptr %a, i64 %i
  %x = load i32, ptr %p, align 4
  %acc.next = add i32 %acc, %x
  %i.next = add i64 %i, 1
  %cond = icmp slt i64 %i.next, %n
  br i1 %cond, label %loop, label %exit
exit:
  %r = phi i32 [ 0, %entry ], [ %acc.next, %loop ]
  ret i32 %r
}

; SCALAR-LABEL: add_loop:
; SCALAR-LABEL: mul_loop:
; SCALAR-LABEL: sum:
; SCALAR-NOT: vsetvli
