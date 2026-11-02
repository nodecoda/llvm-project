// RUN: not llvm-tblgen -gen-legalizer -I %p/../../include %s 2>&1 | FileCheck %s
// RUN: not llvm-tblgen -gen-legalizer-core -I %p/../../include %s 2>&1 | FileCheck %s

// A rule set with an empty Rules list has nothing to emit.  The C++ emitter
// rejected it; the JSON emitter used to accept it and then drop the record,
// because a rule set whose every rule escaped is omitted on purpose and an
// empty one looked the same.  The two outputs have to agree on the input, so
// both reject it now.

include "llvm/Target/LegalizerInfo.td"

def Test_EMPTY : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [];
}

// CHECK: error: legalizer rule set has no rules
