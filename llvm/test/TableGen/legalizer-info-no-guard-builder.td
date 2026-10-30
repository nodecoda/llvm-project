// RUN: not llvm-tblgen -gen-legalizer -I %p/../../include %s 2>&1 | FileCheck %s
// RUN: not llvm-tblgen -gen-legalizer-core -I %p/../../include %s 2>&1 | FileCheck %s

// lower() has no guard overload at all.  A rule that carried one used to emit
// `.lower(HasSSE1, )`, which no builder declares -- and the C++ emitter and the
// Core IR emitter disagreed about whether that was an error.

include "llvm/Target/LegalizerInfo.td"

def HasSSE1 : LegalizeFeature<"Subtarget.hasSSE1()">;

def BadRule : LegalizeGuardedCall<BuilderLower, HasSSE1, []>;

def Bad : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [BadRule];
}

// CHECK: error: legalizer builder 'lower' takes no guard
