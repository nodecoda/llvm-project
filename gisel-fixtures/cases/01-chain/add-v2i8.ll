; 第 1 篇 §1：一条指令可以被反复锉。
; X86 的 G_ADD 对 vXi8 的 lane 下限写死是 16（clampMinNumElements(0, s8, 16)），
; 所以 <2 x i8> 不是"补到下一个 2 的幂"，而是直接被拉到 16 lane。
;
; RUN: %llc -global-isel -O0 -mtriple=x86_64-linux-gnu -debug-only=legalizer-info -stop-after=legalizer %s -o /dev/null 2>&1 | %FileCheck %s --check-prefix=TRACE
; RUN: %llc -global-isel -O0 -mtriple=x86_64-linux-gnu -stop-after=legalizer %s -o - | %FileCheck %s --check-prefix=MIR

; TRACE: Applying legalizer ruleset to: Opcode=55, Tys={<2 x s8>, }
; TRACE: .. .. MoreElements, 0, <16 x s8>
; TRACE: Applying legalizer ruleset to: Opcode=55, Tys={<16 x s8>, }
; TRACE: .. .. Legal, 0, LLT_invalid

; MIR: %{{[0-9]+}}:_(<16 x s8>) = G_ADD

define <2 x i8> @add_v2i8(<2 x i8> %a, <2 x i8> %b) {
  %r = add <2 x i8> %a, %b
  ret <2 x i8> %r
}
