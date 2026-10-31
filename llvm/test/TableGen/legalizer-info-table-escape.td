// RUN: llvm-tblgen -gen-legalizer-table -I %p/../../include %s | FileCheck %s

// A rule set with an escape has a predicate whose key dependencies the table
// generator cannot see.  It must not be tabulated: filling a cell from the
// chain would still produce an answer, but that cell would stand for every
// query sharing the core rules' key, including queries the escaped rule
// distinguishes.  The chain answers instead.
//
// "Escape" is every rule that expands to no core rule, not just a raw C++
// body.  A feature-dependent type list is no longer one by itself -- it is
// residualized into guarded rules, and legalizer-info-param-bounds.td covers
// that -- but a choice whose arms name no type list still hides the same key
// dependency one level down.  Test_CHOICE is the negative control for that
// half: names that cannot be resolved stay escapes, never guesses.

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

def Test_CHOICE : LegalizeRuleSet<["G_PTRTOINT"]> {
  let Rules = [
    LegalForCartesianProduct<
        LegalizeTypeListChoice<"Is64Bit ? PtrTypes64 : PtrTypes32">,
        LegalizeInlineTypeList<[s32]>>,
  ];
}

def Test_ALLOpaque : LegalizeRuleSet<["G_LOAD"]> {
  let Rules = [
    LegalizeOpaqueRule<BuilderCustomIf, [{
      [=](const LegalityQuery &Q) { return Q.Types[0] == Q.Types[0]; }
    }]>,
  ];
}

// Expressible in the Core IR, but reads the memory type as a whole.  The table
// key today is an access size, which would merge two different 64-bit types;
// the rule set is refused until the mem axis is keyed by type class.
def DiffMem : LegalizeTypePredicate<"typeDiffersFromMemDesc(0, 0)">;
def Test_NEEDSCLASS : LegalizeRuleSet<["G_EXTRACT"]> {
  let Rules = [CustomIf<DiffMem>];
}

// CHECK: // Not tabulated because they contain a rule the Core IR cannot express;
// CHECK-NEXT: // the chain answers:
// CHECK-NEXT: //   Test_OPAQUE
// CHECK-NEXT: //   Test_CHOICE
// CHECK-NEXT: //   Test_ALLOpaque
// CHECK: // Not tabulated because the key cannot represent an axis their rules read;
// CHECK-NEXT: // the chain answers:
// CHECK-NEXT: //   Test_NEEDSCLASS
// CHECK: // Test_CORE
// CHECK-NOT: // Test_OPAQUE
// CHECK-NOT: // Test_ALLOpaque
// CHECK-NOT: // Test_CHOICE
// CHECK: case TargetOpcode::G_ADD: return 0;
// CHECK-NOT: case TargetOpcode::G_STORE:
// CHECK-NOT: case TargetOpcode::G_LOAD:
// CHECK-NOT: case TargetOpcode::G_PTRTOINT:
// CHECK-NOT: case TargetOpcode::G_EXTRACT:
