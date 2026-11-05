// RUN: not llvm-tblgen -gen-legalizer-check -I %p/../../include %s 2>&1 | FileCheck %s

// LLT has operator== but no operator<.  A `lt` over Type used to emit
// `Query.Types[0] < s32`, which only fails when the target is compiled.

include "llvm/Target/LegalizerInfo.td"

def s32 : LegalizeNamedType<"LLT::scalar(32)">;

def R : LegalizeRuleSet<["G_TYPE"]> {
  let Rules = [
    CoreWidenIf<(Slot 0), (Pow2), (CoreLt (Slot 0), (Ty s32))>
  ];
}

// CHECK: error: core rule: 'CoreLt' cannot order values of sort Type; only eq/ne are defined
