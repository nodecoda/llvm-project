; 第 2 篇 §1：配置是坐标的一部分，不是环境变量。
; 同一条 IR、同一个 <32 x i8>，SSE2 基线要拆，+avx2 一次命中。
;
; RUN: %llc -global-isel -O0 -mtriple=x86_64-linux-gnu -debug-only=legalizer-info -stop-after=legalizer %s -o /dev/null 2>&1 | %FileCheck %s --check-prefix=SSE2
; RUN: %llc -global-isel -O0 -mtriple=x86_64-linux-gnu -mattr=+avx2 -debug-only=legalizer-info -stop-after=legalizer %s -o /dev/null 2>&1 | %FileCheck %s --check-prefix=AVX2

; SSE2: Applying legalizer ruleset to: Opcode=55, Tys={<32 x s8>, }
; SSE2: .. .. FewerElements, 0, <16 x s8>

; AVX2: Applying legalizer ruleset to: Opcode=55, Tys={<32 x s8>, }
; AVX2: .. .. Legal, 0, LLT_invalid
; AVX2-NOT: FewerElements
; AVX2-NOT: MoreElements

define <32 x i8> @add_v32i8(<32 x i8> %a, <32 x i8> %b) {
  %r = add <32 x i8> %a, %b
  ret <32 x i8> %r
}
