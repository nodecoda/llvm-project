// RUN: not llvm-tblgen -gen-legalizer -I %p/../../include %s 2>&1 | FileCheck %s

// An opaque body is printed as it was written, so the emitter never looks at
// Pred: a rule carrying both would lose its guard without a word.

include "llvm/Target/LegalizerInfo.td"

def HasAVX : LegalizeQueryPredicate<"isVector(0)">;

def BadRule : LegalizeOpaqueRule<BuilderLegalIf, [{ typeIs(0, s8) }]> {
  let Pred = HasAVX;
}

def Bad : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [BadRule];
}

// CHECK: error: legalizer rule 'legalIf' has both a Pred and an opaque body; the predicate would be dropped
