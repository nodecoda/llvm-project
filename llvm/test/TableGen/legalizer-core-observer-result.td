// RUN: not llvm-tblgen -gen-legalizer-check -I %p/../../include %s 2>&1 | FileCheck %s

// NumElems yields an Int, so it cannot be the operand of ElemOf, which
// observes a Type.  The diagnostic has to name the observer, the expected
// sort, the actual sort, and the .td line.

include "llvm/Target/LegalizerInfo.td"

def R : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [CoreWiden<(ElemOf (NumElems (Slot 0))), (Pow2)>];
}

// CHECK: error: core rule: 'ElemOf' operand 0 expects sort Type, got Int
