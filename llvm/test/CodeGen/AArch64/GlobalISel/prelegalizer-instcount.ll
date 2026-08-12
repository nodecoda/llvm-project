; REQUIRES: asserts
; RUN: llc -mtriple=aarch64 -O0 -global-isel=1 -global-isel-abort=1 \
; RUN:   --save-stats=obj -o %t.s %s
; RUN: FileCheck %s < %t.stats

; CHECK-DAG: "mir-instcount-prelegalizer.NumG_BRCONDInst": 1
; CHECK-DAG: "mir-instcount-prelegalizer.NumG_BRInst": 2
; CHECK-DAG: "mir-instcount-prelegalizer.NumG_TRUNCInst": 2
; CHECK-DAG: "mir-instcount-prelegalizer.TotalInsts": 5
; CHECK-DAG: "mir-instcount-preregbankselect.NumG_ANDInst": 1
; CHECK-DAG: "mir-instcount-preregbankselect.NumG_BRCONDInst": 1
; CHECK-DAG: "mir-instcount-preregbankselect.NumG_BRInst": 2
; CHECK-DAG: "mir-instcount-preregbankselect.NumG_CONSTANTInst": 1
; CHECK-DAG: "mir-instcount-preregbankselect.TotalInsts": 5
; CHECK-DAG: "mir-instcount-preinstructionselect.NumG_ANDInst": 1
; CHECK-DAG: "mir-instcount-preinstructionselect.NumG_BRCONDInst": 1
; CHECK-DAG: "mir-instcount-preinstructionselect.NumG_BRInst": 2
; CHECK-DAG: "mir-instcount-preinstructionselect.NumG_CONSTANTInst": 1
; CHECK-DAG: "mir-instcount-preinstructionselect.TotalInsts": 5

define void @f(i1 %cond) {
entry:
  br i1 %cond, label %then, label %exit

then:
  br label %exit

exit:
  ret void
}
