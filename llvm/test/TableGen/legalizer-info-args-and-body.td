// RUN: not llvm-tblgen -gen-legalizer -I %p/../../include %s 2>&1 | FileCheck %s

// An opaque body is printed as it was written and the emitter never looks at
// Args, exactly like Pred: a rule carrying both would lose the arguments
// without a word.

include "llvm/Target/LegalizerInfo.td"

def BadRule : LegalizeOpaqueRule<BuilderLegalIf, [{ typeIs(0, s8) }]> {
  let Args = [LegalizeArgInt<0>];
}

def Bad : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [BadRule];
}

// CHECK: error: legalizer rule 'legalIf' has both Args and an opaque body; the arguments would be dropped