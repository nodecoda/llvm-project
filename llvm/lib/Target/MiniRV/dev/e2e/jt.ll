target triple = "minirv64"

define i64 @sw(i64 %x) {
entry:
  switch i64 %x, label %def [
    i64 0, label %c0
    i64 1, label %c1
    i64 2, label %c2
    i64 3, label %c3
    i64 4, label %c4
    i64 5, label %c5
    i64 6, label %c6
    i64 7, label %c7
  ]
c0: ret i64 10
c1: ret i64 11
c2: ret i64 12
c3: ret i64 13
c4: ret i64 14
c5: ret i64 15
c6: ret i64 16
c7: ret i64 17
def: ret i64 0
}

; A switch dense enough to need a table but too sparse to be a range.
define i64 @sw2(i64 %x) {
entry:
  switch i64 %x, label %def [
    i64 100, label %c0
    i64 102, label %c1
    i64 104, label %c2
    i64 106, label %c3
    i64 108, label %c4
    i64 110, label %c5
  ]
c0: ret i64 1
c1: ret i64 2
c2: ret i64 3
c3: ret i64 4
c4: ret i64 5
c5: ret i64 6
def: ret i64 0
}

define i64 @main() {
entry:
  %a0 = call i64 @sw(i64 0)
  %a1 = call i64 @sw(i64 1)
  %a2 = call i64 @sw(i64 2)
  %a3 = call i64 @sw(i64 3)
  %a4 = call i64 @sw(i64 4)
  %a5 = call i64 @sw(i64 5)
  %a6 = call i64 @sw(i64 6)
  %a7 = call i64 @sw(i64 7)
  %ax = call i64 @sw(i64 9)
  %b0 = call i64 @sw2(i64 100)
  %b1 = call i64 @sw2(i64 102)
  %b2 = call i64 @sw2(i64 104)
  %b3 = call i64 @sw2(i64 106)
  %b4 = call i64 @sw2(i64 108)
  %b5 = call i64 @sw2(i64 110)
  %bx = call i64 @sw2(i64 103)
  %s0 = add i64 %a0, %a1
  %s1 = add i64 %a2, %a3
  %s2 = add i64 %a4, %a5
  %s3 = add i64 %a6, %a7
  %s4 = add i64 %s0, %s1
  %s5 = add i64 %s2, %s3
  %s6 = add i64 %s4, %s5
  %s7 = add i64 %s6, %ax
  %t0 = add i64 %b0, %b1
  %t1 = add i64 %b2, %b3
  %t2 = add i64 %b4, %b5
  %t3 = add i64 %t0, %t1
  %t4 = add i64 %t3, %t2
  %t5 = add i64 %t4, %bx
  %r = add i64 %s7, %t5
  ret i64 %r
}
