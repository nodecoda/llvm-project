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

// An expressible rule whose *predicate* is raw C++: the rule is in the dump,
// so the escape ledger counts nothing, but the key collector cannot see which
// slots the text reads.  Tabulating it would key the table on whatever the
// other rules mention and answer the rest first-cell-wins.
def RawPred
    : LegalizeQueryPredicate<"[=](const LegalityQuery &Q) { return Q.Types[1].isScalar(); }">;
def Test_OPAQUE_PRED : LegalizeRuleSet<["G_PHI"]> {
  let Rules = [LegalFor<[s32]>, ScalarizeIf<RawPred, 0>];
}

// Expressible in the Core IR, but reads the memory type as a whole while
// naming none.  A size-keyed axis would merge two different types of the same
// size, and a class-keyed one has nothing to enumerate, so there is no finite
// key for it: refused, not guessed.  A rule set that does name memory types is
// keyed by class instead; legalizer-info-memclass.td covers that.
def DiffMem : LegalizeTypePredicate<"typeDiffersFromMemDesc(0, 0)">;
def Test_NEEDSCLASS : LegalizeRuleSet<["G_EXTRACT"]> {
  let Rules = [CustomIf<DiffMem>];
}

// CHECK: // Not tabulated because the mem axis compares the memory type as a whole
// CHECK-NEXT: // and no rule names one to key it by; the chain answers:
// CHECK-NEXT: //   Test_NEEDSCLASS
// CHECK: // Not tabulated because they contain a predicate written as raw C++,
// CHECK-NEXT: // so the slots it reads are unknown to the key; the chain answers:
// CHECK-NEXT: //   Test_OPAQUE_PRED
// CHECK: // Not tabulated because they contain a rule the Core IR cannot express;
// CHECK-NEXT: // the chain answers:
// CHECK-NEXT: //   Test_OPAQUE
// CHECK-NEXT: //   Test_CHOICE
// CHECK-NEXT: //   Test_ALLOpaque
// CHECK: // Test_CORE
// CHECK-NOT: // Test_OPAQUE
// CHECK-NOT: // Test_ALLOpaque
// CHECK-NOT: // Test_CHOICE
// CHECK: case TargetOpcode::G_ADD: return 0;
// CHECK-NOT: case TargetOpcode::G_STORE:
// CHECK-NOT: case TargetOpcode::G_LOAD:
// CHECK-NOT: case TargetOpcode::G_PHI:
// CHECK-NOT: case TargetOpcode::G_PTRTOINT:
// CHECK-NOT: case TargetOpcode::G_EXTRACT:
