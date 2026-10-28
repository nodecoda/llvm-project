// RUN: not llvm-tblgen -gen-legalizer -I %p/../../include %s 2>&1 | FileCheck %s

// A rule cannot name its own action.  It used to be able to, and nothing read
// that field: -gen-legalizer wrote the C++ for the builder and
// -gen-legalizer-core wrote the action of the builder, so a rule saying
// "Legal" while naming lower() described two different rules, one per emitter.
// The action now belongs to the builder, and naming one is a template error.

include "llvm/Target/LegalizerInfo.td"

def BadRule : LegalizeCall<BuilderLower, [], ActionLegal>;

def Bad : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [BadRule];
}

// CHECK: error: Too many template arguments: 3
