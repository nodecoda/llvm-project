// RUN: llvm-tblgen -gen-legalizer-core -I %p/../../include %s | FileCheck %s

// Scalarize<Idx> is emitted as set_lanes 1, and that spelling is exact rather
// than a shorthand: ElementCount::isScalar() is true for exactly one element,
// so changeElementCount(getFixed(1)) returns getScalarType() -- the element
// type for a vector -- which is what LegalizeMutations::scalarize returns.
// LLT cannot represent a one-lane vector, so there is no other type it could
// mean.

// CHECK: "action":"FewerElements"
// CHECK: "mutation":{"target":0,"kind":"set_lanes","value":1}
// CHECK-NOT: "kind":"scalarize"

include "llvm/Target/LegalizerInfo.td"

def Foo_BAR : LegalizeRuleSet<["G_BAR"]> {
  let Rules = [Scalarize<0>];
}