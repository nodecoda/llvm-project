// RUN: not llvm-tblgen -gen-legalizer -I %p/../../include %s 2>&1 | FileCheck %s

// An opaque body is printed as it was written and the emitter never looks at
// Args, exactly like Pred: a rule carrying both would lose the arguments
// without a word.

include "llvm/Target/LegalizerInfo.td"

def s32 : LegalizeNamedType<"LLT::scalar(32)">;

// legalFor's bool guard matches the opaque body and its ShapeType overload
// matches the argument, so both halves of the rule parse; the rule is still
// wrong because the body would be printed and the argument dropped.
def BadRule : LegalizeOpaqueRule<BuilderLegalFor, [{ true }]> {
  let Args = [s32];
}

def Bad : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [BadRule];
}

// CHECK: error: legalizer rule 'legalFor' has both Args and an opaque body; the arguments would be dropped