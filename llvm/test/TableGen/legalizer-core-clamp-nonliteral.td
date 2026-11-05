// RUN: not llvm-tblgen -gen-legalizer-check -I %p/../../include %s 2>&1 | FileCheck %s

// A Clamp in an `in` guard is a compile-time interval.  An Int-sorted
// expression such as BitsOf is not a literal, so accepting it used to emit
// an interval of [0, 8] without any diagnostic.

include "llvm/Target/LegalizerInfo.td"

def R : LegalizeRuleSet<["G_CLAMP"]> {
  let Rules = [
    CoreWidenIf<(Slot 0), (Pow2),
                (CoreIn (NumElems (Slot 0)), (Clamp (BitsOf (Slot 0)), 8))>
  ];
}

// CHECK: error: core rule: 'Clamp' lower bound must be an integer literal
