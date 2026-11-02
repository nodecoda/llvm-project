// RUN: not llvm-tblgen -gen-legalizer -I %p/../../include %s 2>&1 | FileCheck %s

// An opcode may be described by only one rule set: two rule sets would silently
// append their rules to the same LegalizerInfo rule set at run time.

include "llvm/Target/LegalizerInfo.td"

def First : LegalizeRuleSet<["G_CTPOP"]> {
  let Rules = [LegalFor<True, ["s32, s32"]>];
}

def Second : LegalizeRuleSet<["G_CTPOP"]> {
  let Rules = [LegalFor<True, ["s32, s32"]>];
}

// CHECK: error: opcode 'G_CTPOP' is described by more than one rule set
