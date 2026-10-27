// RUN: not llvm-tblgen -gen-legalizer -I %p/../../include %s 2>&1 | FileCheck %s

// An LGExpr that is nothing but a boolean combination of declared features
// could have been written with the predicate AST.  Allowing it would turn the
// AST back into opaque text: the schema could no longer tell a predicate's
// structure from a string, which is the whole point of describing rules here.

include "llvm/Target/LegalizerInfo.td"

def Ctx : LegalizeContext {
  let Bools = [
    LG_Bool<"HasSSE1", "Subtarget.hasSSE1()">,
    LG_Bool<"UseX87", "Subtarget.hasX87()">,
  ];
}

def Bad : LegalizeRuleSet<["G_FSQRT"]> {
  let Rules = [LG_LegalFor<LGExpr<"HasSSE1 || UseX87">, ["s32"], 0>];
}

// CHECK: error: LGExpr 'HasSSE1 || UseX87' is a boolean combination of declared features; write it with LGAnd / LGOr / LGNot
