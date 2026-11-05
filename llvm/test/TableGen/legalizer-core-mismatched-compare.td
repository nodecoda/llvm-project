// RUN: not llvm-tblgen -gen-legalizer-check -I %p/../../include %s 2>&1 | FileCheck %s

// A comparison does not coerce: an Int observation and a Bool feature are
// different sorts.

include "llvm/Target/LegalizerInfo.td"

def IsVector : LegalizeFeature<"Subtarget.isVector()">;

def R : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [
    CoreWidenIf<(Slot 0), (Pow2), (CoreEq (BitsOf (Slot 0)), IsVector)>
  ];
}

// CHECK: error: core rule: 'CoreEq' compares Int with Bool; both operands must have the same sort
