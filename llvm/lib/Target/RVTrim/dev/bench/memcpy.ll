define void @k(ptr %dst, ptr %src, i64 %n) {
entry:
  %c0 = icmp sgt i64 %n, 0
  br i1 %c0, label %loop, label %exit
loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %ps = getelementptr i32, ptr %src, i64 %i
  %x = load i32, ptr %ps, align 4
  %pd = getelementptr i32, ptr %dst, i64 %i
  store i32 %x, ptr %pd, align 4
  %i.next = add i64 %i, 1
  %cond = icmp slt i64 %i.next, %n
  br i1 %cond, label %loop, label %exit
exit:
  ret void
}
