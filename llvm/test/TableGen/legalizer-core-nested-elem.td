// RUN: not llvm-tblgen -gen-legalizer-check -I %p/../../include %s 2>&1 | FileCheck %s

// A shim can observe a slot or one element of a slot.  A nested ElemOf used
// to be accepted as an "elem" observation while its TypeIdx silently fell
// back to 0, so the generated predicate read the wrong slot.

include "llvm/Target/LegalizerInfo.td"

def R : LegalizeRuleSet<["G_NESTED"]> {
  let Rules = [CoreWiden<(ElemOf (ElemOf (Slot 1))), (Pow2)>];
}

// CHECK: error: core rule: nested 'ElemOf' has no type-index projection
