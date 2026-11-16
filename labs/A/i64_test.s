--- |
  ; ModuleID = 'i64_test.ll'
  source_filename = "i64_test.c"
  target datalayout = "e-m:e-p:32:32-i64:64-n32-S128"
  target triple = "riscv32-unknown-unknown-elf"
  
  ; Function Attrs: noinline nounwind optnone
  define dso_local i64 @add64(i64 noundef %a, i64 noundef %b) #0 {
  entry:
    %a.addr = alloca i64, align 8
    %b.addr = alloca i64, align 8
    store i64 %a, ptr %a.addr, align 8
    store i64 %b, ptr %b.addr, align 8
    %0 = load i64, ptr %a.addr, align 8
    %1 = load i64, ptr %b.addr, align 8
    %add = add i64 %0, %1
    ret i64 %add
  }
  
  ; Function Attrs: noinline nounwind optnone
  define dso_local i32 @cmp64_signed(i64 noundef %a, i64 noundef %b) #0 {
  entry:
    %a.addr = alloca i64, align 8
    %b.addr = alloca i64, align 8
    store i64 %a, ptr %a.addr, align 8
    store i64 %b, ptr %b.addr, align 8
    %0 = load i64, ptr %a.addr, align 8
    %1 = load i64, ptr %b.addr, align 8
    %cmp = icmp slt i64 %0, %1
    %conv = zext i1 %cmp to i32
    ret i32 %conv
  }
  
  ; Function Attrs: noinline nounwind optnone
  define dso_local i64 @shl64(i64 noundef %x, i32 noundef %amt) #0 {
  entry:
    %x.addr = alloca i64, align 8
    %amt.addr = alloca i32, align 4
    store i64 %x, ptr %x.addr, align 8
    store i32 %amt, ptr %amt.addr, align 4
    %0 = load i64, ptr %x.addr, align 8
    %1 = load i32, ptr %amt.addr, align 4
    %sh_prom = zext i32 %1 to i64
    %shl = shl i64 %0, %sh_prom
    ret i64 %shl
  }
  
  ; Function Attrs: noinline nounwind optnone
  define dso_local i64 @mul64(i64 noundef %a, i64 noundef %b) #0 {
  entry:
    %a.addr = alloca i64, align 8
    %b.addr = alloca i64, align 8
    store i64 %a, ptr %a.addr, align 8
    store i64 %b, ptr %b.addr, align 8
    %0 = load i64, ptr %a.addr, align 8
    %1 = load i64, ptr %b.addr, align 8
    %mul = mul i64 %0, %1
    ret i64 %mul
  }
  
  attributes #0 = { noinline nounwind optnone "frame-pointer"="all" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="generic-rv32" "target-features"="+32bit,+a,+c,+i,+m,+relax,+zaamo,+zalrsc,+zca,+zmmul,-b,-d,-e,-experimental-p,-experimental-smcsps,-experimental-smehv,-experimental-smidctrl,-experimental-smijt,-experimental-smip,-experimental-smnip,-experimental-smpmpmt,-experimental-sscsps,-experimental-ssehv,-experimental-ssidctrl,-experimental-ssijt,-experimental-ssip,-experimental-ssnip,-experimental-svukte,-experimental-xqccmi,-experimental-xqccmt,-experimental-xsfmclic,-experimental-xsfsclic,-experimental-y,-experimental-zibi,-experimental-zicfilp,-experimental-zilx,-experimental-zvabd,-experimental-zvbc32e,-experimental-zvdot4a8i,-experimental-zvfbdota32f,-experimental-zvfbfa,-experimental-zvfofp8min,-experimental-zvfqwbdota8f,-experimental-zvfqwdota8f,-experimental-zvfwbdota16bf,-experimental-zvfwdota16bf,-experimental-zvkgs,-experimental-zvqwbdota16i,-experimental-zvqwbdota8i,-experimental-zvqwdota16i,-experimental-zvqwdota8i,-experimental-zvvfmm,-experimental-zvvmm,-experimental-zvvmtls,-experimental-zvvmttls,-experimental-zvzip,-experimental-zyhybrid,-f,-h,-q,-sdext,-sdtrig,-sha,-shcounterenw,-shgatpa,-shlcofideleg,-shtvala,-shvsatpa,-shvstvala,-shvstvecd,-smaia,-smcdeleg,-smcntrpmf,-smcsrind,-smctr,-smdbltrp,-smepmp,-smmpm,-smnpm,-smpmpdeleg,-smrnmi,-smstateen,-ssaia,-ssccfg,-ssccptr,-sscofpmf,-sscounterenw,-sscsrind,-ssctr,-ssdbltrp,-ssnpm,-sspm,-sspmp,-sspmpen,-ssqosid,-ssstateen,-ssstrict,-sstc,-sstvala,-sstvecd,-ssu64xl,-supm,-svade,-svadu,-svbare,-svinval,-svnapot,-svpbmt,-svrsw60t59b,-svvptc,-v,-xaifet,-xandesbfhcvt,-xandesperf,-xandesvbfhcvt,-xandesvdot,-xandesvpackfph,-xandesvsinth,-xandesvsintload,-xcheriot,-xcvalu,-xcvbi,-xcvbitmanip,-xcvelw,-xcvmac,-xcvmem,-xcvsimd,-xdemo,-xmipscbop,-xmipscmov,-xmipsexectl,-xmipslsp,-xqccmp,-xqci,-xqcia,-xqciac,-xqcibi,-xqcibm,-xqcicli,-xqcicm,-xqcics,-xqcicsr,-xqciint,-xqciio,-xqcilb,-xqcili,-xqcilia,-xqcilo,-xqcilsm,-xqcisim,-xqcisls,-xqcisync,-xsfcease,-xsfmm128t,-xsfmm16t,-xsfmm32a,-xsfmm32a16f,-xsfmm32a32f,-xsfmm32a8f,-xsfmm32a8i,-xsfmm32t,-xsfmm64a64f,-xsfmm64t,-xsfmmbase,-xsfvcp,-xsfvfbfexp16e,-xsfvfexp16e,-xsfvfexp32e,-xsfvfexpa,-xsfvfexpa64e,-xsfvfnrclipxfqf,-xsfvfwmaccqqq,-xsfvqmaccdod,-xsfvqmaccqoq,-xsifivecdiscarddlone,-xsifivecflushdlone,-xsmtvdot,-xsmtvdotii,-xtheadba,-xtheadbb,-xtheadbs,-xtheadcmo,-xtheadcondmov,-xtheadfmemidx,-xtheadmac,-xtheadmemidx,-xtheadmempair,-xtheadsync,-xtheadvdot,-xwchc,-za128rs,-za64rs,-zabha,-zacas,-zalasr,-zama16b,-zawrs,-zba,-zbb,-zbc,-zbkb,-zbkc,-zbkx,-zbs,-zcb,-zcd,-zce,-zcf,-zclsd,-zcmop,-zcmp,-zcmt,-zdinx,-zfa,-zfbfmin,-zfh,-zfhmin,-zfinx,-zhinx,-zhinxmin,-zic64b,-zicbom,-zicbop,-zicboz,-ziccamoa,-ziccamoc,-ziccid,-ziccif,-zicclsm,-ziccrse,-zicfiss,-zicntr,-zicond,-zicsr,-zifencei,-zihintntl,-zihintpause,-zihpm,-zilsd,-zimop,-zk,-zkn,-zknd,-zkne,-zknh,-zkr,-zks,-zksed,-zksh,-zkt,-ztso,-zvbb,-zvbc,-zve32f,-zve32x,-zve64d,-zve64f,-zve64x,-zvfbfmin,-zvfbfwma,-zvfh,-zvfhmin,-zvkb,-zvkg,-zvkn,-zvknc,-zvkned,-zvkng,-zvknha,-zvknhb,-zvks,-zvksc,-zvksed,-zvksg,-zvksh,-zvkt,-zvl1024b,-zvl128b,-zvl16384b,-zvl2048b,-zvl256b,-zvl32768b,-zvl32b,-zvl4096b,-zvl512b,-zvl64b,-zvl65536b,-zvl8192b" }
  
  !llvm.module.flags = !{!0, !1, !2, !4, !5}
  !llvm.ident = !{!6}
  
  !0 = !{i32 2, !"debugger-tuning", !"gdb"}
  !1 = !{i32 1, !"target-abi", !"ilp32"}
  !2 = !{i32 6, !"riscv-isa", !3}
  !3 = !{!"rv32i2p1_m2p0_a2p1_c2p0_zmmul1p0_zaamo1p0_zalrsc1p0_zca1p0"}
  !4 = !{i32 7, !"frame-pointer", i32 2}
  !5 = !{i32 8, !"SmallDataLimit", i32 0}
  !6 = !{!"clang version 24.0.0git (https://github.com/nodecoda/llvm-project.git 756265e4ce650c26861c4850668e6d42022edcbc)"}
