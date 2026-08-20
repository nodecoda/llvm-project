; REQUIRES: asserts
; RUN: llc -mtriple=aarch64 -O0 -global-isel=1 -global-isel-abort=1 \
; RUN:   --save-stats=obj -o %t.s %s
; RUN: FileCheck %s < %t.stats

; CHECK: "gisel-irtranslator.i63616C6C7C{{[0-9A-F]+}}": 1
; CHECK: "gisel-irtranslator.i696E766F6B657C{{[0-9A-F]+}}": 1
; CHECK: "gisel-irtranslator-call.c63616C6C7C{{[0-9A-F]+}}": 1
; CHECK: "gisel-irtranslator-call.c696E766F6B657C{{[0-9A-F]+}}": 1
; CHECK: "gisel-irtranslator-gep.AllZero": 1
; CHECK: "gisel-irtranslator-gep.ConstantOffsetOnly": 2
; CHECK: "gisel-irtranslator-gep.MultipleDynamic": 2
; CHECK: "gisel-irtranslator-gep.OneDynamicScaled": 2
; CHECK: "gisel-irtranslator-gep.OneDynamicUnitStride": 1
; CHECK: "gisel-irtranslator-gep-i8.SingleIndexConstant": 1
; CHECK: "gisel-irtranslator-gep-i8.SingleIndexDynamic": 1
; CHECK: "gisel-irtranslator-gep-ptr-adds.NoPtrAdds": 1
; CHECK: "gisel-irtranslator-gep-ptr-adds.OnePtrAdd": 4
; CHECK: "gisel-irtranslator-gep-ptr-adds.ThreeOrMorePtrAdds": 1
; CHECK: "gisel-irtranslator-gep-ptr-adds.TwoPtrAdds": 2

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

define void @geps(ptr %base, i64 %i, i64 %j, i64 %k) {
  %zero = getelementptr [4 x i32], ptr %base, i64 0, i64 0
  %constant = getelementptr [4 x i32], ptr %base, i64 0, i64 3
  %i8.constant = getelementptr i8, ptr %base, i64 12
  %i8.dynamic = getelementptr i8, ptr %base, i64 %i
  %scaled = getelementptr i32, ptr %base, i64 %i
  %multiple = getelementptr [16 x i32], ptr %base, i64 %i, i64 %j
  %two.adds = getelementptr [16 x i32], ptr %base, i64 1, i64 %i
  %three.adds = getelementptr [4 x [8 x i32]], ptr %base, i64 %i, i64 %j,
    i64 %k
  ret void
}
