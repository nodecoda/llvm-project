// RUN: not llvm-tblgen -gen-legalizer-check -I %p/../../include %s 2>&1 | FileCheck %s

// A terminal operator has no target, so a rule that supplies one is an error
// rather than a rule whose second operand is silently ignored.

include "llvm/Target/LegalizerInfo.td"

def R : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [LegalizeCoreRule<(LegalOp (Slot 0))>];
}

// CHECK: error: core rule: operator 'LegalOp' expects 0 operand(s), got 1
