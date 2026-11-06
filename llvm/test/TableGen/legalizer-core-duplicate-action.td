// RUN: not llvm-tblgen -gen-legalizer -I %p/../../include %s 2>&1 | FileCheck %s

// The action table is a function of (operator, target sort).  Two rows with
// the same key would make it a relation, and every lookup would silently take
// the first one -- so the duplicate has to be an error, not an ordering rule.

include "llvm/Target/LegalizerInfo.td"

def s32 : LegalizeNamedType<"LLT::scalar(32)">;

// One extra row is enough: "legal" already has one in LegalizerCore.td.
def DupLegal : LegalizeCoreAction<"legal", "", "Custom">;

def R : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [ CoreLegal<(CoreTrue)> ];
}

// CHECK: error: core action table has 2 rows for operator 'legal' with target sort '<none>'
