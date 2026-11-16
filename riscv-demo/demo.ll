; ModuleID = '/home/ubuntu/llvm/trees/pr-ctpop/riscv-demo/demo.c'
source_filename = "/home/ubuntu/llvm/trees/pr-ctpop/riscv-demo/demo.c"
target datalayout = "e-m:e-p:64:64-i64:64-i128:128-n32:64-S128"
target triple = "riscv64-unknown-unknown-elf"

; Function Attrs: mustprogress nofree norecurse nosync nounwind willreturn memory(none)
define dso_local i64 @xdemo_madd(i64 noundef %a, i64 noundef %b, i64 noundef %c) local_unnamed_addr #0 {
entry:
  %mul = mul nsw i64 %b, %a
  %add = add nsw i64 %mul, %c
  ret i64 %add
}

; Function Attrs: mustprogress nofree norecurse nosync nounwind willreturn memory(none)
define dso_local i64 @xdemo_madd_commuted(i64 noundef %a, i64 noundef %b, i64 noundef %c) local_unnamed_addr #0 {
entry:
  %mul = mul nsw i64 %b, %a
  %add = add nsw i64 %c, %mul
  ret i64 %add
}

; Function Attrs: mustprogress nofree norecurse nosync nounwind willreturn memory(none)
define dso_local i64 @xdemo_dot2(i64 noundef %a, i64 noundef %b, i64 noundef %c, i64 noundef %d) local_unnamed_addr #0 {
entry:
  %mul = mul nsw i64 %b, %a
  %mul1 = mul nsw i64 %d, %c
  %add = add nsw i64 %mul1, %mul
  ret i64 %add
}

