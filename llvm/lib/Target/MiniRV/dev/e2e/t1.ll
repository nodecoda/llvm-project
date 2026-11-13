target triple = "minirv64"
define i64 @rec(i64 %n) {
entry:
  %a = alloca i64, i64 %n, align 8
  %p = getelementptr i64, ptr %a, i64 1
  store i64 %n, ptr %p
  %c = icmp eq i64 %n, 0
  br i1 %c, label %base, label %step
base:
  ret i64 0
step:
  %n1 = sub i64 %n, 1
  %r = call i64 @rec(i64 %n1)
  %v = load i64, ptr %p
  %t = add i64 %r, %v
  ret i64 %t
}
define i32 @main() {
  %a = call i64 @rec(i64 6)
  %r = trunc i64 %a to i32
  ret i32 %r
}
