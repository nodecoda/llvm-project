// RUN: not llvm-tblgen -gen-legalizer -I %p/../../include %s 2>&1 | FileCheck %s

// Mutation directions are a closed set, and a rule may only override the one
// its builder's action implies when the builder is a composite (clampScalar,
// clampNumElements) that picks a direction at query time.  A misspelling used
// to reach the emitters, which keyed off it.

include "llvm/Target/LegalizerInfo.td"

def s32 : LegalizeNamedType<"LLT::scalar(32)">;

def BadRule : ClampScalar<1, s32, s32> {
  let Mutation = "wiiden";
}

def BadSet : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [BadRule];
}

// CHECK: error: 'wiiden' is not a legalizer mutation direction