...
---
name:            add64
alignment:       2
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
  - { id: 20, class: _, preferred-register: '', flags: [  ] }
  - { id: 21, class: _, preferred-register: '', flags: [  ] }
  - { id: 22, class: _, preferred-register: '', flags: [  ] }
  - { id: 23, class: _, preferred-register: '', flags: [  ] }
  - { id: 24, class: _, preferred-register: '', flags: [  ] }
  - { id: 25, class: _, preferred-register: '', flags: [  ] }
  - { id: 26, class: _, preferred-register: '', flags: [  ] }
  - { id: 27, class: _, preferred-register: '', flags: [  ] }
  - { id: 28, class: _, preferred-register: '', flags: [  ] }
  - { id: 29, class: _, preferred-register: '', flags: [  ] }
  - { id: 30, class: _, preferred-register: '', flags: [  ] }
  - { id: 31, class: _, preferred-register: '', flags: [  ] }
  - { id: 32, class: _, preferred-register: '', flags: [  ] }
  - { id: 33, class: _, preferred-register: '', flags: [  ] }
  - { id: 34, class: _, preferred-register: '', flags: [  ] }
  - { id: 35, class: _, preferred-register: '', flags: [  ] }
  - { id: 36, class: _, preferred-register: '', flags: [  ] }
  - { id: 37, class: _, preferred-register: '', flags: [  ] }
  - { id: 38, class: _, preferred-register: '', flags: [  ] }
  - { id: 39, class: _, preferred-register: '', flags: [  ] }
  - { id: 40, class: _, preferred-register: '', flags: [  ] }
  - { id: 41, class: _, preferred-register: '', flags: [  ] }
  - { id: 42, class: _, preferred-register: '', flags: [  ] }
