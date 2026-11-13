target triple = "minirv64"

; Sum 0..n-1 through a variable-length array on the stack.
define i64 @sum_vla(i64 %n) {
entry:
  %a = alloca i64, i64 %n, align 8
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %i1, %loop ]
  %acc = phi i64 [ 0, %entry ], [ %acc1, %loop ]
  %p = getelementptr i64, ptr %a, i64 %i
  store i64 %i, ptr %p
  %v = load i64, ptr %p
  %acc1 = add i64 %acc, %v
  %i1 = add i64 %i, 1
  %c = icmp ult i64 %i1, %n
  br i1 %c, label %loop, label %exit
exit:
  ret i64 %acc1
}

; The array has to survive a call, which moves the stack pointer for the
; outgoing argument area as well.
define i64 @keep_vla(i64 %n) {
entry:
  %a = alloca i64, i64 %n, align 8
  %p = getelementptr i64, ptr %a, i64 2
  store i64 11, ptr %p
  %r = call i64 @sum_vla(i64 %n)
  %v = load i64, ptr %p
  %t = add i64 %r, %v
  ret i64 %t
}

; A variable-length array next to a large fixed frame.
define i64 @big_vla(i64 %n) {
entry:
  %fixed = alloca [300 x i64], align 8
  %dyn = alloca i64, i64 %n, align 8
  %fp = getelementptr i64, ptr %fixed, i64 299
  store i64 7, ptr %fp
  %dp = getelementptr i64, ptr %dyn, i64 1
  store i64 5, ptr %dp
  %a = load i64, ptr %fp
  %b = load i64, ptr %dp
  %s = add i64 %a, %b
  %r = call i64 @sum_vla(i64 %n)
  %t = add i64 %s, %r
  ret i64 %t
}

define i32 @main() {
  %a = call i64 @sum_vla(i64 10)     ; 45
  %b = call i64 @keep_vla(i64 20)    ; 190 + 11 = 201
  %c = call i64 @big_vla(i64 6)      ; 12 + 15 = 27
  %s1 = add i64 %a, %b               ; 246
  %s2 = add i64 %s1, %c              ; 273
  %s3 = and i64 %s2, 255
  %r = trunc i64 %s3 to i32
  ret i32 %r
}
