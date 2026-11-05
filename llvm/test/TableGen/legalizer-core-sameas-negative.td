// RUN: not llvm-tblgen -gen-legalizer-check -I %p/../../include %s 2>&1 | FileCheck %s

// SameAs takes a slot index and the shim reads Query.Types[that index].
// A negative index is not a usable operand.

include "llvm/Target/LegalizerInfo.td"

def R : LegalizeRuleSet<["G_SAME"]> {
  let Rules = [CoreWiden<(Slot 0), (SameAs -1)>];
}

// CHECK: error: core rule: 'SameAs' index must not be negative
