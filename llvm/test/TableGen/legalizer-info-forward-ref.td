// RUN: not llvm-tblgen -gen-legalizer -I %p/../../include %s 2>&1 | FileCheck %s

// Types are emitted in .td order, so a type that names one declared further
// down would be emitted before its declaration and the generated C++ would not
// compile.  The name sits inside a C++ expression, which TableGen cannot
// resolve, so -gen-legalizer has to look for it.

include "llvm/Target/LegalizerInfo.td"

def Wide : LegalizeNamedType<"Subtarget.is64Bit() ? s64 : s32">;
def s32 : LegalizeNamedType<"LLT::scalar(32)">;
def s64 : LegalizeNamedType<"LLT::scalar(64)">;

def Bad : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [Lower];
}

// CHECK: error: 's64' is used by 'Wide' but declared below it; the fragment is emitted in .td order