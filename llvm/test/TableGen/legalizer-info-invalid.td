// RUN: not llvm-tblgen -gen-legalizer -I %p/../../include %s 2>&1 | FileCheck %s

// An action and a mutation that disagree cannot be expressed at all any more:
// the action belongs to the builder, so there is no field for a rule to
// disagree with.  What is left to check is a rule that overrides the direction
// its builder implies with one the action cannot produce.
//
// This is the bug class behind https://github.com/llvm/llvm-project/issues/220802:
// in hand-written C++ the builder call and the mutation live in two different
// places and nothing ties them together.

include "llvm/Target/LegalizerInfo.td"

def s32 : LegalizeNamedType<"LLT::scalar(32)">;

def BadRule : MinScalar<1, s32> {
  let Mutation = "narrow";
}

def BadSet : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [BadRule];
}

// CHECK: error: action 'WidenScalar' requires a 'widen' mutation, but this rule carries 'narrow'
