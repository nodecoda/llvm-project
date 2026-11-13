target triple = "minirv64"

define signext i32 @sum9b(i8 signext %a, i8 signext %b, i8 signext %c,
                          i8 signext %d, i8 signext %e, i8 signext %f,
                          i8 signext %g, i8 signext %h, i8 signext %i) {
  %a1 = sext i8 %a to i32
  %b1 = sext i8 %b to i32
  %c1 = sext i8 %c to i32
  %d1 = sext i8 %d to i32
  %e1 = sext i8 %e to i32
  %f1 = sext i8 %f to i32
  %g1 = sext i8 %g to i32
  %h1 = sext i8 %h to i32
  %i1 = sext i8 %i to i32
  %s1 = add i32 %a1, %b1
  %s2 = add i32 %c1, %d1
  %s3 = add i32 %e1, %f1
  %s4 = add i32 %g1, %h1
  %s5 = add i32 %s1, %s2
  %s6 = add i32 %s3, %s4
  %s7 = add i32 %s5, %s6
  %s8 = add i32 %s7, %i1
  ret i32 %s8
}

define signext i32 @sum9h(i16 signext %a, i16 signext %b, i16 signext %c,
                          i16 signext %d, i16 signext %e, i16 signext %f,
                          i16 signext %g, i16 signext %h, i16 signext %i) {
  %a1 = sext i16 %a to i32
  %b1 = sext i16 %b to i32
  %c1 = sext i16 %c to i32
  %d1 = sext i16 %d to i32
  %e1 = sext i16 %e to i32
  %f1 = sext i16 %f to i32
  %g1 = sext i16 %g to i32
  %h1 = sext i16 %h to i32
  %i1 = sext i16 %i to i32
  %s1 = add i32 %a1, %b1
  %s2 = add i32 %c1, %d1
  %s3 = add i32 %e1, %f1
  %s4 = add i32 %g1, %h1
  %s5 = add i32 %s1, %s2
  %s6 = add i32 %s3, %s4
  %s7 = add i32 %s5, %s6
  %s8 = add i32 %s7, %i1
  ret i32 %s8
}

define i32 @main() {
  %b = call signext i32 @sum9b(i8 1, i8 -2, i8 3, i8 -4, i8 5, i8 -6, i8 7,
                               i8 -8, i8 -9)
  %h = call signext i32 @sum9h(i16 100, i16 -200, i16 300, i16 -400, i16 500,
                               i16 -600, i16 700, i16 -800, i16 -900)
  %s = add i32 %b, %h
  %r = and i32 %s, 255
  ret i32 %r
}
