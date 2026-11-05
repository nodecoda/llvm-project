// RUN: not llvm-tblgen -gen-legalizer-check -I %p/../../include %s 2>&1 | FileCheck %s

// `typePairAndMemDescInSet` takes the memory slot index beside the two type
// indices.  A type slot in its place would be emitted as an MMO index and read
// the wrong array.

include "llvm/Target/LegalizerInfo.td"

def s32 : LegalizeNamedType<"LLT::scalar(32)">;
def s64 : LegalizeNamedType<"LLT::scalar(64)">;

def MemSet : LegalizeCoreMemDescSet<[LegalizeMemDesc<s32, s64, s32, 8>]>;

def R : LegalizeRuleSet<["G_LOAD"]> {
  let Rules = [
    CoreLegal<(CoreInMemDesc (Slot 0), (Slot 1), (Slot 0), MemSet)>
  ];
}

// CHECK: error: core rule: the third operand of 'CoreInMemDesc' must be a memory slot
