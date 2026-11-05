// RUN: not llvm-tblgen -gen-legalizer-check -I %p/../../include %s 2>&1 | FileCheck %s

// The first member of a tuple set fixes the arity; every later member has to
// have the same number of columns, or the C++ initializer would not be
// homogeneous.

include "llvm/Target/LegalizerInfo.td"

def s32 : LegalizeNamedType<"LLT::scalar(32)">;
def s64 : LegalizeNamedType<"LLT::scalar(64)">;

def R : LegalizeRuleSet<["G_RAGGED"]> {
  let Rules = [
    CoreLegal<(CoreInPairs (Slot 0), (Slot 1), [[s32, s64], [s32]])>
  ];
}

// CHECK: error: core rule: every member of a tuple set must have the same number of columns
