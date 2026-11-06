; 第 6 篇 §0：同一条 IR、同一个 opcode，三个后端的 legalizer 给出三个答案。
;   X86    : s32 直接 Legal
;   AArch64: i32 直接 Legal（注意 LLT 打印成 i32 而不是 s32）
;   RISCV  : Custom —— 扩到 s64 算，再用 G_SEXT_INREG 截回 32 位
;
; RUN: %llc -global-isel -O0 -mtriple=x86_64-linux-gnu -debug-only=legalizer-info -stop-after=legalizer %s -o /dev/null 2>&1 | %FileCheck %s --check-prefix=X86
; RUN: %llc -global-isel -O0 -mtriple=aarch64-linux-gnu -debug-only=legalizer-info -stop-after=legalizer %s -o /dev/null 2>&1 | %FileCheck %s --check-prefix=A64
; RUN: %llc -global-isel -O0 -mtriple=riscv64-linux-gnu -debug-only=legalizer-info -stop-after=legalizer %s -o /dev/null 2>&1 | %FileCheck %s --check-prefix=RISCV
; RUN: %llc -global-isel -O0 -mtriple=riscv64-linux-gnu -stop-after=legalizer %s -o - | %FileCheck %s --check-prefix=RISCV-MIR

; X86: Applying legalizer ruleset to: Opcode=55, Tys={s32, }
; X86-NEXT: .. match
; X86-NEXT: .. .. Legal, 0, LLT_invalid

; A64: Applying legalizer ruleset to: Opcode=55, Tys={i32, }
; A64-NEXT: .. match
; A64-NEXT: .. .. Legal, 0, LLT_invalid

; RISCV: Applying legalizer ruleset to: Opcode=55, Tys={s32, }
; RISCV: .. .. Custom, 0, LLT_invalid
; RISCV: Applying legalizer ruleset to: Opcode=55, Tys={s64, }

; RISCV-MIR: %{{[0-9]+}}:_(s64) = G_SEXT_INREG {{%[0-9]+}}, 32

define i32 @add_i32(i32 %a, i32 %b) {
  %r = add i32 %a, %b
  ret i32 %r
}
