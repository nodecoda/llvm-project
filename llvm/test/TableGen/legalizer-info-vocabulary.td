// RUN: not llvm-tblgen -gen-legalizer -I %p/../../include %s 2>&1 | FileCheck %s

// A predicate that names a feature the target never declared is a typo.  The
// C++ compiler catches it too, but only after the generated file exists, with
// a diagnostic pointing at machine-generated code.

include "llvm/Target/LegalizerInfo.td"

def Ctx : LegalizeContext {
  let Bools = [LG_Bool<"HasAVX", "Subtarget.hasAVX()">];
}

def Bad : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [LG_LegalFor<LGFeature<"HasAVXX">, ["s32"], 0>];
}

// CHECK: error: feature 'HasAVXX' is not declared by a LegalizeContext
