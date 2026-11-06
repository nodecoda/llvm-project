; 第 2 篇 §1：地址空间是轴，不是注解。
; 同样是 s32 的 load：p0 一次命中，p272 直接 "unable to legalize"。
; 两个函数只差地址空间，规则的答案从 Legal 变成 abort。
;
; RUN: not %llc -global-isel -O0 -mtriple=x86_64-linux-gnu -debug-only=legalizer-info -stop-after=legalizer %s -o /dev/null 2>&1 | %FileCheck %s

; CHECK: Applying legalizer ruleset to: Opcode=99, Tys={s32, p0, }
; CHECK: .. .. Legal, 0, LLT_invalid
; CHECK: Applying legalizer ruleset to: Opcode=99, Tys={s32, p272, }
; CHECK: LLVM ERROR: unable to legalize instruction: %{{[0-9]+}}:_(s32) = G_LOAD

define i32 @load_p0(ptr %p) {
  %v = load i32, ptr %p
  ret i32 %v
}

define i32 @load_p272(ptr addrspace(272) %p) {
  %v = load i32, ptr addrspace(272) %p
  ret i32 %v
}
