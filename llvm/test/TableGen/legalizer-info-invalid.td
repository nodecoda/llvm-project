// RUN: not llvm-tblgen -gen-legalizer -I %p/../../include %s 2>&1 | FileCheck %s

// The point of the schema: an action and a mutation that disagree cannot be
// expressed without -gen-legalizer rejecting it.  In hand-written C++ these
// two pieces of information live in different places and drift silently;
// see https://github.com/llvm/llvm-project/issues/220802.

include "llvm/Target/LegalizerInfo.td"

def BadRule : LegalizeRule {
  let Method = "widenScalarToNextPow2";
  let Args = ["1", "16"];
  let Action = ActionNarrowScalar;
  let Mutation = "widen";
}

def BadSet : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [BadRule];
}

// CHECK: error: action 'NarrowScalar' requires a 'narrow' mutation, but this rule carries 'widen'