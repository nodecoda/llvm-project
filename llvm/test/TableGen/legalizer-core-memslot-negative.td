// RUN: not llvm-tblgen -gen-legalizer-check -I %p/../../include %s 2>&1 | FileCheck %s

// MemSlot indexes Query.MMODescrs, so it needs the same non-negative check as
// a type slot.

include "llvm/Target/LegalizerInfo.td"

def s32 : LegalizeNamedType<"LLT::scalar(32)">;

def R : LegalizeRuleSet<["G_MEM"]> {
  let Rules = [
    CoreWidenIf<(Slot 0), (Pow2), (CoreEq (MemTy (MemSlot -1)), s32)>
  ];
}

// CHECK: error: core rule: 'MemSlot' index must not be negative
