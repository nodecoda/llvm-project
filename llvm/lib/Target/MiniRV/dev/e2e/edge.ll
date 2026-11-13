target triple = "minirv64"

define signext i32 @wrap(i32 signext %a) {
  %r = add i32 %a, 1
  ret i32 %r
}

define signext i32 @topbit(i32 signext %a) {
  %r = lshr i32 %a, 31
  ret i32 %r
}

define signext i32 @ucmp(i32 signext %a, i32 signext %b) {
  %c = icmp ult i32 %a, %b
  %r = zext i1 %c to i32
  ret i32 %r
}

define signext i32 @scmp(i32 signext %a, i32 signext %b) {
  %c = icmp slt i32 %a, %b
  %r = select i1 %c, i32 1, i32 2
  ret i32 %r
}

define signext i32 @sh(i32 signext %a, i32 signext %n) {
  %l = shl i32 %a, %n
  %r = ashr i32 %l, %n
  ret i32 %r
}

define signext i32 @tr(i64 %x) {
  %t = trunc i64 %x to i32
  ret i32 %t
}

define signext i32 @tr2(i64 %hi, i64 %lo) {
  %h = shl i64 %hi, 32
  %x = or i64 %h, %lo
  %t = trunc i64 %x to i32
  ret i32 %t
}

define signext i32 @narrow(i32 signext %a) {
  %b = trunc i32 %a to i8
  %c = zext i8 %b to i32
  %d = trunc i32 %a to i16
  %e = sext i16 %d to i32
  %f = add i32 %c, %e
  ret i32 %f
}

define i32 @main() {
  %w = call signext i32 @wrap(i32 2147483647)
  %t = call signext i32 @topbit(i32 %w)
  %u1 = call signext i32 @ucmp(i32 -1, i32 1)
  %u2 = call signext i32 @ucmp(i32 1, i32 -1)
  %s1 = call signext i32 @scmp(i32 -1, i32 1)
  %s2 = call signext i32 @scmp(i32 1, i32 -1)
  %h = call signext i32 @sh(i32 -8, i32 3)
  %r1 = call signext i32 @tr2(i64 4294967295, i64 1)
  %r2 = call signext i32 @tr(i64 -1)
  %n = call signext i32 @narrow(i32 305419896)
  %a1 = add i32 %t, %u1
  %a2 = add i32 %a1, %u2
  %a3 = add i32 %a2, %s1
  %a4 = add i32 %a3, %s2
  %a5 = add i32 %a4, %h
  %a6 = add i32 %a5, %r1
  %a7 = add i32 %a6, %r2
  %a8 = add i32 %a7, %n
  %r = and i32 %a8, 255
  ret i32 %r
}
