// RUN: not llvm-tblgen -gen-legalizer -I %p/../../include %s 2>&1 | FileCheck %s

// A memdesc set carries two types, a memory type and an alignment.  Feeding
// it to the pair form used to emit typePairInSet(0, 1, {{t0, t1}}), which
// compares only the first two fields: a strictly weaker predicate than the
// rule asked for, with no diagnostic.

include "llvm/Target/LegalizerInfo.td"

def s32 : LegalizeNamedType<"LLT::scalar(32)">;
def p0  : LegalizeNamedType<"LLT::pointer(0, 64)">;

def MemSet : LegalizeCoreMemDescSet<[LegalizeMemDesc<s32, p0, s32, 8>]>;

def R : LegalizeRuleSet<["G_STORE"]> {
  let Rules = [ CoreLegal<(CoreInPairs (Slot 0), (Slot 1), MemSet)> ];
}

// CHECK: error: core rule: 'CoreInPairs' needs a type set, but its last operand is a memory-descriptor set; use 'inMemDesc'
