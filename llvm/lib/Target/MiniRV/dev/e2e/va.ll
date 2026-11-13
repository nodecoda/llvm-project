target triple = "minirv64"

declare void @llvm.va_start.p0(ptr)
declare void @llvm.va_end.p0(ptr)

; Sum %n unnamed i64 arguments.
define i64 @sum(i64 %n, ...) {
entry:
  %ap = alloca ptr, align 8
  call void @llvm.va_start.p0(ptr %ap)
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %acc = phi i64 [ 0, %entry ], [ %acc.next, %loop ]
  %v = va_arg ptr %ap, i64
  %acc.next = add i64 %acc, %v
  %i.next = add i64 %i, 1
  %done = icmp eq i64 %i.next, %n
  br i1 %done, label %exit, label %loop
exit:
  call void @llvm.va_end.p0(ptr %ap)
  ret i64 %acc.next
}

; Sum %n unnamed i32 arguments; each still occupies a whole slot.
define i64 @sum32(i64 %n, ...) {
entry:
  %ap = alloca ptr, align 8
  call void @llvm.va_start.p0(ptr %ap)
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %acc = phi i64 [ 0, %entry ], [ %acc.next, %loop ]
  %v = va_arg ptr %ap, i32
  %w = sext i32 %v to i64
  %acc.next = add i64 %acc, %w
  %i.next = add i64 %i, 1
  %done = icmp eq i64 %i.next, %n
  br i1 %done, label %exit, label %loop
exit:
  call void @llvm.va_end.p0(ptr %ap)
  ret i64 %acc.next
}

; Every argument register is taken by a named argument, so the save area is
; empty and va_start points straight at the incoming stack arguments.
define i64 @sum_stack(i64 %a, i64 %b, i64 %c, i64 %d, i64 %e, i64 %f, i64 %g,
                      i64 %h, ...) {
entry:
  %ap = alloca ptr, align 8
  call void @llvm.va_start.p0(ptr %ap)
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %acc = phi i64 [ 0, %entry ], [ %acc.next, %loop ]
  %v = va_arg ptr %ap, i64
  %acc.next = add i64 %acc, %v
  %i.next = add i64 %i, 1
  %done = icmp eq i64 %i.next, 3
  br i1 %done, label %exit, label %loop
exit:
  call void @llvm.va_end.p0(ptr %ap)
  ret i64 %acc.next
}

; A dynamic alloca forces a frame pointer, so the save area is reached through
; X8 instead of the stack pointer.
define i64 @vla_sum(i64 %n, i64 %m, ...) {
entry:
  %buf = alloca i64, i64 %m, align 8
  store i64 7, ptr %buf
  %ap = alloca ptr, align 8
  call void @llvm.va_start.p0(ptr %ap)
  br label %loop
loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %acc = phi i64 [ 0, %entry ], [ %acc.next, %loop ]
  %v = va_arg ptr %ap, i64
  %acc.next = add i64 %acc, %v
  %i.next = add i64 %i, 1
  %done = icmp eq i64 %i.next, %n
  br i1 %done, label %exit, label %loop
exit:
  call void @llvm.va_end.p0(ptr %ap)
  %w = load i64, ptr %buf
  %r = add i64 %acc.next, %w
  ret i64 %r
}

define i64 @main() {
entry:
  %a = call i64 (i64, ...) @sum(i64 4, i64 10, i64 20, i64 30, i64 40)
  %b = call i64 (i64, ...) @sum(i64 10, i64 1, i64 2, i64 3, i64 4, i64 5,
                                i64 6, i64 7, i64 8, i64 9, i64 10)
  %c = call i64 (i64, ...) @sum32(i64 4, i32 1, i32 2, i32 3, i32 4)
  %d = call i64 (i64, i64, i64, i64, i64, i64, i64, i64, ...)
      @sum_stack(i64 0, i64 0, i64 0, i64 0, i64 0, i64 0, i64 0, i64 0,
                 i64 1, i64 2, i64 3)
  %e = call i64 (i64, i64, ...) @vla_sum(i64 3, i64 5, i64 1, i64 2, i64 3)
  %ab = add i64 %a, %b
  %cd = add i64 %c, %d
  %abcd = add i64 %ab, %cd
  %r = add i64 %abcd, %e
  ret i64 %r
}
