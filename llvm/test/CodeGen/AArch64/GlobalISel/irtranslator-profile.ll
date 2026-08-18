; REQUIRES: asserts
; RUN: llc -mtriple=aarch64 -O0 -global-isel=1 -global-isel-abort=1 \
; RUN:   --save-stats=obj -o %t.s %s
; RUN: FileCheck %s < %t.stats

; CHECK: "gisel-irtranslator.i63616C6C7C{{[0-9A-F]+}}": 1
; CHECK: "gisel-irtranslator.i696E766F6B657C{{[0-9A-F]+}}": 1
; CHECK: "gisel-irtranslator-call.c63616C6C7C{{[0-9A-F]+}}": 1
; CHECK: "gisel-irtranslator-call.c696E766F6B657C{{[0-9A-F]+}}": 1

declare i64 @callee(ptr, i32)
declare void @sink()
declare i32 @__gxx_personality_v0(...)

define i64 @caller(ptr %p, i32 %x) {
  call void @sink()
  %result = call i64 @callee(ptr %p, i32 %x)
  ret i64 %result
}

define void @invoke_caller() personality ptr @__gxx_personality_v0 {
entry:
  invoke void @sink() to label %normal unwind label %unwind

normal:
  ret void

unwind:
  %landingpad = landingpad { ptr, i32 } cleanup
  resume { ptr, i32 } %landingpad
}
