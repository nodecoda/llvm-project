// RUN: not llvm-tblgen -gen-legalizer-check -I %p/../../include %s 2>&1 | FileCheck %s

// `typePairInSet` takes slot indices, not values, so every leading operand of
// `CoreInPairs` names a slot.  A general type expression cannot be emitted as
// an index.

include "llvm/Target/LegalizerInfo.td"

def s32 : LegalizeNamedType<"LLT::scalar(32)">;
def s64 : LegalizeNamedType<"LLT::scalar(64)">;

def PairSet : LegalizeCoreTypePairSet<[LegalizeTypePair<s32, s64>]>;

def R : LegalizeRuleSet<["G_PAIR"]> {
  let Rules = [
    CoreLegal<(CoreInPairs (ElemOf (Slot 0)), (Slot 1), PairSet)>
  ];
}

// CHECK: error: core rule: operand 0 of 'CoreInPairs' must name a type slot, e.g. (Slot 0)
