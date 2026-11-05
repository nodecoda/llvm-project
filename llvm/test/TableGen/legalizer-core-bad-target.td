// RUN: not llvm-tblgen -gen-legalizer-check -I %p/../../include %s 2>&1 | FileCheck %s

// Pow2 is an Int constructor, so it cannot stand where Ty's operand expects a
// type.  This is the "literal constructor used at the wrong sort" case.

include "llvm/Target/LegalizerInfo.td"

def R : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [CoreWiden<(Slot 0), (Ty (Pow2))>];
}

// CHECK: error: core rule: 'Ty' operand 0 expects sort Type, got Int
