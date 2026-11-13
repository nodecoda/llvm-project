target triple = "minirv64"

define signext i32 @mix(i32 signext %a, i32 signext %b) {
  %t1 = add i32 %a, %b
  %t2 = mul i32 %t1, 3
  %t3 = ashr i32 %t2, 2
  %t4 = xor i32 %t3, -2147483648
  %t5 = lshr i32 %t4, 28
  %c = icmp slt i32 %t5, 8
  %r = select i1 %c, i32 %t5, i32 8
  ret i32 %r
}

define signext i32 @divs(i32 signext %a, i32 signext %b) {
  %q = sdiv i32 %a, %b
  %r = srem i32 %a, %b
  %s = add i32 %q, %r
  ret i32 %s
}

define signext i32 @sum9(i32 signext %a, i32 signext %b, i32 signext %c,
                         i32 signext %d, i32 signext %e, i32 signext %f,
                         i32 signext %g, i32 signext %h, i32 signext %i) {
  %s1 = add i32 %a, %b
  %s2 = add i32 %c, %d
  %s3 = add i32 %e, %f
  %s4 = add i32 %g, %h
  %s5 = add i32 %s1, %s2
  %s6 = add i32 %s3, %s4
  %s7 = add i32 %s5, %s6
  %s8 = add i32 %s7, %i
  ret i32 %s8
}

define signext i32 @sumsq(i32 signext %n) {
entry:
  br label %head
head:
  %i = phi i32 [ 0, %entry ], [ %i1, %head ]
  %s = phi i32 [ 0, %entry ], [ %s1, %head ]
  %ii = mul i32 %i, %i
  %s1 = add i32 %s, %ii
  %i1 = add i32 %i, 1
  %c = icmp slt i32 %i1, %n
  br i1 %c, label %head, label %exit
exit:
  ret i32 %s1
}

define i32 @main() {
  %a = call signext i32 @mix(i32 7, i32 5)
  %b = call signext i32 @divs(i32 -100, i32 7)
  %c = call signext i32 @sum9(i32 1, i32 2, i32 3, i32 4, i32 5, i32 6, i32 7,
                              i32 8, i32 9)
  %d = call signext i32 @sumsq(i32 10)
  %s1 = add i32 %a, %b
  %s2 = add i32 %s1, %c
  %s3 = add i32 %s2, %d
  %r = and i32 %s3, 255
  ret i32 %r
}
