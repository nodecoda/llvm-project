// RUN: not llvm-tblgen -gen-legalizer -I %p/../../include %s 2>&1 | FileCheck %s

// Mutation directions are a closed set.  Hand-written C++ has no such thing --
// the direction is implied by the mutation the rule performs, and a typo in a
// comment is invisible -- so the direction is spelled out here and checked.

include "llvm/Target/LegalizerInfo.td"

def BadRule : LegalizeCall<"lower", [], ActionLower, "wiiden">;

def BadSet : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [BadRule];
}

// CHECK: error: 'wiiden' is not a legalizer mutation direction