attributes #0 = { mustprogress nofree norecurse nosync nounwind willreturn memory(none) "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="generic-rv64" "target-features"="+64bit,+a,+c,+i,+m,+relax,+zaamo,+zalrsc,+zca,+zmmul,-b,-d,-e,-experimental-p,-experimental-smcsps,-experimental-smehv,-experimental-smidctrl,-experimental-smijt,-experimental-smip,-experimental-smnip,-experimental-smpmpmt,-experimental-sscsps,-experimental-ssehv,-experimental-ssidctrl,-experimental-ssijt,-experimental-ssip,-experimental-ssnip,-experimental-svukte,-experimental-xqccmi,-experimental-xqccmt,-experimental-xsfmclic,-experimental-xsfsclic,-experimental-y,-experimental-zibi,-experimental-zicfilp,-experimental-zilx,-experimental-zvabd,-experimental-zvbc32e,-experimental-zvdot4a8i,-experimental-zvfbdota32f,-experimental-zvfbfa,-experimental-zvfofp8min,-experimental-zvfqwbdota8f,-experimental-zvfqwdota8f,-experimental-zvfwbdota16bf,-experimental-zvfwdota16bf,-experimental-zvkgs,-experimental-zvqwbdota16i,-experimental-zvqwbdota8i,-experimental-zvqwdota16i,-experimental-zvqwdota8i,-experimental-zvvfmm,-experimental-zvvmm,-experimental-zvvmtls,-experimental-zvvmttls,-experimental-zvzip,-experimental-zyhybrid,-f,-h,-q,-sdext,-sdtrig,-sha,-shcounterenw,-shgatpa,-shlcofideleg,-shtvala,-shvsatpa,-shvstvala,-shvstvecd,-smaia,-smcdeleg,-smcntrpmf,-smcsrind,-smctr,-smdbltrp,-smepmp,-smmpm,-smnpm,-smpmpdeleg,-smrnmi,-smstateen,-ssaia,-ssccfg,-ssccptr,-sscofpmf,-sscounterenw,-sscsrind,-ssctr,-ssdbltrp,-ssnpm,-sspm,-sspmp,-sspmpen,-ssqosid,-ssstateen,-ssstrict,-sstc,-sstvala,-sstvecd,-ssu64xl,-supm,-svade,-svadu,-svbare,-svinval,-svnapot,-svpbmt,-svrsw60t59b,-svvptc,-v,-xaifet,-xandesbfhcvt,-xandesperf,-xandesvbfhcvt,-xandesvdot,-xandesvpackfph,-xandesvsinth,-xandesvsintload,-xcheriot,-xcvalu,-xcvbi,-xcvbitmanip,-xcvelw,-xcvmac,-xcvmem,-xcvsimd,-xdemo,-xmipscbop,-xmipscmov,-xmipsexectl,-xmipslsp,-xqccmp,-xqci,-xqcia,-xqciac,-xqcibi,-xqcibm,-xqcicli,-xqcicm,-xqcics,-xqcicsr,-xqciint,-xqciio,-xqcilb,-xqcili,-xqcilia,-xqcilo,-xqcilsm,-xqcisim,-xqcisls,-xqcisync,-xsfcease,-xsfmm128t,-xsfmm16t,-xsfmm32a,-xsfmm32a16f,-xsfmm32a32f,-xsfmm32a8f,-xsfmm32a8i,-xsfmm32t,-xsfmm64a64f,-xsfmm64t,-xsfmmbase,-xsfvcp,-xsfvfbfexp16e,-xsfvfexp16e,-xsfvfexp32e,-xsfvfexpa,-xsfvfexpa64e,-xsfvfnrclipxfqf,-xsfvfwmaccqqq,-xsfvqmaccdod,-xsfvqmaccqoq,-xsifivecdiscarddlone,-xsifivecflushdlone,-xsmtvdot,-xsmtvdotii,-xtheadba,-xtheadbb,-xtheadbs,-xtheadcmo,-xtheadcondmov,-xtheadfmemidx,-xtheadmac,-xtheadmemidx,-xtheadmempair,-xtheadsync,-xtheadvdot,-xwchc,-za128rs,-za64rs,-zabha,-zacas,-zalasr,-zama16b,-zawrs,-zba,-zbb,-zbc,-zbkb,-zbkc,-zbkx,-zbs,-zcb,-zcd,-zce,-zcf,-zclsd,-zcmop,-zcmp,-zcmt,-zdinx,-zfa,-zfbfmin,-zfh,-zfhmin,-zfinx,-zhinx,-zhinxmin,-zic64b,-zicbom,-zicbop,-zicboz,-ziccamoa,-ziccamoc,-ziccid,-ziccif,-zicclsm,-ziccrse,-zicfiss,-zicntr,-zicond,-zicsr,-zifencei,-zihintntl,-zihintpause,-zihpm,-zilsd,-zimop,-zk,-zkn,-zknd,-zkne,-zknh,-zkr,-zks,-zksed,-zksh,-zkt,-ztso,-zvbb,-zvbc,-zve32f,-zve32x,-zve64d,-zve64f,-zve64x,-zvfbfmin,-zvfbfwma,-zvfh,-zvfhmin,-zvkb,-zvkg,-zvkn,-zvknc,-zvkned,-zvkng,-zvknha,-zvknhb,-zvks,-zvksc,-zvksed,-zvksg,-zvksh,-zvkt,-zvl1024b,-zvl128b,-zvl16384b,-zvl2048b,-zvl256b,-zvl32768b,-zvl32b,-zvl4096b,-zvl512b,-zvl64b,-zvl65536b,-zvl8192b" }

!llvm.module.flags = !{!0, !1, !2, !4}
!llvm.ident = !{!5}
!llvm.errno.tbaa = !{!6}

!0 = !{i32 2, !"debugger-tuning", !"gdb"}
!1 = !{i32 1, !"target-abi", !"lp64"}
!2 = !{i32 6, !"riscv-isa", !3}
!3 = !{!"rv64i2p1_m2p0_a2p1_c2p0_zmmul1p0_zaamo1p0_zalrsc1p0_zca1p0"}
!4 = !{i32 8, !"SmallDataLimit", i32 0}
!5 = !{!"clang version 24.0.0git (https://github.com/nodecoda/llvm-project.git 756265e4ce650c26861c4850668e6d42022edcbc)"}
!6 = !{!7, !8, i64 0}
!7 = !{!"__libc_errno", !8, i64 0}
!8 = !{!"int", !9, i64 0}
!9 = !{!"omnipotent char", !10, i64 0}
!10 = !{!"Simple C/C++ TBAA"}
