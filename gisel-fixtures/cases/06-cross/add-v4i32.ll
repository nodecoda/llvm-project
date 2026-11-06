; 第 6 篇 §0：向量版本上差异更大——X86 / AArch64 直接合法，RISCV 直接 abort。
; 这不是"某条规则不同"，是同一个输入在一个后端上根本走不通。
;
; RUN: %llc -global-isel -O0 -mtriple=x86_64-linux-gnu -debug-only=legalizer-info -stop-after=legalizer %s -o /dev/null 2>&1 | %FileCheck %s --check-prefix=X86
; RUN: %llc -global-isel -O0 -mtriple=aarch64-linux-gnu -debug-only=legalizer-info -stop-after=legalizer %s -o /dev/null 2>&1 | %FileCheck %s --check-prefix=A64
; RUN: not %llc -global-isel -O0 -mtriple=riscv64-linux-gnu -stop-after=legalizer %s -o /dev/null 2>&1 | %FileCheck %s --check-prefix=RISCV

; X86: Applying legalizer ruleset to: Opcode=55, Tys={<4 x s32>, }
; X86: .. .. Legal, 0, LLT_invalid

; A64: Applying legalizer ruleset to: Opcode=55, Tys={<4 x i32>, }
; A64: .. .. Legal, 0, LLT_invalid

; RISCV: LLVM ERROR: unable to legalize instruction: %{{[0-9]+}}:_(<4 x s32>) = G_ADD

define <4 x i32> @add_v4i32(<4 x i32> %a, <4 x i32> %b) {
  %r = add <4 x i32> %a, %b
  ret <4 x i32> %r
}
