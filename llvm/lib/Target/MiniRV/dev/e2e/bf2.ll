target triple = "minirv64"

define i32 @main() {
entry:
  %a = alloca [300 x i64], align 8
  br label %fill
fill:
  %i = phi i64 [ 0, %entry ], [ %i1, %fill ]
  %p = getelementptr inbounds [300 x i64], ptr %a, i64 0, i64 %i
  %v = mul i64 %i, 3
  store i64 %v, ptr %p
  %i1 = add i64 %i, 1
  %c = icmp ult i64 %i1, 300
  br i1 %c, label %fill, label %sum
sum:
  br label %loop
loop:
  %j = phi i64 [ 0, %sum ], [ %j1, %loop ]
  %s = phi i64 [ 0, %sum ], [ %s1, %loop ]
  %q = getelementptr inbounds [300 x i64], ptr %a, i64 0, i64 %j
  %w = load i64, ptr %q
  %s1 = add i64 %s, %w
  %j1 = add i64 %j, 1
  %c2 = icmp ult i64 %j1, 300
  br i1 %c2, label %loop, label %done
done:
  %r = trunc i64 %s to i32
  ret i32 %r
}
