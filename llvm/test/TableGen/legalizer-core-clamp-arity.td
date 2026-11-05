// RUN: not llvm-tblgen -gen-legalizer-check -I %p/../../include %s 2>&1 | FileCheck %s

// Clamp is a range, and a range needs both ends.  Arity is checked before the
// operand sorts, so the diagnostic names the operator and the count.

include "llvm/Target/LegalizerInfo.td"

def R : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [
    CoreWidenIf<(Slot 0), (Pow2), (CoreIn (NumElems (Slot 0)), (Clamp 8))>
  ];
}

// CHECK: error: core rule: 'Clamp' expects 2 operands
