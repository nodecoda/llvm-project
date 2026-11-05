// RUN: not llvm-tblgen -gen-legalizer-check -I %p/../../include %s 2>&1 | FileCheck %s

// A descriptor carries an alignment, which is an integer and cannot sit in a
// homogeneous type list.  The set is therefore all descriptors or no
// descriptors, never a mixture.

include "llvm/Target/LegalizerInfo.td"

def s32 : LegalizeNamedType<"LLT::scalar(32)">;
def p0 : LegalizeNamedType<"LLT::pointer(0, 64)">;

def R : LegalizeRuleSet<["G_MIXED"]> {
  let Rules = [
    CoreLegal<(CoreInMemDesc (Slot 0), (Slot 1), (MemSlot 0),
                             [LegalizeMemDesc<s32, p0, s32, 8>, s32])>
  ];
}

// CHECK: error: core rule: a memory-descriptor set may only hold LegalizeMemDesc members
