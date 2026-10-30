// RUN: not llvm-tblgen -gen-legalizer -I %p/../../include %s 2>&1 | FileCheck %s

// "Every rule must say which action it implements" is no longer something a
// rule can get wrong: the action comes from the builder it names.  What a rule
// can still get wrong is not naming a builder at all, which is what this test
// pins down now.

include "llvm/Target/LegalizerInfo.td"

def BadRule : LegalizeRule;

def BadSet : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [BadRule];
}

// CHECK: error: legalizer rule is missing its Builder
