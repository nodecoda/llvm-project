target triple = "minirv64"

; Recursion, each level with its own variable-length array.
define i64 @rec(i64 %n) {
entry:
  %a = alloca i64, i64 %n, align 8
  %p = getelementptr i64, ptr %a, i64 0
  store i64 %n, ptr %p
  %c = icmp eq i64 %n, 1
  br i1 %c, label %base, label %step
base:
  ret i64 1
step:
  %n1 = sub i64 %n, 1
  %r = call i64 @rec(i64 %n1)
  %v = load i64, ptr %p
  %t = add i64 %r, %v
  ret i64 %t
}

; A 16-byte aligned variable-length array: the stack pointer has to be
; rounded down after it has been moved.
define i64 @aligned16(i64 %n) {
entry:
  %a = alloca i64, i64 %n, align 16
  %p = getelementptr i64, ptr %a, i64 0
  store i64 3, ptr %p
  %v = load i64, ptr %p
  %lo = and i64 %v, 15
  ret i64 %lo
}

; The array outlives several calls.
define i64 @chain(i64 %n) {
entry:
  %a = alloca i64, i64 %n, align 8
  %p0 = getelementptr i64, ptr %a, i64 0
  %p1 = getelementptr i64, ptr %a, i64 1
  %p2 = getelementptr i64, ptr %a, i64 2
  store i64 1, ptr %p0
  store i64 2, ptr %p1
  store i64 3, ptr %p2
  %r1 = call i64 @rec(i64 3)
  %r2 = call i64 @aligned16(i64 %n)
  %v0 = load i64, ptr %p0
  %v1 = load i64, ptr %p1
  %v2 = load i64, ptr %p2
  %s1 = add i64 %v0, %v1
  %s2 = add i64 %s1, %v2
  %s3 = add i64 %s2, %r1
  %s4 = add i64 %s3, %r2
  ret i64 %s4
}

define i32 @main() {
  %a = call i64 @rec(i64 6)        ; 6+5+4+3+2+1 = 21
  %b = call i64 @aligned16(i64 5)  ; 3
  %c = call i64 @chain(i64 4)      ; 6 + 6 + 3 = 15
  %s1 = add i64 %a, %b             ; 24
  %s2 = add i64 %s1, %c            ; 39
  %s3 = and i64 %s2, 255
  %r = trunc i64 %s3 to i32
  ret i32 %r
}
