// RUN: not llvm-tblgen -gen-legalizer -I %p/../../include %s 2>&1 | FileCheck %s

// The registry is keyed by (builder, overload): an entry for an overload the
// builder does not declare is as much an error as a missing one.  LegalFor
// declares overloads 0..3, so 9 is out of range.

include "llvm/Target/LegalizerInfo.td"

def Extra : LegalizeLowering<BuilderLegalFor, 9, ExOne, [ActionLegal]>;

def Ok : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [Lower];
}

// CHECK: error: lowering registry has 65 entries for 64 declared overloads
