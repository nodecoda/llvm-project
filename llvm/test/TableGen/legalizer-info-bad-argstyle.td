// RUN: not llvm-tblgen -gen-legalizer -I %p/../../include %s 2>&1 | FileCheck %s

// The argument layout is chosen by the schema and rendered by the emitter, so
// a target cannot invent one; a wrong style is caught here rather than in the
// generated C++.

include "llvm/Target/LegalizerInfo.td"

def BadRule : LegalizeCall<"legalFor", ["s32"], ActionLegal> {
  let ArgStyle = "braces";
}

def BadSet : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [BadRule];
}

// CHECK: error: 'braces' is not a legalizer argument style