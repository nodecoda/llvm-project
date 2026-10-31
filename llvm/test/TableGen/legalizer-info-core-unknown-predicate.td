// RUN: not llvm-tblgen -gen-legalizer-core -I %p/../../include %s 2>&1 | FileCheck %s

// The type-predicate vocabulary is a whitelist, not "whatever the C++ API
// has".  scalarOrEltNarrowerThan exists in LegalizerInfo.h and is the mirror of
// the leaf next door, but nothing in the Core IR defines it yet, so a rule that
// uses it is refused instead of being written out as a leaf the evaluator would
// have to guess at.

include "llvm/Target/LegalizerInfo.td"

def s32 : LegalizeNamedType<"LLT::scalar(32)">;
def NarrowerIn0 : LegalizeTypePredicate<"scalarOrEltNarrowerThan(0, 64)">;

def Test_UNKNOWN : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [ScalarizeIf<NarrowerIn0, 0>];
}

// CHECK: error: type predicate 'scalarOrEltNarrowerThan' has no Core IR expansion
