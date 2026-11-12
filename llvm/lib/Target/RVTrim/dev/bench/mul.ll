define void @k(ptr %a, ptr %b, ptr %c, i64 %n) {
entry:
  %c0 = icmp sgt i64 %n, 0
  br i1 %c0, label %loop, label %exit
loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %pa = getelementptr i32, ptr %a, i64 %i
  %x = load i32, ptr %pa, align 4
  %pb = getelementptr i32, ptr %b, i64 %i
  %y = load i32, ptr %pb, align 4
  %z = mul i32 %x, %y
  %pc = getelementptr i32, ptr %c, i64 %i
  store i32 %z, ptr %pc, align 4
  %i.next = add i64 %i, 1
  %cond = icmp slt i64 %i.next, %n
  br i1 %cond, label %loop, label %exit
exit:
  ret void
}
