// RUN: not llvm-tblgen -gen-legalizer -I %p/../../include %s 2>&1 | FileCheck %s

// Two rule sets for the same opcode silently append rules to the same builder
// chain at runtime, so the second one only takes effect for queries the first
// one did not answer.  Nothing in the C++ API flags it; -gen-legalizer does.

include "llvm/Target/LegalizerInfo.td"

def First : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [Lower];
}

def Second : LegalizeRuleSet<["G_ADD", "G_SUB"]> {
  let Rules = [Libcall];
}

// CHECK: error: opcode 'G_ADD' is described by more than one rule set