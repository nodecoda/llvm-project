; 第 2 篇 §0：同样一条 G_ADD，16 lane 一次命中，没有任何动作。
; 与 add-v15i8.ll 配对：输入只差一个 lane，规则命中次数差一次。
;
; RUN: %llc -global-isel -O0 -mtriple=x86_64-linux-gnu -debug-only=legalizer-info -stop-after=legalizer %s -o /dev/null 2>&1 | %FileCheck %s --check-prefix=TRACE
; RUN: %llc -global-isel -O0 -mtriple=x86_64-linux-gnu -stop-after=legalizer %s -o - | %FileCheck %s --check-prefix=MIR

; TRACE: Applying legalizer ruleset to: Opcode=55, Tys={<16 x s8>, }
; TRACE: .. .. Legal, 0, LLT_invalid
; TRACE-NOT: MoreElements
; TRACE-NOT: FewerElements

; MIR: %{{[0-9]+}}:_(<16 x s8>) = G_ADD

define <16 x i8> @add_v16i8(<16 x i8> %a, <16 x i8> %b) {
  %r = add <16 x i8> %a, %b
  ret <16 x i8> %r
}
