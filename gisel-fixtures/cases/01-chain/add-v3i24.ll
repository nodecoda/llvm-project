; 第 1 篇 §1：真的锉了两刀。<3 x i24> 先被 scalarize 成 s24（FewerElements 到标量），
; 再 widen 成 s32；每一步之后都要重新查一遍规则。
;
; RUN: %llc -global-isel -O0 -mtriple=x86_64-linux-gnu -debug-only=legalizer-info -stop-after=legalizer %s -o /dev/null 2>&1 | %FileCheck %s --check-prefix=TRACE
; RUN: %llc -global-isel -O0 -mtriple=x86_64-linux-gnu -stop-after=legalizer %s -o - | %FileCheck %s --check-prefix=MIR

; TRACE: Applying legalizer ruleset to: Opcode=55, Tys={<3 x s24>, }
; TRACE: .. .. FewerElements, 0, s24
; TRACE: Applying legalizer ruleset to: Opcode=55, Tys={s24, }
; TRACE: .. .. WidenScalar, 0, s32
; TRACE: Applying legalizer ruleset to: Opcode=55, Tys={s32, }
; TRACE: .. .. Legal, 0, LLT_invalid

; MIR-COUNT-3: %{{[0-9]+}}:_(s32) = G_ADD

define <3 x i24> @add_v3i24(<3 x i24> %a, <3 x i24> %b) {
  %r = add <3 x i24> %a, %b
  ret <3 x i24> %r
}
