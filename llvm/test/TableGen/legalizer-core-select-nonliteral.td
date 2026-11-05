// RUN: not llvm-tblgen -gen-legalizer-check -I %p/../../include %s 2>&1 | FileCheck %s

// Select chooses one compile-time integer.  Its branches must be literals or
// nested selects, so the value is fixed once the subtarget is known and the
// C++ shim and the 0.3 mutation projection agree on it.

include "llvm/Target/LegalizerInfo.td"

def Is32 : LegalizeFeature<"Subtarget.is32()">;

def R : LegalizeRuleSet<["G_SELECT"]> {
  let Rules = [
    CoreWiden<(Slot 0), (CoreSelect Is32, (BitsOf (Slot 0)), 32)>
  ];
}

// CHECK: error: core rule: 'Select' branches must be non-negative integer literals or nested selects
