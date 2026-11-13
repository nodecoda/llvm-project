target triple = "minirv64"

define i64 @args_vla(i64 %a, i64 %b, i64 %c, i64 %d, i64 %e, i64 %f, i64 %g,
                     i64 %h, i64 %i, i64 %n) {
entry:
  %v = alloca i64, i64 %n, align 8
  %p = getelementptr i64, ptr %v, i64 0
  store i64 %i, ptr %p
  %x = load i64, ptr %p
  %s1 = add i64 %a, %b
  %s2 = add i64 %s1, %c
  %s3 = add i64 %s2, %d
  %s4 = add i64 %s3, %e
  %s5 = add i64 %s4, %f
  %s6 = add i64 %s5, %g
  %s7 = add i64 %s6, %h
  %s8 = add i64 %s7, %x
  ret i64 %s8
}

define i32 @main() {
  ; 1+2+3+4+5+6+7+8+9 = 45
  %r = call i64 @args_vla(i64 1, i64 2, i64 3, i64 4, i64 5, i64 6, i64 7,
                          i64 8, i64 9, i64 4)
  %t = and i64 %r, 255
  %x = trunc i64 %t to i32
  ret i32 %x
}
