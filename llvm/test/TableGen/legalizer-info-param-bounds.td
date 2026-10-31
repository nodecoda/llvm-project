// RUN: llvm-tblgen -gen-legalizer-core -I %p/../../include %s | FileCheck %s --check-prefix=CORE
// RUN: llvm-tblgen -gen-legalizer-table -I %p/../../include %s | FileCheck %s --check-prefix=TABLE

// A bound spelled as a feature conditional used to make the whole rule set an
// escape: expandRule() demanded a literal type, so the rule produced no core
// rule, and the table is not built for a rule set with an escape.
//
// The bound is residualized instead: one rule per branch, guarded by
// hasFeature, with the payload still literal.  That keeps the key complete --
// every branch reads the same slot -- and leaves the condition visible in the
// Core IR instead of resolving it against one configuration.
//
// The negative controls at the bottom are expressions this expansion refuses
// (an unparenthesised conditional in the "then" arm).  They must stay escapes,
// not become guesses.

include "llvm/Target/LegalizerInfo.td"

def HasAVX : LegalizeFeature<"Subtarget.hasAVX()">;
def HasAVX512 : LegalizeFeature<"Subtarget.hasAVX512()">;

def T_s16 : LegalizeNamedType<"LLT::scalar(16)">;
def T_s32 : LegalizeNamedType<"LLT::scalar(32)">;
def T_s64 : LegalizeNamedType<"LLT::scalar(64)">;
def T_v4s16 : LegalizeNamedType<"LLT::fixed_vector(4, 16)">;
def T_v8s16 : LegalizeNamedType<"LLT::fixed_vector(8, 16)">;
def T_v16s16 : LegalizeNamedType<"LLT::fixed_vector(16, 16)">;

def T_NestedMax : LegalizeNamedType<
    "HasAVX512 ? T_v16s16 : HasAVX ? T_v8s16 : T_v4s16">;

def T_List32 : LegalizeNamedTypeList<[T_s16, T_s32]>;
def T_List64 : LegalizeNamedTypeList<[T_s16, T_s32, T_s64]>;

// An inline literal is one alternative, so the bound stays a single rule.
def Test_LITERAL : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [MinScalar<0, LegalizeInlineType<"LLT::scalar(32)">>];
}

// A conditional bound is one rule per branch.  The narrowing branch is
// HasAVX ? s64 : s32, which is also the order minScalar().maxScalar() uses:
// widen first, then narrow.
def Test_INLINE : LegalizeRuleSet<["G_SUB"]> {
  let Rules = [ClampScalar<0, T_s16,
                           LegalizeInlineType<"HasAVX ? T_s64 : T_s32">>];
}

// Nested conditionals accumulate: "then" keeps both conditions, "else" negates
// the outer one before taking the next question.
def Test_NESTED : LegalizeRuleSet<["G_MUL"]> {
  let Rules = [ClampNumElements<0, T_v4s16, T_NestedMax>];
}

// A choice between two type lists is the union of the two.  A type both arms
// name is legal under either condition and must be emitted unguarded; only the
// arm-specific s64 keeps the condition.
def Test_CHOICE : LegalizeRuleSet<["G_PTRTOINT"]> {
  let Rules = [LegalForCartesianProduct<T_List32,
      LegalizeTypeListChoice<"HasAVX ? T_List64 : T_List32">>];
}

// Negative control: the inner colon of an unparenthesised "then" arm cannot be
// told from the outer one, so the expression is refused rather than guessed.
def Test_UNPAREN : LegalizeRuleSet<["G_AND"]> {
  let Rules = [MinScalar<0,
      LegalizeInlineType<"HasAVX ? HasAVX512 ? T_s64 : T_s32 : T_s16">>];
}

// CORE-LABEL: "def": "Test_LITERAL"
// CORE: "name":"scalarNarrowerThan","args":[0,"32"]{{.*}}"ruleset": "Test_LITERAL"

// CORE-LABEL: "def": "Test_INLINE"
// CORE: "name":"scalarNarrowerThan","args":[0,"16"]{{.*}}"value":"s16"
// CORE: "name":"hasFeature","args":["Subtarget.hasAVX()"]{{.*}}"name":"scalarWiderThan","args":[0,"64"]{{.*}}"value":"s64"
// CORE: "kind":"not","arg":{{.*}}"name":"hasFeature","args":["Subtarget.hasAVX()"]{{.*}}"name":"scalarWiderThan","args":[0,"32"]{{.*}}"value":"s32"

// CORE-LABEL: "def": "Test_NESTED"
// CORE: "name":"elementTypeIs","args":[0,"s16"]{{.*}}"name":"fewerElementsThan","args":[0,4]{{.*}}"value":4
// CORE: "name":"hasFeature","args":["Subtarget.hasAVX512()"]{{.*}}"name":"elementTypeIs","args":[0,"s16"]{{.*}}"value":16
// CORE: "kind":"not","arg":{{.*}}"hasFeature","args":["Subtarget.hasAVX512()"]{{.*}}"hasFeature","args":["Subtarget.hasAVX()"]{{.*}}"value":8
// CORE: "kind":"not","arg":{{.*}}"hasFeature","args":["Subtarget.hasAVX512()"]{{.*}}"kind":"not","arg":{{.*}}"hasFeature","args":["Subtarget.hasAVX()"]{{.*}}"value":4

// CORE-LABEL: "def": "Test_CHOICE"
// CORE: "typeIs","args":[0,"s16"]},{"kind":"leaf","name":"typeIs","args":[1,"s16"]{{.*}}"ruleset": "Test_CHOICE"
// CORE: "typeIs","args":[0,"s16"]},{"kind":"leaf","name":"typeIs","args":[1,"s32"]{{.*}}"ruleset": "Test_CHOICE"
// CORE: "hasFeature","args":["Subtarget.hasAVX()"]{{.*}}"typeIs","args":[0,"s16"]},{"kind":"leaf","name":"typeIs","args":[1,"s64"]
// CORE: "typeIs","args":[0,"s32"]},{"kind":"leaf","name":"typeIs","args":[1,"s16"]{{.*}}"ruleset": "Test_CHOICE"
// CORE: "typeIs","args":[0,"s32"]},{"kind":"leaf","name":"typeIs","args":[1,"s32"]{{.*}}"ruleset": "Test_CHOICE"
// CORE: "hasFeature","args":["Subtarget.hasAVX()"]{{.*}}"typeIs","args":[0,"s32"]},{"kind":"leaf","name":"typeIs","args":[1,"s64"]
// CORE-NOT: "kind":"not"{{.*}}"typeIs","args":[1,"s64"]

// CORE: "kind": "nonliteral_type"{{.*}}"ruleset": "Test_UNPAREN"

// TABLE: // Not tabulated because they contain a rule the Core IR cannot express;
// TABLE-NEXT: // the chain answers:
// TABLE-NEXT: //   Test_UNPAREN
// TABLE: static constexpr LegalizerTypeClass
// The row ends with the theta atoms the table reads: the booleans its own rules
// mention, one entry per atom in the generated list.  Test_INLINE is guarded by
// HasAVX alone, Test_NESTED by HasAVX and HasAVX512, Test_CHOICE by HasAVX.
// TABLE: TestAtomsT1, 1, TestCellsT1},   // Test_INLINE
// TABLE: TestAtomsT2, 2, TestCellsT2},   // Test_NESTED
// TABLE: TestAtomsT3, 1, TestCellsT3},   // Test_CHOICE
// TABLE-NOT: //   Test_UNPAREN
