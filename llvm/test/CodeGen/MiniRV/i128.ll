; RUN: llc -mtriple=minirv64 < %s | FileCheck %s
; RUN: llc -mtriple=minirv64 -filetype=obj < %s -o %t.o
; RUN: llvm-objdump -d --triple=minirv64 %t.o | FileCheck %s --check-prefix=OBJ

; A 128-bit value is held in two 64-bit registers and returned through the
; sret pointer in x10.  The type legalizer splits the arithmetic into 64-bit
; pieces; the pieces that need MULH/MULHU are selected by the patterns in
; MiniRVInstrInfo.td, so this test also covers the auto-td multiply records.

; Addition is a two-instruction carry chain: the low halves are added and the
; carry out (low < low) is folded into the high halves.
; CHECK-LABEL: add:
; CHECK:         add     {{x[0-9]+}}, {{x[0-9]+}}, {{x[0-9]+}}
; CHECK:         add     {{x[0-9]+}}, {{x[0-9]+}}, {{x[0-9]+}}
; CHECK:         sltu    {{x[0-9]+}}, {{x[0-9]+}}, {{x[0-9]+}}
; CHECK:         add     {{x[0-9]+}}, {{x[0-9]+}}, {{x[0-9]+}}
; CHECK:         sd      {{x[0-9]+}}, 8(x10)
; CHECK:         sd      {{x[0-9]+}}, 0(x10)
define i128 @add(i128 %a, i128 %b) {
  %r = add i128 %a, %b
  ret i128 %r
}

; Subtraction borrows through sltu (high < low means a borrow).
; CHECK-LABEL: sub:
; CHECK:         sub     {{x[0-9]+}}, {{x[0-9]+}}, {{x[0-9]+}}
; CHECK:         sltu    {{x[0-9]+}}, {{x[0-9]+}}, {{x[0-9]+}}
; CHECK:         sub     {{x[0-9]+}}, {{x[0-9]+}}, {{x[0-9]+}}
define i128 @sub(i128 %a, i128 %b) {
  %r = sub i128 %a, %b
  ret i128 %r
}

; The 128-bit product needs the high half of the cross products, which is
; exactly what mulhu provides.
; CHECK-LABEL: mul:
; CHECK-DAG:     mulhu   {{x[0-9]+}}, {{x[0-9]+}}, {{x[0-9]+}}
; CHECK-DAG:     mul     {{x[0-9]+}}, {{x[0-9]+}}, {{x[0-9]+}}
; CHECK:         sd      {{x[0-9]+}}, 8(x10)
define i128 @mul(i128 %a, i128 %b) {
  %r = mul i128 %a, %b
  ret i128 %r
}

; A variable shift amount goes through the generic "shift parts" expansion:
; the high and low halves are shifted separately and ORed together.
; CHECK-LABEL: shl:
; CHECK-DAG:     sll
; CHECK-DAG:     srl
; CHECK:         or
define i128 @shl(i128 %a, i128 %s) {
  %r = shl i128 %a, %s
  ret i128 %r
}

; CHECK-LABEL: lshr:
; CHECK-DAG:     srl
; CHECK-DAG:     sll
; CHECK:         or
define i128 @lshr(i128 %a, i128 %s) {
  %r = lshr i128 %a, %s
  ret i128 %r
}

; CHECK-LABEL: ashr:
; CHECK-DAG:     sra
; CHECK-DAG:     srai    {{x[0-9]+}}, {{x[0-9]+}}, 63
; CHECK:         or
define i128 @ashr(i128 %a, i128 %s) {
  %r = ashr i128 %a, %s
  ret i128 %r
}

; Constant shift amounts that cross the 64-bit boundary lower to a plain
; register move; small amounts lower to a funnel shift built from srli/slli.
; CHECK-LABEL: shl64:
; CHECK-NOT:     slli
; CHECK:         sd      {{x[0-9]+}}, 8(x10)
define i128 @shl64(i128 %a) {
  %r = shl i128 %a, 64
  ret i128 %r
}

; CHECK-LABEL: lshr4:
; CHECK:         srli    {{x[0-9]+}}, {{x[0-9]+}}, 4
; CHECK:         slli    {{x[0-9]+}}, {{x[0-9]+}}, 60
; CHECK:         or
define i128 @lshr4(i128 %a) {
  %r = lshr i128 %a, 4
  ret i128 %r
}

; CHECK-LABEL: ashr100:
; CHECK:         srai    {{x[0-9]+}}, {{x[0-9]+}}, 63
; CHECK:         srai    {{x[0-9]+}}, {{x[0-9]+}}, 36
define i128 @ashr100(i128 %a) {
  %r = ashr i128 %a, 100
  ret i128 %r
}

; The object file must carry the same instructions the assembly printer emits.
; OBJ-LABEL: <add>:
; OBJ:         sltu
; OBJ-LABEL: <mul>:
; OBJ-DAG:     mulhu
; OBJ-LABEL: <ashr100>:
; OBJ:         srai
