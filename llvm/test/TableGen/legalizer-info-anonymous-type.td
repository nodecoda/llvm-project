// RUN: not llvm-tblgen -gen-legalizer -I %p/../../include %s 2>&1 | FileCheck %s

// A type is emitted as its record name, so an inline instantiation would put
// `anonymous_0` in the generated call.  Say so where the type is written.

include "llvm/Target/LegalizerInfo.td"

def Bad : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [
    LegalFor<[LegalizeNamedType<"LLT::scalar(8)">]>,
  ];
}

// CHECK: error: a legalizer type has to be a named def, e.g. `def s32 : LegalizeNamedType<...>`