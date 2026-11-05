// RUN: not llvm-tblgen -gen-legalizer-check -I %p/../../include %s 2>&1 | FileCheck %s

// A slot index is an index into Query.Types.  A negative index used to be
// formatted as an unsigned value and read out of bounds at run time.

include "llvm/Target/LegalizerInfo.td"

def R : LegalizeRuleSet<["G_NEG"]> {
  let Rules = [
    CoreWidenIf<(Slot 0), (Pow2), (CoreGt (BitsOf (Slot -1)), 8)>
  ];
}

// CHECK: error: core rule: 'Slot' index must not be negative
