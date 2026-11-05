// RUN: not llvm-tblgen -gen-legalizer-check -I %p/../../include %s 2>&1 | FileCheck %s

// A tuple operator tests as many slots as the set has columns.  A one-slot
// set under `CoreInPairs` used to be a pair query with an unconstrained
// second slot, which the table domain then could not describe.

include "llvm/Target/LegalizerInfo.td"

def s32 : LegalizeNamedType<"LLT::scalar(32)">;
def s64 : LegalizeNamedType<"LLT::scalar(64)">;

def IntSet : LegalizeCoreTypeSet<[s32, s64]>;

def R : LegalizeRuleSet<["G_PAIR"]> {
  let Rules = [
    CoreLegal<(CoreInPairs (Slot 0), (Slot 1), IntSet)>
  ];
}

// CHECK: error: core rule: 'CoreInPairs' tests 2 slot(s) but its set holds 1-tuples