liveins:
  - { reg: '$x10', virtual-reg: '' }
  - { reg: '$x11', virtual-reg: '' }
  - { reg: '$x12', virtual-reg: '' }
  - { reg: '$x13', virtual-reg: '' }
frameInfo:
  isFrameAddressTaken: false
  isReturnAddressTaken: false
  hasStackMap:     false
  hasPatchPoint:   false
  stackSize:       0
  offsetAdjustment: 0
  maxAlignment:    8
  adjustsStack:    false
  hasCalls:        false
  framePointerPolicy: all
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
stack:
  - { id: 0, name: a.addr, type: default, offset: 0, size: 8, alignment: 8, 
      stack-id: default, callee-saved-register: '', callee-saved-restored: true, 
      debug-info-variable: '', debug-info-expression: '', debug-info-location: '' }
  - { id: 1, name: b.addr, type: default, offset: 0, size: 8, alignment: 8, 
      stack-id: default, callee-saved-register: '', callee-saved-restored: true, 
      debug-info-variable: '', debug-info-expression: '', debug-info-location: '' }
entry_values:    []
callSites:       []
debugValueSubstitutions: []
constants:       []
machineFunctionInfo:
  varArgsFrameIndex: 0
  varArgsSaveSize: 0
body:             |
  bb.1.entry:
    liveins: $x10, $x11, $x12, $x13
  
    %2:_(s32) = COPY $x10
    %3:_(s32) = COPY $x11
    %4:_(s32) = COPY $x12
    %5:_(s32) = COPY $x13
    %6:_(p0) = G_FRAME_INDEX %stack.0.a.addr
    %7:_(p0) = G_FRAME_INDEX %stack.1.b.addr
    G_STORE %2(s32), %6(p0) :: (store (s32) into %ir.a.addr, align 8)
    %32:_(s32) = G_CONSTANT i32 4
    %35:_(p0) = nuw inbounds G_PTR_ADD %6, %32(s32)
    %42:_(p0) = COPY %35(p0)
    G_STORE %3(s32), %42(p0) :: (store (s32) into %ir.a.addr + 4, basealign 8)
    G_STORE %4(s32), %7(p0) :: (store (s32) into %ir.b.addr, align 8)
    %31:_(p0) = nuw inbounds G_PTR_ADD %7, %32(s32)
    %39:_(p0) = COPY %31(p0)
    G_STORE %5(s32), %39(p0) :: (store (s32) into %ir.b.addr + 4, basealign 8)
    %34:_(s32) = G_LOAD %6(p0) :: (dereferenceable load (s32) from %ir.a.addr, align 8)
    %36:_(s32) = G_LOAD %35(p0) :: (dereferenceable load (s32) from %ir.a.addr + 4, basealign 8)
    %30:_(s32) = G_LOAD %7(p0) :: (dereferenceable load (s32) from %ir.b.addr, align 8)
    %33:_(s32) = G_LOAD %31(p0) :: (dereferenceable load (s32) from %ir.b.addr + 4, basealign 8)
    %28:_(s32) = G_ADD %34, %30
    %29:_(s32) = G_ICMP intpred(ult), %28(s32), %30
    %17:_(s32) = COPY %28(s32)
    %22:_(s32) = G_ADD %36, %33
    %21:_(s32) = G_ADD %22, %29
    %19:_(s32) = COPY %21(s32)
    $x10 = COPY %17(s32)
    $x11 = COPY %19(s32)
    PseudoRET implicit $x10, implicit $x11
