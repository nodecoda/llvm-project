// RUN: not llvm-tblgen -gen-legalizer -I %p/../../include %s 2>&1 | FileCheck %s

// A dag leaf must name a LegalizeFeature: anything else would put text back
// into the predicate, which is what the AST exists to prevent.  (A leaf naming
// a record the target never defined is rejected earlier, by TableGen itself.)

include "llvm/Target/LegalizerInfo.td"

def HasAVX : LegalizeFeature<"Subtarget.hasAVX()">;

def Bad : LegalizeCompound<(any_of HasAVX, "HasAVXX")>;

def BadSet : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [LegalizeLegalFor<Bad, ["s32"], 0>];
}

// CHECK: error: predicate dag leaf '"HasAVXX"' is not a LegalizeFeature