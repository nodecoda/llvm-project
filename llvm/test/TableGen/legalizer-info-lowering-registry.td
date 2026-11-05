// RUN: llvm-tblgen -gen-legalizer-check -I %p/../../include %s | FileCheck %s

// The -gen-legalizer-check report: every backend runs the same verification,
// this one prints what it covered.  The counts here are for this file only.

include "llvm/Target/LegalizerInfo.td"

def s32 : LegalizeNamedType<"LLT::scalar(32)">;

def Ok : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [LegalFor<[s32]>, Lower];
}

// CHECK:       "builders":
// CHECK:       "overloads":
// CHECK:       "entries":
// CHECK:       "rule_sets": 1,
// CHECK:       "rules": 2,
// CHECK:       "escapes": 0,
// CHECK:       "rule_sets_without_table_translation": 0
