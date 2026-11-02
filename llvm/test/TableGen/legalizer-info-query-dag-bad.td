// RUN: not llvm-tblgen -gen-legalizer -I %p/../../include %s 2>&1 | FileCheck %s

// A type predicate is a callable, so a bool dag cannot hold one: the C++ would
// be `legalFor(typeIs(0, s32), {s32})`, which no overload accepts.  Rejecting
// it while the .td is parsed names the offending line instead of leaving the
// error to the target build, where it points at generated code.

include "llvm/Target/LegalizerInfo.td"

def HasSSE1 : LegalizeFeature<"Subtarget.hasSSE1()">;
def s32 : LegalizeNamedType<"LLT::scalar(32)">;
def IsS32 : LegalizeTypePredicate<"typeIs(0, s32)">;

def BoolDagWithTypeLeaf : LegalizeBoolPredicateDag<(all_of (not HasSSE1), IsS32)>;

def Bad : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [LegalForIf<BoolDagWithTypeLeaf, [s32]>];
}

// CHECK: error: predicate dag leaf 'IsS32' reads the query, so the dag has to be a LegalizeQueryPredicateDag