...
---
name:            cmp64_signed
alignment:       2
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
  - { id: 20, class: _, preferred-register: '', flags: [  ] }
  - { id: 21, class: _, preferred-register: '', flags: [  ] }
  - { id: 22, class: _, preferred-register: '', flags: [  ] }
  - { id: 23, class: _, preferred-register: '', flags: [  ] }
  - { id: 24, class: _, preferred-register: '', flags: [  ] }
  - { id: 25, class: _, preferred-register: '', flags: [  ] }
  - { id: 26, class: _, preferred-register: '', flags: [  ] }
  - { id: 27, class: _, preferred-register: '', flags: [  ] }
  - { id: 28, class: _, preferred-register: '', flags: [  ] }
  - { id: 29, class: _, preferred-register: '', flags: [  ] }
  - { id: 30, class: _, preferred-register: '', flags: [  ] }
  - { id: 31, class: _, preferred-register: '', flags: [  ] }
  - { id: 32, class: _, preferred-register: '', flags: [  ] }
  - { id: 33, class: _, preferred-register: '', flags: [  ] }
  - { id: 34, class: _, preferred-register: '', flags: [  ] }
  - { id: 35, class: _, preferred-register: '', flags: [  ] }
  - { id: 36, class: _, preferred-register: '', flags: [  ] }
  - { id: 37, class: _, preferred-register: '', flags: [  ] }
  - { id: 38, class: _, preferred-register: '', flags: [  ] }
liveins:
  - { reg: '$x10', virtual-reg: '' }
  - { reg: '$x11', virtual-reg: '' }
  - { reg: '$x12', virtual-reg: '' }
  - { reg: '$x13', virtual-reg: '' }
frameInfo:
  isFrameAddressTaken: false
  isReturnAddressTaken: false
  hasStackMap:     false
  hasPatchPoint:   false
  stackSize:       0
  offsetAdjustment: 0
  maxAlignment:    8
  adjustsStack:    false
  hasCalls:        false
  framePointerPolicy: all
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
stack:
  - { id: 0, name: a.addr, type: default, offset: 0, size: 8, alignment: 8, 
      stack-id: default, callee-saved-register: '', callee-saved-restored: true, 
      debug-info-variable: '', debug-info-expression: '', debug-info-location: '' }
  - { id: 1, name: b.addr, type: default, offset: 0, size: 8, alignment: 8, 
      stack-id: default, callee-saved-register: '', callee-saved-restored: true, 
      debug-info-variable: '', debug-info-expression: '', debug-info-location: '' }
