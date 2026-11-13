; RUN: llc -mtriple=minirv64 -O2 < %s | FileCheck %s

; MiniRV has no unwinder, so the codegen pipeline runs LowerInvoke: an invoke
; becomes a plain call, its landing pad is unreachable and is removed, and the
; personality routine is not referenced.

declare void @may_throw()
declare i32 @__gxx_personality_v0(...)

; CHECK-LABEL: invoke_test:
; CHECK:       jal     x1, may_throw
; CHECK-NOT:   lpad
; CHECK-NOT:   __gxx_personality
; CHECK:       jalr    x0, 0(x1)
define void @invoke_test() personality ptr @__gxx_personality_v0 {
entry:
  invoke void @may_throw() to label %ok unwind label %lpad
ok:
  ret void
lpad:
  %lp = landingpad { ptr, i32 } cleanup
  resume { ptr, i32 } %lp
}
