// RUN: not llvm-tblgen -gen-legalizer -I %p/../../include %s 2>&1 | FileCheck %s

// A guard is not "a boolean the rule may attach".  The C++ builder declares
// which guard overloads it has, and there are two, with different types:
//
//   legalFor(bool Pred, initializer_list<LLT>)   -- cached bool
//   legalIf(const LegalityPredicate &)           -- callable
//
// Passing one where the other is expected used to be accepted here and fail
// when the target was compiled.

include "llvm/Target/LegalizerInfo.td"

def Always : LegalizeQueryPredicate<"always">;

// legalFor takes a cached bool; this hands it a callable.
def BadRule : LegalizeQueryGuardedCall<BuilderLegalFor, Always, []>;

def Bad : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [BadRule];
}

// CHECK: error: legalizer builder 'legalFor' takes a different kind of guard
