// RUN: not llvm-tblgen -gen-legalizer -I %p/../../include %s 2>&1 | FileCheck %s

// A row must name a declared operator; otherwise it is dead data that reads
// like it does something.

include "llvm/Target/LegalizerInfo.td"

def s32 : LegalizeNamedType<"LLT::scalar(32)">;

def StrayRow : LegalizeCoreAction<"frobnicate", "", "Legal">;

def R : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [ CoreLegal<(CoreTrue)> ];
}

// CHECK: error: core action table names 'frobnicate', which is not a LegalizeCoreOperator