entry_values:    []
callSites:       []
debugValueSubstitutions: []
constants:       []
machineFunctionInfo:
  varArgsFrameIndex: 0
  varArgsSaveSize: 0
body:             |
  bb.1.entry:
    liveins: $x10, $x11, $x12, $x13
  
    %2:_(s32) = COPY $x10
    %3:_(s32) = COPY $x11
    %4:_(s32) = COPY $x12
    %5:_(s32) = COPY $x13
    %6:_(p0) = G_FRAME_INDEX %stack.0.a.addr
    %7:_(p0) = G_FRAME_INDEX %stack.1.b.addr
    G_STORE %2(s32), %6(p0) :: (store (s32) into %ir.a.addr, align 8)
    %28:_(s32) = G_CONSTANT i32 4
    %31:_(p0) = nuw inbounds G_PTR_ADD %6, %28(s32)
    %38:_(p0) = COPY %31(p0)
    G_STORE %3(s32), %38(p0) :: (store (s32) into %ir.a.addr + 4, basealign 8)
    G_STORE %4(s32), %7(p0) :: (store (s32) into %ir.b.addr, align 8)
    %27:_(p0) = nuw inbounds G_PTR_ADD %7, %28(s32)
    %35:_(p0) = COPY %27(p0)
    G_STORE %5(s32), %35(p0) :: (store (s32) into %ir.b.addr + 4, basealign 8)
    %30:_(s32) = G_LOAD %6(p0) :: (dereferenceable load (s32) from %ir.a.addr, align 8)
    %32:_(s32) = G_LOAD %31(p0) :: (dereferenceable load (s32) from %ir.a.addr + 4, basealign 8)
    %26:_(s32) = G_LOAD %7(p0) :: (dereferenceable load (s32) from %ir.b.addr, align 8)
    %29:_(s32) = G_LOAD %27(p0) :: (dereferenceable load (s32) from %ir.b.addr + 4, basealign 8)
    %25:_(s32) = G_ICMP intpred(ult), %30(s32), %26
    %24:_(s32) = G_ICMP intpred(slt), %32(s32), %29
    %23:_(s32) = G_ICMP intpred(eq), %32(s32), %29
    %21:_(s32) = G_SELECT %23(s32), %25, %24
    $x10 = COPY %21(s32)
    PseudoRET implicit $x10
...
---
name:            shl64
alignment:       2
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
  - { id: 20, class: _, preferred-register: '', flags: [  ] }
  - { id: 21, class: _, preferred-register: '', flags: [  ] }
  - { id: 22, class: _, preferred-register: '', flags: [  ] }
  - { id: 23, class: _, preferred-register: '', flags: [  ] }
  - { id: 24, class: _, preferred-register: '', flags: [  ] }
  - { id: 25, class: _, preferred-register: '', flags: [  ] }
  - { id: 26, class: _, preferred-register: '', flags: [  ] }
  - { id: 27, class: _, preferred-register: '', flags: [  ] }
  - { id: 28, class: _, preferred-register: '', flags: [  ] }
  - { id: 29, class: _, preferred-register: '', flags: [  ] }
  - { id: 30, class: _, preferred-register: '', flags: [  ] }
  - { id: 31, class: _, preferred-register: '', flags: [  ] }
  - { id: 32, class: _, preferred-register: '', flags: [  ] }
  - { id: 33, class: _, preferred-register: '', flags: [  ] }
  - { id: 34, class: _, preferred-register: '', flags: [  ] }
  - { id: 35, class: _, preferred-register: '', flags: [  ] }
  - { id: 36, class: _, preferred-register: '', flags: [  ] }
  - { id: 37, class: _, preferred-register: '', flags: [  ] }
  - { id: 38, class: _, preferred-register: '', flags: [  ] }
  - { id: 39, class: _, preferred-register: '', flags: [  ] }
  - { id: 40, class: _, preferred-register: '', flags: [  ] }
