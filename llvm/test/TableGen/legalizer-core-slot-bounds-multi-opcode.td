// RUN: not llvm-tblgen -gen-legalizer -I %p/../../include %s 2>&1 | FileCheck %s

// A rule applies to every opcode in its set, so it has to fit the smallest of
// them.  Slot 1 is a real slot for G_STORE (type0 is the stored value, ptype1
// the address) and does not exist for G_ADD, which has one type slot.

include "llvm/Target/LegalizerInfo.td"
include "llvm/Target/Target.td"

def T_s32 : LegalizeNamedType<"LLT::scalar(32)">;

def R : LegalizeRuleSet<["G_ADD", "G_STORE"]> {
  let Rules = [ CoreWiden<(Slot 1), (Pow2)> ];
}

// CHECK: error: core rule: rule set 'R' reads type slot 1, but G_ADD has 1 type slot(s)
