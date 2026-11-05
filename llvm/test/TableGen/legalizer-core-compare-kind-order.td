// RUN: not llvm-tblgen -gen-legalizer-check -I %p/../../include %s 2>&1 | FileCheck %s

// A Kind is a tag, not an ordered number.  Relational comparisons used to be
// folded into equality, so `lt` silently meant `isVector()`.

include "llvm/Target/LegalizerInfo.td"

def R : LegalizeRuleSet<["G_KIND"]> {
  let Rules = [
    CoreWidenIf<(Slot 0), (Pow2), (CoreLt (SortOf (Slot 0)), KindVector)>
  ];
}

// CHECK: error: core rule: 'CoreLt' cannot order values of sort Kind; only eq/ne are defined