liveins:
  - { reg: '$x10', virtual-reg: '' }
  - { reg: '$x11', virtual-reg: '' }
  - { reg: '$x12', virtual-reg: '' }
frameInfo:
  isFrameAddressTaken: false
  isReturnAddressTaken: false
  hasStackMap:     false
  hasPatchPoint:   false
  stackSize:       0
  offsetAdjustment: 0
  maxAlignment:    8
  adjustsStack:    false
  hasCalls:        false
  framePointerPolicy: all
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
stack:
  - { id: 0, name: x.addr, type: default, offset: 0, size: 8, alignment: 8, 
      stack-id: default, callee-saved-register: '', callee-saved-restored: true, 
      debug-info-variable: '', debug-info-expression: '', debug-info-location: '' }
  - { id: 1, name: amt.addr, type: default, offset: 0, size: 4, alignment: 4, 
      stack-id: default, callee-saved-register: '', callee-saved-restored: true, 
      debug-info-variable: '', debug-info-expression: '', debug-info-location: '' }
entry_values:    []
callSites:       []
debugValueSubstitutions: []
constants:       []
machineFunctionInfo:
  varArgsFrameIndex: 0
  varArgsSaveSize: 0
body:             |
  bb.1.entry:
    liveins: $x10, $x11, $x12
  
    %2:_(s32) = COPY $x10
    %3:_(s32) = COPY $x11
    %1:_(s32) = COPY $x12
    %4:_(p0) = G_FRAME_INDEX %stack.0.x.addr
    %5:_(p0) = G_FRAME_INDEX %stack.1.amt.addr
    G_STORE %2(s32), %4(p0) :: (store (s32) into %ir.x.addr, align 8)
    %36:_(s32) = G_CONSTANT i32 4
    %35:_(p0) = nuw inbounds G_PTR_ADD %4, %36(s32)
    %40:_(p0) = COPY %35(p0)
    G_STORE %3(s32), %40(p0) :: (store (s32) into %ir.x.addr + 4, basealign 8)
    G_STORE %1(s32), %5(p0) :: (store (s32) into %ir.amt.addr)
    %34:_(s32) = G_LOAD %4(p0) :: (dereferenceable load (s32) from %ir.x.addr, align 8)
    %37:_(s32) = G_LOAD %35(p0) :: (dereferenceable load (s32) from %ir.x.addr + 4, basealign 8)
    %33:_(s32) = G_LOAD %5(p0) :: (dereferenceable load (s32) from %ir.amt.addr)
    %13:_(s32) = G_CONSTANT i32 32
    %16:_(s32) = G_SUB %33, %13
    %17:_(s32) = G_SUB %13, %33
    %18:_(s32) = G_CONSTANT i32 0
    %32:_(s32) = G_ICMP intpred(ult), %33(s32), %13
    %31:_(s32) = G_ICMP intpred(eq), %33(s32), %18
    %21:_(s32) = G_SHL %34, %33(s32)
    %22:_(s32) = G_LSHR %34, %17(s32)
    %23:_(s32) = G_SHL %37, %33(s32)
    %24:_(s32) = G_OR %22, %23
    %25:_(s32) = G_SHL %34, %16(s32)
    %26:_(s32) = G_SELECT %32(s32), %21, %18
    %27:_(s32) = G_SELECT %32(s32), %24, %25
    %28:_(s32) = G_SELECT %31(s32), %37, %27
    $x10 = COPY %26(s32)
    $x11 = COPY %28(s32)
    PseudoRET implicit $x10, implicit $x11
