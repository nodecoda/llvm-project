// RUN: not llvm-tblgen -gen-legalizer-check -I %p/../../include %s 2>&1 | FileCheck %s

// A tuple set is a table of shapes.  It is only meaningful as the last
// operand of `CoreInPairs`/`CoreInTriples`; as a value it would silently read
// as a set of its first column.

include "llvm/Target/LegalizerInfo.td"

def s32 : LegalizeNamedType<"LLT::scalar(32)">;
def s64 : LegalizeNamedType<"LLT::scalar(64)">;

def PairSet : LegalizeCoreTypePairSet<[LegalizeTypePair<s32, s64>]>;
def Is32 : LegalizeFeature<"Subtarget.is32()">;

def R : LegalizeRuleSet<["G_PAIR"]> {
  let Rules = [
    CoreWidenIf<(Slot 0), (Pow2), (CoreAll Is32, PairSet)>
  ];
}

// CHECK: error: core rule: 'PairSet' is a set of 2-tuples and can only be the last operand of 'inPairs' or 'inTriples'
