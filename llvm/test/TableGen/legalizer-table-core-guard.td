// RUN: llvm-tblgen -gen-legalizer-table -legalizer-table-prefix=Guard -I %p/../../include %s | FileCheck %s
// RUN: llvm-tblgen -gen-legalizer-table-points -legalizer-table-prefix=Guard -I %p/../../include %s | FileCheck %s --check-prefix=POINTS

// The table's key metadata is derived from what the rules read.  A core rule
// has a tree instead of a predicate string, so the arity and the memory
// descriptor axis have to be computed from the tree.  Missing either is
// silent: the evaluator is then handed a query whose Types or MMODescrs array
// is shorter than the guard indexes, and reads past its end.

include "llvm/Target/LegalizerInfo.td"

def p0 : LegalizeNamedType<"LLT::pointer(0, 64)">;
def s32 : LegalizeNamedType<"LLT::scalar(32)">;

// Nothing but the guard says this query has two type slots.
def TwoSlots : LegalizeRuleSet<["G_TWO"]> {
  let Rules = [
    CoreLower<(CoreGt (BitsOf (Slot 1)), 64)>
  ];
}

// The descriptor is read by the guard, not named by a memdesc tuple.
def MemDescGuard : LegalizeRuleSet<["G_MEM"]> {
  let Rules = [
    CoreLegal<(CoreAll (CoreEq (Slot 0), (Ty s32)),
                       (CoreEq (MemTy (MemSlot 0)), (Ty s32)))>
  ];
}

// The two class counts are the arity: one per type slot the rule set reads.
// CHECK: 0, 1}, {2, 2}, kNoMemDesc, 0}, // TwoSlots
// The last field counts memdesc classes; zero means the key has no descriptor
// axis, which is what a guard-only descriptor read used to produce.
// CHECK: 1}, {2, 2}, 0, GuardNumMemClasses * GuardNumAlignClasses}, // MemDescGuard

// CHECK: if (((Q.Types[1]).getScalarSizeInBits() > 64)) {

// The probe carries one type per slot the rule set reads, so a guard-only
// second slot has to widen it as well; a one-type probe would let the
// evaluator index Types[1] of a query that has none.
// POINTS: {TargetOpcode::G_TWO, "G_TWO", 2, {LLT::pointer(0, PointerBits), LLT::pointer(0, PointerBits), LLT()
