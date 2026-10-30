// RUN: not llvm-tblgen -gen-legalizer -I %p/../../include %s 2>&1 | FileCheck %s

// An opaque predicate spelled with nothing but declared features and boolean
// operators could have been written with the dag form.  Allowing it would turn
// the AST back into opaque text: the schema could no longer tell a predicate's
// structure from a string, which is the whole point of describing rules here.

include "llvm/Target/LegalizerInfo.td"

def HasSSE1 : LegalizeFeature<"Subtarget.hasSSE1()">;
def UseX87 : LegalizeFeature<"Subtarget.hasX87()">;
def s32 : LegalizeNamedType<"LLT::scalar(32)">;

def Bad : LegalizeRuleSet<["G_FSQRT"]> {
  let Rules = [
    LegalForIf<LegalizeOpaqueBool<"HasSSE1 || UseX87">, [s32]>,
  ];
}

// CHECK: error: LegalizeOpaqueBool 'HasSSE1 || UseX87' is a boolean combination of declared features; write it with all_of / any_of / not