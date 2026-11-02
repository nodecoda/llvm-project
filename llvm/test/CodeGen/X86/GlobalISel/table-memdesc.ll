; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -x86-gisel-table \
; RUN:     -x86-gisel-table-stats -o /dev/null 2>&1 | FileCheck %s
; RUN: llc < %s -mtriple=i386-unknown-linux-gnu -x86-gisel-table \
; RUN:     -x86-gisel-table-stats -o /dev/null 2>&1 | FileCheck %s

; Loads and stores with a memory descriptor are tabulated: the key carries the
; descriptor's LLT (matched by identity, not by size, because G_STORE asks
; whether the value type is the memory type) and its alignment class.  These
; queries used to be untabulated, so every one of them walked the rule chain.
;
; A non-zero escape, outside or untabulated count means the table declined a
; query the rules can answer.  The hit count is not checked: it depends on how
; often the legalizer revisits a value.

; CHECK: x86-gisel-table: hit {{[0-9]+}} escape 0 outside 0 ptrvec 0 untabulated 0

define void @copy8(ptr %d, ptr %s) {
  %v = load i8, ptr %s
  store i8 %v, ptr %d
  ret void
}

define void @copy16(ptr %d, ptr %s) {
  %v = load i16, ptr %s
  store i16 %v, ptr %d
  ret void
}

define void @copy64(ptr %d, ptr %s) {
  %v = load i64, ptr %s
  store i64 %v, ptr %d
  ret void
}

define void @copy128(ptr %d, ptr %s) {
  %v = load i128, ptr %s
  store i128 %v, ptr %d
  ret void
}

define void @copyp(ptr %d, ptr %s) {
  %v = load ptr, ptr %s
  store ptr %v, ptr %d
  ret void
}

define void @copy4i32(ptr %d, ptr %s) {
  %v = load <4 x i32>, ptr %s
  store <4 x i32> %v, ptr %d
  ret void
}

define void @copy2i64(ptr %d, ptr %s) {
  %v = load <2 x i64>, ptr %s
  store <2 x i64> %v, ptr %d
  ret void
}

define void @copy8i16(ptr %d, ptr %s) {
  %v = load <8 x i16>, ptr %s
  store <8 x i16> %v, ptr %d
  ret void
}

define void @copy16i8(ptr %d, ptr %s) {
  %v = load <16 x i8>, ptr %s
  store <16 x i8> %v, ptr %d
  ret void
}

define void @misaligned(ptr %p, i64 %v) {
  store i64 %v, ptr %p, align 8
  ret void
}
