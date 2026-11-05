// RUN: llvm-tblgen -gen-legalizer -I %p/../../include %s | FileCheck %s
// RUN: llvm-tblgen -gen-legalizer-core -I %p/../../include %s | FileCheck %s --check-prefix=CORE

// A lane clamp's bound can be chosen by a cached feature, and the chain of
// features nests the way the C++ select-lanes argument does.  The literal form
// keeps an `int` parameter because a dag parameter cannot take a bare integer;
// these two classes are the dag form of the same rule.
//
// A select's branches have to stay query-independent: the whole point is that
// the value is fixed once the subtarget is known, which is what lets the table
// evaluator share one entry per feature signature.

include "llvm/Target/LegalizerInfo.td"

def s32 : LegalizeNamedType<"LLT::scalar(32)">;
def HasAVX2 : LegalizeFeature<"Subtarget.hasAVX2()">;
def HasBWI : LegalizeFeature<"Subtarget.hasBWI()">;

def Clamps : LegalizeRuleSet<["G_LANES"]> {
  let Rules = [
    CoreClampMaxNumElementsSel<0, s32,
        (CoreSelect HasBWI, 64, (CoreSelect HasAVX2, 32, 16))>,
    CoreClampMinNumElementsSel<0, s32, (CoreSelect HasAVX2, 2, 4)>
  ];
}

// CHECK: .coreFewerTo([=](const LegalityQuery &Query) { return (Query.Types[0].isVector()) && (!(Query.Types[0].isScalableVector())) && (((Query.Types[0]).getElementType() == s32)) && (((Query.Types[0]).getNumElements() > (HasBWI ? 64 : (HasAVX2 ? 32 : 16)))); }, 0, [=](const LegalityQuery &Query) { return std::make_pair(0, LLT::scalarOrVector(ElementCount::getFixed((HasBWI ? 64 : (HasAVX2 ? 32 : 16))), (Query.Types[0]).getElementType())); })
// CHECK: .coreMoreTo([=](const LegalityQuery &Query) { return (Query.Types[0].isVector()) && (!(Query.Types[0].isScalableVector())) && (((Query.Types[0]).getElementType() == s32)) && (((Query.Types[0]).getNumElements() < (HasAVX2 ? 2 : 4))); }, 0, [=](const LegalityQuery &Query) { return std::make_pair(0, LLT::fixed_vector((HasAVX2 ? 2 : 4), (Query.Types[0]).getElementType())); })
// CORE: "op": "fewer"
// CORE: "kind":"select"
