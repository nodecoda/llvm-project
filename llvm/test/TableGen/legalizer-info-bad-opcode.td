// RUN: not llvm-tblgen -gen-legalizer -I %p/../../include %s 2>&1 | FileCheck %s

// A rule set must name generic opcodes; a target-specific opcode would be
// accepted by the C++ builder but is not part of the legalizer vocabulary.

include "llvm/Target/LegalizerInfo.td"

def Bad : LegalizeRuleSet<["X86_CTPOP"]> {
  let Rules = [LegalFor<True, ["s32, s32"]>];
}

// CHECK: error: 'X86_CTPOP' is not a generic opcode
