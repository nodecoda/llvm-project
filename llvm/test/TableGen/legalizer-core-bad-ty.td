// RUN: not llvm-tblgen -gen-legalizer-check -I %p/../../include %s 2>&1 | FileCheck %s

// Ty names a LegalizeNamedType.  Anything else cannot be rendered as the
// LLT constant the generated code refers to.

include "llvm/Target/LegalizerInfo.td"

def R : LegalizeRuleSet<["G_TY"]> {
  let Rules = [CoreWiden<(Slot 0), (Ty (Slot 0))>];
}

// CHECK: error: core rule: 'Ty' expects a LegalizeNamedType record
