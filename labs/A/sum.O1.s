--- |
  ; ModuleID = 'sum.O1.ll'
  source_filename = "sum.c"
  target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
  target triple = "x86_64-unknown-linux-gnu"
  
  ; Function Attrs: nofree norecurse nosync nounwind memory(argmem: read) uwtable
  define dso_local i32 @sum(ptr nofree noundef readonly captures(none) %a, i32 noundef %n) local_unnamed_addr #0 {
  entry:
    %cmp4 = icmp sgt i32 %n, 0
    br i1 %cmp4, label %for.body.preheader, label %for.cond.cleanup
  
  for.body.preheader:                               ; preds = %entry
    %wide.trip.count = zext nneg i32 %n to i64
    br label %for.body
  
  for.cond.cleanup:                                 ; preds = %for.body, %entry
    %s.0.lcssa = phi i32 [ 0, %entry ], [ %add, %for.body ]
    ret i32 %s.0.lcssa
  
  for.body:                                         ; preds = %for.body, %for.body.preheader
    %indvars.iv = phi i64 [ 0, %for.body.preheader ], [ %indvars.iv.next, %for.body ]
    %s.05 = phi i32 [ 0, %for.body.preheader ], [ %add, %for.body ]
    %0 = shl nuw nsw i64 %indvars.iv, 2
    %scevgep = getelementptr i8, ptr %a, i64 %0
    %1 = load i32, ptr %scevgep, align 4, !tbaa !10
    %add = add nsw i32 %1, %s.05
    %indvars.iv.next = add nuw nsw i64 %indvars.iv, 1
    %exitcond.not = icmp eq i64 %wide.trip.count, %indvars.iv.next
    br i1 %exitcond.not, label %for.cond.cleanup, label %for.body, !llvm.loop !11
  }
  
  attributes #0 = { nofree norecurse nosync nounwind memory(argmem: read) uwtable "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
  
  !llvm.module.flags = !{!0, !1, !2, !3}
  !llvm.ident = !{!4}
  !llvm.errno.tbaa = !{!5}
  
  !0 = !{i32 2, !"debugger-tuning", !"gdb"}
  !1 = !{i32 8, !"PIC Level", i32 2}
  !2 = !{i32 7, !"PIE Level", i32 2}
  !3 = !{i32 7, !"uwtable", i32 2}
  !4 = !{!"clang version 24.0.0git (https://github.com/nodecoda/llvm-project.git 756265e4ce650c26861c4850668e6d42022edcbc)"}
  !5 = !{!6, !7, i64 0}
  !6 = !{!"__libc_errno", !7, i64 0}
  !7 = !{!"int", !8, i64 0}
  !8 = !{!"omnipotent char", !9, i64 0}
  !9 = !{!"Simple C/C++ TBAA"}
  !10 = !{!7, !7, i64 0}
  !11 = distinct !{!11, !12, !13}
  !12 = !{!"llvm.loop.mustprogress"}
  !13 = !{!"llvm.loop.unroll.disable"}
...
---
name:            sum
alignment:       1
exposesReturnsTwice: false
legalized:       true
regBankSelected: false
selected:        false
failedISel:      false
tracksRegLiveness: true
hasWinCFI:       false
noPhis:          false
isSSA:           true
noVRegs:         false
hasFakeUses:     false
callsEHReturn:   false
callsUnwindInit: false
hasEHContTarget: false
hasEHScopes:     false
hasEHFunclets:   false
isOutlined:      false
debugInstrRef:   false
failsVerification: false
tracksDebugUserValues: false
registers:
  - { id: 0, class: _, preferred-register: '', flags: [  ] }
  - { id: 1, class: _, preferred-register: '', flags: [  ] }
  - { id: 2, class: _, preferred-register: '', flags: [  ] }
  - { id: 3, class: _, preferred-register: '', flags: [  ] }
  - { id: 4, class: _, preferred-register: '', flags: [  ] }
  - { id: 5, class: _, preferred-register: '', flags: [  ] }
  - { id: 6, class: _, preferred-register: '', flags: [  ] }
  - { id: 7, class: _, preferred-register: '', flags: [  ] }
  - { id: 8, class: _, preferred-register: '', flags: [  ] }
  - { id: 9, class: _, preferred-register: '', flags: [  ] }
  - { id: 10, class: _, preferred-register: '', flags: [  ] }
  - { id: 11, class: _, preferred-register: '', flags: [  ] }
  - { id: 12, class: _, preferred-register: '', flags: [  ] }
  - { id: 13, class: _, preferred-register: '', flags: [  ] }
  - { id: 14, class: _, preferred-register: '', flags: [  ] }
  - { id: 15, class: _, preferred-register: '', flags: [  ] }
  - { id: 16, class: _, preferred-register: '', flags: [  ] }
  - { id: 17, class: _, preferred-register: '', flags: [  ] }
  - { id: 18, class: _, preferred-register: '', flags: [  ] }
  - { id: 19, class: _, preferred-register: '', flags: [  ] }
