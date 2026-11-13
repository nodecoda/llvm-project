define void @k(ptr %x, ptr %y, i32 %a, i64 %n) {
entry:
  %c0 = icmp sgt i64 %n, 0
  br i1 %c0, label %loop, label %exit
loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %px = getelementptr i32, ptr %x, i64 %i
  %xv = load i32, ptr %px, align 4
  %py = getelementptr i32, ptr %y, i64 %i
  %yv = load i32, ptr %py, align 4
  %m = mul i32 %xv, %a
  %r = add i32 %m, %yv
  store i32 %r, ptr %py, align 4
  %i.next = add i64 %i, 1
  %cond = icmp slt i64 %i.next, %n
  br i1 %cond, label %loop, label %exit
exit:
  ret void
}
