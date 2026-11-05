// RUN: not llvm-tblgen -gen-legalizer-check -I %p/../../include %s 2>&1 | FileCheck %s

// BitsOf observes a type; a MemDesc has a SizeOf, not an element size.

include "llvm/Target/LegalizerInfo.td"

def R : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [
    CoreWidenIf<(Slot 0), (Pow2), (CoreGt (BitsOf (MemSlot 0)), 64)>
  ];
}

// CHECK: error: core rule: 'BitsOf' operand 0 expects sort Type, got MemDesc
