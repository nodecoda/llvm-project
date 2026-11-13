target triple = "minirv64"

; A tail call turns the recursion into a jump, so the stack does not grow.
define i64 @count(i64 %n, i64 %acc) {
entry:
  %c = icmp eq i64 %n, 0
  br i1 %c, label %done, label %step
step:
  %acc1 = add i64 %acc, 1
  %n1 = sub i64 %n, 1
  %r = tail call i64 @count(i64 %n1, i64 %acc1)
  ret i64 %r
done:
  ret i64 %acc
}

; A tail call from a function with a frame, through a function pointer.
define i64 @via_ptr(ptr %f, i64 %x) {
entry:
  %a = alloca [4 x i64], align 8
  store i64 %x, ptr %a
  %v = load i64, ptr %a
  %r = tail call i64 %f(i64 %v)
  ret i64 %r
}

define i64 @inc(i64 %x) {
  %r = add i64 %x, 1
  ret i64 %r
}

define i32 @main() {
  %a = call i64 @count(i64 1000000, i64 0)
  %b = call i64 @via_ptr(ptr @inc, i64 40)
  %c = call i64 @count(i64 %b, i64 0)
  %s = add i64 %a, %c
  %t = and i64 %s, 255
  %r = trunc i64 %t to i32
  ret i32 %r
}
