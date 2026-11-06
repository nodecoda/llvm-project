// RUN: not llvm-tblgen -gen-legalizer -I %p/../../include %s 2>&1 | FileCheck %s

// The table must cover every operator.  This is the half that used to be
// impossible to check from C++ when the rows lived in an array: a new
// operator parsed fine and then failed in every rule that used it.

include "llvm/Target/LegalizerInfo.td"

def s32 : LegalizeNamedType<"LLT::scalar(32)">;

def Frobnicate : LegalizeCoreOperator<"frobnicate", SortType, [SortType]>;

def R : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [ CoreLegal<(CoreTrue)> ];
}

// CHECK: error: core action table has no row for operator 'Frobnicate' with target sort Type
