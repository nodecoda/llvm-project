// RUN: not llvm-tblgen -gen-legalizer -I %p/../../include %s 2>&1 | FileCheck %s

// A shape is one field, so a constructor has exactly one.  The price of that
// is exhaustiveness: adding a shape and forgetting to teach the checker about
// it must be loud, not a silent fall-through into the fixed-arity path.

include "llvm/Target/LegalizerInfo.td"

def s32 : LegalizeNamedType<"LLT::scalar(32)">;

def ShapeBogus : LegalizeCoreShape<"bogus">;
def BogusOp : LegalizeCoreExpr<"bogus", [SortBool], SortBool, ShapeBogus>;

def R : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [ CoreLegal<(BogusOp (CoreEq (Slot 0), (Ty s32)))> ];
}

// CHECK: error: core rule: 'BogusOp' has an unknown shape 'bogus'
