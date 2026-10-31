// RUN: llvm-tblgen -gen-legalizer-table -I %p/../../include %s | FileCheck %s

// A rule set with an opaque rule has a predicate whose key dependencies the
// table generator cannot see.  It must not be tabulated: filling a cell from
// the chain would still produce an answer, but that cell would stand for every
// query sharing the core rules' key, including queries the opaque predicate
// distinguishes.  The chain answers instead.

include "llvm/Target/LegalizerInfo.td"

def s32 : LegalizeNamedType<"LLT::scalar(32)">;

def Test_CORE : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [LegalFor<[s32]>];
}

def Test_OPAQUE : LegalizeRuleSet<["G_STORE"]> {
  let Rules = [
    LegalFor<[s32]>,
    LegalizeOpaqueRule<BuilderCustomIf, [{
      [=](const LegalityQuery &Q) { return Q.Types[0] == Q.Types[0]; }
    }]>,
  ];
}

def Test_ALLOpaque : LegalizeRuleSet<["G_LOAD"]> {
  let Rules = [
    LegalizeOpaqueRule<BuilderCustomIf, [{
      [=](const LegalityQuery &Q) { return Q.Types[0] == Q.Types[0]; }
    }]>,
  ];
}

// CHECK: // Not tabulated because they contain an opaque rule; the chain answers:
// CHECK-NEXT: //   Test_OPAQUE
// CHECK-NEXT: //   Test_ALLOpaque
// CHECK: // Test_CORE
// CHECK-NOT: // Test_OPAQUE
// CHECK-NOT: // Test_ALLOpaque
// CHECK: case TargetOpcode::G_ADD: return 0;
// CHECK-NOT: case TargetOpcode::G_STORE:
// CHECK-NOT: case TargetOpcode::G_LOAD:
