; 第 2 篇 §0：另一个方向的边界。SSE2 下 vXi8 的 lane 上限也是 16，
; 32 lane 会被拆成两条 16 lane 的 G_ADD（FewerElements）。
;
; RUN: %llc -global-isel -O0 -mtriple=x86_64-linux-gnu -debug-only=legalizer-info -stop-after=legalizer %s -o /dev/null 2>&1 | %FileCheck %s --check-prefix=TRACE
; RUN: %llc -global-isel -O0 -mtriple=x86_64-linux-gnu -stop-after=legalizer %s -o - | %FileCheck %s --check-prefix=MIR

; TRACE: Applying legalizer ruleset to: Opcode=55, Tys={<32 x s8>, }
; TRACE: .. .. FewerElements, 0, <16 x s8>

; MIR-COUNT-2: %{{[0-9]+}}:_(<16 x s8>) = G_ADD

define <32 x i8> @add_v32i8(<32 x i8> %a, <32 x i8> %b) {
  %r = add <32 x i8> %a, %b
  ret <32 x i8> %r
}
