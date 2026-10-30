// RUN: not llvm-tblgen -gen-legalizer -I %p/../../include %s 2>&1 | FileCheck %s
// RUN: not llvm-tblgen -gen-legalizer-core -I %p/../../include %s 2>&1 | FileCheck %s

// A builder declares how many argument positions it has, so a rule that
// supplies the wrong number is rejected here instead of emitting a call that
// fails when the target is compiled.  Both emitters apply the check: the Core
// IR one used to skip it entirely, and the two then disagreed about whether a
// description was well formed.

include "llvm/Target/LegalizerInfo.td"

def s32 : LegalizeNamedType<"LLT::scalar(32)">;

// clampScalar takes (Idx, MinTy, MaxTy); this supplies one argument.
def BadRule : LegalizeCall<BuilderClampScalar, [s32]>;

def Bad : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [BadRule];
}

// CHECK: error: legalizer builder 'clampScalar' does not take these arguments; declared (ShapeIndex, ShapeType, ShapeType)
