// RUN: not llvm-tblgen -gen-legalizer -I %p/../../include %s 2>&1 | FileCheck %s

// A predicate no rule can reach is accepted by TableGen, produces no code, and
// says nothing; X86 accumulated 32 of them while its rules moved to the core
// IR.  The check is structural, so it only covers records whose references are
// defs.  A feature named from another feature's text is not one of those: that
// case belongs to the text scan in emitFeatures(), which legalizer-info.td
// covers.

include "llvm/Target/LegalizerInfo.td"

def s32 : LegalizeNamedType<"LLT::scalar(32)">;

def HasSSE1 : LegalizeFeature<"Subtarget.hasSSE1()">;
// Named only by TextOnly's condition string, which the structural walk cannot
// see.  Reporting it would be a false positive.
def TextOnly : LegalizeFeature<"HasSSE1 || Subtarget.is64Bit()">;

// Both the leaf and the combination that names it are unreachable, and the
// diagnostic lists both rather than only the outermost record.
def DeadLeaf : LegalizeTypePredicate<"typeIs(0, s32)">;
def DeadCombo : LegalizePredicateDag<(all_of HasSSE1, DeadLeaf)>;

def R : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [ CoreLegal<(CoreIn (Slot 0), [s32])> ];
}

// CHECK: error: legalizer predicates 'DeadCombo', 'DeadLeaf' are not reachable from any rule; delete the record
