; 第 1 篇 §1：位宽轴上的同一件事，标量版本。
; s24 不是 2 的幂，widenScalarToNextPow2 把它锉成 s32；
; 在 X86 的 G_ADD 链里它排第 12 条，前面 11 条都不匹配——顺序本身是规则的一部分。
;
; RUN: %llc -global-isel -O0 -mtriple=x86_64-linux-gnu -debug-only=legalizer-info -stop-after=legalizer %s -o /dev/null 2>&1 | %FileCheck %s --check-prefix=TRACE
; RUN: %llc -global-isel -O0 -mtriple=x86_64-linux-gnu -stop-after=legalizer %s -o - | %FileCheck %s --check-prefix=MIR

; TRACE: Applying legalizer ruleset to: Opcode=55, Tys={s24, }
; TRACE: .. .. WidenScalar, 0, s32
; TRACE: Applying legalizer ruleset to: Opcode=55, Tys={s32, }
; TRACE: .. .. Legal, 0, LLT_invalid

; MIR: %{{[0-9]+}}:_(s32) = G_ADD

define i24 @add_i24(i24 %a, i24 %b) {
  %r = add i24 %a, %b
  ret i24 %r
}
