target triple = "minirv64"

declare ptr @llvm.stacksave.p0()
declare void @llvm.stackrestore.p0(ptr)
declare ptr @llvm.frameaddress.p0(i32)
declare ptr @llvm.returnaddress(i32)

; A C99 variable-length array inside a loop: the stack is saved before the
; allocation and restored after, so the frame does not grow per iteration.
define i64 @vla_loop(i64 %iters, i64 %n) {
entry:
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %acc = phi i64 [ 0, %entry ], [ %acc.next, %loop ]
  %sp = call ptr @llvm.stacksave.p0()
  %a = alloca i64, i64 %n, align 8
  %p = getelementptr i64, ptr %a, i64 1
  store i64 3, ptr %p
  %v = load i64, ptr %p
  call void @llvm.stackrestore.p0(ptr %sp)
  %acc.next = add i64 %acc, %v
  %i.next = add i64 %i, 1
  %done = icmp eq i64 %i.next, %iters
  br i1 %done, label %exit, label %loop
exit:
  ret i64 %acc.next
}

; The frame address of the callee is the caller's stack pointer at the call,
; so it must be non-zero and aligned, and it must be below the caller's own
; frame address.
define ptr @callee_fa() {
entry:
  %p = call ptr @llvm.frameaddress.p0(i32 0)
  ret ptr %p
}

define i64 @fa_check() {
entry:
  %mine = call ptr @llvm.frameaddress.p0(i32 0)
  %theirs = call ptr @callee_fa()
  %nonzero = icmp ne ptr %theirs, null
  %ti = ptrtoint ptr %theirs to i64
  %aligned = and i64 %ti, 7
  %isaligned = icmp eq i64 %aligned, 0
  %below = icmp ult ptr %theirs, %mine
  %ok0 = and i1 %nonzero, %isaligned
  %ok = and i1 %ok0, %below
  %r = zext i1 %ok to i64
  ret i64 %r
}

; The return address of a leaf is the link register; of a function that calls
; something it is the value the prologue saved.
define ptr @leaf_ra() {
entry:
  %p = call ptr @llvm.returnaddress(i32 0)
  ret ptr %p
}

define i64 @ra_check() {
entry:
  %ra = call ptr @leaf_ra()
  %nonzero = icmp ne ptr %ra, null
  %r = zext i1 %nonzero to i64
  ret i64 %r
}

define i64 @main() {
entry:
  %a = call i64 @vla_loop(i64 2000, i64 1024)
  %b = call i64 @fa_check()
  %c = call i64 @ra_check()
  %s = add i64 %a, %b
  %t = add i64 %s, %c
  ret i64 %t
}
