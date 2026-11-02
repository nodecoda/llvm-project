; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -global-isel -x86-gisel-table-stats \
; RUN:     -o /dev/null 2>&1 | FileCheck %s
; RUN: llc < %s -mtriple=i386-unknown-linux-gnu -global-isel -x86-gisel-table-stats \
; RUN:     -o /dev/null 2>&1 | FileCheck %s

; The table is keyed on the classes the rules name.  IR can carry widths and
; shapes that are not in that list, and the legalizer must not fall back to a
; rule chain: the generated evaluator answers the concrete query instead.
; These functions walk that residual path, so the codegen has to complete.
;
; CHECK: x86-gisel-table: hit {{[0-9]+}} residual {{[1-9][0-9]*}} escape 0 outside 0 ptrvec 0 addrspace {{[0-9]+}} untabulated 0

define i12 @add12(i12 %a, i12 %b) {
  %r = add i12 %a, %b
  ret i12 %r
}

define i27 @add27(i27 %a, i27 %b) {
  %r = add i27 %a, %b
  ret i27 %r
}

define i35 @add35(i35 %a, i35 %b) {
  %r = add i35 %a, %b
  ret i35 %r
}

define i42 @add42(i42 %a, i42 %b) {
  %r = add i42 %a, %b
  ret i42 %r
}

define i48 @add48(i48 %a, i48 %b) {
  %r = add i48 %a, %b
  ret i48 %r
}

define <3 x i32> @add3i32(<3 x i32> %a, <3 x i32> %b) {
  %r = add <3 x i32> %a, %b
  ret <3 x i32> %r
}
