// RUN: not llvm-tblgen -gen-legalizer -I %p/../../include %s 2>&1 | FileCheck %s

// Every builder needs one lowering entry per declared overload.  Without the
// registry such a builder reached the emitter's fallback and left the rule set
// to the C++ chain without a word; now the table itself is incomplete and says
// which builder and which overload.

include "llvm/Target/LegalizerInfo.td"

def BuilderUnlisted : LegalizeBuilder<"unlisted", ActionLegal> {
  let Formals = [[]];
}

def Ok : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [Lower];
}

// CHECK: error: builder 'BuilderUnlisted' declares overload 0 but has no lowering for it
