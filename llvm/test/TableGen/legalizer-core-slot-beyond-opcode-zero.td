// RUN: not llvm-tblgen -gen-legalizer -I %p/../../include %s 2>&1 | FileCheck %s

// Zero is a real bound, not "no information": G_BR has no type operands, so
// its LegalityQuery has no type slots.  A rule that reads one has to fail
// rather than make the set look like a one-slot one.

include "llvm/Target/LegalizerInfo.td"
include "llvm/Target/Target.td"

def T_s32 : LegalizeNamedType<"LLT::scalar(32)">;

def R : LegalizeRuleSet<["G_BR"]> {
  let Rules = [ CoreLegal<(CoreIn (Slot 0), [T_s32])> ];
}

// CHECK: error: core rule: rule set 'R' reads type slot 0, but G_BR has 0 type slot(s)
