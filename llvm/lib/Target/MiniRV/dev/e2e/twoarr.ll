target triple = "minirv64"

define i32 @main() {
entry:
  %a = alloca [300 x i64], align 8
  %b = alloca [300 x i64], align 8
  br label %fill
fill:
  %i = phi i64 [ 0, %entry ], [ %i1, %fill ]
  %pa = getelementptr inbounds [300 x i64], ptr %a, i64 0, i64 %i
  %pb = getelementptr inbounds [300 x i64], ptr %b, i64 0, i64 %i
  store i64 1, ptr %pa
  store i64 2, ptr %pb
  %i1 = add i64 %i, 1
  %c = icmp ult i64 %i1, 299
  br i1 %c, label %fill, label %sum
sum:
  br label %loop
loop:
  %j = phi i64 [ 0, %sum ], [ %j1, %loop ]
  %s = phi i64 [ 0, %sum ], [ %s1, %loop ]
  %qa = getelementptr inbounds [300 x i64], ptr %a, i64 0, i64 %j
  %qb = getelementptr inbounds [300 x i64], ptr %b, i64 0, i64 %j
  %va = load i64, ptr %qa
  %vb = load i64, ptr %qb
  %v = add i64 %va, %vb
  %s1 = add i64 %s, %v
  %j1 = add i64 %j, 1
  %c2 = icmp ult i64 %j1, 299
  br i1 %c2, label %loop, label %done
done:
  %r = trunc i64 %s1 to i32
  ret i32 %r
}
