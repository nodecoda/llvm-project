// RUN: not llvm-tblgen -gen-legalizer -I %p/../../include %s 2>&1 | FileCheck %s

// Every rule has to say which action it implements: the action/mutation check
// is what ties a rule to the direction it is allowed to move types in, and a
// rule that stays silent would silently opt out of it.

include "llvm/Target/LegalizerInfo.td"

def BadRule : LegalizeRule {
  let Method = "lower";
}

def BadSet : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [BadRule];
}

// CHECK: error: legalizer rule 'lower' does not name an Action