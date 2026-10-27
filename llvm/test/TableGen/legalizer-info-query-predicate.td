// RUN: not llvm-tblgen -gen-legalizer -I %p/../../include %s 2>&1 | FileCheck %s

// scalarizeIf() takes a LegalityPredicate -- a std::function<bool(const
// LegalityQuery &)> -- while legalFor() and friends take a bool computed once
// in the constructor.  They are different C++ types, so a cached feature here
// would only fail when the generated fragment is compiled; the two classes keep
// the mistake in the .td.

include "llvm/Target/LegalizerInfo.td"

def HasAVX2 : LegalizeFeature<"Subtarget.hasAVX2()">;
def s32 : LegalizeNamedType<"LLT::scalar(32)">;

def Bad : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [
    LegalForIf<HasAVX2, [s32]>,
    ScalarizeIf<HasAVX2, 0>,
  ];
}

// CHECK: error: Value specified for template argument 'ScalarizeIf:P' is of type LegalizeFeature; expected type LegalizeQueryPredicate: HasAVX2