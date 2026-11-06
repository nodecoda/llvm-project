; 第 2 篇 §1：类型元组里没有的一根轴——立即数。
; RISC-V 的 G_SEXT_INREG 同时读槽 0 的类型和 imm 值：
;   imm 32            → Legal（任何配置）
;   imm 8 / 16        → 只有 +zbb 才 Legal，否则 Lower
;   imm 24            → 永远 Lower
; 四个函数的 Tys 全是 {s64, }，答案却分成三类。只看类型，分不出来。
;
; RUN: %llc -global-isel -O0 -mtriple=riscv64-linux-gnu -debug-only=legalizer-info -stop-after=legalizer %s -o /dev/null 2>&1 | %FileCheck %s --check-prefix=NOSEXT
; RUN: %llc -global-isel -O0 -mtriple=riscv64-linux-gnu -mattr=+zbb -debug-only=legalizer-info -stop-after=legalizer %s -o /dev/null 2>&1 | %FileCheck %s --check-prefix=ZBB

; NOSEXT: Applying legalizer ruleset to: Opcode=153, Tys={s64, }, MMOs={}, Imms={8, }
; NOSEXT: .. .. Lower, 0, LLT_invalid
; NOSEXT: Applying legalizer ruleset to: Opcode=153, Tys={s64, }, MMOs={}, Imms={16, }
; NOSEXT: .. .. Lower, 0, LLT_invalid
; NOSEXT: Applying legalizer ruleset to: Opcode=153, Tys={s64, }, MMOs={}, Imms={24, }
; NOSEXT: .. .. Lower, 0, LLT_invalid
; NOSEXT: Applying legalizer ruleset to: Opcode=153, Tys={s64, }, MMOs={}, Imms={32, }
; NOSEXT: .. .. Legal, 0, LLT_invalid

; ZBB: Applying legalizer ruleset to: Opcode=153, Tys={s64, }, MMOs={}, Imms={8, }
; ZBB: .. .. Legal, 0, LLT_invalid
; ZBB: Applying legalizer ruleset to: Opcode=153, Tys={s64, }, MMOs={}, Imms={16, }
; ZBB: .. .. Legal, 0, LLT_invalid
; ZBB: Applying legalizer ruleset to: Opcode=153, Tys={s64, }, MMOs={}, Imms={24, }
; ZBB: .. .. Lower, 0, LLT_invalid
; ZBB: Applying legalizer ruleset to: Opcode=153, Tys={s64, }, MMOs={}, Imms={32, }
; ZBB: .. .. Legal, 0, LLT_invalid

define i64 @sext_i8(i8 %a) {
  %e = sext i8 %a to i64
  ret i64 %e
}

define i64 @sext_i16(i16 %a) {
  %e = sext i16 %a to i64
  ret i64 %e
}

define i64 @sext_i24(i24 %a) {
  %e = sext i24 %a to i64
  ret i64 %e
}

define i64 @sext_i32(i32 %a) {
  %e = sext i32 %a to i64
  ret i64 %e
}
