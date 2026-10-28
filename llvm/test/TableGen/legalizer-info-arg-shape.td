// RUN: not llvm-tblgen -gen-legalizer -I %p/../../include %s 2>&1 | FileCheck %s

// An integer argument does not say what it means.  A slot index, a bit width
// and a lane count were one class, so a lane count could be written where a
// slot index belongs; the rule was well formed here and wrong at run time.

include "llvm/Target/LegalizerInfo.td"

def s8 : LegalizeNamedType<"LLT::scalar(8)">;

// clampMinNumElements(Idx, EltTy, MinElements): this puts a lane count first.
def BadRule
    : LegalizeCall<BuilderClampMinNumElements,
                   [LegalizeArgLanes<8>, s8, LegalizeArgLanes<8>]>;

def Bad : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [BadRule];
}

// CHECK: error: legalizer builder 'clampMinNumElements' does not take these arguments; declared (ShapeIndex, ShapeType, ShapeLanes)
