; 第 2 篇 §0：15 和 16 只差一个 lane，待遇不同——15 要补到 16。
;
; RUN: %llc -global-isel -O0 -mtriple=x86_64-linux-gnu -debug-only=legalizer-info -stop-after=legalizer %s -o /dev/null 2>&1 | %FileCheck %s --check-prefix=TRACE
; RUN: %llc -global-isel -O0 -mtriple=x86_64-linux-gnu -stop-after=legalizer %s -o - | %FileCheck %s --check-prefix=MIR

; TRACE: Applying legalizer ruleset to: Opcode=55, Tys={<15 x s8>, }
; TRACE: .. .. MoreElements, 0, <16 x s8>
; TRACE: Applying legalizer ruleset to: Opcode=55, Tys={<16 x s8>, }

; MIR: %{{[0-9]+}}:_(<16 x s8>) = G_ADD

define <15 x i8> @add_v15i8(<15 x i8> %a, <15 x i8> %b) {
  %r = add <15 x i8> %a, %b
  ret <15 x i8> %r
}
