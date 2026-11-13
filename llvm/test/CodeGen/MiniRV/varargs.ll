; RUN: llc < %s -mtriple=minirv64 -O2 | FileCheck %s

; A va_list is a pointer to the next argument slot.  The unnamed arguments
; start in the argument registers that the named ones left over and continue
; with the incoming stack arguments, so the prologue saves the remaining
; registers into an area that ends exactly at the incoming stack pointer: the
; two areas then form one array of XLEN-sized slots.

declare void @llvm.va_start.p0(ptr)
declare void @llvm.va_copy.p0(ptr, ptr)
declare void @llvm.va_end.p0(ptr)

; %n takes x10, so x11..x17 are saved.  The va_list object is the only local
; object, and it sits below the 56-byte save area in the 64-byte frame.
; CHECK-LABEL: sum:
; CHECK:       addi x2, x2, -64
; CHECK-NEXT:  addi [[AP:x[0-9]+]], x2, 0
; CHECK-NEXT:  addi [[AREA:x[0-9]+]], x2, 8
; CHECK-NEXT:  sd [[AREA]], 0([[AP]])
; CHECK-NEXT:  sd x11, 0([[AREA]])
; CHECK-NEXT:  sd x12, 8([[AREA]])
; CHECK-NEXT:  sd x13, 16([[AREA]])
; CHECK-NEXT:  sd x14, 24([[AREA]])
; CHECK-NEXT:  sd x15, 32([[AREA]])
; CHECK-NEXT:  sd x16, 40([[AREA]])
; CHECK-NEXT:  sd x17, 48([[AREA]])
; CHECK:       ld [[SLOT:x[0-9]+]], 0([[AP]])
; CHECK-NEXT:  addi [[NEXT:x[0-9]+]], [[SLOT]], 8
; CHECK-NEXT:  sd [[NEXT]], 0([[AP]])
; CHECK-NEXT:  ld {{x[0-9]+}}, 0([[SLOT]])
; CHECK:       addi x2, x2, 64
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

; A narrow argument still occupies a whole slot: the load reads the low word of
; the slot and the pointer advances by eight bytes.
; CHECK-LABEL: sum32:
; CHECK:       ld [[SLOT:x[0-9]+]], 0(
; CHECK-NEXT:  addi [[NEXT:x[0-9]+]], [[SLOT]], 8
; CHECK-NEXT:  sd [[NEXT]], 0(
; CHECK-NEXT:  lw {{x[0-9]+}}, 0([[SLOT]])
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

; Eight named arguments use every argument register, so there is nothing to
; save and va_start points straight at the incoming stack arguments.  The
; va_list object is at the stack pointer and the argument area starts one slot
; above it.
; CHECK-LABEL: sum_stack:
; CHECK:       addi x2, x2, -16
; CHECK-NEXT:  addi [[AP:x[0-9]+]], x2, 8
; CHECK-NEXT:  addi [[AREA:x[0-9]+]], x2, 16
; CHECK-NEXT:  sd [[AREA]], 0([[AP]])
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

; When the named arguments already overflow to the stack, the save area is
; empty and the va_list starts past those named slots.
; CHECK-LABEL: named_stack:
; CHECK:       addi x2, x2, -16
; CHECK-NEXT:  addi [[AP:x[0-9]+]], x2, 8
; CHECK-NEXT:  addi [[AREA:x[0-9]+]], x2, 32
; CHECK-NEXT:  sd [[AREA]], 0([[AP]])
define i64 @named_stack(i64 %a0, i64 %a1, i64 %a2, i64 %a3, i64 %a4, i64 %a5,
                        i64 %a6, i64 %a7, i64 %a8, i64 %a9, ...) {
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
  %done = icmp eq i64 %i.next, 2
  br i1 %done, label %exit, label %loop
exit:
  call void @llvm.va_end.p0(ptr %ap)
  ret i64 %acc.next
}

; A dynamic alloca puts the frame pointer at the incoming stack pointer, so the
; save area is addressed from x8 rather than from the moved stack pointer.
; CHECK-LABEL: vla_sum:
; CHECK:       addi x2, x2, -64
; CHECK-NEXT:  sd x8, 8(x2)
; CHECK-NEXT:  addi x8, x2, 64
; CHECK-NEXT:  addi [[AREA:x[0-9]+]], x8, -48
; CHECK:       sd x12, 0([[AREA]])
; CHECK:       sd x14, 16([[AREA]])
; CHECK:       sd x15, 24([[AREA]])
; CHECK:       sd x16, 32([[AREA]])
; CHECK:       sd x17, 40([[AREA]])
; CHECK:       addi [[AP:x[0-9]+]], x8, -64
; CHECK-NEXT:  sd [[AREA]], 0([[AP]])
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

; va_copy is a pointer copy and va_end does nothing: the copy holds the slot
; the first va_arg read, while the original has already moved past it.
; CHECK-LABEL: copy_test:
; CHECK:       addi x2, x2, -80
; CHECK:       addi [[AREA:x[0-9]+]], x2, 24
; CHECK-DAG:   addi [[NEXT:x[0-9]+]], [[AREA]], 8
; CHECK-DAG:   sd [[AREA]], 0(
; CHECK-DAG:   sd [[NEXT]], 0(
define i64 @copy_test(i64 %n, ...) {
entry:
  %ap = alloca ptr, align 8
  %ap2 = alloca ptr, align 8
  call void @llvm.va_start.p0(ptr %ap)
  call void @llvm.va_copy.p0(ptr %ap2, ptr %ap)
  %a = va_arg ptr %ap, i64
  call void @llvm.va_end.p0(ptr %ap)
  call void @llvm.va_end.p0(ptr %ap2)
  ret i64 %a
}

; A call to a variadic function passes the unnamed arguments like any other
; argument: the first seven go in x11..x17, the rest goes on the stack.
; CHECK-LABEL: caller:
; CHECK:       sd {{x[0-9]+}}, 0(x2)
; CHECK:       sd {{x[0-9]+}}, 8(x2)
; CHECK:       addi x17, x0, 7
; CHECK:       jal x1, sum
define i64 @caller() {
entry:
  %r = call i64 (i64, ...) @sum(i64 9, i64 1, i64 2, i64 3, i64 4, i64 5,
                                i64 6, i64 7, i64 8, i64 9)
  ret i64 %r
}
