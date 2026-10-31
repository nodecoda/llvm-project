// RUN: llvm-tblgen -gen-legalizer-core -I %p/../../include %s | FileCheck %s --check-prefix=CORE
// RUN: llvm-tblgen -gen-legalizer -I %p/../../include %s | FileCheck %s --check-prefix=CPP
// RUN: llvm-tblgen -gen-legalizer-table -I %p/../../include %s | FileCheck %s --check-prefix=TABLE

// scalarOrEltWiderThan reads the *element* size, so a v4s32 answers 32 and not
// 128.  The X86 rule set used this through LegalizeQueryPredicate, i.e. raw
// C++, which the Core IR could only record as an opaque leaf: the dump kept the
// rule but lost both the slot it reads and the answer.  It is a leaf now.
//
// The point of the third check is the key: the table generator learns which
// slots a rule reads from the leaf *names*, so a name that is not in its table
// contributes no slot.  Test_WIDE has no other type predicate, so if the leaf
// were missing there the table would have no keyed axis (NumSlots 0) and would
// answer every query from one cell.

include "llvm/Target/LegalizerInfo.td"

def s32 : LegalizeNamedType<"LLT::scalar(32)">;
def v4s32 : LegalizeNamedType<"LLT::fixed_vector(4, 32)">;
def WiderIn0 : LegalizeTypePredicate<"scalarOrEltWiderThan(0, 64)">;

def Test_WIDE : LegalizeRuleSet<["G_IMPLICIT_DEF"]> {
  let Rules = [ScalarizeIf<WiderIn0, 0>];
}

// The width is a literal, so the Core IR keeps it as one; the slot is the
// leading integer, which is what the key collector reads.
// CORE: "name":"scalarOrEltWiderThan","args":[0,"64"]
// CORE-SAME: "name":"isVector","args":[0]
// CORE-NOT: "name":"opaque"

// CPP: .scalarizeIf([=](const LegalityQuery &Query) { return scalarOrEltWiderThan(0, 64)(Query); }, 0);

// TABLE: /* 0*/ {1, {0, 0, 0},
// TABLE-SAME: // Test_WIDE