...
---
name:            mul64
alignment:       2
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
  - { id: 20, class: _, preferred-register: '', flags: [  ] }
  - { id: 21, class: _, preferred-register: '', flags: [  ] }
  - { id: 22, class: _, preferred-register: '', flags: [  ] }
  - { id: 23, class: _, preferred-register: '', flags: [  ] }
  - { id: 24, class: _, preferred-register: '', flags: [  ] }
  - { id: 25, class: _, preferred-register: '', flags: [  ] }
  - { id: 26, class: _, preferred-register: '', flags: [  ] }
  - { id: 27, class: _, preferred-register: '', flags: [  ] }
  - { id: 28, class: _, preferred-register: '', flags: [  ] }
  - { id: 29, class: _, preferred-register: '', flags: [  ] }
  - { id: 30, class: _, preferred-register: '', flags: [  ] }
  - { id: 31, class: _, preferred-register: '', flags: [  ] }
  - { id: 32, class: _, preferred-register: '', flags: [  ] }
  - { id: 33, class: _, preferred-register: '', flags: [  ] }
  - { id: 34, class: _, preferred-register: '', flags: [  ] }
  - { id: 35, class: _, preferred-register: '', flags: [  ] }
liveins:
  - { reg: '$x10', virtual-reg: '' }
  - { reg: '$x11', virtual-reg: '' }
  - { reg: '$x12', virtual-reg: '' }
  - { reg: '$x13', virtual-reg: '' }
frameInfo:
  isFrameAddressTaken: false
  isReturnAddressTaken: false
  hasStackMap:     false
  hasPatchPoint:   false
  stackSize:       0
  offsetAdjustment: 0
  maxAlignment:    8
  adjustsStack:    false
  hasCalls:        false
  framePointerPolicy: all
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
stack:
  - { id: 0, name: a.addr, type: default, offset: 0, size: 8, alignment: 8, 
      stack-id: default, callee-saved-register: '', callee-saved-restored: true, 
      debug-info-variable: '', debug-info-expression: '', debug-info-location: '' }
  - { id: 1, name: b.addr, type: default, offset: 0, size: 8, alignment: 8, 
      stack-id: default, callee-saved-register: '', callee-saved-restored: true, 
      debug-info-variable: '', debug-info-expression: '', debug-info-location: '' }
entry_values:    []
callSites:       []
debugValueSubstitutions: []
constants:       []
machineFunctionInfo:
  varArgsFrameIndex: 0
  varArgsSaveSize: 0
body:             |
  bb.1.entry:
    liveins: $x10, $x11, $x12, $x13
  
    %2:_(s32) = COPY $x10
    %3:_(s32) = COPY $x11
    %4:_(s32) = COPY $x12
    %5:_(s32) = COPY $x13
    %6:_(p0) = G_FRAME_INDEX %stack.0.a.addr
    %7:_(p0) = G_FRAME_INDEX %stack.1.b.addr
    G_STORE %2(s32), %6(p0) :: (store (s32) into %ir.a.addr, align 8)
    %25:_(s32) = G_CONSTANT i32 4
    %28:_(p0) = nuw inbounds G_PTR_ADD %6, %25(s32)
    %35:_(p0) = COPY %28(p0)
    G_STORE %3(s32), %35(p0) :: (store (s32) into %ir.a.addr + 4, basealign 8)
    G_STORE %4(s32), %7(p0) :: (store (s32) into %ir.b.addr, align 8)
    %24:_(p0) = nuw inbounds G_PTR_ADD %7, %25(s32)
    %32:_(p0) = COPY %24(p0)
    G_STORE %5(s32), %32(p0) :: (store (s32) into %ir.b.addr + 4, basealign 8)
    %27:_(s32) = G_LOAD %6(p0) :: (dereferenceable load (s32) from %ir.a.addr, align 8)
    %29:_(s32) = G_LOAD %28(p0) :: (dereferenceable load (s32) from %ir.a.addr + 4, basealign 8)
    %23:_(s32) = G_LOAD %7(p0) :: (dereferenceable load (s32) from %ir.b.addr, align 8)
    %26:_(s32) = G_LOAD %24(p0) :: (dereferenceable load (s32) from %ir.b.addr + 4, basealign 8)
    %17:_(s32) = G_MUL %27, %23
    %18:_(s32) = G_UMULH %27, %23
    %19:_(s32) = G_MUL %27, %26
    %20:_(s32) = G_MUL %29, %23
    %21:_(s32) = G_ADD %18, %19
    %22:_(s32) = G_ADD %21, %20
    $x10 = COPY %17(s32)
    $x11 = COPY %22(s32)
    PseudoRET implicit $x10, implicit $x11
...
