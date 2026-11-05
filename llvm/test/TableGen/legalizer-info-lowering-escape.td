// RUN: not llvm-tblgen -gen-legalizer -I %p/../../include %s 2>&1 | FileCheck %s

// A zero-row expansion has to say why it produces no Core IR row.  Otherwise
// the escape is invisible in the generated escapes[] array, and "this builder
// never reaches the Core IR" becomes a claim nobody checks.

include "llvm/Target/LegalizerInfo.td"

def BuilderCovered : LegalizeBuilder<"covered", ActionLegal> {
  let Formals = [[]];
}
def Covered0 : LegalizeLowering<BuilderCovered, 0, ExRawMutation, [ActionLegal]>;

def Ok : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [Lower];
}

// CHECK: error: zero-row expansion 'ExRawMutation' declares actions