liveins:
  - { reg: '$rdi', virtual-reg: '' }
  - { reg: '$esi', virtual-reg: '' }
frameInfo:
  isFrameAddressTaken: false
  isReturnAddressTaken: false
  hasStackMap:     false
  hasPatchPoint:   false
  stackSize:       0
  offsetAdjustment: 0
  maxAlignment:    1
  adjustsStack:    false
  hasCalls:        false
  framePointerPolicy: none
  stackProtector:  ''
  functionContext: ''
  maxCallFrameSize: 4294967295
  cvBytesOfCalleeSavedRegisters: 0
  hasOpaqueSPAdjustment: false
  hasVAStart:      false
  hasMustTailInVarArgFunc: false
  hasTailCall:     false
  isCalleeSavedInfoValid: false
  localFrameSize:  0
fixedStack:      []
stack:           []
entry_values:    []
callSites:       []
debugValueSubstitutions: []
constants:       []
machineFunctionInfo:
  amxProgModel:    None
body:             |
  bb.1.entry:
    successors: %bb.2(0x50000000), %bb.3(0x30000000)
    liveins: $esi, $rdi
  
    %0:_(p0) = COPY $rdi
    %1:_(s32) = COPY $esi
    %2:_(s32) = G_CONSTANT i32 0
    %12:_(s64) = G_CONSTANT i64 1
    %16:_(s64) = G_CONSTANT i64 0
    %19:_(s8) = G_ICMP intpred(sgt), %1(s32), %2
    %3:_(s1) = G_TRUNC %19(s8)
    G_BRCOND %3(s1), %bb.2
    G_BR %bb.3
  
  bb.2.for.body.preheader:
    successors: %bb.4(0x80000000)
  
    %4:_(s64) = nneg G_ZEXT %1(s32)
    G_BR %bb.4
  
  bb.3.for.cond.cleanup:
    %15:_(s32) = G_PHI %2(s32), %bb.1, %11(s32), %bb.4
    $eax = COPY %15(s32)
    RET 0, implicit $eax
  
  bb.4.for.body:
    successors: %bb.3(0x04000000), %bb.4(0x7c000000)
  
    %5:_(s64) = G_PHI %16(s64), %bb.2, %13(s64), %bb.4
    %6:_(s32) = G_PHI %2(s32), %bb.2, %11(s32), %bb.4
    %18:_(s8) = G_CONSTANT i8 2
    %8:_(s64) = nuw nsw G_SHL %5, %18(s8)
    %9:_(p0) = G_PTR_ADD %0, %8(s64)
    %10:_(s32) = G_LOAD %9(p0) :: (load (s32) from %ir.scevgep, !tbaa !10)
    %11:_(s32) = nsw G_ADD %10, %6
    %13:_(s64) = nuw nsw G_ADD %5, %12
    %17:_(s8) = G_ICMP intpred(eq), %4(s64), %13
    %14:_(s1) = G_TRUNC %17(s8)
    G_BRCOND %14(s1), %bb.3
    G_BR %bb.4
...
